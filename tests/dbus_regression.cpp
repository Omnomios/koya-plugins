#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include <dbus/dbus.h>
#include "../sdk/quickjs/quickjs.h"
#include "helix/plugin.h"

// Exercise the built plugin through its exported JS API without the renderer.

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void append(DBusMessageIter* it, int type, const void* value) {
    check(dbus_message_iter_append_basic(it, type, value), "append basic");
}
void open(DBusMessageIter* it, int type, const char* sig, DBusMessageIter* sub) {
    check(dbus_message_iter_open_container(it, type, sig, sub), "open container");
}
void close(DBusMessageIter* it, DBusMessageIter* sub) {
    check(dbus_message_iter_close_container(it, sub), "close container");
}
void bytes(DBusMessageIter* it) {
    DBusMessageIter array;
    open(it, DBUS_TYPE_ARRAY, "y", &array);
    for (uint8_t value : {0, 1, 127, 128, 255}) append(&array, DBUS_TYPE_BYTE, &value);
    close(it, &array);
}
void properties(DBusMessageIter* it) {
    DBusMessageIter array, entry, variant;
    open(it, DBUS_TYPE_ARRAY, "{sv}", &array);
    open(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    const char* key = "Strength";
    append(&entry, DBUS_TYPE_STRING, &key);
    open(&entry, DBUS_TYPE_VARIANT, "y", &variant);
    uint8_t strength = 73;
    append(&variant, DBUS_TYPE_BYTE, &strength);
    close(&entry, &variant);
    close(&array, &entry);
    open(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    key = "ssid";
    append(&entry, DBUS_TYPE_STRING, &key);
    open(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
    bytes(&variant);
    close(&entry, &variant);
    close(&array, &entry);
    close(it, &array);
}
void settings(DBusMessageIter* it) {
    DBusMessageIter array, entry;
    open(it, DBUS_TYPE_ARRAY, "{sa{sv}}", &array);
    open(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    const char* key = "802-11-wireless";
    append(&entry, DBUS_TYPE_STRING, &key);
    properties(&entry);
    close(&array, &entry);
    // Include an empty nested dictionary.
    open(&array, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    key = "ipv4";
    append(&entry, DBUS_TYPE_STRING, &key);
    DBusMessageIter empty;
    open(&entry, DBUS_TYPE_ARRAY, "{sv}", &empty);
    close(&entry, &empty);
    close(&array, &entry);
    close(it, &array);
}
void check_exception(JSContext* ctx, JSValue value) {
    if (!JS_IsException(value)) return;
    JSValue error = JS_GetException(ctx);
    const char* text = JS_ToCString(ctx, error);
    std::string message = text ? text : "JS exception";
    if (text) JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, error);
    throw std::runtime_error(message);
}
void eval(JSContext* ctx, const char* source, int flags = JS_EVAL_TYPE_GLOBAL) {
    JSValue value = JS_Eval(ctx, source, strlen(source), "<dbus-test>", flags);
    check_exception(ctx, value);
    JS_FreeValue(ctx, value);
}
bool truth(JSContext* ctx, const char* source) {
    JSValue value = JS_Eval(ctx, source, strlen(source), "<dbus-test-check>", JS_EVAL_TYPE_GLOBAL);
    check_exception(ctx, value);
    bool result = JS_ToBool(ctx, value);
    JS_FreeValue(ctx, value);
    return result;
}
struct Runtime {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    HelixPluginInstance* plugin = nullptr;
    Runtime() {
        static HelixScriptCapabilityV1 script{1, nullptr, nullptr};
        HelixPluginHost host{HELIX_PLUGIN_ABI_VERSION, nullptr,
            [](void*, const char* name, uint32_t) -> const void* {
                return strcmp(name, HELIX_PLUGIN_CAPABILITY_SCRIPT) == 0 ? &script : nullptr;
            }};
        check(helix_plugin_integrate(ctx, "Module/dbus", &host, &plugin) == 0, "integrate");
    }
    void import() { eval(ctx, "import * as D from 'Module/dbus'; globalThis.DBus = D;", JS_EVAL_TYPE_MODULE); }
    void update() {
        plugin->scriptUpdate(plugin->state, nullptr);
        JSContext* jobCtx = nullptr;
        int result;
        while ((result = JS_ExecutePendingJob(rt, &jobCtx)) > 0) {}
        if (result < 0) check_exception(jobCtx, JS_EXCEPTION);
        eval(ctx, "if (globalThis.failure) throw new Error(failure);");
    }
    ~Runtime() {
        plugin->scriptShutdown(plugin->state);
        // Native closures may outlive shutdown, but reject further calls.
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        plugin->destroy(plugin->state);
    }
};
struct Service {
    DBusConnection* conn;
    explicit Service(DBusBusType bus) {
        DBusError error; dbus_error_init(&error);
        conn = dbus_bus_get_private(bus, &error);
        check(conn, error.message ? error.message : "service connect");
        dbus_connection_set_exit_on_disconnect(conn, false);
        check(dbus_bus_request_name(conn, "org.example.HelixTest", 0, &error) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER, "service name");
        dbus_error_free(&error);
    }
    void update() {
        dbus_connection_read_write(conn, 0);
        while (DBusMessage* request = dbus_connection_pop_message(conn)) {
            if (dbus_message_get_type(request) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
                const char* method = dbus_message_get_member(request);
                DBusMessage* reply = dbus_message_new_method_return(request);
                DBusMessageIter it; dbus_message_iter_init_append(reply, &it);
                if (strcmp(method, "GetByte") == 0) {
                    uint8_t value = 231; append(&it, DBUS_TYPE_BYTE, &value);
                } else if (strcmp(method, "GetStrength") == 0) {
                    DBusMessageIter variant; open(&it, DBUS_TYPE_VARIANT, "y", &variant);
                    uint8_t value = 73; append(&variant, DBUS_TYPE_BYTE, &value);
                    close(&it, &variant);
                } else if (strcmp(method, "GetBytes") == 0) {
                    bytes(&it);
                } else if (strcmp(method, "GetVariantBytes") == 0) {
                    DBusMessageIter variant; open(&it, DBUS_TYPE_VARIANT, "ay", &variant);
                    bytes(&variant); close(&it, &variant);
                } else if (strcmp(method, "GetSettings") == 0) {
                    settings(&it);
                } else if (strcmp(method, "GetText") == 0) {
                    const char* value = "{literal text}"; append(&it, DBUS_TYPE_STRING, &value);
                } else if (strcmp(method, "Fail") == 0) {
                    dbus_message_unref(reply);
                    reply = dbus_message_new_error(request, "org.example.Failure", "expected");
                } else if (strcmp(method, "Emit") == 0) {
                    emit_signals();
                }
                check(dbus_connection_send(conn, reply, nullptr), "send reply");
                dbus_message_unref(reply);
                dbus_connection_flush(conn);
            }
            dbus_message_unref(request);
        }
    }
    void emit_signals() {
        for (const char* member : {"HardwareButton", "PropertiesChanged", "Text", "Empty", "Struct"}) {
            DBusMessage* signal = dbus_message_new_signal("/org/example/Test", "org.example.HelixTest", member);
            DBusMessageIter it; dbus_message_iter_init_append(signal, &it);
            if (strcmp(member, "HardwareButton") == 0) {
                properties(&it);
            } else if (strcmp(member, "PropertiesChanged") == 0) {
                const char* iface = "org.example.Device"; append(&it, DBUS_TYPE_STRING, &iface);
                properties(&it);
                DBusMessageIter array; open(&it, DBUS_TYPE_ARRAY, "s", &array);
                const char* name = "State"; append(&array, DBUS_TYPE_STRING, &name);
                close(&it, &array);
            } else if (strcmp(member, "Text") == 0) {
                const char* value = "hello \"world\"\n"; append(&it, DBUS_TYPE_STRING, &value);
                dbus_bool_t enabled = true; append(&it, DBUS_TYPE_BOOLEAN, &enabled);
                uint8_t byte = 255; append(&it, DBUS_TYPE_BYTE, &byte);
            } else if (strcmp(member, "Struct") == 0) {
                DBusMessageIter fields; open(&it, DBUS_TYPE_STRUCT, nullptr, &fields);
                const char* value = "field"; append(&fields, DBUS_TYPE_STRING, &value);
                properties(&fields); close(&it, &fields);
            }
            check(dbus_connection_send(conn, signal, nullptr), "send signal");
            dbus_message_unref(signal);
        }
        dbus_connection_flush(conn);
    }
    ~Service() { dbus_connection_close(conn); dbus_connection_unref(conn); }
};
void wait(Runtime& runtime, Service& session, Service& system, const char* condition, Runtime* other = nullptr) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!truth(runtime.ctx, condition)) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(std::string("timed out waiting for JS: ") + condition);
        session.update(); system.update();
        runtime.update();
        if (other) other->update();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
}
int main() {
    try {
        dbus_threads_init_default();
        Service session(DBUS_BUS_SESSION), system(DBUS_BUS_SYSTEM);
        Runtime first;
        Runtime second;
        // Import first after second integrated: deferred initialization must use
        // the correct context. Update second before invoking first's API too.
        first.import(); second.import(); second.update();
        const char* setup = R"JS(
            globalThis.assert = (ok, message) => { if (!ok) throw new Error(message); };
            globalThis.done = false;
            globalThis.sessionSignals = []; globalThis.systemSignals = [];
            (async () => {
                const {session, system} = DBus;
                assert(session !== system, 'distinct handles');
                let threw = false;
                try { session.addMatch("type='signal'"); } catch(e) { threw = true; }
                assert(threw, 'not connected guard');
                await Promise.all([session.connect(), system.connect(), session.connect(), DBus.connect('session')]);
                await DBus.connect();
                let wrongBus = false;
                try { DBus.connect('system'); } catch(e) { wrongBus = true; }
                assert(wrongBus, 'legacy bus change fails explicitly');
                assert(await DBus.call('org.example.HelixTest','/org/example/Test','org.example.HelixTest','GetByte') === '231', 'legacy API');
                const detached = session.call;
                const ids = await Promise.all([
                    detached('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetId'),
                    system.call('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetId')
                ]);
                assert(ids[0] !== ids[1], 'independent bus destinations');
                const call = method => session.call('org.example.HelixTest','/org/example/Test','org.example.HelixTest',method);
                assert(await call('GetByte') === '231', 'byte scalar');
                assert(await call('GetStrength') === '73', 'variant byte property');
                for (const method of ['GetBytes','GetVariantBytes']) {
                    assert(JSON.stringify(await call(method)) === '[0,1,127,128,255]', method);
                }
                const settings = await call('GetSettings');
                assert(settings['802-11-wireless'].Strength === 73, 'nested dictionary byte');
                assert(JSON.stringify(settings['802-11-wireless'].ssid) === '[0,1,127,128,255]', 'nested ay');
                assert(JSON.stringify(settings.ipv4) === '{}', 'empty nested dictionary');
                assert(await call('GetText') === '{literal text}', 'JSON-looking string stays string');
                let error;
                try { await call('Fail'); } catch(e) { error = e; }
                assert(error === 'ERROR:org.example.Failure', 'error reply');
                const complex = await system.callComplex('org.example.HelixTest','/org/example/Test','org.example.HelixTest','GetSettings','');
                assert(complex['802-11-wireless'].Strength === 73, 'complex call decoder');
                for (const [bus, received] of [[session,sessionSignals],[system,systemSignals]]) {
                    bus.addMatch("type='signal',interface='org.example.HelixTest'");
                    // The in-flight callback snapshot must survive removal.
                    const removeSelf = () => bus.offSignal(removeSelf);
                    bus.onSignal(removeSelf);
                    bus.onSignal(sig => received.push(sig));
                    // A round trip ensures AddMatch is applied before emission.
                    await bus.call('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','GetId');
                }
                await call('Emit');
                globalThis.done = true;
            })().catch(e => globalThis.failure = String(e));
        )JS";
        eval(first.ctx, setup);
        wait(first, session, system, "done && sessionSignals.length === 5", &second);
        eval(first.ctx, R"JS(
            assert(systemSignals.length === 0, 'session signals isolated');
            const byName = Object.fromEntries(sessionSignals.map(s => [s.member,s]));
            const button = byName.HardwareButton;
            assert(button.signature === 'a{sv}', 'dictionary signal signature');
            assert(button.args[0].Strength === 73, 'dictionary signal argument');
            assert(JSON.parse(button.body).Strength === 73, 'composite body JSON');
            const changed = byName.PropertiesChanged;
            assert(changed.args.length === 3, 'all signal arguments');
            assert(changed.args[0] === 'org.example.Device', 'first string argument');
            assert(changed.args[1].Strength === 73 && changed.args[2][0] === 'State', 'remaining composite arguments');
            assert(changed.body === changed.args[0], 'legacy string body');
            assert(JSON.stringify(byName.Text.args) === JSON.stringify(['hello "world"\n',true,255]), 'typed and escaped basic arguments');
            assert(byName.Empty.args.length === 0 && byName.Empty.body === '', 'empty signal');
            assert(byName.Struct.args[0][1].Strength === 73, 'struct recursion');
            globalThis.systemDone = false;
            DBus.system.call('org.example.HelixTest','/org/example/Test','org.example.HelixTest','Emit')
                .then(() => globalThis.systemDone = true).catch(e => globalThis.failure = String(e));
        )JS");
        wait(first, session, system, "systemDone && systemSignals.length === 5", &second);
        eval(first.ctx, "assert(sessionSignals.length === 5, 'system signals isolated');");
        // Connect another runtime to the same bus: each gets a private stream.
        eval(second.ctx, R"JS(
            globalThis.done = false;
            DBus.session.connect().then(() => DBus.session.call('org.example.HelixTest','/org/example/Test','org.example.HelixTest','GetByte'))
                .then(value => { if (value !== '231') throw new Error('second runtime reply'); globalThis.done = true; })
                .catch(e => globalThis.failure = String(e));
        )JS");
        wait(second, session, system, "done", &first);
        first.plugin->scriptShutdown(first.plugin->state);
        eval(first.ctx, "let closed=false; try { DBus.session.connect(); } catch(e) { closed=true; } assert(closed, 'shutdown guard');");
        eval(second.ctx, R"JS(
            globalThis.done = false;
            DBus.session.call('org.example.HelixTest','/org/example/Test','org.example.HelixTest','GetByte')
                .then(value => { if (value !== '231') throw new Error('surviving runtime'); globalThis.done = true; })
                .catch(e => globalThis.failure = String(e));
        )JS");
        wait(second, session, system, "done");
        puts("D-Bus decoding, signal delivery, independent buses, and runtime isolation passed");
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "D-Bus regression failed: %s\n", error.what());
        return 1;
    }
}
