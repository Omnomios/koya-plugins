// Incoming messages are retained by the pump and handled only on the JS thread.
static void send_service_reply(DbusState* state, DBusMessage* request, DBusMessage* reply) {
    if (!dbus_message_get_no_reply(request) && state->connection) {
        std::lock_guard<std::mutex> lock(state->busIoMutex);
        dbus_connection_send(state->connection, reply, nullptr);
        dbus_connection_flush(state->connection);
    }
    dbus_message_unref(reply);
}

static JSValue invocation_finish(JSContext* ctx, JSValueConst, int argc,
                                 JSValueConst* argv, int magic, JSValue* data) {
    int64_t pointer = 0; uint32_t id = 0;
    if (JS_ToBigInt64(ctx, &pointer, data[0]) < 0 || JS_ToUint32(ctx, &id, data[1]) < 0) return JS_EXCEPTION;
    auto* state = reinterpret_cast<DbusState*>(static_cast<intptr_t>(pointer));
    if (state->shuttingDown.load()) return JS_ThrowInternalError(ctx, "bus is shut down");
    auto found = state->invocations.find(id);
    if (found == state->invocations.end()) return JS_ThrowInternalError(ctx, "method call already completed or unexported");
    DBusMessage* request = found->second;
    DBusMessage* reply = nullptr;
    if (magic) {
        if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) return JS_ThrowTypeError(ctx, "error expects (name, message)");
        std::string name = js_to_string(ctx, argv[0]), message = js_to_string(ctx, argv[1]);
        if (!dbus_validate_error_name(name.c_str(), nullptr)) return JS_ThrowTypeError(ctx, "invalid D-Bus error name");
        reply = dbus_message_new_error(request, name.c_str(), message.c_str());
    } else {
        if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "reply expects (signature, ...args)");
        reply = dbus_message_new_method_return(request);
        if (!reply) return JS_ThrowOutOfMemory(ctx);
        DBusMessageIter out; dbus_message_iter_init_append(reply, &out);
        if (!append_values(&out, js_to_string(ctx, argv[0]), ctx, argc - 1, argv + 1)) {
            dbus_message_unref(reply);
            return JS_ThrowTypeError(ctx, "reply arguments do not match signature");
        }
    }
    if (!reply) return JS_ThrowOutOfMemory(ctx);
    send_service_reply(state, request, reply);
    dbus_message_unref(request);
    state->invocations.erase(found);
    return JS_UNDEFINED;
}

static std::string service_key(const std::string& path, const std::string& iface) {
    return path + '\n' + iface;
}

static JSValue js_exportObject(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_conn) return JS_ThrowInternalError(ctx, "bus is not connected; await connect() first");
    if (argc != 3 || !JS_IsString(argv[0]) || !JS_IsString(argv[1]) || !JS_IsFunction(ctx, argv[2])) return JS_ThrowTypeError(ctx, "exportObject expects (path, interface, handler)");
    std::string path = js_to_string(ctx, argv[0]), iface = js_to_string(ctx, argv[1]);
    if (!dbus_validate_path(path.c_str(), nullptr) || !dbus_validate_interface(iface.c_str(), nullptr)) return JS_ThrowTypeError(ctx, "invalid object path or interface");
    std::string key = service_key(path, iface);
    if (dbusState->exports.count(key)) return JS_ThrowTypeError(ctx, "object interface already exported");
    dbusState->exports.emplace(key, JS_DupValue(ctx, argv[2]));
    return JS_UNDEFINED;
}

static JSValue js_unexportObject(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc != 2) return JS_ThrowTypeError(ctx, "unexportObject expects (path, interface)");
    std::string key = service_key(js_to_string(ctx, argv[0]), js_to_string(ctx, argv[1]));
    auto found = dbusState->exports.find(key);
    if (found == dbusState->exports.end()) return JS_UNDEFINED;
    JS_FreeValue(ctx, found->second);
    dbusState->exports.erase(found);
    for (auto it = dbusState->invocations.begin(); it != dbusState->invocations.end();) {
        DBusMessage* request = it->second;
        const char* iface = dbus_message_get_interface(request);
        if (service_key(dbus_message_get_path(request), iface ? iface : "") == key) {
            DBusMessage* reply = dbus_message_new_error(request, DBUS_ERROR_UNKNOWN_OBJECT, "Object interface unexported");
            if (reply) send_service_reply(dbusState, request, reply);
            dbus_message_unref(request);
            it = dbusState->invocations.erase(it);
        } else ++it;
    }
    return JS_UNDEFINED;
}

static JSValue js_emitSignal(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_conn) return JS_ThrowInternalError(ctx, "bus is not connected; await connect() first");
    if (argc < 4) return JS_ThrowTypeError(ctx, "emitSignal expects (path, interface, member, signature, ...args)");
    std::string path = js_to_string(ctx, argv[0]), iface = js_to_string(ctx, argv[1]), member = js_to_string(ctx, argv[2]);
    if (!dbus_validate_path(path.c_str(), nullptr) || !dbus_validate_interface(iface.c_str(), nullptr) || !dbus_validate_member(member.c_str(), nullptr)) return JS_ThrowTypeError(ctx, "invalid signal address");
    DBusMessage* message = dbus_message_new_signal(path.c_str(), iface.c_str(), member.c_str());
    if (!message) return JS_ThrowOutOfMemory(ctx);
    DBusMessageIter out; dbus_message_iter_init_append(message, &out);
    if (!append_values(&out, js_to_string(ctx, argv[3]), ctx, argc - 4, argv + 4)) {
        dbus_message_unref(message);
        return JS_ThrowTypeError(ctx, "signal arguments do not match signature");
    }
    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_busIoMutex);
        ok = dbus_connection_send(g_conn, message, nullptr);
        dbus_connection_flush(g_conn);
    }
    dbus_message_unref(message);
    return ok ? JS_UNDEFINED : JS_ThrowOutOfMemory(ctx);
}

// Directed signals use the same connection that owns the exported service.
static JSValue js_emitSignalTo(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!g_conn) return JS_ThrowInternalError(ctx, "bus is not connected");
    if (argc < 5) return JS_ThrowTypeError(ctx, "emitSignalTo expects (destination, path, interface, member, signature, ...args)");
    std::string destination = js_to_string(ctx, argv[0]);
    std::string path = js_to_string(ctx, argv[1]), iface = js_to_string(ctx, argv[2]), member = js_to_string(ctx, argv[3]);
    if (!dbus_validate_bus_name(destination.c_str(), nullptr) ||
        !dbus_validate_path(path.c_str(), nullptr) || !dbus_validate_interface(iface.c_str(), nullptr) || !dbus_validate_member(member.c_str(), nullptr))
        return JS_ThrowTypeError(ctx, "invalid directed signal address");
    DBusMessage* message = dbus_message_new_signal(path.c_str(), iface.c_str(), member.c_str());
    if (!message) return JS_ThrowOutOfMemory(ctx);
    if (!dbus_message_set_destination(message, destination.c_str())) {
        dbus_message_unref(message);
        return JS_ThrowOutOfMemory(ctx);
    }
    DBusMessageIter out; dbus_message_iter_init_append(message, &out);
    if (!append_values(&out, js_to_string(ctx, argv[4]), ctx, argc - 5, argv + 5)) {
        dbus_message_unref(message);
        return JS_ThrowTypeError(ctx, "signal arguments do not match signature");
    }
    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_busIoMutex);
        ok = dbus_connection_send(g_conn, message, nullptr);
        dbus_connection_flush(g_conn);
    }
    dbus_message_unref(message);
    return ok ? JS_UNDEFINED : JS_ThrowOutOfMemory(ctx);
}

static void service_update() {
    std::queue<DBusMessage*> incoming;
    {
        std::lock_guard<std::mutex> lock(dbusState->methodMutex);
        incoming.swap(dbusState->methods);
    }
    while (!incoming.empty()) {
        DBusMessage* request = incoming.front(); incoming.pop();
        const char* iface = dbus_message_get_interface(request);
        std::string path = dbus_message_get_path(request);
        auto found = dbusState->exports.find(service_key(path, iface ? iface : ""));
        if (found == dbusState->exports.end()) {
            bool knownPath = false;
            for (const auto& entry : dbusState->exports) if (entry.first.compare(0, path.size() + 1, path + '\n') == 0) knownPath = true;
            DBusMessage* reply = dbus_message_new_error(request, knownPath ? DBUS_ERROR_UNKNOWN_METHOD : DBUS_ERROR_UNKNOWN_OBJECT, "No exported handler for object interface");
            if (reply) send_service_reply(dbusState, request, reply);
            dbus_message_unref(request);
            continue;
        }
        uint32_t id = dbusState->nextInvocation++;
        dbusState->invocations.emplace(id, request);
        SignalItem decoded = decode_signal(request);
        JSValue call = JS_NewObject(g_ctx);
        JS_SetPropertyStr(g_ctx, call, "sender", JS_NewString(g_ctx, decoded.sender.c_str()));
        JS_SetPropertyStr(g_ctx, call, "path", JS_NewString(g_ctx, decoded.path.c_str()));
        JS_SetPropertyStr(g_ctx, call, "interface", JS_NewString(g_ctx, decoded.iface.c_str()));
        JS_SetPropertyStr(g_ctx, call, "member", JS_NewString(g_ctx, decoded.member.c_str()));
        JS_SetPropertyStr(g_ctx, call, "signature", JS_NewString(g_ctx, decoded.signature.c_str()));
        JSValue args = JS_ParseJSON(g_ctx, decoded.argsJson.data(), decoded.argsJson.size(), "<dbus-method>");
        if (JS_IsException(args)) {
            JS_FreeValue(g_ctx, JS_GetException(g_ctx));
            args = JS_NewArray(g_ctx);
        }
        JS_SetPropertyStr(g_ctx, call, "args", args);
        JSValue data[] = {JS_NewBigInt64(g_ctx, static_cast<int64_t>(reinterpret_cast<intptr_t>(dbusState))), JS_NewUint32(g_ctx, id)};
        JS_SetPropertyStr(g_ctx, call, "reply", JS_NewCFunctionData(g_ctx, invocation_finish, 1, 0, 2, data));
        JS_SetPropertyStr(g_ctx, call, "error", JS_NewCFunctionData(g_ctx, invocation_finish, 2, 1, 2, data));
        for (auto value : data) JS_FreeValue(g_ctx, value);
        // The handler may unregister itself while being called.
        JSValue handler = JS_DupValue(g_ctx, found->second);
        JSValue result = JS_Call(g_ctx, handler, JS_UNDEFINED, 1, &call);
        JS_FreeValue(g_ctx, handler);
        if (JS_IsException(result)) {
            JSValue exception = JS_GetException(g_ctx);
            std::string text = js_to_string(g_ctx, exception);
            if (g_debug) fprintf(stderr, "dbus method handler exception: %s\n", text.c_str());
            JS_FreeValue(g_ctx, exception);
            auto pending = dbusState->invocations.find(id);
            if (pending != dbusState->invocations.end()) {
                DBusMessage* reply = dbus_message_new_error(request, DBUS_ERROR_FAILED, text.c_str());
                if (reply) send_service_reply(dbusState, request, reply);
                dbus_message_unref(request);
                dbusState->invocations.erase(pending);
            }
        }
        JS_FreeValue(g_ctx, result); JS_FreeValue(g_ctx, call);
    }
}
