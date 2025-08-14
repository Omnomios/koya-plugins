/*
   Koya Plugin: Process

   What this is:
   - Application code that spawns and manages child processes, exposing a JS
     interface that feels similar to Node.js child_process streams.

   Koya vs Application responsibilities:
   - Koya provides the JS context and hook registration.
   - The plugin (application) manages OS resources (pipes, fds, signals) and
     cross-thread communication.

   Exposed JS API (selected):
   - spawn({ cmd, args? }) -> { pid, id, stdout.on/off, stderr.on/off, on('exit'), write(), closeStdin(), kill() }
   - exec(cmd) -> Promise<{ stdout, stderr }>
   - drain() to pump IO and emit events (also wired to `update`)

   Design pattern you can reuse:
   - Do blocking IO in the background and queue events; deliver on the engine
     thread in the `update` hook to keep JS single-threaded and safe.
*/
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <memory>
#include <unordered_map>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../../sdk/quickjs/quickjs.h"
#include "../../module_hooks.h"

// Tracks a single child process and its pipes/buffers.
struct ChildProcess {
    pid_t pid = -1;
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    std::string stdout_buf;
    std::string stderr_buf;
    int exit_code = -1;
    bool exited = false;
    bool exit_emitted = false;
};

static std::mutex g_proc_mutex;
static std::unordered_map<uint32_t, std::unique_ptr<ChildProcess>> g_processes;
static std::atomic<uint32_t> g_next_id{1};
static JSContext* g_ctx = nullptr;

// Per-process listener lists for stdout, stderr, and exit events.
struct ListenerLists {
    std::vector<JSValue> stdout_data;
    std::vector<JSValue> stderr_data;
    std::vector<JSValue> exit_listeners;
};
static std::unordered_map<uint32_t, ListenerLists> g_listeners;

struct ExecRequest {
    uint32_t id;
    std::string out;
    std::string err;
    JSValue resolve;
    JSValue reject;
};
static std::unordered_map<uint32_t, ExecRequest> g_exec_requests;

// Utility: convert JS string to std::string.
static std::string js_to_std_string(JSContext* ctx, JSValueConst v) {
    const char* c = JS_ToCString(ctx, v);
    std::string s = c ? c : "";
    if (c) JS_FreeCString(ctx, c);
    return s;
}

// JS: spawn({ cmd, args? }) -> processObject with stdout/stderr event emitters.
static JSValue js_spawn(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "spawn expects an options object");
    }

    JSValue cmdv = JS_GetPropertyStr(ctx, argv[0], "cmd");
    if (!JS_IsString(cmdv)) {
        JS_FreeValue(ctx, cmdv);
        return JS_ThrowTypeError(ctx, "spawn: options.cmd (string) is required");
    }
    std::string cmd = js_to_std_string(ctx, cmdv);
    JS_FreeValue(ctx, cmdv);

    JSValue argvv = JS_GetPropertyStr(ctx, argv[0], "args");
    std::vector<std::string> args;
    if (JS_IsArray(ctx, argvv)) {
        uint32_t len = 0;
        JS_GetPropertyUint32(ctx, argvv, 0); // touch
        JSValue lenv = JS_GetPropertyStr(ctx, argvv, "length");
        JS_ToInt32(ctx, (int32_t*)&len, lenv);
        JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue iv = JS_GetPropertyUint32(ctx, argvv, i);
            args.push_back(js_to_std_string(ctx, iv));
            JS_FreeValue(ctx, iv);
        }
    }
    JS_FreeValue(ctx, argvv);

    int inpipe[2];
    int outpipe[2];
    int errpipe[2];
    if (pipe(inpipe) < 0 || pipe(outpipe) < 0 || pipe(errpipe) < 0) {
        return JS_ThrowTypeError(ctx, "spawn: pipe() failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        return JS_ThrowTypeError(ctx, "spawn: fork() failed");
    }

    if (pid == 0) {
        // child
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        close(inpipe[1]); close(outpipe[0]); close(errpipe[0]);

        std::vector<char*> cargv;
        cargv.push_back(const_cast<char*>(cmd.c_str()));
        for (auto& s : args) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        execvp(cmd.c_str(), cargv.data());
        _exit(127);
    }

    // parent
    close(inpipe[0]);
    close(outpipe[1]);
    close(errpipe[1]);

    fcntl(outpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errpipe[0], F_SETFL, O_NONBLOCK);

    uint32_t id = g_next_id++;
    auto proc = std::make_unique<ChildProcess>();
    proc->pid = pid;
    proc->stdin_fd = inpipe[1];
    proc->stdout_fd = outpipe[0];
    proc->stderr_fd = errpipe[0];
    {
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        g_processes[id] = std::move(proc);
    }

    JSValue procObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, procObj, "pid", JS_NewInt32(ctx, (int32_t)pid));
    JS_SetPropertyStr(ctx, procObj, "id", JS_NewInt32(ctx, (int32_t)id));

    // stdout event target
    JSValue stdoutObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stdoutObj, "_id", JS_NewInt32(ctx, (int32_t)id));
    // on(event, cb) for stdout
    auto js_on_stdout = [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
        if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
            return JS_ThrowTypeError(ctx, "stdout.on(event, cb)");
        int64_t id64 = 0; JSValue idv = JS_GetPropertyStr(ctx, this_val, "_id"); JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
        std::string ev = js_to_std_string(ctx, argv[0]);
        if (ev != "data") return JS_UNDEFINED;
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        g_listeners[(uint32_t)id64].stdout_data.push_back(JS_DupValue(ctx, argv[1]));
        return JS_UNDEFINED;
    };
    // off(event, cb) for stdout
    auto js_off_stdout = [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
        if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
        int64_t id64 = 0; JSValue idv = JS_GetPropertyStr(ctx, this_val, "_id"); JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
        std::string ev = js_to_std_string(ctx, argv[0]); if (ev != "data") return JS_UNDEFINED;
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        auto &vec = g_listeners[(uint32_t)id64].stdout_data;
        for (auto it = vec.begin(); it != vec.end(); ) {
            if (JS_VALUE_GET_TAG(*it) == JS_VALUE_GET_TAG(argv[1]) && JS_VALUE_GET_PTR(*it) == JS_VALUE_GET_PTR(argv[1])) {
                JS_FreeValue(ctx, *it); it = vec.erase(it);
            } else { ++it; }
        }
        return JS_UNDEFINED;
    };
    JS_SetPropertyStr(ctx, stdoutObj, "on", JS_NewCFunction(ctx, js_on_stdout, "on", 2));
    JS_SetPropertyStr(ctx, stdoutObj, "off", JS_NewCFunction(ctx, js_off_stdout, "off", 2));

    // stderr event target
    JSValue stderrObj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, stderrObj, "_id", JS_NewInt32(ctx, (int32_t)id));
    auto js_on_stderr = [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
        if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
            return JS_ThrowTypeError(ctx, "stderr.on(event, cb)");
        int64_t id64 = 0; JSValue idv = JS_GetPropertyStr(ctx, this_val, "_id"); JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
        std::string ev = js_to_std_string(ctx, argv[0]);
        if (ev != "data") return JS_UNDEFINED;
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        g_listeners[(uint32_t)id64].stderr_data.push_back(JS_DupValue(ctx, argv[1]));
        return JS_UNDEFINED;
    };
    auto js_off_stderr = [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
        if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
        int64_t id64 = 0; JSValue idv = JS_GetPropertyStr(ctx, this_val, "_id"); JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
        std::string ev = js_to_std_string(ctx, argv[0]); if (ev != "data") return JS_UNDEFINED;
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        auto &vec = g_listeners[(uint32_t)id64].stderr_data;
        for (auto it = vec.begin(); it != vec.end(); ) {
            if (JS_VALUE_GET_TAG(*it) == JS_VALUE_GET_TAG(argv[1]) && JS_VALUE_GET_PTR(*it) == JS_VALUE_GET_PTR(argv[1])) {
                JS_FreeValue(ctx, *it); it = vec.erase(it);
            } else { ++it; }
        }
        return JS_UNDEFINED;
    };
    JS_SetPropertyStr(ctx, stderrObj, "on", JS_NewCFunction(ctx, js_on_stderr, "on", 2));
    JS_SetPropertyStr(ctx, stderrObj, "off", JS_NewCFunction(ctx, js_off_stderr, "off", 2));

    // process-level events (exit)
    JS_SetPropertyStr(ctx, procObj, "on", JS_NewCFunction(ctx,
        [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
                return JS_ThrowTypeError(ctx, "on(event, cb)");
            std::string ev = js_to_std_string(ctx, argv[0]);
            if (ev != "exit") return JS_UNDEFINED;
            JSValue idv = JS_GetPropertyStr(ctx, this_val, "id");
            int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            g_listeners[(uint32_t)id64].exit_listeners.push_back(JS_DupValue(ctx, argv[1]));
            return JS_UNDEFINED;
        }, "on", 2));
    JS_SetPropertyStr(ctx, procObj, "off", JS_NewCFunction(ctx,
        [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;
            std::string ev = js_to_std_string(ctx, argv[0]); if (ev != "exit") return JS_UNDEFINED;
            JSValue idv = JS_GetPropertyStr(ctx, this_val, "id");
            int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            auto &vec = g_listeners[(uint32_t)id64].exit_listeners;
            for (auto it = vec.begin(); it != vec.end(); ) {
                if (JS_VALUE_GET_TAG(*it) == JS_VALUE_GET_TAG(argv[1]) && JS_VALUE_GET_PTR(*it) == JS_VALUE_GET_PTR(argv[1])) {
                    JS_FreeValue(ctx, *it); it = vec.erase(it);
                } else { ++it; }
            }
            return JS_UNDEFINED;
        }, "off", 2));

    JS_SetPropertyStr(ctx, procObj, "stdout", stdoutObj);
    JS_SetPropertyStr(ctx, procObj, "stderr", stderrObj);

    // Instance methods: write(data), closeStdin(), kill([signal])
    JS_SetPropertyStr(ctx, procObj, "write", JS_NewCFunction(ctx,
        [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            if (argc < 1) return JS_ThrowTypeError(ctx, "write(data)");
            JSValue idv = JS_GetPropertyStr(ctx, this_val, "id");
            int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
            std::string data = js_to_std_string(ctx, argv[0]);
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            auto it = g_processes.find((uint32_t)id64);
            if (it == g_processes.end()) return JS_ThrowTypeError(ctx, "write: invalid process");
            if (it->second->stdin_fd >= 0) { ssize_t n = ::write(it->second->stdin_fd, data.data(), data.size()); (void)n; }
            return JS_UNDEFINED;
        }, "write", 1));

    JS_SetPropertyStr(ctx, procObj, "closeStdin", JS_NewCFunction(ctx,
        [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            (void)argc; (void)argv;
            JSValue idv = JS_GetPropertyStr(ctx, this_val, "id");
            int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            auto it = g_processes.find((uint32_t)id64);
            if (it == g_processes.end()) return JS_ThrowTypeError(ctx, "closeStdin: invalid process");
            if (it->second->stdin_fd >= 0) { ::close(it->second->stdin_fd); it->second->stdin_fd = -1; }
            return JS_UNDEFINED;
        }, "closeStdin", 0));

    JS_SetPropertyStr(ctx, procObj, "kill", JS_NewCFunction(ctx,
        [](JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) -> JSValue {
            int sig = SIGTERM; if (argc >= 1) JS_ToInt32(ctx, &sig, argv[0]);
            JSValue idv = JS_GetPropertyStr(ctx, this_val, "id");
            int64_t id64 = 0; JS_ToInt64(ctx, &id64, idv); JS_FreeValue(ctx, idv);
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            auto it = g_processes.find((uint32_t)id64);
            if (it == g_processes.end()) return JS_ThrowTypeError(ctx, "kill: invalid process");
            if (it->second->pid > 0) ::kill(it->second->pid, sig);
            return JS_UNDEFINED;
        }, "kill", 1));
    return procObj;
}

// JS: write(id, data) — legacy helper; prefer instance method.
static JSValue js_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 2) return JS_ThrowTypeError(ctx, "write(id, data)");
    int64_t id64 = 0;
    JS_ToInt64(ctx, &id64, argv[0]);
    std::string data = js_to_std_string(ctx, argv[1]);
    std::lock_guard<std::mutex> lk(g_proc_mutex);
    auto it = g_processes.find((uint32_t)id64);
    if (it == g_processes.end()) return JS_ThrowTypeError(ctx, "write: invalid id");
    ssize_t n = ::write(it->second->stdin_fd, data.data(), data.size());
    (void)n;
    return JS_UNDEFINED;
}

// JS: closeStdin(id) — legacy helper; prefer instance method.
static JSValue js_close_stdin(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "closeStdin(id)");
    int64_t id64 = 0; JS_ToInt64(ctx, &id64, argv[0]);
    std::lock_guard<std::mutex> lk(g_proc_mutex);
    auto it = g_processes.find((uint32_t)id64);
    if (it == g_processes.end()) return JS_ThrowTypeError(ctx, "closeStdin: invalid id");
    if (it->second->stdin_fd >= 0) { ::close(it->second->stdin_fd); it->second->stdin_fd = -1; }
    return JS_UNDEFINED;
}

// JS: kill(id[, signal]) — legacy helper; prefer instance method.
static JSValue js_kill(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "kill(id[, signal])");
    int64_t id64 = 0; JS_ToInt64(ctx, &id64, argv[0]);
    int sig = SIGTERM;
    if (argc >= 2) JS_ToInt32(ctx, &sig, argv[1]);
    std::lock_guard<std::mutex> lk(g_proc_mutex);
    auto it = g_processes.find((uint32_t)id64);
    if (it == g_processes.end()) return JS_ThrowTypeError(ctx, "kill: invalid id");
    if (it->second->pid > 0) ::kill(it->second->pid, sig);
    return JS_UNDEFINED;
}

// Non-blocking read of stdout/stderr; appends to buffers.
static void process_poll_io(ChildProcess& p)
{
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(p.stdout_fd, buf, sizeof(buf));
        if (n > 0) p.stdout_buf.append(buf, (size_t)n);
        if (n <= 0) break;
    }
    for (;;) {
        ssize_t n = ::read(p.stderr_fd, buf, sizeof(buf));
        if (n > 0) p.stderr_buf.append(buf, (size_t)n);
        if (n <= 0) break;
    }
}

// JS: drain() — emits stdout/stderr chunks and exit events; also run on `update`.
static JSValue js_drain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    // Keep behavior but also emit events immediately
    (void)this_val; (void)argc; (void)argv;
    // Reuse update callback logic
    // We'll call it with no lock here
    // Build emissions
    struct Emit { enum Type { Stdout, Stderr, Exit } type; uint32_t id; std::string data; int code; };
    std::vector<Emit> emits;
    {
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        for (auto& [id, up] : g_processes) {
            process_poll_io(*up);
            auto ex = g_exec_requests.find(id);
            if (ex != g_exec_requests.end()) {
                if (!up->stdout_buf.empty()) ex->second.out.append(up->stdout_buf);
                if (!up->stderr_buf.empty()) ex->second.err.append(up->stderr_buf);
            }
            if (!up->stdout_buf.empty()) { emits.push_back({Emit::Stdout, id, up->stdout_buf, 0}); up->stdout_buf.clear(); }
            if (!up->stderr_buf.empty()) { emits.push_back({Emit::Stderr, id, up->stderr_buf, 0}); up->stderr_buf.clear(); }
            int status = 0; pid_t r = waitpid(up->pid, &status, WNOHANG);
            if (r == up->pid) { up->exited = true; up->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status)); }
            if (up->exited && !up->exit_emitted) { emits.push_back({Emit::Exit, id, std::string(), up->exit_code}); up->exit_emitted = true; }
        }
    }
    // Emit outside lock
    for (auto &e : emits) {
        if (!g_ctx) break;
        if (e.type == Emit::Stdout) {
            auto it = g_listeners.find(e.id); if (it == g_listeners.end()) continue;
            for (auto &cb : it->second.stdout_data) {
                JSValue arg = JS_NewStringLen(g_ctx, e.data.data(), e.data.size());
                JSValue ret = JS_Call(g_ctx, cb, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(g_ctx, arg);
                JS_FreeValue(g_ctx, ret);
            }
        } else if (e.type == Emit::Stderr) {
            auto it = g_listeners.find(e.id); if (it == g_listeners.end()) continue;
            for (auto &cb : it->second.stderr_data) {
                JSValue arg = JS_NewStringLen(g_ctx, e.data.data(), e.data.size());
                JSValue ret = JS_Call(g_ctx, cb, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(g_ctx, arg);
                JS_FreeValue(g_ctx, ret);
            }
        } else if (e.type == Emit::Exit) {
            auto it = g_listeners.find(e.id); if (it == g_listeners.end()) continue;
            for (auto &cb : it->second.exit_listeners) {
                JSValue arg = JS_NewInt32(g_ctx, e.code);
                JSValue ret = JS_Call(g_ctx, cb, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(g_ctx, arg);
                JS_FreeValue(g_ctx, ret);
            }
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "read(id)");
    int64_t id64 = 0; JS_ToInt64(ctx, &id64, argv[0]);
    std::lock_guard<std::mutex> lk(g_proc_mutex);
    auto it = g_processes.find((uint32_t)id64);
    if (it == g_processes.end()) return JS_ThrowTypeError(ctx, "read: invalid id");
    process_poll_io(*it->second);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "stdout", JS_NewStringLen(ctx, it->second->stdout_buf.data(), it->second->stdout_buf.size()));
    JS_SetPropertyStr(ctx, obj, "stderr", JS_NewStringLen(ctx, it->second->stderr_buf.data(), it->second->stderr_buf.size()));
    JS_SetPropertyStr(ctx, obj, "exited", JS_NewBool(ctx, it->second->exited));
    JS_SetPropertyStr(ctx, obj, "exitCode", JS_NewInt32(ctx, it->second->exit_code));
    it->second->stdout_buf.clear();
    it->second->stderr_buf.clear();
    return obj;
}

// JS: exec(command) -> Promise<{ stdout, stderr }> and proper error object on failure.
static JSValue js_exec(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0])) {
        return JS_ThrowTypeError(ctx, "exec(command: string)");
    }
    std::string command = js_to_std_string(ctx, argv[0]);

    int inpipe[2]; int outpipe[2]; int errpipe[2];
    if (pipe(inpipe) < 0 || pipe(outpipe) < 0 || pipe(errpipe) < 0) {
        return JS_ThrowTypeError(ctx, "exec: pipe() failed");
    }
    pid_t pid = fork();
    if (pid < 0) {
        return JS_ThrowTypeError(ctx, "exec: fork() failed");
    }
    if (pid == 0) {
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(errpipe[1], STDERR_FILENO);
        close(inpipe[1]); close(outpipe[0]); close(errpipe[0]);
        const char* shell = "sh";
        const char* argvv[] = { shell, "-c", command.c_str(), nullptr };
        execvp(shell, (char* const*)argvv);
        _exit(127);
    }
    // parent
    close(inpipe[0]); close(outpipe[1]); close(errpipe[1]);
    fcntl(outpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errpipe[0], F_SETFL, O_NONBLOCK);

    uint32_t id = g_next_id++;
    auto proc = std::make_unique<ChildProcess>();
    proc->pid = pid; proc->stdin_fd = inpipe[1]; proc->stdout_fd = outpipe[0]; proc->stderr_fd = errpipe[0];
    {
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        g_processes[id] = std::move(proc);
    }

    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    ExecRequest req; req.id = id; req.resolve = JS_DupValue(ctx, funcs[0]); req.reject = JS_DupValue(ctx, funcs[1]);
    {
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        g_exec_requests[id] = std::move(req);
    }
    JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
    return promise;
}

static int process_module_init(JSContext* ctx, JSModuleDef* m)
{
    JS_SetModuleExport(ctx, m, "spawn", JS_NewCFunction(ctx, js_spawn, "spawn", 1));
    JS_SetModuleExport(ctx, m, "exec", JS_NewCFunction(ctx, js_exec, "exec", 1));
    return 0;
}

// Hook: update — drains IO, emits events, and finalizes any pending exec promises.
static void process_update_callback(void* /*data*/)
{
    struct Emit { enum Type { Stdout, Stderr, Exit } type; uint32_t id; std::string data; int code; };
    std::vector<Emit> emits;
    struct ExecFinalize { uint32_t id; bool ok; int code; std::string out; std::string err; JSValue resolve; JSValue reject; };
    std::vector<ExecFinalize> finals;
    std::vector<uint32_t> dead_ids;
    {
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        for (auto& [id, up] : g_processes) {
            process_poll_io(*up);
            auto ex = g_exec_requests.find(id);
            if (ex != g_exec_requests.end()) {
                if (!up->stdout_buf.empty()) ex->second.out.append(up->stdout_buf);
                if (!up->stderr_buf.empty()) ex->second.err.append(up->stderr_buf);
            }
            if (!up->stdout_buf.empty()) { emits.push_back({Emit::Stdout, id, up->stdout_buf, 0}); up->stdout_buf.clear(); }
            if (!up->stderr_buf.empty()) { emits.push_back({Emit::Stderr, id, up->stderr_buf, 0}); up->stderr_buf.clear(); }
            int status = 0; pid_t r = waitpid(up->pid, &status, WNOHANG);
            if (r == up->pid) { up->exited = true; up->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status)); }
            if (up->exited && !up->exit_emitted) { emits.push_back({Emit::Exit, id, std::string(), up->exit_code}); up->exit_emitted = true; }
            if (up->exited) {
                auto it2 = g_exec_requests.find(id);
                if (it2 != g_exec_requests.end()) {
                    finals.push_back({id, up->exit_code == 0, up->exit_code, it2->second.out, it2->second.err, JS_DupValue(g_ctx, it2->second.resolve), JS_DupValue(g_ctx, it2->second.reject)});
                }
                dead_ids.push_back(id);
            }
        }
        for (auto &f : finals) {
            auto it = g_exec_requests.find(f.id);
            if (it != g_exec_requests.end()) {
                JS_FreeValue(g_ctx, it->second.resolve);
                JS_FreeValue(g_ctx, it->second.reject);
                g_exec_requests.erase(it);
            }
        }
    }
    // Emit outside lock using g_ctx
    if (!g_ctx) return;
    for (auto &e : emits) {
        if (e.type == Emit::Stdout) {
            auto it = g_listeners.find(e.id); if (it == g_listeners.end()) continue;
            for (auto &cb : it->second.stdout_data) {
                JSValue arg = JS_NewStringLen(g_ctx, e.data.data(), e.data.size());
                JSValue ret = JS_Call(g_ctx, cb, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(g_ctx, arg);
                JS_FreeValue(g_ctx, ret);
            }
        } else if (e.type == Emit::Stderr) {
            auto it = g_listeners.find(e.id); if (it == g_listeners.end()) continue;
            for (auto &cb : it->second.stderr_data) {
                JSValue arg = JS_NewStringLen(g_ctx, e.data.data(), e.data.size());
                JSValue ret = JS_Call(g_ctx, cb, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(g_ctx, arg);
                JS_FreeValue(g_ctx, ret);
            }
        } else if (e.type == Emit::Exit) {
            auto it = g_listeners.find(e.id); if (it == g_listeners.end()) continue;
            for (auto &cb : it->second.exit_listeners) {
                JSValue arg = JS_NewInt32(g_ctx, e.code);
                JSValue ret = JS_Call(g_ctx, cb, JS_UNDEFINED, 1, &arg);
                JS_FreeValue(g_ctx, arg);
                JS_FreeValue(g_ctx, ret);
            }
        }
    }
    for (auto &f : finals) {
        if (f.ok) {
            // Resolve with { stdout, stderr }
            JSValue resObj = JS_NewObject(g_ctx);
            JS_SetPropertyStr(g_ctx, resObj, "stdout", JS_NewStringLen(g_ctx, f.out.data(), f.out.size()));
            JS_SetPropertyStr(g_ctx, resObj, "stderr", JS_NewStringLen(g_ctx, f.err.data(), f.err.size()));
            JSValue ret = JS_Call(g_ctx, f.resolve, JS_UNDEFINED, 1, &resObj);
            JS_FreeValue(g_ctx, resObj);
            JS_FreeValue(g_ctx, ret);
        } else {
            JSValue errObj = JS_NewObject(g_ctx);
            std::string msg = std::string("exec failed with code ") + std::to_string(f.code);
            JS_SetPropertyStr(g_ctx, errObj, "message", JS_NewString(g_ctx, msg.c_str()));
            JS_SetPropertyStr(g_ctx, errObj, "code", JS_NewInt32(g_ctx, f.code));
            JS_SetPropertyStr(g_ctx, errObj, "stdout", JS_NewStringLen(g_ctx, f.out.data(), f.out.size()));
            JS_SetPropertyStr(g_ctx, errObj, "stderr", JS_NewStringLen(g_ctx, f.err.data(), f.err.size()));
            JSValue ret = JS_Call(g_ctx, f.reject, JS_UNDEFINED, 1, &errObj);
            JS_FreeValue(g_ctx, errObj);
            JS_FreeValue(g_ctx, ret);
        }
        JS_FreeValue(g_ctx, f.resolve);
        JS_FreeValue(g_ctx, f.reject);
    }

    // Cleanup exited processes and their listeners
    if (!dead_ids.empty()) {
        std::lock_guard<std::mutex> lk(g_proc_mutex);
        for (uint32_t id : dead_ids) {
            auto pit = g_processes.find(id);
            if (pit != g_processes.end()) {
                if (pit->second->stdin_fd  >= 0) { ::close(pit->second->stdin_fd);  pit->second->stdin_fd  = -1; }
                if (pit->second->stdout_fd >= 0) { ::close(pit->second->stdout_fd); pit->second->stdout_fd = -1; }
                if (pit->second->stderr_fd >= 0) { ::close(pit->second->stderr_fd); pit->second->stderr_fd = -1; }
                g_processes.erase(pit);
            }
            auto lit = g_listeners.find(id);
            if (lit != g_listeners.end()) {
                for (auto &cb : lit->second.stdout_data) JS_FreeValue(g_ctx, cb);
                for (auto &cb : lit->second.stderr_data) JS_FreeValue(g_ctx, cb);
                for (auto &cb : lit->second.exit_listeners) JS_FreeValue(g_ctx, cb);
                g_listeners.erase(lit);
            }
        }
    }
}

extern "C" {
// How to extend:
// - Export `integrate` from your shared library.
// - Wire an `update` hook to deliver cross-thread events back to JS.
// - Wire a `cleanup` hook to release OS handles and JS references.
// - Create your JS module and list named exports with JS_AddModuleExport.
JSModuleDef* integrate(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook)
{
    g_ctx = ctx;
    registerHook("update", process_update_callback);
    // Ensure we release all JSValue references and OS resources on engine cleanup
    registerHook("cleanup", [](void* /*data*/){
        // Free pending exec resolve/reject functions
        {
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            for (auto &kv : g_exec_requests) {
                JS_FreeValue(g_ctx, kv.second.resolve);
                JS_FreeValue(g_ctx, kv.second.reject);
            }
            g_exec_requests.clear();
        }
        // Free all listener callbacks
        {
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            for (auto &kv : g_listeners) {
                for (auto &cb : kv.second.stdout_data) JS_FreeValue(g_ctx, cb);
                for (auto &cb : kv.second.stderr_data) JS_FreeValue(g_ctx, cb);
                for (auto &cb : kv.second.exit_listeners) JS_FreeValue(g_ctx, cb);
            }
            g_listeners.clear();
        }
        // Terminate any remaining child processes and close fds, then reap
        std::vector<std::pair<uint32_t,pid_t>> ids_and_pids;
        {
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            for (auto &kv : g_processes) {
                auto &p = *kv.second;
                if (p.pid > 0) {
                    ids_and_pids.emplace_back(kv.first, p.pid);
                    ::kill(p.pid, SIGTERM);
                }
                if (p.stdin_fd  >= 0) { ::close(p.stdin_fd);  p.stdin_fd  = -1; }
                if (p.stdout_fd >= 0) { ::close(p.stdout_fd); p.stdout_fd = -1; }
                if (p.stderr_fd >= 0) { ::close(p.stderr_fd); p.stderr_fd = -1; }
            }
        }
        // Give processes a brief grace period to exit, then SIGKILL if needed
        for (int tries = 0; tries < 10; ++tries) {
            bool any_alive = false;
            for (auto &ip : ids_and_pids) {
                int status = 0;
                pid_t r = waitpid(ip.second, &status, WNOHANG);
                if (r == 0) any_alive = true; // still alive
            }
            if (!any_alive) break;
            usleep(50000); // 50ms
        }
        for (auto &ip : ids_and_pids) {
            int status = 0;
            if (waitpid(ip.second, &status, WNOHANG) == 0) {
                ::kill(ip.second, SIGKILL);
                (void)waitpid(ip.second, &status, 0);
            }
        }
        // Finally clear process/listener maps under lock
        {
            std::lock_guard<std::mutex> lk(g_proc_mutex);
            g_processes.clear();
            g_listeners.clear();
        }
    });
    JSModuleDef* m = JS_NewCModule(ctx, module_name, process_module_init);
    if (!m) return nullptr;
    JS_AddModuleExport(ctx, m, "spawn");
    JS_AddModuleExport(ctx, m, "exec");
    return m;
}
}


