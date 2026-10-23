#include "../sdk/quickjs/quickjs.h"
#include "helix/plugin.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
void exception(JSContext *ctx, JSValue value) {
  if (!JS_IsException(value))
    return;
  JSValue error = JS_GetException(ctx);
  const char *text = JS_ToCString(ctx, error);
  std::string message = text ? text : "JS exception";
  if (text)
    JS_FreeCString(ctx, text);
  JS_FreeValue(ctx, error);
  throw std::runtime_error(message);
}
void eval(JSContext *ctx, const std::string &source,
          int flags = JS_EVAL_TYPE_GLOBAL) {
  JSValue result =
      JS_Eval(ctx, source.c_str(), source.size(), "<hypr-regression>", flags);
  exception(ctx, result);
  JS_FreeValue(ctx, result);
}
bool truth(JSContext *ctx, const char *source) {
  JSValue result =
      JS_Eval(ctx, source, strlen(source), "<hypr-check>", JS_EVAL_TYPE_GLOBAL);
  exception(ctx, result);
  bool value = JS_ToBool(ctx, result);
  JS_FreeValue(ctx, result);
  return value;
}
struct Runtime {
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  HelixPluginInstance *plugin = nullptr;
  bool stopped = false;
  Runtime() {
    static HelixScriptCapabilityV1 script{1, nullptr, nullptr};
    HelixPluginHost host{
        HELIX_PLUGIN_ABI_VERSION, nullptr,
        [](void *, const char *name, uint32_t) -> const void * {
          return strcmp(name, HELIX_PLUGIN_CAPABILITY_SCRIPT) == 0 ? &script
                                                                   : nullptr;
        }};
    check(helix_plugin_integrate(ctx, "Module/hypr", &host, &plugin) == 0,
          "integrate");
  }
  void import() {
    eval(ctx, "import * as H from 'Module/hypr'; globalThis.Hypr=H;",
         JS_EVAL_TYPE_MODULE);
  }
  void update() {
    if (!stopped)
      plugin->scriptUpdate(plugin->state, nullptr);
    JSContext *jobCtx = nullptr;
    int result;
    while ((result = JS_ExecutePendingJob(rt, &jobCtx)) > 0) {
    }
    if (result < 0)
      exception(jobCtx, JS_EXCEPTION);
    eval(ctx, "if(globalThis.failure) throw new Error(failure)");
  }
  void stop() {
    if (!stopped) {
      plugin->scriptShutdown(plugin->state);
      stopped = true;
    }
  }
  ~Runtime() {
    stop();
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    plugin->destroy(plugin->state);
  }
};
struct Temporary {
  std::string path;
  Temporary() {
    char name[] = "/tmp/helix-hypr.XXXXXX";
    check(mkdtemp(name), "temporary directory");
    path = name;
  }
  ~Temporary() { std::filesystem::remove_all(path); }
};
struct Compositor {
  int commands = -1, events = -1;
  std::atomic<bool> running{true};
  std::atomic<unsigned> stalls{0};
  std::thread commandThread, eventThread;
  std::mutex mutex;
  std::condition_variable wake;
  bool drop = false;
  int commandPeer = -1, eventPeer = -1;
  std::string name;
  static int listen(const std::string &path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0, "socket");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    check(path.size() < sizeof address.sun_path, "socket path");
    strcpy(address.sun_path, path.c_str());
    check(bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0,
          "bind");
    check(::listen(fd, 128) == 0, "listen");
    return fd;
  }
  static bool send(int fd, const std::string &text) {
    size_t offset = 0;
    while (offset < text.size()) {
      ssize_t size =
          ::send(fd, text.data() + offset,
                 std::min<size_t>(1024, text.size() - offset), MSG_NOSIGNAL);
      if (size < 0 && errno == EINTR)
        continue;
      if (size <= 0)
        return false;
      offset += size;
    }
    return true;
  }
  Compositor(const std::string &root, std::string instance)
      : name(std::move(instance)) {
    std::string directory = root + "/hypr/" + name;
    std::filesystem::create_directories(directory);
    commands = listen(directory + "/.socket.sock");
    events = listen(directory + "/.socket2.sock");
    commandThread = std::thread([this] {
      while (running) {
        int fd = accept(commands, nullptr, nullptr);
        if (fd < 0)
          break;
        {
          std::lock_guard<std::mutex> lock(mutex);
          commandPeer = fd;
        }
        std::string command;
        char byte;
        while (recv(fd, &byte, 1, 0) == 1 && byte)
          command += byte;
        if (command == "stall") {
          ++stalls;
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait_for(lock, 3s, [this] { return !running; });
        } else if (command == "j/clients")
          send(fd, "[{\"title\":\"" + std::string(100000, 'x') + "\"}]");
        else if (command == "j/invalid")
          send(fd, "invalid JSON");
        else
          send(fd, name);
        {
          std::lock_guard<std::mutex> lock(mutex);
          commandPeer = -1;
          close(fd);
        }
      }
    });
    eventThread = std::thread([this] {
      while (running) {
        int fd = accept(events, nullptr, nullptr);
        if (fd < 0)
          break;
        {
          std::lock_guard<std::mutex> lock(mutex);
          eventPeer = fd;
        }
        send(fd, "work");
        std::this_thread::sleep_for(5ms);
        send(fd, "spacev2>>2,2\n");
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock, [this] { return drop || !running; });
          drop = false;
          eventPeer = -1;
          close(fd);
        }
      }
    });
  }
  void disconnect() {
    std::lock_guard<std::mutex> lock(mutex);
    drop = true;
    wake.notify_all();
  }
  ~Compositor() {
    running = false;
    wake.notify_all();
    shutdown(commands, SHUT_RDWR);
    shutdown(events, SHUT_RDWR);
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (commandPeer >= 0)
        shutdown(commandPeer, SHUT_RDWR);
      if (eventPeer >= 0)
        shutdown(eventPeer, SHUT_RDWR);
    }
    commandThread.join();
    eventThread.join();
    close(commands);
    close(events);
  }
};
void wait(Runtime &first, Runtime &second, const char *condition) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (!truth(first.ctx, condition)) {
    check(std::chrono::steady_clock::now() < deadline, condition);
    first.update();
    second.update();
    std::this_thread::sleep_for(2ms);
  }
}
void setup(Runtime &runtime, const std::string &root, const std::string &name) {
  eval(runtime.ctx, "globalThis.socketRoot='" + root +
                        "'; globalThis.instance='" + name + "';");
  eval(runtime.ctx, R"JS(
        globalThis.assert=(value,message)=>{if(!value)throw new Error(message)};
        globalThis.connections=[]; globalThis.events=0; globalThis.siblingEvents=0;
        Hypr.on('connection',event=>connections.push(event.payload));
        const sibling=event=>{assert(event.payload==='2,2','fragmented event');++siblingEvents;};
        Hypr.on('workspacev2',()=>{++events;Hypr.off('workspacev2',sibling)});
        Hypr.on('workspacev2',sibling);
        Hypr.connect({runtimeDir:socketRoot,instanceSignature:instance});
        globalThis.done=false;
        (async()=>{
            const detached=Hypr.send;
            assert(await detached('version')===instance,'runtime-bound detached function');
            const clients=await Hypr.clients();
            assert(Array.isArray(clients)&&clients[0].title.length===100000,'complete large reply');
            let invalid=false;try{await Hypr.json('invalid')}catch(e){invalid=true}
            assert(invalid,'invalid JSON must reject');
            const pending=[];let rejected=false;
            for(let i=0;i<65;i++){try{pending.push(Hypr.send('version'))}catch(e){rejected=true}}
            assert(rejected&&pending.length===64,'pending request limit');
            assert((await Promise.all(pending)).every(value=>value===instance),'request results');
            done=true;
        })().catch(e=>globalThis.failure=String(e));
    )JS");
}
} // namespace
int main() {
  try {
    Temporary temporary;
    Compositor left(temporary.path, "first"), right(temporary.path, "second");
    Runtime first, second;
    first.import();
    second.import();
    second.update();
    setup(first, temporary.path, "first");
    setup(second, temporary.path, "second");
    wait(first, second,
         "done && connections.length===1 && events===1 && siblingEvents===1");
    wait(second, first,
         "done && connections.length===1 && events===1 && siblingEvents===1");
    left.disconnect();
    wait(first, second, "connections.length>=3 && events>=2");
    eval(first.ctx,
         "assert(connections.slice(0,3).join(',')==='connected,disconnected,"
         "connected','connection recovery');assert(siblingEvents===1,'removed "
         "callback remained active');");
    eval(second.ctx, "assert(connections.length===1 && events===1,'runtime "
                     "event isolation');");
    eval(first.ctx,
         "globalThis.timeoutDone=false;Hypr.send('stall').then(()=>globalThis."
         "failure='stalled IPC resolved').catch(()=>timeoutDone=true);");
    wait(first, second, "timeoutDone");
    // A shutdown must interrupt blocked reads and leave the other runtime
    // usable.
    eval(second.ctx, "Hypr.send('stall').catch(()=>{});");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!right.stalls) {
      check(std::chrono::steady_clock::now() < deadline, "stall did not start");
      first.update();
      second.update();
      std::this_thread::sleep_for(2ms);
    }
    const auto before = std::chrono::steady_clock::now();
    second.stop();
    check(std::chrono::steady_clock::now() - before < 1s,
          "shutdown did not interrupt socket");
    eval(second.ctx, "let "
                     "stopped=false;try{Hypr.send('version')}catch(e){stopped="
                     "true}assert(stopped,'shutdown guard');");
    eval(first.ctx,
         "globalThis.survived=false;Hypr.send('version').then(value=>{assert("
         "value==='first','surviving "
         "runtime');survived=true}).catch(e=>globalThis.failure=String(e));");
    wait(first, second, "survived");
    puts("Hyprland complete replies, runtime isolation, events, reconnect and "
         "shutdown passed");
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "Hyprland regression failed: %s\n", error.what());
    return 1;
  }
}
