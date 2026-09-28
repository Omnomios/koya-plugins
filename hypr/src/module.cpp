/*
   Koya Plugin: Hypr IPC

   What this is:
   - Application code that connects to Hyprland's UNIX sockets to receive events
     and send commands/JSON queries, exposing a friendly JS API to Koya.

   Koya vs Application responsibilities:
   - Koya provides the JS engine (QuickJS) and the hook system to tick work on
     the engine thread.
   - The plugin (application) owns:
     - Socket I/O and reconnection strategy off-thread
     - Marshalling data into JS values and resolving Promises during `update`

   Exposed JS API (selected):
   - connect({ runtimeDir?, instanceSignature? })
   - on(name, cb), off(name, cb?) for event bus
   - send(command): Promise<string>
   - json(topic, args?): Promise<any>
   - convenience: workspaces(), monitors(), clients(), activeworkspace(), ...

   Integration notes:
   - Results from worker threads are enqueued and delivered in `hypr_update_callback`.
   - All JS invocations happen on the engine thread for safety.
*/
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../../sdk/quickjs/quickjs.h"
#include "../../sdk/plugin_support.hpp"

namespace {

// Stores a JS function for event dispatch.
struct JsCallback {
    JSContext* ctx {nullptr};
    JSValue func {JS_NULL};
};

// A single Hyprland event read from the socket (name and raw payload string).
struct EventItem {
    std::string name;
    std::string payloadRaw;
};

// Async command handling (send/json) resolved on update hook
enum class JobType : uint8_t { Send, Json };
struct PendingPromise {
    JSContext* ctx;
    JSValue resolve;
    JSValue reject;
    JobType type;
};
struct JobResult {
    uint32_t id;
    bool success;
    std::string payload; // raw output string
};
struct HyprState {
    std::mutex queueMutex;
    std::queue<EventItem> eventQueue;
    std::mutex subscriptionMutex;
    std::unordered_map<std::string, std::vector<JsCallback>> subscribers;
    std::atomic<bool> running{false};
    std::atomic<bool> stopping{false};
    std::thread eventThread;
    std::atomic<int> eventFd{-1};
    std::mutex wakeMutex;
    std::condition_variable wake;
    std::string runtimeDir;
    std::string instanceSignature;
    std::string commandSocketPath;
    std::string eventSocketPath;
    JSContext* context{nullptr};
    std::atomic<uint32_t> nextJobId{1};
    std::mutex promisesMutex;
    std::unordered_map<uint32_t, PendingPromise> promises;
    std::mutex resultsMutex;
    std::queue<JobResult> results;
    std::vector<std::thread> jobThreads;
    std::mutex activeSocketsMutex;
    std::vector<int> activeSockets;
};

static constexpr size_t maxQueueSize = 2048;
static thread_local HyprState* hyprState = nullptr;

#define g_queueMutex (hyprState->queueMutex)
#define g_eventQueue (hyprState->eventQueue)
#define g_subMutex (hyprState->subscriptionMutex)
#define g_subscribers (hyprState->subscribers)
#define g_running (hyprState->running)
#define g_eventThread (hyprState->eventThread)
#define g_runtimeDir (hyprState->runtimeDir)
#define g_instanceSig (hyprState->instanceSignature)
#define g_sockCmdPath (hyprState->commandSocketPath)
#define g_sockEvtPath (hyprState->eventSocketPath)
#define g_ctx (hyprState->context)
#define g_nextJobId (hyprState->nextJobId)
#define g_promisesMutex (hyprState->promisesMutex)
#define g_promises (hyprState->promises)
#define g_resultsMutex (hyprState->resultsMutex)
#define g_results (hyprState->results)

struct HyprPlugin : HelixPluginSupport {
    HyprState runtime;
};

// Forward declaration for helper used by background workers
static int connect_unix(const std::string& path);

static JSValue enqueue_job_promise(JSContext* ctx, JobType type, std::string cmd) {
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue resolve = funcs[0];
    JSValue reject  = funcs[1];

    uint32_t id = g_nextJobId++;
    {
        std::lock_guard<std::mutex> lk(g_promisesMutex);
        g_promises.emplace(id, PendingPromise{ctx, resolve, reject, type});
    }

    HyprState* state = hyprState;
    state->jobThreads.emplace_back([state, id, cmd = std::move(cmd)]() mutable {
        hyprState = state;
        int fd = connect_unix(g_sockCmdPath);
        bool ok = false;
        std::string out;
        if (fd >= 0) {
            {
                std::lock_guard<std::mutex> lk(state->activeSocketsMutex);
                if (!state->stopping.load()) state->activeSockets.push_back(fd);
            }
            if (!state->stopping.load()) {
                std::string line = cmd;
                line.push_back('\0');
                ::send(fd, line.data(), line.size(), 0);
                char buf[65536];
                ssize_t rn = ::recv(fd, buf, sizeof(buf) - 1, 0);
                if (rn > 0) {
                    buf[rn] = '\0';
                    out.assign(buf);
                    ok = true;
                }
            }
            {
                std::lock_guard<std::mutex> lk(state->activeSocketsMutex);
                auto it = std::find(state->activeSockets.begin(), state->activeSockets.end(), fd);
                if (it != state->activeSockets.end()) state->activeSockets.erase(it);
            }
            ::close(fd);
        }
        if (!state->stopping.load()) {
            std::lock_guard<std::mutex> lk(g_resultsMutex);
            g_results.push(JobResult{id, ok, std::move(out)});
        }
        hyprState = nullptr;
    });

    return promise;
}

static std::string getenv_str(const char* name) {
    const char* v = ::getenv(name);
    return v ? std::string(v) : std::string();
}

// Compose the Hyprland UNIX socket path from XDG runtime + instance signature.
static std::string build_sock_path(const std::string& runtimeDir,
                                   const std::string& instanceSig,
                                   bool event) {
    std::string base = runtimeDir;
    if (!base.empty() && base.back() == '/') base.pop_back();
    std::string path = base + "/hypr/" + instanceSig + (event ? "/.socket2.sock" : "/.socket.sock");
    return path;
}

static int connect_unix(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static std::optional<std::string> read_line(int fd) {
    std::string buf;
    buf.reserve(256);
    char ch;
    ssize_t n;
    while (true) {
        n = ::recv(fd, &ch, 1, 0);
        if (n == 0) {
            // EOF
            if (buf.empty()) return std::nullopt;
            return buf;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::nullopt;
        }
        if (ch == '\n') break;
        buf.push_back(ch);
    }
    return buf;
}

// Background loop: (re)connects to the event socket and enqueues events.
static void event_loop(HyprState* state) {
    hyprState = state;
    const int backoffStartMs = 200;
    const int backoffMaxMs = 5000;
    int backoffMs = backoffStartMs;
    while (g_running.load()) {
        int fd = connect_unix(g_sockEvtPath);
        if (fd < 0) {
            std::unique_lock<std::mutex> lock(state->wakeMutex);
            state->wake.wait_for(lock, std::chrono::milliseconds(backoffMs), [state] {
                return !state->running.load();
            });
            backoffMs = std::min(backoffMaxMs, backoffMs * 2);
            continue;
        }
        state->eventFd.store(fd);
        backoffMs = backoffStartMs;

        // Read lines as events
        while (g_running.load()) {
            auto lineOpt = read_line(fd);
            if (!lineOpt.has_value()) break; // reconnect
            const std::string& line = *lineOpt;

            // Format: name>>payload
            auto sep = line.find(">>");
            std::string name = sep == std::string::npos ? line : line.substr(0, sep);
            std::string payload = sep == std::string::npos ? std::string() : line.substr(sep + 2);

            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (g_eventQueue.size() >= maxQueueSize) {
                // drop oldest
                g_eventQueue.pop();
            }
            g_eventQueue.push(EventItem{std::move(name), std::move(payload)});
        }
        state->eventFd.store(-1);
        ::close(fd);
    }
    hyprState = nullptr;
}

static JSValue throw_type(JSContext* ctx, const char* msg) {
    return JS_ThrowTypeError(ctx, "%s", msg);
}

static std::string js_to_string(JSContext* ctx, JSValueConst v) {
    const char* c = JS_ToCString(ctx, v);
    std::string s = c ? c : "";
    if (c) JS_FreeCString(ctx, c);
    return s;
}

// JS: connect({ instanceSignature?, runtimeDir? })
static JSValue js_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc > 0 && !JS_IsObject(argv[0])) {
        return throw_type(ctx, "connect expects an options object");
    }

    std::string runtimeDir = getenv_str("XDG_RUNTIME_DIR");
    std::string instanceSig = getenv_str("HYPRLAND_INSTANCE_SIGNATURE");
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue v = JS_GetPropertyStr(ctx, argv[0], "runtimeDir");
        if (!JS_IsUndefined(v)) runtimeDir = js_to_string(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "instanceSignature");
        if (!JS_IsUndefined(v)) instanceSig = js_to_string(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (runtimeDir.empty() || instanceSig.empty()) {
        return JS_ThrowReferenceError(ctx, "Module/hypr: Hyprland not detected (missing HYPRLAND_INSTANCE_SIGNATURE or XDG_RUNTIME_DIR). This plugin requires the Hyprland compositor; if you are running another compositor (e.g. Weston), do not load Module/hypr.");
    }

    g_runtimeDir = runtimeDir;
    g_instanceSig = instanceSig;
    g_sockCmdPath = build_sock_path(g_runtimeDir, g_instanceSig, false);
    g_sockEvtPath = build_sock_path(g_runtimeDir, g_instanceSig, true);

    if (!g_running.exchange(true)) {
        HyprState* state = hyprState;
        g_eventThread = std::thread([state] { event_loop(state); });
    }
    return JS_UNDEFINED;
}

// JS: on(eventName, callback)
static JSValue js_on(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1])) {
        return throw_type(ctx, "on expects (string, function)");
    }
    std::string name = js_to_string(ctx, argv[0]);
    JsCallback cb{ctx, JS_DupValue(ctx, argv[1])};
    std::lock_guard<std::mutex> lk(g_subMutex);
    g_subscribers[name].push_back(cb);
    return JS_UNDEFINED;
}

// JS: off(eventName, callback?)
static JSValue js_off(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0])) {
        return throw_type(ctx, "off expects (string[, function])");
    }
    std::string name = js_to_string(ctx, argv[0]);
    std::lock_guard<std::mutex> lk(g_subMutex);
    auto it = g_subscribers.find(name);
    if (it == g_subscribers.end()) return JS_UNDEFINED;
    if (argc >= 2 && JS_IsFunction(ctx, argv[1])) {
        // Remove specific callback by pointer identity
        for (auto vit = it->second.begin(); vit != it->second.end(); ++vit) {
            if (JS_VALUE_GET_PTR(vit->func) == JS_VALUE_GET_PTR(argv[1])) {
                JS_FreeValue(ctx, vit->func);
                it->second.erase(vit);
                break;
            }
        }
        if (it->second.empty()) g_subscribers.erase(it);
    } else {
        // Remove all
        for (auto& c : it->second) {
            JS_FreeValue(ctx, c.func);
        }
        g_subscribers.erase(it);
    }
    return JS_UNDEFINED;
}

static JSValue call_js_and_catch(JSContext* ctx, JSValue func, int argc, JSValue* argv) {
    JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, argc, argv);
    if (JS_IsException(ret)) {
        // Print exception to console if possible
        JSValue exc = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, exc);
        fprintf(stderr, "Hypr plugin callback exception: %s\n", s ? s : "<unknown>");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);
    return JS_UNDEFINED;
}

// Engine-thread delivery: moves queued events and invokes subscribed JS callbacks.
static void dispatch_events(JSContext* ctx) {
    // Swap out queue
    std::vector<EventItem> local;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        while (!g_eventQueue.empty()) {
            local.push_back(std::move(g_eventQueue.front()));
            g_eventQueue.pop();
        }
    }
    if (local.empty()) return;

    // For each event, call subscribers
    for (const auto& ev : local) {
        std::vector<JsCallback> subs;
        {
            std::lock_guard<std::mutex> lk(g_subMutex);
            auto it = g_subscribers.find(ev.name);
            if (it != g_subscribers.end()) subs = it->second; // copy for reentrancy safety
        }
        if (subs.empty()) continue;

        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, ev.name.c_str()));
        JS_SetPropertyStr(ctx, obj, "payload", JS_NewString(ctx, ev.payloadRaw.c_str()));
        // TODO: for known events, parse payload and attach structured fields if desired

        for (auto& c : subs) {
            JSValue arg = JS_DupValue(ctx, obj);
            call_js_and_catch(ctx, c.func, 1, &arg);
            JS_FreeValue(ctx, arg);
        }
        JS_FreeValue(ctx, obj);
    }
}

// JS: send(command) -> Promise<string>
// JS: send(command) -> Promise<string> (raw reply)
static JSValue js_send(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type(ctx, "send expects (string)");
    std::string cmd = js_to_string(ctx, argv[0]);
    return enqueue_job_promise(ctx, JobType::Send, std::move(cmd));
}

// JS: json(topic, args?) -> Promise<any>
// JS: json(topic, args?) -> Promise<any> (parsed JSON when possible)
static JSValue js_json(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type(ctx, "json expects (string[, string[]])");
    std::string topic = js_to_string(ctx, argv[0]);
    std::string full = "j/" + topic;
    if (argc >= 2 && JS_IsArray(ctx, argv[1])) {
        // Fallback method to get array length: read 'length' property
        JSValue lenv = JS_GetPropertyStr(ctx, argv[1], "length");
        uint32_t len = 0;
        JS_ToUint32(ctx, &len, lenv);
        JS_FreeValue(ctx, lenv);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue v = JS_GetPropertyUint32(ctx, argv[1], i);
            full.push_back(' ');
            full += js_to_string(ctx, v);
            JS_FreeValue(ctx, v);
        }
    }
    return enqueue_job_promise(ctx, JobType::Json, std::move(full));
}

// Convenience JSON wrappers
static JSValue js_workspaces(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return enqueue_job_promise(ctx, JobType::Json, std::string("j/workspaces"));
}
static JSValue js_monitors(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return enqueue_job_promise(ctx, JobType::Json, std::string("j/monitors"));
}
static JSValue js_clients(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return enqueue_job_promise(ctx, JobType::Json, std::string("j/clients"));
}
static JSValue js_activeworkspace(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return enqueue_job_promise(ctx, JobType::Json, std::string("j/activeworkspace"));
}
static JSValue js_activewindow(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return enqueue_job_promise(ctx, JobType::Json, std::string("j/activewindow"));
}
static JSValue js_version(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    return enqueue_job_promise(ctx, JobType::Send, std::string("version"));
}
static JSValue js_getoption(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return throw_type(ctx, "getOption expects (string)");
    std::string name = js_to_string(ctx, argv[0]);
    return enqueue_job_promise(ctx, JobType::Json, std::string("j/getoption ") + name);
}

// Update hook: drain queue and invoke JS callbacks
// Hook: update — deliver events and settle any pending command/json promises.
static void hypr_update_callback(void* /*data*/) {
    if (!g_ctx) return;
    // 1) Dispatch bus events
    dispatch_events(g_ctx);
    // 2) Resolve completed command/json promises on JS thread
    std::vector<JobResult> completed;
    {
        std::lock_guard<std::mutex> lk(g_resultsMutex);
        while (!g_results.empty()) { completed.push_back(std::move(g_results.front())); g_results.pop(); }
    }
    if (!completed.empty()) {
        std::lock_guard<std::mutex> lk(g_promisesMutex);
        for (auto& r : completed) {
            auto it = g_promises.find(r.id);
            if (it == g_promises.end()) continue;
            auto pend = it->second;
            g_promises.erase(it);
            if (!r.success) {
                JSValue err = JS_NewString(g_ctx, "Hypr IPC command failed");
                JS_Call(g_ctx, pend.reject, JS_UNDEFINED, 1, &err);
                JS_FreeValue(g_ctx, err);
            } else {
                if (pend.type == JobType::Json) {
                    JSValue parsed = JS_ParseJSON(g_ctx, r.payload.c_str(), r.payload.size(), "hypr-json");
                    if (JS_IsException(parsed)) {
                        // fallback to string
                        JSValue s = JS_NewString(g_ctx, r.payload.c_str());
                        JS_Call(g_ctx, pend.resolve, JS_UNDEFINED, 1, &s);
                        JS_FreeValue(g_ctx, s);
                    } else {
                        JS_Call(g_ctx, pend.resolve, JS_UNDEFINED, 1, &parsed);
                        JS_FreeValue(g_ctx, parsed);
                    }
                } else {
                    JSValue s = JS_NewString(g_ctx, r.payload.c_str());
                    JS_Call(g_ctx, pend.resolve, JS_UNDEFINED, 1, &s);
                    JS_FreeValue(g_ctx, s);
                }
            }
            JS_FreeValue(g_ctx, pend.resolve);
            JS_FreeValue(g_ctx, pend.reject);
        }
    }
}

static void hypr_shutdown(HyprState* state) {
    hyprState = state;
    state->stopping.store(true);
    g_running.store(false);
    state->wake.notify_all();
    const int eventFd = state->eventFd.load();
    if (eventFd >= 0) ::shutdown(eventFd, SHUT_RDWR);
    {
        std::lock_guard<std::mutex> lk(state->activeSocketsMutex);
        for (int fd : state->activeSockets) ::shutdown(fd, SHUT_RDWR);
    }
    if (g_eventThread.joinable()) g_eventThread.join();
    for (auto& worker : state->jobThreads) {
        if (worker.joinable()) worker.join();
    }
    state->jobThreads.clear();
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        while (!g_eventQueue.empty()) g_eventQueue.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_subMutex);
        for (auto &kv : g_subscribers) {
            for (auto &cb : kv.second) JS_FreeValue(g_ctx, cb.func);
        }
        g_subscribers.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_promisesMutex);
        for (auto &kv : g_promises) {
            JS_FreeValue(g_ctx, kv.second.resolve);
            JS_FreeValue(g_ctx, kv.second.reject);
        }
        g_promises.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_resultsMutex);
        while (!g_results.empty()) g_results.pop();
    }
    g_ctx = nullptr;
    hyprState = nullptr;
}

static int hypr_module_init(JSContext* ctx, JSModuleDef* m) {
    JS_SetModuleExport(ctx, m, "connect", JS_NewCFunction(ctx, js_connect, "connect", 1));
    JS_SetModuleExport(ctx, m, "on", JS_NewCFunction(ctx, js_on, "on", 2));
    JS_SetModuleExport(ctx, m, "off", JS_NewCFunction(ctx, js_off, "off", 2));
    JS_SetModuleExport(ctx, m, "send", JS_NewCFunction(ctx, js_send, "send", 1));
    JS_SetModuleExport(ctx, m, "json", JS_NewCFunction(ctx, js_json, "json", 2));
    // Convenience wrappers matching Waybar/commonly used queries
    JS_SetModuleExport(ctx, m, "workspaces", JS_NewCFunction(ctx, js_workspaces, "workspaces", 0));
    JS_SetModuleExport(ctx, m, "monitors", JS_NewCFunction(ctx, js_monitors, "monitors", 0));
    JS_SetModuleExport(ctx, m, "clients", JS_NewCFunction(ctx, js_clients, "clients", 0));
    JS_SetModuleExport(ctx, m, "activeworkspace", JS_NewCFunction(ctx, js_activeworkspace, "activeworkspace", 0));
    JS_SetModuleExport(ctx, m, "activewindow", JS_NewCFunction(ctx, js_activewindow, "activewindow", 0));
    JS_SetModuleExport(ctx, m, "version", JS_NewCFunction(ctx, js_version, "version", 0));
    JS_SetModuleExport(ctx, m, "getOption", JS_NewCFunction(ctx, js_getoption, "getOption", 1));
    return 0;
}

extern "C" {
HELIX_PLUGIN_EXPORT int helix_plugin_integrate(JSContext* ctx, const char* module_name, const HelixPluginHost* host, HelixPluginInstance** out) {
    if(!out) return 1;
    auto* owner = new HyprPlugin();
    if(!owner->initialize(host)) { delete owner; return 1; }
    hyprState = &owner->runtime;
    g_ctx = ctx;
    owner->instance.scriptUpdate = [](void* state, const HelixPluginUpdate*) {
        hyprState = &static_cast<HyprPlugin*>(state)->runtime;
        hypr_update_callback(nullptr);
    };
    owner->instance.scriptShutdown = [](void* state){
        hypr_shutdown(&static_cast<HyprPlugin*>(state)->runtime);
    };
    JSModuleDef* m = JS_NewCModule(ctx, module_name, hypr_module_init);
    if (!m) { hypr_shutdown(&owner->runtime); delete owner; return 1; }
    JS_AddModuleExport(ctx, m, "connect");
    JS_AddModuleExport(ctx, m, "on");
    JS_AddModuleExport(ctx, m, "off");
    JS_AddModuleExport(ctx, m, "send");
    JS_AddModuleExport(ctx, m, "json");
    JS_AddModuleExport(ctx, m, "workspaces");
    JS_AddModuleExport(ctx, m, "monitors");
    JS_AddModuleExport(ctx, m, "clients");
    JS_AddModuleExport(ctx, m, "activeworkspace");
    JS_AddModuleExport(ctx, m, "activewindow");
    JS_AddModuleExport(ctx, m, "version");
    JS_AddModuleExport(ctx, m, "getOption");
    owner->instance.abiVersion = HELIX_PLUGIN_ABI_VERSION;
    owner->instance.module = m;
    owner->instance.state = owner;
    owner->instance.destroy = [](void* state) { delete static_cast<HyprPlugin*>(state); };
    *out = &owner->instance;
    return 0;
}
}

} // namespace
