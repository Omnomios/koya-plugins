/*
   Koya Plugin: DBus

   What this is:
   - Application code that exposes a small, pragmatic DBus client to the Koya
     JavaScript runtime (QuickJS).

   Koya vs Application responsibilities:
   - Koya provides:
     - The QuickJS context (`JSContext*`) and module system
     - The hook system (`registerHook("update" | "cleanup", fn)`) to run work on the
       engine thread
   - This plugin provides (application):
     - A background pump thread that reads DBus messages and queues results
     - A tiny JS API exported from `integrate(...)`:
       - connect(bus: "session"|"system")
       - addMatch(rule)
       - call(dest, path, iface, method[, signature, ...args]) -> Promise<string>
       - onSignal(cb), offSignal(cb?)

   Integration points you can copy for your own plugins:
   - `integrate(JSContext*, const char*, RegisterHookFunc)` creates a JS module and
     registers hooks via the provided `registerHook` function.
   - The `update` hook drains cross-thread queues and resolves JS Promises on the
     Koya/JS thread. The `cleanup` hook shuts down threads and releases JS values.

   Threading model:
   - DBus I/O happens off-thread in `g_pumpThread`.
   - Results are transferred via lock-protected queues, then delivered during the
     `update` hook so all JS runs on the engine thread.

   Notes:
   - This file intentionally focuses on a minimal feature set to illustrate the
     Koya plugin shape, not a full DBus surface.
*/
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dbus/dbus.h>

#include "../../sdk/quickjs/quickjs.h"
#include "../../module_hooks.h"

namespace {

// Which DBus bus to connect to.

enum class BusType : uint8_t { Session, System };

// Tracks a JS Promise's resolve/reject so we can settle it on the engine thread.
struct PendingPromise {
    JSContext* ctx;
    JSValue resolve;
    JSValue reject;
};

// Result produced by the pump thread for a method return or error.
struct ReplyItem {
    uint32_t id;
    bool ok;
    std::string payload; // string or JSON-encoded object
};

// Result produced by the pump thread for a signal emission.
struct SignalItem {
    std::string sender;
    std::string path;
    std::string iface;
    std::string member;
    std::string signature;
    std::string body; // simple debug string; consumers can parse further
};

static std::atomic<uint32_t> g_nextId{1};
static std::mutex g_promMutex;
static std::unordered_map<uint32_t, PendingPromise> g_promises;
static std::mutex g_replyMutex;
static std::queue<ReplyItem> g_replies;
static std::mutex g_sigMutex;
static std::queue<SignalItem> g_signals;

static std::mutex g_cbMutex;
static std::vector<JSValue> g_signalCallbacks;

static JSContext* g_ctx = nullptr;

static std::thread g_pumpThread;
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_threadsInited{false};
static DBusConnection* g_conn = nullptr;
static BusType g_busType = BusType::Session;

static std::string js_to_string(JSContext* ctx, JSValueConst v) {
    const char* c = JS_ToCString(ctx, v);
    std::string s = c ? c : "";
    if (c) JS_FreeCString(ctx, c);
    return s;
}

// Background I/O loop. Runs on g_pumpThread, pulls messages and enqueues
// lightweight records for delivery during the `update` hook.
static void pump_loop() {
    while (g_running.load()) {
        DBusConnection* conn = g_conn;
        if (!conn) break;
        dbus_connection_read_write(conn, 50 /* ms */);
        DBusMessage* msg = dbus_connection_pop_message(conn);
        if (!msg) continue;
        int type = dbus_message_get_type(msg);
        if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN || type == DBUS_MESSAGE_TYPE_ERROR) {
            // Correlate via serial? For now, just push string body and let JS resolve latest
            // Better: use pending call API, but keep simple initial version
            const char* errname = type == DBUS_MESSAGE_TYPE_ERROR ? dbus_message_get_error_name(msg) : nullptr;
            DBusMessageIter iter;
            dbus_message_iter_init(msg, &iter);
            std::string out;
            if (errname) {
                out = std::string("ERROR:") + errname;
            } else if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
                int argt = dbus_message_iter_get_arg_type(&iter);
                if (argt == DBUS_TYPE_STRING) {
                    const char* s; dbus_message_iter_get_basic(&iter, &s); out = s ? s : "";
                } else if (argt == DBUS_TYPE_VARIANT) {
                    DBusMessageIter var;
                    dbus_message_iter_recurse(&iter, &var);
                    int vt = dbus_message_iter_get_arg_type(&var);
                    if (vt == DBUS_TYPE_STRING) {
                        const char* s; dbus_message_iter_get_basic(&var, &s); out = s ? s : "";
                    } else if (vt == DBUS_TYPE_BOOLEAN) {
                        dbus_bool_t b; dbus_message_iter_get_basic(&var, &b); out = b ? "true" : "false";
                    } else if (vt == DBUS_TYPE_UINT32) {
                        uint32_t u; dbus_message_iter_get_basic(&var, &u); out = std::to_string(u);
                    } else if (vt == DBUS_TYPE_INT32) {
                        int32_t i; dbus_message_iter_get_basic(&var, &i); out = std::to_string(i);
                    }
                } else if (argt == DBUS_TYPE_ARRAY) {
                    // Try to parse a{sv} into a JSON string
                    int elem_type = dbus_message_iter_get_element_type(&iter);
                    if (elem_type == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter arr;
                        dbus_message_iter_recurse(&iter, &arr);
                        std::string json = "{";
                        bool first = true;
                        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
                            DBusMessageIter dict;
                            dbus_message_iter_recurse(&arr, &dict);
                            // key
                            const char* key = "";
                            if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_STRING) {
                                dbus_message_iter_get_basic(&dict, &key);
                                dbus_message_iter_next(&dict);
                            }
                            // value (variant)
                            std::string valueStr = "null";
                            if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_VARIANT) {
                                DBusMessageIter var;
                                dbus_message_iter_recurse(&dict, &var);
                                int vt = dbus_message_iter_get_arg_type(&var);
                                if (vt == DBUS_TYPE_STRING) {
                                    const char* s; dbus_message_iter_get_basic(&var, &s);
                                    valueStr = std::string("\"") + (s ? s : "") + "\"";
                                } else if (vt == DBUS_TYPE_BOOLEAN) {
                                    dbus_bool_t b; dbus_message_iter_get_basic(&var, &b);
                                    valueStr = b ? "true" : "false";
                                } else if (vt == DBUS_TYPE_UINT32) {
                                    uint32_t u; dbus_message_iter_get_basic(&var, &u);
                                    valueStr = std::to_string(u);
                                } else if (vt == DBUS_TYPE_INT32) {
                                    int32_t i; dbus_message_iter_get_basic(&var, &i);
                                    valueStr = std::to_string(i);
                                } else {
                                    valueStr = "null";
                                }
                            }
                            if (!first) json += ","; first = false;
                            json += "\""; json += (key ? key : ""); json += "\":"; json += valueStr;
                            dbus_message_iter_next(&arr);
                        }
                        json += "}";
                        out = std::move(json);
                    }
                }
            }
            // Deliver to latest promise if any
            uint32_t id = 0;
            {
                std::lock_guard<std::mutex> lk(g_promMutex);
                if (!g_promises.empty()) id = g_promises.begin()->first;
            }
            if (id != 0) {
                std::lock_guard<std::mutex> lk(g_replyMutex);
                g_replies.push(ReplyItem{id, errname == nullptr, std::move(out)});
            }
        } else if (type == DBUS_MESSAGE_TYPE_SIGNAL) {
            SignalItem si;
            si.sender = dbus_message_get_sender(msg) ? dbus_message_get_sender(msg) : "";
            si.path = dbus_message_get_path(msg) ? dbus_message_get_path(msg) : "";
            si.iface = dbus_message_get_interface(msg) ? dbus_message_get_interface(msg) : "";
            si.member = dbus_message_get_member(msg) ? dbus_message_get_member(msg) : "";
            si.signature = dbus_message_get_signature(msg) ? dbus_message_get_signature(msg) : "";
            DBusMessageIter iter;
            dbus_message_iter_init(msg, &iter);
            if (dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_STRING) {
                const char* s; dbus_message_iter_get_basic(&iter, &s);
                si.body = s ? s : "";
            }
            std::lock_guard<std::mutex> lk(g_sigMutex);
            g_signals.push(std::move(si));
        }
            dbus_message_unref(msg);
    }
}

// JS: connect("session"|"system") — initializes libdbus (once), connects, and
// starts the pump thread. No I/O happens on the JS thread.
static JSValue js_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    // Initialize libdbus threading once if using threads
    bool expected = false;
    if (g_threadsInited.compare_exchange_strong(expected, true)) {
        dbus_threads_init_default();
    }
    BusType type = BusType::Session;
    if (argc >= 1 && JS_IsString(argv[0])) {
        std::string t = js_to_string(ctx, argv[0]);
        if (t == "system") type = BusType::System;
    }
    DBusError err; dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(type == BusType::System ? DBUS_BUS_SYSTEM : DBUS_BUS_SESSION, &err);
    if (!conn) {
        std::string e = err.message ? err.message : "failed to connect to bus";
        dbus_error_free(&err);
        return JS_ThrowInternalError(ctx, "%s", e.c_str());
    }
    g_conn = conn; g_busType = type;
    if (!g_running.exchange(true)) {
        if (g_pumpThread.joinable()) {
            // Shouldn't happen, but ensure no stray joinable thread
            g_pumpThread.join();
        }
        g_pumpThread = std::thread(pump_loop);
    }
    return JS_UNDEFINED;
}

// JS: addMatch(rule) — convenience wrapper for dbus_bus_add_match.
static JSValue js_addMatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "addMatch expects (string)");
    std::string rule = js_to_string(ctx, argv[0]);
    DBusError err; dbus_error_init(&err);
    dbus_bus_add_match(g_conn, rule.c_str(), &err);
    dbus_connection_flush(g_conn);
    if (dbus_error_is_set(&err)) {
        std::string e = err.message ? err.message : "add_match failed";
        dbus_error_free(&err);
        return JS_ThrowInternalError(ctx, "%s", e.c_str());
    }
    return JS_UNDEFINED;
}

// JS: call(dest, path, iface, method[, signature, ...args]) -> Promise<string>
// Sends a method call; the pump thread pairs the next reply with the earliest
// pending Promise (simple correlation for this minimal example).
static JSValue js_call(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "call expects (dest, path, iface, method[, signature, ...args])");
    std::string dest = js_to_string(ctx, argv[0]);
    std::string path = js_to_string(ctx, argv[1]);
    std::string iface= js_to_string(ctx, argv[2]);
    std::string method= js_to_string(ctx, argv[3]);
    std::string sig;
    int argi = 4;
    if (argc >= 5 && JS_IsString(argv[4])) { sig = js_to_string(ctx, argv[4]); argi = 5; }

    DBusMessage* msg = dbus_message_new_method_call(dest.c_str(), path.c_str(), iface.c_str(), method.c_str());
    if (!msg) return JS_ThrowInternalError(ctx, "failed to allocate message");
    DBusMessageIter iter; dbus_message_iter_init_append(msg, &iter);
    // Minimal support: single string arg if provided
    if (!sig.empty() && sig == "s" && argc > argi) {
        std::string val = js_to_string(ctx, argv[argi]);
        const char* s = val.c_str();
        if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &s)) {
            dbus_message_unref(msg);
            return JS_ThrowInternalError(ctx, "failed to append arg");
        }
    }

    // Create promise
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue resolve = funcs[0];
    JSValue reject  = funcs[1];
    uint32_t id = g_nextId++;
    {
        std::lock_guard<std::mutex> lk(g_promMutex);
        g_promises.emplace(id, PendingPromise{ctx, resolve, reject});
    }
    // Send message (no pending call API for now; pump_loop will push a reply)
    dbus_connection_send(g_conn, msg, nullptr);
    dbus_connection_flush(g_conn);
    dbus_message_unref(msg);
    return promise;
}

// JS: onSignal(cb) — registers a callback to receive signal objects.
static JSValue js_onSignal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "onSignal expects (function)");
    std::lock_guard<std::mutex> lk(g_cbMutex);
    g_signalCallbacks.push_back(JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

// JS: offSignal([cb]) — removes a specific callback or clears all.
static JSValue js_offSignal(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    std::lock_guard<std::mutex> lk(g_cbMutex);
    if (argc >= 1 && JS_IsFunction(ctx, argv[0])) {
        // remove one by pointer identity
        for (auto it = g_signalCallbacks.begin(); it != g_signalCallbacks.end(); ++it) {
            if (JS_VALUE_GET_PTR(*it) == JS_VALUE_GET_PTR(argv[0])) {
                JS_FreeValue(ctx, *it);
                g_signalCallbacks.erase(it);
                break;
            }
        }
    } else {
        // clear all
        for (auto& cb : g_signalCallbacks) JS_FreeValue(ctx, cb);
        g_signalCallbacks.clear();
    }
    return JS_UNDEFINED;
}

// Hook: update — runs on the engine thread. Delivers pump-thread replies and
// signals to JS by resolving Promises and calling registered handlers.
static void dbus_update(void*) {
    if (!g_ctx) return;
    // 1) replies
    std::vector<ReplyItem> replies;
    {
        std::lock_guard<std::mutex> lk(g_replyMutex);
        while (!g_replies.empty()) { replies.push_back(std::move(g_replies.front())); g_replies.pop(); }
    }
    if (!replies.empty()) {
        std::lock_guard<std::mutex> lk(g_promMutex);
        for (auto& r : replies) {
            auto it = g_promises.find(r.id);
            if (it == g_promises.end()) continue;
            auto pend = it->second; g_promises.erase(it);
            if (r.ok) {
                JSValue s = JS_NewString(g_ctx, r.payload.c_str());
                JS_Call(g_ctx, pend.resolve, JS_UNDEFINED, 1, &s);
                JS_FreeValue(g_ctx, s);
            } else {
                JSValue s = JS_NewString(g_ctx, r.payload.c_str());
                JS_Call(g_ctx, pend.reject, JS_UNDEFINED, 1, &s);
                JS_FreeValue(g_ctx, s);
            }
            JS_FreeValue(g_ctx, pend.resolve);
            JS_FreeValue(g_ctx, pend.reject);
        }
    }
    // 2) signals
    std::vector<SignalItem> sigs;
    {
        std::lock_guard<std::mutex> lk(g_sigMutex);
        while (!g_signals.empty()) { sigs.push_back(std::move(g_signals.front())); g_signals.pop(); }
    }
    if (!sigs.empty()) {
        std::vector<JSValue> cbs;
        {
            std::lock_guard<std::mutex> lk(g_cbMutex);
            cbs = g_signalCallbacks; // copy
        }
        for (auto& s : sigs) {
            JSValue obj = JS_NewObject(g_ctx);
            JS_SetPropertyStr(g_ctx, obj, "sender", JS_NewString(g_ctx, s.sender.c_str()));
            JS_SetPropertyStr(g_ctx, obj, "path", JS_NewString(g_ctx, s.path.c_str()));
            JS_SetPropertyStr(g_ctx, obj, "interface", JS_NewString(g_ctx, s.iface.c_str()));
            JS_SetPropertyStr(g_ctx, obj, "member", JS_NewString(g_ctx, s.member.c_str()));
            JS_SetPropertyStr(g_ctx, obj, "signature", JS_NewString(g_ctx, s.signature.c_str()));
            JS_SetPropertyStr(g_ctx, obj, "body", JS_NewString(g_ctx, s.body.c_str()));
            for (auto& cb : cbs) {
                JSValue arg = JS_DupValue(g_ctx, obj);
                JSValue unused = JS_Call(g_ctx, cb, JS_UNDEFINED, 1, &arg);
                if (JS_IsException(unused)) {
                    JSValue exc = JS_GetException(g_ctx);
                    const char* c = JS_ToCString(g_ctx, exc);
                    fprintf(stderr, "dbus signal handler exception: %s\n", c ? c : "<unknown>");
                    if (c) JS_FreeCString(g_ctx, c);
                    JS_FreeValue(g_ctx, exc);
                }
                JS_FreeValue(g_ctx, unused);
                JS_FreeValue(g_ctx, arg);
            }
            JS_FreeValue(g_ctx, obj);
        }
    }
}

// Module init — binds native functions as JS module exports.
static int dbus_module_init(JSContext* ctx, JSModuleDef* m) {
    JS_SetModuleExport(ctx, m, "connect", JS_NewCFunction(ctx, js_connect, "connect", 1));
    JS_SetModuleExport(ctx, m, "addMatch", JS_NewCFunction(ctx, js_addMatch, "addMatch", 1));
    JS_SetModuleExport(ctx, m, "call", JS_NewCFunction(ctx, js_call, "call", 5));
    JS_SetModuleExport(ctx, m, "onSignal", JS_NewCFunction(ctx, js_onSignal, "onSignal", 1));
    JS_SetModuleExport(ctx, m, "offSignal", JS_NewCFunction(ctx, js_offSignal, "offSignal", 1));
    return 0;
}

extern "C" {
// How to extend:
// - Export an `integrate` symbol from your shared library.
// - Register any hooks you need using the provided RegisterHookFunc.
// - Construct a QuickJS module with your exported functions.
// Koya will call this when it loads the module from disk.
JSModuleDef* integrate(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook) {
    g_ctx = ctx;
    registerHook("update", dbus_update);
    registerHook("cleanup", [](void*){
        // Stop pump loop
        g_running.store(false);
        // Join pump thread to avoid races with connection teardown
        if (g_pumpThread.joinable()) {
            g_pumpThread.join();
        }
        // Free signal callbacks
        {
            std::lock_guard<std::mutex> lk(g_cbMutex);
            for (auto& cb : g_signalCallbacks) JS_FreeValue(g_ctx, cb);
            g_signalCallbacks.clear();
        }
        // Free pending promises' JSValues
        {
            std::lock_guard<std::mutex> lk(g_promMutex);
            for (auto& kv : g_promises) {
                JS_FreeValue(g_ctx, kv.second.resolve);
                JS_FreeValue(g_ctx, kv.second.reject);
            }
            g_promises.clear();
        }
        // Drop queued data
        {
            std::lock_guard<std::mutex> lk(g_replyMutex);
            while (!g_replies.empty()) g_replies.pop();
        }
        {
            std::lock_guard<std::mutex> lk(g_sigMutex);
            while (!g_signals.empty()) g_signals.pop();
        }
        // Release shared DBus connection (do not close shared connections)
        if (g_conn) {
            dbus_connection_unref(g_conn);
            g_conn = nullptr;
        }
    });
    JSModuleDef* m = JS_NewCModule(ctx, module_name, dbus_module_init);
    if (!m) return nullptr;
    JS_AddModuleExport(ctx, m, "connect");
    JS_AddModuleExport(ctx, m, "addMatch");
    JS_AddModuleExport(ctx, m, "call");
    JS_AddModuleExport(ctx, m, "onSignal");
    JS_AddModuleExport(ctx, m, "offSignal");
    return m;
}
}

} // namespace


