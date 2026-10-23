// Shared argument marshalling for calls, service replies, and signals.
static bool append_value(DBusMessageIter*, DBusSignatureIter*, JSContext*, JSValueConst);

static bool append_values(DBusMessageIter* out, const std::string& signature,
                          JSContext* ctx, int argc, JSValueConst* argv) {
    DBusError error; dbus_error_init(&error);
    if (!dbus_signature_validate(signature.c_str(), &error)) {
        dbus_error_free(&error);
        return false;
    }
    DBusSignatureIter sig; dbus_signature_iter_init(&sig, signature.c_str());
    int index = 0;
    while (dbus_signature_iter_get_current_type(&sig) != DBUS_TYPE_INVALID) {
        if (index >= argc || !append_value(out, &sig, ctx, argv[index++])) { if (g_debug) fprintf(stderr, "marshal failed sig=%s index=%d type=%c argc=%d\n", signature.c_str(), index, dbus_signature_iter_get_current_type(&sig), argc); return false; }
        dbus_signature_iter_next(&sig);
    }
    return index == argc;
}

static bool append_value(DBusMessageIter* out, DBusSignatureIter* sig,
                         JSContext* ctx, JSValueConst value) {
    int type = dbus_signature_iter_get_current_type(sig);
    if (type == DBUS_TYPE_VARIANT) {
        JSValue tag = JS_GetPropertyStr(ctx, value, "_t");
        if (!JS_IsString(tag)) { JS_FreeValue(ctx, tag); tag = JS_GetPropertyStr(ctx, value, "t"); }
        std::string inner;
        JSValue payload = JS_UNDEFINED;
        if (JS_IsString(tag)) {
            inner = js_to_string(ctx, tag);
            payload = JS_GetPropertyStr(ctx, value, "_v");
            if (JS_IsUndefined(payload)) { JS_FreeValue(ctx, payload); payload = JS_GetPropertyStr(ctx, value, "v"); }
        } else {
            payload = JS_DupValue(ctx, value);
            if (JS_IsString(value)) inner = "s";
            else if (JS_IsBool(value)) inner = "b";
            else if (JS_IsNumber(value)) inner = "i";
            else if (JS_IsArray(ctx, value)) {
                JSValue first = JS_GetPropertyUint32(ctx, value, 0);
                inner = JS_IsNumber(first) ? "ay" : "as";
                JS_FreeValue(ctx, first);
            }
        }
        JS_FreeValue(ctx, tag);
        bool ok = false;
        if (dbus_signature_validate_single(inner.c_str(), nullptr)) {
            DBusMessageIter sub;
            if (dbus_message_iter_open_container(out, type, inner.c_str(), &sub)) {
                ok = append_values(&sub, inner, ctx, 1, &payload);
                if (ok) ok = dbus_message_iter_close_container(out, &sub);
                else dbus_message_iter_abandon_container(out, &sub);
            }
        }
        JS_FreeValue(ctx, payload);
        return ok;
    }
    if (type == DBUS_TYPE_ARRAY || type == DBUS_TYPE_STRUCT) {
        DBusSignatureIter child; dbus_signature_iter_recurse(sig, &child);
        char* element = type == DBUS_TYPE_ARRAY ? dbus_signature_iter_get_signature(&child) : nullptr;
        DBusMessageIter sub;
        bool opened = dbus_message_iter_open_container(out, type, element, &sub);
        dbus_free(element);
        if (!opened) return false;
        bool ok = true;
        if (type == DBUS_TYPE_ARRAY && dbus_signature_iter_get_current_type(&child) == DBUS_TYPE_DICT_ENTRY) {
            if (!JS_IsObject(value) || JS_IsArray(ctx, value)) ok = false;
            JSPropertyEnum* keys = nullptr; uint32_t count = 0;
            if (ok && JS_GetOwnPropertyNames(ctx, &keys, &count, value, JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0) ok = false;
            for (uint32_t i = 0; i < count; ++i) {
                if (ok) {
                    DBusMessageIter entry;
                    ok = dbus_message_iter_open_container(&sub, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
                    if (ok) {
                        DBusSignatureIter fields; dbus_signature_iter_recurse(&child, &fields);
                        JSValue key = JS_AtomToString(ctx, keys[i].atom);
                        JSValue item = JS_GetProperty(ctx, value, keys[i].atom);
                        ok = append_value(&entry, &fields, ctx, key);
                        dbus_signature_iter_next(&fields);
                        if (ok) ok = append_value(&entry, &fields, ctx, item);
                        JS_FreeValue(ctx, key); JS_FreeValue(ctx, item);
                        if (ok) ok = dbus_message_iter_close_container(&sub, &entry);
                        else dbus_message_iter_abandon_container(&sub, &entry);
                    }
                }
                JS_FreeAtom(ctx, keys[i].atom);
            }
            js_free(ctx, keys);
        } else {
            if (!JS_IsArray(ctx, value)) ok = false;
            uint32_t count = 0;
            JSValue length = JS_GetPropertyStr(ctx, value, "length");
            if (JS_ToUint32(ctx, &count, length) < 0) ok = false;
            JS_FreeValue(ctx, length);
            for (uint32_t i = 0; ok && i < count; ++i) {
                if (dbus_signature_iter_get_current_type(&child) == DBUS_STRUCT_END_CHAR) { ok = false; break; }
                JSValue item = JS_GetPropertyUint32(ctx, value, i);
                ok = append_value(&sub, &child, ctx, item);
                JS_FreeValue(ctx, item);
                if (type == DBUS_TYPE_STRUCT) dbus_signature_iter_next(&child);
            }
            if (type == DBUS_TYPE_STRUCT && dbus_signature_iter_get_current_type(&child) != DBUS_STRUCT_END_CHAR) ok = false;
        }
        if (ok) return dbus_message_iter_close_container(out, &sub);
        dbus_message_iter_abandon_container(out, &sub);
        return false;
    }
    if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH || type == DBUS_TYPE_SIGNATURE) {
        if (!JS_IsString(value)) return false;
        std::string text = js_to_string(ctx, value);
        if (text.find('\0') != std::string::npos) return false;
        if (type == DBUS_TYPE_OBJECT_PATH && !dbus_validate_path(text.c_str(), nullptr)) return false;
        if (type == DBUS_TYPE_SIGNATURE && !dbus_signature_validate(text.c_str(), nullptr)) return false;
        const char* c = text.c_str();
        return dbus_message_iter_append_basic(out, type, &c);
    }
    if (type == DBUS_TYPE_BOOLEAN) {
        if (!JS_IsBool(value)) return false;
        dbus_bool_t boolean = JS_ToBool(ctx, value);
        return dbus_message_iter_append_basic(out, type, &boolean);
    }
    if (type == DBUS_TYPE_DOUBLE) {
        double number;
        if (!JS_IsNumber(value) || JS_ToFloat64(ctx, &number, value) < 0 || !std::isfinite(number)) return false;
        return dbus_message_iter_append_basic(out, type, &number);
    }
    if (type == DBUS_TYPE_INT64 || type == DBUS_TYPE_UINT64) {
        int64_t number;
        if (JS_IsBigInt(ctx, value)) {
            if (JS_ToBigInt64(ctx, &number, value) < 0) return false;
        } else {
            double d;
            if (!JS_IsNumber(value) || JS_ToFloat64(ctx, &d, value) < 0 || !std::isfinite(d) || std::trunc(d) != d || std::fabs(d) > 9007199254740991.0) return false;
            number = static_cast<int64_t>(d);
        }
        if (type == DBUS_TYPE_UINT64 && number < 0 && !JS_IsBigInt(ctx, value)) return false;
        return dbus_message_iter_append_basic(out, type, &number);
    }
    double number;
    if (!JS_IsNumber(value) || JS_ToFloat64(ctx, &number, value) < 0 || !std::isfinite(number) || std::trunc(number) != number) return false;
    switch (type) {
        case DBUS_TYPE_BYTE: { if (number < 0 || number > 255) return false; uint8_t n = number; return dbus_message_iter_append_basic(out, type, &n); }
        case DBUS_TYPE_INT16: { if (number < -32768 || number > 32767) return false; int16_t n = number; return dbus_message_iter_append_basic(out, type, &n); }
        case DBUS_TYPE_UINT16: { if (number < 0 || number > 65535) return false; uint16_t n = number; return dbus_message_iter_append_basic(out, type, &n); }
        case DBUS_TYPE_INT32: { if (number < -2147483648.0 || number > 2147483647.0) return false; int32_t n = number; return dbus_message_iter_append_basic(out, type, &n); }
        case DBUS_TYPE_UINT32: { if (number < 0 || number > 4294967295.0) return false; uint32_t n = number; return dbus_message_iter_append_basic(out, type, &n); }
        default: return false;
    }
}
