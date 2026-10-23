/*
   Koya Plugin: PAM Authentication

   Exposes a minimal JS API to authenticate a username/password against PAM.

   JS API:
   - authenticate({ service, user, password }) -> Promise<{ ok, pamCode, message? }>

   Notes:
   - PAM conversation runs on a background thread; results are delivered on the
     engine thread via the `update` hook.
   - This module does not keep any global PAM state between calls.
*/
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>

#include <security/pam_appl.h>

#include "../../sdk/quickjs/quickjs.h"
#include "../../sdk/plugin_support.hpp"

namespace {

struct AuthRequest {
    uint32_t id;
    std::string service;
    std::string user;
    std::string password;
};

struct AuthResult {
    uint32_t id;
    int pamCode;
    bool ok;
    std::string message;
};

struct PendingPromise {
    JSContext* ctx;
    JSValue resolve;
    JSValue reject;
};

struct PamState {
    std::atomic<uint32_t> nextId{1};
    std::mutex requestMutex;
    std::condition_variable requestCv;
    std::queue<AuthRequest> requests;
    std::mutex resultMutex;
    std::queue<AuthResult> results;
    std::mutex promiseMutex;
    std::unordered_map<uint32_t, PendingPromise> promises;
    std::atomic<bool> running{false};
    std::thread worker;
    JSContext* context = nullptr;
};

static thread_local PamState* pamState = nullptr;

#define g_nextId (pamState->nextId)
#define g_reqMutex (pamState->requestMutex)
#define g_reqCv (pamState->requestCv)
#define g_requests (pamState->requests)
#define g_resMutex (pamState->resultMutex)
#define g_results (pamState->results)
#define g_promMutex (pamState->promiseMutex)
#define g_promises (pamState->promises)
#define g_running (pamState->running)
#define g_worker (pamState->worker)
#define g_ctx (pamState->context)

struct PamPlugin : HelixPluginSupport {
    PamState runtime;
};

static void erase_password(std::string& password) {
    volatile char* bytes = password.data();
    for (size_t i = 0; i < password.size(); ++i) bytes[i] = 0;
    password.clear();
}

static int conv_func(int count, const pam_message** messages, pam_response** out, void* data)
{
    if (!out || !data || !messages || count < 1 || count > PAM_MAX_NUM_MSG) return PAM_CONV_ERR;
    auto& request = *static_cast<AuthRequest*>(data);
    auto* replies = static_cast<pam_response*>(calloc(count, sizeof(pam_response)));
    if (!replies) return PAM_BUF_ERR;
    const auto fail = [&](int code) {
        for (int i = 0; i < count; ++i) if (replies[i].resp) {
            volatile char* bytes = replies[i].resp;
            for (size_t j = 0, length = strlen(replies[i].resp); j < length; ++j) bytes[j] = 0;
            free(replies[i].resp);
        }
        free(replies);
        return code;
    };
    for (int i = 0; i < count; ++i) {
        if (!messages[i]) return fail(PAM_CONV_ERR);
        const char* value = nullptr;
        switch (messages[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF: value = request.password.c_str(); break;
            case PAM_PROMPT_ECHO_ON: value = request.user.c_str(); break;
            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO: break;
            default: return fail(PAM_CONV_ERR);
        }
        if (value && !(replies[i].resp = strdup(value))) return fail(PAM_BUF_ERR);
    }
    *out = replies;
    return PAM_SUCCESS;
}

static void worker_loop(PamState* state)
{
    pamState = state;
    while (g_running.load()) {
        AuthRequest req;
        {
            std::unique_lock<std::mutex> lk(g_reqMutex);
            g_reqCv.wait(lk, []{ return !g_running.load() || !g_requests.empty(); });
            if (!g_running.load()) break;
            req = std::move(g_requests.front());
            g_requests.pop();
        }

        // Perform PAM conversation
        pam_handle_t* pamh = nullptr;
        struct pam_conv conv { conv_func, &req };
        int rc = pam_start(req.service.c_str(), req.user.c_str(), &conv, &pamh);
        int finalCode = rc;
        std::string message;
        if (rc == PAM_SUCCESS) {
            rc = pam_authenticate(pamh, PAM_DISALLOW_NULL_AUTHTOK);
            finalCode = rc;
            if (rc == PAM_SUCCESS) {
                rc = pam_acct_mgmt(pamh, 0);
                finalCode = rc;
            }
        }
        if (pamh) {
            pam_end(pamh, finalCode);
        }
        erase_password(req.password);
        bool ok = (finalCode == PAM_SUCCESS);
        if (!ok) {
            const char* s = pam_strerror(nullptr, finalCode);
            if (s) message = s;
        }

        {
            std::lock_guard<std::mutex> lk(g_resMutex);
            g_results.push(AuthResult{req.id, finalCode, ok, std::move(message)});
        }
    }
    pamState = nullptr;
}

static std::string js_to_string(JSContext* ctx, JSValueConst v) {
    size_t length = 0;
    const char* c = JS_ToCStringLen(ctx, &length, v);
    std::string s = c ? std::string(c, length) : "";
    if (c) JS_FreeCString(ctx, c);
    return s;
}

static JSValue js_authenticate(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "authenticate expects an options object");
    }
    std::string service = "login";
    JSValue v = JS_GetPropertyStr(ctx, argv[0], "service");
    if (!JS_IsUndefined(v)) service = js_to_string(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "user");
    std::string user = js_to_string(ctx, v);
    JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, argv[0], "password");
    std::string password = js_to_string(ctx, v);
    JS_FreeValue(ctx, v);
    if (service.empty() || user.empty() ||
        service.find('\0') != std::string::npos || user.find('\0') != std::string::npos || password.find('\0') != std::string::npos) {
        erase_password(password);
        return JS_ThrowTypeError(ctx, "authenticate: invalid service, user or password");
    }
    if (g_promises.size() >= 16) {
        erase_password(password);
        return JS_ThrowInternalError(ctx, "authenticate: too many pending requests");
    }

    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    JSValue resolve = funcs[0];
    JSValue reject  = funcs[1];

    uint32_t id = g_nextId++;
    {
        std::lock_guard<std::mutex> lk(g_promMutex);
        g_promises.emplace(id, PendingPromise{ctx, resolve, reject});
    }

    {
        std::lock_guard<std::mutex> lk(g_reqMutex);
        g_requests.push(AuthRequest{id, std::move(service), std::move(user), std::move(password)});
    }
    g_reqCv.notify_one();

    return promise;
}

static int pam_module_init(JSContext* ctx, JSModuleDef* m)
{
    JS_SetModuleExport(ctx, m, "authenticate", JS_NewCFunction(ctx, js_authenticate, "authenticate", 1));
    return 0;
}

static void pam_update(void*)
{
    if (!g_ctx) return;
    std::vector<AuthResult> items;
    {
        std::lock_guard<std::mutex> lk(g_resMutex);
        while (!g_results.empty()) { items.push_back(std::move(g_results.front())); g_results.pop(); }
    }
    if (items.empty()) return;
    std::lock_guard<std::mutex> lk(g_promMutex);
    for (auto &r : items) {
        auto it = g_promises.find(r.id);
        if (it == g_promises.end()) continue;
        PendingPromise p = it->second;
        g_promises.erase(it);
        JSValue obj = JS_NewObject(g_ctx);
        JS_SetPropertyStr(g_ctx, obj, "ok", JS_NewBool(g_ctx, r.ok));
        JS_SetPropertyStr(g_ctx, obj, "pamCode", JS_NewInt32(g_ctx, r.pamCode));
        if (!r.message.empty()) {
            JS_SetPropertyStr(g_ctx, obj, "message", JS_NewString(g_ctx, r.message.c_str()));
        }
        JSValue ret = r.ok ? JS_Call(g_ctx, p.resolve, JS_UNDEFINED, 1, &obj)
                           : JS_Call(g_ctx, p.reject,  JS_UNDEFINED, 1, &obj);
        JS_FreeValue(g_ctx, ret);
        JS_FreeValue(g_ctx, obj);
        JS_FreeValue(g_ctx, p.resolve);
        JS_FreeValue(g_ctx, p.reject);
    }
}

static void pam_shutdown(PamState* state)
{
    pamState = state;
    g_running.store(false);
    g_reqCv.notify_all();
    if(g_worker.joinable()) g_worker.join();
    {
        std::lock_guard<std::mutex> lk(g_promMutex);
        for (auto &kv : g_promises) {
            JS_FreeValue(g_ctx, kv.second.resolve);
            JS_FreeValue(g_ctx, kv.second.reject);
        }
        g_promises.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_resMutex);
        while (!g_results.empty()) g_results.pop();
    }
    {
        std::lock_guard<std::mutex> lk(g_reqMutex);
        while (!g_requests.empty()) { erase_password(g_requests.front().password); g_requests.pop(); }
    }
    g_ctx = nullptr;
    pamState = nullptr;
}

} // namespace

extern "C" {
HELIX_PLUGIN_EXPORT int helix_plugin_integrate(JSContext* ctx, const char* module_name, const HelixPluginHost* host, HelixPluginInstance** out)
{
    if(!out) return 1;
    auto* owner = new PamPlugin();
    if(!owner->initialize(host)) { delete owner; return 1; }
    pamState = &owner->runtime;
    g_ctx = ctx;
    if (!g_running.exchange(true)) {
        g_worker = std::thread(worker_loop, &owner->runtime);
    }
    owner->instance.scriptUpdate = [](void* state, const HelixPluginUpdate*) {
        pamState = &static_cast<PamPlugin*>(state)->runtime;
        pam_update(nullptr);
    };
    owner->instance.scriptShutdown = [](void* state){
        pam_shutdown(&static_cast<PamPlugin*>(state)->runtime);
    };
    JSModuleDef* m = JS_NewCModule(ctx, module_name, pam_module_init);
    if (!m) { pam_shutdown(&owner->runtime); delete owner; return 1; }
    JS_AddModuleExport(ctx, m, "authenticate");
    owner->instance.abiVersion = HELIX_PLUGIN_ABI_VERSION;
    owner->instance.module = m;
    owner->instance.state = owner;
    owner->instance.destroy = [](void* state) { delete static_cast<PamPlugin*>(state); };
    *out = &owner->instance;
    return 0;
}
}
