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
#include <chrono>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dbus/dbus.h>

#include "../../sdk/quickjs/quickjs.h"
#include "../../sdk/module_hooks.h"

namespace {

// Which DBus bus to connect to.

enum class BusType : uint8_t { Session, System };

// Tracks a JS Promise's resolve/reject so we can settle it on the engine thread.
struct PendingPromise {
    JSContext* ctx;
    JSValue resolve;
    JSValue reject;
    uint64_t createdMs;
    uint32_t timeoutMs;
};

// Result produced by the pump thread for a method return or error.
struct ReplyItem {
    uint32_t id;
    bool ok;
    bool isJson = false;                // true if payload holds JSON to parse
    bool isBytes = false;               // true when returning ay (array of bytes)
    std::string payload;                // string or JSON-encoded object
    std::vector<uint8_t> bytesPayload;  // for ay
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
// Map DBus reply serial -> our promise id for correct correlation
static std::mutex g_serialMapMutex;
static std::unordered_map<uint32_t, uint32_t> g_serialToPromiseId;
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
static std::atomic<bool> g_connecting{false};
static std::mutex g_connectWaitersMutex;
static std::vector<uint32_t> g_connectWaiterIds;
// Serialize bus I/O between sender and pump to avoid reply-mapping race
static std::mutex g_busIoMutex;
static bool g_debug = false;

static std::string js_to_string(JSContext* ctx, JSValueConst v) {
    const char* c = JS_ToCString(ctx, v);
    std::string s = c ? c : "";
    if (c) JS_FreeCString(ctx, c);
    return s;
}

// -------- DBus value stringification helpers --------
static void json_append_escaped(std::string& out, const char* s) {
    out.push_back('"');
    for (const char* p = s ? s : ""; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out.push_back((char)ch);
                }
        }
    }
    out.push_back('"');
}

static std::string dbus_basic_to_string(int type, DBusMessageIter* it) {
    switch (type) {
        case DBUS_TYPE_STRING: {
            const char* s; dbus_message_iter_get_basic(it, &s);
            return s ? std::string(s) : std::string();
        }
        case DBUS_TYPE_OBJECT_PATH: {
            const char* p; dbus_message_iter_get_basic(it, &p);
            return p ? std::string(p) : std::string();
        }
        case DBUS_TYPE_BOOLEAN: {
            dbus_bool_t b; dbus_message_iter_get_basic(it, &b);
            return b ? "true" : "false";
        }
        case DBUS_TYPE_BYTE: { unsigned int v; dbus_message_iter_get_basic(it, &v); return std::to_string(v); }
        case DBUS_TYPE_INT16: { int16_t v; dbus_message_iter_get_basic(it, &v); return std::to_string(v); }
        case DBUS_TYPE_UINT16:{ uint16_t v; dbus_message_iter_get_basic(it, &v); return std::to_string(v); }
        case DBUS_TYPE_INT32: { int32_t v; dbus_message_iter_get_basic(it, &v); return std::to_string(v); }
        case DBUS_TYPE_UINT32:{ uint32_t v; dbus_message_iter_get_basic(it, &v); return std::to_string(v); }
        case DBUS_TYPE_INT64: { long long v; dbus_message_iter_get_basic(it, &v); return std::to_string(v); }
        case DBUS_TYPE_UINT64:{ unsigned long long v; dbus_message_iter_get_basic(it, &v); return std::to_string(v); }
        case DBUS_TYPE_DOUBLE:{ double v; dbus_message_iter_get_basic(it, &v); char buf[64]; snprintf(buf,sizeof(buf),"%g",v); return std::string(buf); }
        case DBUS_TYPE_SIGNATURE: {
            const char* s; dbus_message_iter_get_basic(it, &s);
            return s ? std::string(s) : std::string();
        }
        default: return std::string();
    }
}

static void dbus_value_to_json(std::string& out, DBusMessageIter* it);

static void dbus_array_to_json(std::string& out, DBusMessageIter* it) {
    // it points to ARRAY
    int elem = dbus_message_iter_get_element_type(it);
    DBusMessageIter sub; dbus_message_iter_recurse(it, &sub);
    if (elem == DBUS_TYPE_DICT_ENTRY) {
        // a{sv}
        out.push_back('{');
        bool first = true;
        while (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter dict; dbus_message_iter_recurse(&sub, &dict);
            const char* key = "";
            if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_STRING) {
                dbus_message_iter_get_basic(&dict, &key);
                dbus_message_iter_next(&dict);
            }
            if (!first) out.push_back(','); first = false;
            json_append_escaped(out, key ? key : "");
            out.push_back(':');
            if (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_VARIANT) {
                DBusMessageIter var; dbus_message_iter_recurse(&dict, &var);
                dbus_value_to_json(out, &var);
            } else {
                out += "null";
            }
            dbus_message_iter_next(&sub);
        }
        out.push_back('}');
    } else {
        // Generic array → JSON array
        out.push_back('[');
        bool first = true;
        while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
            if (!first) out.push_back(','); first = false;
            dbus_value_to_json(out, &sub);
            dbus_message_iter_next(&sub);
        }
        out.push_back(']');
    }
}

static void dbus_struct_to_json(std::string& out, DBusMessageIter* it) {
    // Represent struct as JSON array of its fields
    DBusMessageIter sub; dbus_message_iter_recurse(it, &sub);
    out.push_back('[');
    bool first = true;
    while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
        if (!first) out.push_back(','); first = false;
        dbus_value_to_json(out, &sub);
        dbus_message_iter_next(&sub);
    }
    out.push_back(']');
}

static void dbus_variant_to_json(std::string& out, DBusMessageIter* it) {
    DBusMessageIter sub; dbus_message_iter_recurse(it, &sub);
    dbus_value_to_json(out, &sub);
}

static void dbus_value_to_json(std::string& out, DBusMessageIter* it) {
    int t = dbus_message_iter_get_arg_type(it);
    if (t == DBUS_TYPE_ARRAY) {
        dbus_array_to_json(out, it);
        return;
    }
    if (t == DBUS_TYPE_STRUCT) {
        dbus_struct_to_json(out, it);
        return;
    }
    if (t == DBUS_TYPE_VARIANT) {
        dbus_variant_to_json(out, it);
        return;
    }
    // Basic types
    if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH || t == DBUS_TYPE_SIGNATURE) {
        const char* s = nullptr;
        dbus_message_iter_get_basic(it, &s);
        json_append_escaped(out, s ? s : "");
        return;
    }
    std::string s = dbus_basic_to_string(t, it);
    if (!s.empty()) { out += s; return; }
    out += "null";
}


static uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Background I/O loop. Runs on g_pumpThread, pulls messages and enqueues
// lightweight records for delivery during the `update` hook.
static void pump_loop() {
    while (g_running.load()) {
        DBusConnection* conn = g_conn;
        if (!conn) break;
        // Poll for up to 50ms, then drain ALL available messages to avoid backlog-induced latency
        dbus_connection_read_write(conn, 50 /* ms */);
        for (;;) {
            std::lock_guard<std::mutex> iolk(g_busIoMutex);
            DBusMessage* msg = dbus_connection_pop_message(conn);
            if (!msg) break;
            int type = dbus_message_get_type(msg);
        if (type == DBUS_MESSAGE_TYPE_METHOD_RETURN || type == DBUS_MESSAGE_TYPE_ERROR) {
            // Correlate reply using DBus reply serial captured at send time
            uint32_t reply_serial = dbus_message_get_reply_serial(msg);
            const char* errname = type == DBUS_MESSAGE_TYPE_ERROR ? dbus_message_get_error_name(msg) : nullptr;
            DBusMessageIter iter;
            dbus_message_iter_init(msg, &iter);
            std::string out;
            if (errname) {
                out = std::string("ERROR:") + errname;
            } else if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
                int argt = dbus_message_iter_get_arg_type(&iter);
                // ay or variant(ay) → enqueue as bytesReply and continue
                if ((argt == DBUS_TYPE_ARRAY && dbus_message_iter_get_element_type(&iter) == DBUS_TYPE_BYTE) || argt == DBUS_TYPE_VARIANT) {
                    DBusMessageIter arrIt;
                    if (argt == DBUS_TYPE_VARIANT) {
                        DBusMessageIter sub; dbus_message_iter_recurse(&iter, &sub);
                        if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_ARRAY && dbus_message_iter_get_element_type(&sub) == DBUS_TYPE_BYTE) {
                            arrIt = sub;
                        } else {
                            goto not_bytes_path;
                        }
                    } else {
                        arrIt = iter;
                    }
                    // Map reply id now
                    uint32_t bid = 0;
                    if (reply_serial != 0) {
                        std::lock_guard<std::mutex> lk(g_serialMapMutex);
                        auto it = g_serialToPromiseId.find(reply_serial);
                        if (it != g_serialToPromiseId.end()) { bid = it->second; g_serialToPromiseId.erase(it); }
                    }
                    if (bid != 0) {
                        DBusMessageIter sub2; dbus_message_iter_recurse(&arrIt, &sub2);
                        ReplyItem ri; ri.id = bid; ri.ok = (errname == nullptr); ri.isBytes = true;
                        while (dbus_message_iter_get_arg_type(&sub2) == DBUS_TYPE_BYTE) {
                            unsigned int v = 0; dbus_message_iter_get_basic(&sub2, &v);
                            ri.bytesPayload.push_back(static_cast<uint8_t>(v & 0xFF));
                            dbus_message_iter_next(&sub2);
                        }
                        std::lock_guard<std::mutex> lk2(g_replyMutex);
                        g_replies.push(std::move(ri));
                        dbus_message_unref(msg);
                        continue;
                    }
                }
not_bytes_path:
                // Simple basic types returned raw; composite types returned as JSON
                if (argt == DBUS_TYPE_STRING || argt == DBUS_TYPE_OBJECT_PATH || argt == DBUS_TYPE_SIGNATURE ||
                    argt == DBUS_TYPE_BOOLEAN || argt == DBUS_TYPE_BYTE || argt == DBUS_TYPE_INT16 || argt == DBUS_TYPE_UINT16 ||
                    argt == DBUS_TYPE_INT32 || argt == DBUS_TYPE_UINT32 || argt == DBUS_TYPE_INT64 || argt == DBUS_TYPE_UINT64 || argt == DBUS_TYPE_DOUBLE) {
                    out = dbus_basic_to_string(argt, &iter);
                    if (argt == DBUS_TYPE_STRING || argt == DBUS_TYPE_OBJECT_PATH || argt == DBUS_TYPE_SIGNATURE) {
                        const char* s = nullptr; dbus_message_iter_get_basic(&iter, &s); out = s ? s : "";
                    }
                } else {
                    std::string json; dbus_value_to_json(json, &iter); out = std::move(json);
                }
            }
            // Deliver to the matching promise if mapping exists
            uint32_t id = 0;
            if (reply_serial != 0) {
                std::lock_guard<std::mutex> lk(g_serialMapMutex);
                auto it = g_serialToPromiseId.find(reply_serial);
                if (it != g_serialToPromiseId.end()) {
                    id = it->second;
                    g_serialToPromiseId.erase(it);
                }
            }
            if (id != 0) {
                std::lock_guard<std::mutex> lk(g_replyMutex);
                ReplyItem ri; ri.id = id; ri.ok = (errname == nullptr);
                // Mark composite replies as JSON (non-empty and starts with '{' or '[')
                if (!out.empty() && (out[0] == '{' || out[0] == '[')) { ri.isJson = true; }
                ri.payload = std::move(out);
                g_replies.push(std::move(ri));
            }
            if (g_debug) {
                fprintf(stderr, "dbus recv: reply_serial=%u -> id=%u ok=%d payload_len=%zu err=%s\n",
                        reply_serial, id, errname == nullptr, out.size(), errname ? errname : "");
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
}

// JS: connect("session"|"system") — asynchronously connects to dbus on a worker
// thread and starts the pump thread. Returns a Promise<void> that resolves when
// connected. Avoids blocking the JS/engine thread.
static JSValue js_connect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    // If already connected, return immediately
    if (g_conn != nullptr) {
        // Return an already-resolved Promise for API consistency
        JSValue funcs[2];
        JSValue promise = JS_NewPromiseCapability(ctx, funcs);
        JSValue resolve = funcs[0];
        JSValue dummy = JS_NewString(ctx, "");
        JS_Call(ctx, resolve, JS_UNDEFINED, 1, &dummy);
        JS_FreeValue(ctx, dummy);
        JS_FreeValue(ctx, resolve);
        JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
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
    // Enable debug logging if requested
    if (const char* dbg = getenv("KOYA_DBUS_DEBUG")) {
        g_debug = (dbg[0] != '\0' && dbg[0] != '0');
    }

    // Create a Promise and resolve/reject it from the update hook once the
    // background thread finishes connecting.
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue resolve = funcs[0];
    JSValue reject  = funcs[1];
    uint32_t id = g_nextId++;
    {
        std::lock_guard<std::mutex> lk(g_promMutex);
        g_promises.emplace(id, PendingPromise{ctx, resolve, reject, now_ms(), 3000});
    }

    // Queue as a waiter and start a single background connect if not already in-flight
    {
        std::lock_guard<std::mutex> lk(g_connectWaitersMutex);
        g_connectWaiterIds.push_back(id);
    }
    bool expectedConnecting = false;
    if (!g_connecting.compare_exchange_strong(expectedConnecting, true)) {
        return promise; // another connect is in flight; we'll resolve when it finishes
    }

    std::thread([type]() {
    DBusError err; dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(type == BusType::System ? DBUS_BUS_SYSTEM : DBUS_BUS_SESSION, &err);
    if (!conn) {
        std::string e = err.message ? err.message : "failed to connect to bus";
        dbus_error_free(&err);
            // Notify all waiters of failure
            std::vector<uint32_t> waiters;
            {
                std::lock_guard<std::mutex> lk(g_connectWaitersMutex);
                waiters.swap(g_connectWaiterIds);
            }
            if (!waiters.empty()) {
                std::lock_guard<std::mutex> lk(g_replyMutex);
                for (uint32_t wid : waiters) {
                    ReplyItem ri; ri.id = wid; ri.ok = false; ri.payload = e;
                    g_replies.push(std::move(ri));
                }
            }
            g_connecting.store(false);
            return;
        }
        // Do not exit process on disconnect
        dbus_connection_set_exit_on_disconnect(conn, false);
    g_conn = conn; g_busType = type;
        // Start pump thread if not running
    if (!g_running.exchange(true)) {
        if (g_pumpThread.joinable()) {
            g_pumpThread.join();
        }
        g_pumpThread = std::thread(pump_loop);
    }
        if (g_debug) fprintf(stderr, "dbus: connected to %s bus\n", type == BusType::System ? "system" : "session");
        // Notify all waiters of success
        std::vector<uint32_t> waiters;
        {
            std::lock_guard<std::mutex> lk(g_connectWaitersMutex);
            waiters.swap(g_connectWaiterIds);
        }
        if (!waiters.empty()) {
            std::lock_guard<std::mutex> lk(g_replyMutex);
            for (uint32_t wid : waiters) {
                ReplyItem ri; ri.id = wid; ri.ok = true; ri.payload = std::string();
                g_replies.push(std::move(ri));
            }
        }
        g_connecting.store(false);
    }).detach();

    return promise;
}

// JS: addMatch(rule) — convenience wrapper for dbus_bus_add_match.
static JSValue js_addMatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "addMatch expects (string)");
    std::string rule = js_to_string(ctx, argv[0]);
    // Non-blocking AddMatch: send method call without waiting for a reply
    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "AddMatch");
    if (!msg) return JS_ThrowInternalError(ctx, "failed to allocate message");
    DBusMessageIter iter; dbus_message_iter_init_append(msg, &iter);
    const char* s = rule.c_str();
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &s)) {
        dbus_message_unref(msg);
        return JS_ThrowInternalError(ctx, "failed to append arg");
    }
    {
        std::lock_guard<std::mutex> iolk(g_busIoMutex);
        dbus_connection_send(g_conn, msg, nullptr);
    dbus_connection_flush(g_conn);
    }
    dbus_message_unref(msg);
    if (g_debug) fprintf(stderr, "dbus addMatch: %s\n", rule.c_str());
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
    if (!sig.empty()) {
        for (size_t si = 0; si < sig.size(); ++si) {
            if (argi + (int)si >= argc) break;
            char t = sig[si];
            JSValueConst av = argv[argi + si];
            bool ok = true;
            switch (t) {
                case 's': {
                    std::string val = js_to_string(ctx, av);
                    const char* s = val.c_str();
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &s);
                    break;
                }
                case 'o': { // object path as string
                    std::string val = js_to_string(ctx, av);
        const char* s = val.c_str();
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &s);
                    break;
                }
                case 'b': {
                    int b = JS_ToBool(ctx, av);
                    dbus_bool_t bv = b ? 1 : 0;
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_BOOLEAN, &bv);
                    break;
                }
                case 'i': {
                    int32_t v = 0; JS_ToInt32(ctx, &v, av);
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &v);
                    break;
                }
                case 'u': {
                    int64_t tmp = 0; JS_ToInt64(ctx, &tmp, av); uint32_t v = (uint32_t)tmp;
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &v);
                    break;
                }
                case 'x': { // int64
                    int64_t v = 0; JS_ToInt64(ctx, &v, av);
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT64, &v);
                    break;
                }
                case 't': { // uint64
                    int64_t tmp = 0; JS_ToInt64(ctx, &tmp, av); uint64_t v = (uint64_t)tmp;
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT64, &v);
                    break;
                }
                case 'd': { // double
                    double v = 0; JS_ToFloat64(ctx, &v, av);
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_DOUBLE, &v);
                    break;
                }
                case 'y': { // byte
                    int64_t tmp = 0; JS_ToInt64(ctx, &tmp, av); uint8_t v = (uint8_t)tmp;
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_BYTE, &v);
                    break;
                }
                case 'n': { // int16
                    int64_t tmp = 0; JS_ToInt64(ctx, &tmp, av); int16_t v = (int16_t)tmp;
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT16, &v);
                    break;
                }
                case 'q': { // uint16
                    int64_t tmp = 0; JS_ToInt64(ctx, &tmp, av); uint16_t v = (uint16_t)tmp;
                    ok = dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT16, &v);
                    break;
                }
                default: {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
            dbus_message_unref(msg);
            return JS_ThrowInternalError(ctx, "failed to append arg");
            }
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
        g_promises.emplace(id, PendingPromise{ctx, resolve, reject, now_ms(), 5000});
    }
    // Send message and capture serial so we can correlate the reply
    uint32_t serial = 0;
    {
        std::lock_guard<std::mutex> iolk(g_busIoMutex);
    dbus_connection_send(g_conn, msg, &serial);
    if (serial != 0) {
        std::lock_guard<std::mutex> lk(g_serialMapMutex);
        g_serialToPromiseId.emplace(serial, id);
    }
        dbus_connection_flush(g_conn);
    }
    if (g_debug) {
        const char* c_dest = dest.c_str();
        const char* c_path = path.c_str();
        const char* c_iface = iface.c_str();
        const char* c_method = method.c_str();
        fprintf(stderr, "dbus send: id=%u serial=%u dest=%s iface=%s member=%s path=%s sig=%s\n",
                id, serial, c_dest, c_iface, c_method, c_path, sig.empty() ? "" : sig.c_str());
    }
    dbus_message_unref(msg);
    return promise;
}

// ---- Complex call with container/variant support ----
// JS: callComplex(dest, path, iface, method, signature, ...args) -> Promise<string|object>
// Supports signatures containing a{sv}, a{sa{sv}}, v, ay, as, and basic types.
// For variant contents, types are inferred from JS values. To force a type, pass
// an object { _t: 'TYPE', _v: VALUE } where TYPE in { 's','b','i','u','ay','as' }.
static bool append_basic_typed(DBusMessageIter* iter, char t, JSContext* ctx, JSValueConst v) {
    switch (t) {
        case 's': {
            const char* s = JS_ToCString(ctx, v);
            bool ok = dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &s);
            if (s) JS_FreeCString(ctx, s);
            return ok;
        }
        case 'o': {
            const char* s = JS_ToCString(ctx, v);
            bool ok = dbus_message_iter_append_basic(iter, DBUS_TYPE_OBJECT_PATH, &s);
            if (s) JS_FreeCString(ctx, s);
            return ok;
        }
        case 'b': {
            int b = JS_ToBool(ctx, v);
            dbus_bool_t bv = b ? 1 : 0;
            return dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &bv);
        }
        case 'i': {
            int32_t iv = 0; JS_ToInt32(ctx, &iv, v);
            return dbus_message_iter_append_basic(iter, DBUS_TYPE_INT32, &iv);
        }
        case 'u': {
            int64_t tmp = 0; JS_ToInt64(ctx, &tmp, v); uint32_t uv = (uint32_t)tmp;
            return dbus_message_iter_append_basic(iter, DBUS_TYPE_UINT32, &uv);
        }
        case 'y': {
            int64_t tmp = 0; JS_ToInt64(ctx, &tmp, v); uint8_t bv = (uint8_t)tmp;
            return dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE, &bv);
        }
        default:
            return false;
    }
}

static bool infer_and_append_array(DBusMessageIter* iter, JSContext* ctx, JSValueConst arr) {
    // Determine if array is of strings (as) or bytes (ay)
    uint32_t len = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, arr, "length");
    JS_ToUint32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
    bool allString = true, allByte = len > 0;
    for (uint32_t i = 0; i < len; ++i) {
        JSValue iv = JS_GetPropertyUint32(ctx, arr, i);
        if (!JS_IsString(iv)) allString = false;
        if (!JS_IsNumber(iv)) allByte = false;
        JS_FreeValue(ctx, iv);
    }
    if (allByte) {
        DBusMessageIter sub; dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "y", &sub);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue iv = JS_GetPropertyUint32(ctx, arr, i);
            int64_t tmp = 0; JS_ToInt64(ctx, &tmp, iv); JS_FreeValue(ctx, iv);
            uint8_t b = (uint8_t)(tmp & 0xFF);
            if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_BYTE, &b)) { dbus_message_iter_close_container(iter, &sub); return false; }
        }
        dbus_message_iter_close_container(iter, &sub);
        return true;
    }
    if (allString || len == 0) {
        DBusMessageIter sub; dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "s", &sub);
        for (uint32_t i = 0; i < len; ++i) {
            JSValue iv = JS_GetPropertyUint32(ctx, arr, i);
            const char* s = JS_ToCString(ctx, iv); JS_FreeValue(ctx, iv);
            if (!dbus_message_iter_append_basic(&sub, DBUS_TYPE_STRING, &s)) { if (s) JS_FreeCString(ctx, s); dbus_message_iter_close_container(iter, &sub); return false; }
            if (s) JS_FreeCString(ctx, s);
        }
        dbus_message_iter_close_container(iter, &sub);
        return true;
    }
    return false;
}

static bool append_variant(DBusMessageIter* iter, JSContext* ctx, JSValueConst v) {
    // Check for explicit type wrapper: { _t: 'ay', _v: [...] } or { t: 'ay', v: [...] }
    JSValue tv = JS_GetPropertyStr(ctx, v, "_t");
    if (!JS_IsString(tv)) { JS_FreeValue(ctx, tv); tv = JS_GetPropertyStr(ctx, v, "t"); }
    if (JS_IsString(tv)) {
        std::string t = js_to_string(ctx, tv);
        JS_FreeValue(ctx, tv);
        JSValue vv = JS_GetPropertyStr(ctx, v, "_v");
        if (JS_IsUndefined(vv)) { JS_FreeValue(ctx, vv); vv = JS_GetPropertyStr(ctx, v, "v"); }
        bool ok = false;
        if (t == "ay") {
            DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "ay", &var);
            ok = infer_and_append_array(&var, ctx, vv);
            dbus_message_iter_close_container(iter, &var);
        } else if (t == "as") {
            DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "as", &var);
            ok = infer_and_append_array(&var, ctx, vv);
            dbus_message_iter_close_container(iter, &var);
        } else if (t == "s" || t == "b" || t == "i" || t == "u") {
            DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, t.c_str(), &var);
            ok = append_basic_typed(&var, t[0], ctx, vv);
            dbus_message_iter_close_container(iter, &var);
        }
        JS_FreeValue(ctx, vv);
        return ok;
    }
    JS_FreeValue(ctx, tv);
    // Infer type
    if (JS_IsString(v)) {
        DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &var);
        bool ok = append_basic_typed(&var, 's', ctx, v);
        dbus_message_iter_close_container(iter, &var);
        return ok;
    }
    if (JS_IsBool(v)) {
        DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "b", &var);
        bool ok = append_basic_typed(&var, 'b', ctx, v);
        dbus_message_iter_close_container(iter, &var);
        return ok;
    }
    if (JS_IsNumber(v)) {
        // int32 by default
        DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "i", &var);
        bool ok = append_basic_typed(&var, 'i', ctx, v);
        dbus_message_iter_close_container(iter, &var);
        return ok;
    }
    if (JS_IsArray(ctx, v)) {
        // Choose ay if all numeric, else as
        // We'll compute in infer_and_append_array
        // First check numeric or string by scanning
        uint32_t len = 0; JSValue lenv = JS_GetPropertyStr(ctx, v, "length"); JS_ToUint32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
        bool allByte = len > 0, allString = true;
        for (uint32_t i = 0; i < len; ++i) { JSValue iv = JS_GetPropertyUint32(ctx, v, i); if (!JS_IsNumber(iv)) allByte = false; if (!JS_IsString(iv)) allString = false; JS_FreeValue(ctx, iv); }
        const char* sig = allByte ? "ay" : "as";
        DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, sig, &var);
        bool ok = infer_and_append_array(&var, ctx, v);
        dbus_message_iter_close_container(iter, &var);
        return ok;
    }
    // Fallback to string
    DBusMessageIter var; dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT, "s", &var);
    JSValue s = JS_ToString(ctx, v);
    bool ok = append_basic_typed(&var, 's', ctx, s);
    JS_FreeValue(ctx, s);
    dbus_message_iter_close_container(iter, &var);
    return ok;
}

static bool append_a_sv(DBusMessageIter* iter, JSContext* ctx, JSValueConst obj) {
    DBusMessageIter arr; dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &arr);
    JSPropertyEnum* props = nullptr; uint32_t plen = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &plen, obj, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0) {
        dbus_message_iter_close_container(iter, &arr); return false;
    }
    for (uint32_t i = 0; i < plen; ++i) {
        JSAtom atom = props[i].atom;
        JSValue keyv = JS_AtomToString(ctx, atom);
        const char* key = JS_ToCString(ctx, keyv);
        JSValue val = JS_GetProperty(ctx, obj, atom);
        DBusMessageIter dict; dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, nullptr, &dict);
        dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &key);
        if (!append_variant(&dict, ctx, val)) { JS_FreeValue(ctx, val); if (key) JS_FreeCString(ctx, key); JS_FreeValue(ctx, keyv); dbus_message_iter_close_container(&arr, &dict); js_free(ctx, props); dbus_message_iter_close_container(iter, &arr); return false; }
        dbus_message_iter_close_container(&arr, &dict);
        JS_FreeValue(ctx, val);
        if (key) JS_FreeCString(ctx, key);
        JS_FreeValue(ctx, keyv);
        JS_FreeAtom(ctx, atom);
    }
    if (props) js_free(ctx, props);
    dbus_message_iter_close_container(iter, &arr);
    return true;
}

static bool append_a_s_a_sv(DBusMessageIter* iter, JSContext* ctx, JSValueConst obj) {
    DBusMessageIter arr; dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &arr);
    JSPropertyEnum* props = nullptr; uint32_t plen = 0;
    if (JS_GetOwnPropertyNames(ctx, &props, &plen, obj, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0) {
        dbus_message_iter_close_container(iter, &arr); return false;
    }
    for (uint32_t i = 0; i < plen; ++i) {
        JSAtom atom = props[i].atom;
        JSValue keyv = JS_AtomToString(ctx, atom);
        const char* section = JS_ToCString(ctx, keyv);
        JSValue inner = JS_GetProperty(ctx, obj, atom);
        DBusMessageIter dict; dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, nullptr, &dict);
        dbus_message_iter_append_basic(&dict, DBUS_TYPE_STRING, &section);
        // value is a{sv}
        DBusMessageIter innerArr; dbus_message_iter_open_container(&dict, DBUS_TYPE_ARRAY, "{sv}", &innerArr);
        // Reuse append_a_sv logic by writing directly into innerArr
        // But append_a_sv expects to open/close the array; so inline here
        JSPropertyEnum* iprops = nullptr; uint32_t ilen = 0;
        if (JS_GetOwnPropertyNames(ctx, &iprops, &ilen, inner, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0) { if (section) JS_FreeCString(ctx, section); JS_FreeValue(ctx, keyv); JS_FreeAtom(ctx, atom); js_free(ctx, props); dbus_message_iter_close_container(&dict, &innerArr); dbus_message_iter_close_container(&arr, &dict); dbus_message_iter_close_container(iter, &arr); return false; }
        for (uint32_t j = 0; j < ilen; ++j) {
            JSAtom ikeyAtom = iprops[j].atom;
            JSValue ikeyv = JS_AtomToString(ctx, ikeyAtom);
            const char* ikey = JS_ToCString(ctx, ikeyv);
            JSValue ival = JS_GetProperty(ctx, inner, ikeyAtom);
            DBusMessageIter idict; dbus_message_iter_open_container(&innerArr, DBUS_TYPE_DICT_ENTRY, nullptr, &idict);
            dbus_message_iter_append_basic(&idict, DBUS_TYPE_STRING, &ikey);
            if (!append_variant(&idict, ctx, ival)) { JS_FreeValue(ctx, ival); if (ikey) JS_FreeCString(ctx, ikey); JS_FreeValue(ctx, ikeyv); JS_FreeAtom(ctx, ikeyAtom); js_free(ctx, iprops); if (section) JS_FreeCString(ctx, section); JS_FreeValue(ctx, keyv); JS_FreeAtom(ctx, atom); dbus_message_iter_close_container(&innerArr, &idict); dbus_message_iter_close_container(&dict, &innerArr); dbus_message_iter_close_container(&arr, &dict); dbus_message_iter_close_container(iter, &arr); return false; }
            dbus_message_iter_close_container(&innerArr, &idict);
            JS_FreeValue(ctx, ival);
            if (ikey) JS_FreeCString(ctx, ikey);
            JS_FreeValue(ctx, ikeyv);
            JS_FreeAtom(ctx, ikeyAtom);
        }
        if (iprops) js_free(ctx, iprops);
        dbus_message_iter_close_container(&dict, &innerArr);
        dbus_message_iter_close_container(&arr, &dict);
        JS_FreeValue(ctx, inner);
        if (section) JS_FreeCString(ctx, section);
        JS_FreeValue(ctx, keyv);
        JS_FreeAtom(ctx, atom);
    }
    if (props) js_free(ctx, props);
    dbus_message_iter_close_container(iter, &arr);
    return true;
}

static bool append_by_signature(DBusMessageIter* iter, const char** psig, JSContext* ctx, JSValueConst v) {
    const char* sig = *psig;
    if (!sig || *sig == '\0') return false;
    char t = *sig;
    if (t == 'a') {
        ++sig;
        if (*sig == '{') {
            ++sig;
            // Expect sv or sa{sv}
            if (*sig == 's') {
                ++sig;
                if (*sig == 'v') {
                    // a{sv}
                    ++sig; if (*sig != '}') return false; ++sig; // skip '}'
                    bool ok = append_a_sv(iter, ctx, v);
                    *psig = sig; return ok;
                } else if (*sig == 'a') {
                    ++sig; if (*sig != '{') return false; ++sig;
                    if (*sig != 's') return false; ++sig;
                    if (*sig != 'v') return false; ++sig;
                    if (*sig != '}') return false; ++sig; // end inner {sv}
                    if (*sig != '}') return false; ++sig; // end outer {sa{sv}}
                    bool ok = append_a_s_a_sv(iter, ctx, v);
                    *psig = sig; return ok;
                } else {
                    return false;
                }
            }
            return false;
        } else if (*sig == 'y' || *sig == 's') {
            // arrays of basic types via inference
            const char arrSig[2] = { *sig, 0 };
            DBusMessageIter sub; dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, arrSig, &sub);
            bool ok = infer_and_append_array(&sub, ctx, v);
            dbus_message_iter_close_container(iter, &sub);
            ++sig; *psig = sig; return ok;
        } else {
            return false;
        }
    }
    if (t == 'v') {
        ++sig; *psig = sig; return append_variant(iter, ctx, v);
    }
    // Basic types
    bool ok = append_basic_typed(iter, t, ctx, v);
    ++sig; *psig = sig; return ok;
}

static JSValue js_callComplex(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_ThrowTypeError(ctx, "callComplex expects (dest, path, iface, method, signature, ...args)");
    std::string dest = js_to_string(ctx, argv[0]);
    std::string path = js_to_string(ctx, argv[1]);
    std::string iface= js_to_string(ctx, argv[2]);
    std::string method= js_to_string(ctx, argv[3]);
    std::string sig = js_to_string(ctx, argv[4]);

    DBusMessage* msg = dbus_message_new_method_call(dest.c_str(), path.c_str(), iface.c_str(), method.c_str());
    if (!msg) return JS_ThrowInternalError(ctx, "failed to allocate message");
    DBusMessageIter iter; dbus_message_iter_init_append(msg, &iter);
    const char* psig = sig.c_str();
    int argi = 5; // start of JS args
    // Append each top-level arg according to signature
    while (*psig) {
        if (argi >= argc) { dbus_message_unref(msg); return JS_ThrowTypeError(ctx, "callComplex: missing JS arguments for signature"); }
        if (!append_by_signature(&iter, &psig, ctx, argv[argi++])) { dbus_message_unref(msg); return JS_ThrowInternalError(ctx, "callComplex: failed to append arg"); }
    }

    // Create promise and send
    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue resolve = funcs[0];
    JSValue reject  = funcs[1];
    uint32_t id = g_nextId++;
    {
        std::lock_guard<std::mutex> lk(g_promMutex);
        g_promises.emplace(id, PendingPromise{ctx, resolve, reject, now_ms(), 8000});
    }
    uint32_t serial = 0;
    {
        std::lock_guard<std::mutex> iolk(g_busIoMutex);
        dbus_connection_send(g_conn, msg, &serial);
        if (serial != 0) {
            std::lock_guard<std::mutex> lk(g_serialMapMutex);
            g_serialToPromiseId.emplace(serial, id);
        }
        dbus_connection_flush(g_conn);
    }
    if (g_debug) {
        fprintf(stderr, "dbus send(complex): id=%u serial=%u dest=%s iface=%s member=%s path=%s sig=%s\n",
                id, serial, dest.c_str(), iface.c_str(), method.c_str(), path.c_str(), sig.c_str());
    }
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
    // 0) timeouts: synthesize replies for expired promises
    std::vector<ReplyItem> timedOut;
    {
        uint64_t now = now_ms();
        std::lock_guard<std::mutex> lk(g_promMutex);
        for (auto it = g_promises.begin(); it != g_promises.end(); ) {
            const PendingPromise& p = it->second;
            if (p.timeoutMs > 0 && now - p.createdMs > p.timeoutMs) {
                ReplyItem ri; ri.id = it->first; ri.ok = false; ri.payload = std::string("ERROR:timeout");
                timedOut.push_back(std::move(ri));
                // Remove any serial mapping to this id
                {
                    std::lock_guard<std::mutex> lk2(g_serialMapMutex);
                    for (auto it2 = g_serialToPromiseId.begin(); it2 != g_serialToPromiseId.end(); ) {
                        if (it2->second == it->first) it2 = g_serialToPromiseId.erase(it2); else ++it2;
                    }
                }
                it = g_promises.erase(it);
            } else {
                ++it;
            }
        }
    }
    // 1) replies
    std::vector<ReplyItem> replies;
    {
        std::lock_guard<std::mutex> lk(g_replyMutex);
        while (!g_replies.empty()) { replies.push_back(std::move(g_replies.front())); g_replies.pop(); }
    }
    if (!timedOut.empty()) {
        replies.insert(replies.end(), timedOut.begin(), timedOut.end());
    }
    if (!replies.empty()) {
        std::lock_guard<std::mutex> lk(g_promMutex);
        for (auto& r : replies) {
            auto it = g_promises.find(r.id);
            if (it == g_promises.end()) continue;
            auto pend = it->second; g_promises.erase(it);
            if (r.ok) {
                if (r.isBytes) {
                    JSValue arr = JS_NewArray(g_ctx);
                    uint32_t idx = 0;
                    for (uint8_t b : r.bytesPayload) {
                        JS_DefinePropertyValueUint32(
                            g_ctx,
                            arr,
                            idx++,
                            JS_NewInt32(g_ctx, (int)b),
                            JS_PROP_C_W_E
                        );
                    }
                    JS_Call(g_ctx, pend.resolve, JS_UNDEFINED, 1, &arr);
                    JS_FreeValue(g_ctx, arr);
                } else if (r.isJson) {
                    JSValue jsonStr = JS_NewString(g_ctx, r.payload.c_str());
                    JSValue global = JS_GetGlobalObject(g_ctx);
                    JSValue jsonObj = JS_GetPropertyStr(g_ctx, global, "JSON");
                    JSValue parseFn = JS_GetPropertyStr(g_ctx, jsonObj, "parse");
                    JSValue parsed = JS_Call(g_ctx, parseFn, jsonObj, 1, &jsonStr);
                    JS_FreeValue(g_ctx, parseFn);
                    JS_FreeValue(g_ctx, jsonObj);
                    JS_FreeValue(g_ctx, global);
                    JS_FreeValue(g_ctx, jsonStr);
                    JS_Call(g_ctx, pend.resolve, JS_UNDEFINED, 1, &parsed);
                    JS_FreeValue(g_ctx, parsed);
                } else {
                    JSValue s = JS_NewString(g_ctx, r.payload.c_str());
                    JS_Call(g_ctx, pend.resolve, JS_UNDEFINED, 1, &s);
                    JS_FreeValue(g_ctx, s);
                }
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
    JS_SetModuleExport(ctx, m, "callComplex", JS_NewCFunction(ctx, js_callComplex, "callComplex", 6));
    JS_SetModuleExport(ctx, m, "onSignal", JS_NewCFunction(ctx, js_onSignal, "onSignal", 1));
    JS_SetModuleExport(ctx, m, "offSignal", JS_NewCFunction(ctx, js_offSignal, "offSignal", 1));
    return 0;
}

extern "C" {
// Required entry point
JSModuleDef* integrateV1(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook, const KoyaRendererV1*) {
    g_ctx = ctx;
    registerHook("update", dbus_update);
    registerHook("cleanup", [](void*){
        // Stop pump loop
        g_running.store(false);
        g_connecting.store(false);
        // Join pump thread to avoid races with connection teardown
        if (g_pumpThread.joinable()) {
            g_pumpThread.join();
        }
        // Clear any queued connect waiters to avoid leaking promises on shutdown
        {
            std::vector<uint32_t> waiters;
            {
                std::lock_guard<std::mutex> lk(g_connectWaitersMutex);
                waiters.swap(g_connectWaiterIds);
            }
            if (!waiters.empty()) {
                std::lock_guard<std::mutex> lk(g_replyMutex);
                for (uint32_t wid : waiters) {
                    ReplyItem ri; ri.id = wid; ri.ok = false; ri.payload = std::string("canceled");
                    g_replies.push(std::move(ri));
                }
            }
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
    JS_AddModuleExport(ctx, m, "callComplex");
    JS_AddModuleExport(ctx, m, "onSignal");
    JS_AddModuleExport(ctx, m, "offSignal");
    return m;
}
}

} // namespace


