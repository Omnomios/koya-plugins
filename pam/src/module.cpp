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
#include "../../sdk/module_hooks.h"

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

static std::atomic<uint32_t> g_nextId{1};
static std::mutex g_reqMutex;
static std::condition_variable g_reqCv;
static std::queue<AuthRequest> g_requests;

static std::mutex g_resMutex;
static std::queue<AuthResult> g_results;

static std::mutex g_promMutex;
static std::unordered_map<uint32_t, PendingPromise> g_promises;

static std::atomic<bool> g_running{false};
static std::thread g_worker;
static JSContext* g_ctx = nullptr;

static int conv_func(int num_msg, const struct pam_message** msg, struct pam_response** resp, void* appdata_ptr)
{
    if (!resp || !appdata_ptr) return PAM_CONV_ERR;
    const char* password = static_cast<const char*>(appdata_ptr);
    struct pam_response* replies = static_cast<struct pam_response*>(calloc((size_t)num_msg, sizeof(struct pam_response)));
    if (!replies) return PAM_BUF_ERR;
    for (int i = 0; i < num_msg; ++i) {
        replies[i].resp_retcode = 0;
        replies[i].resp = nullptr;
        switch (msg[i]->msg_style) {
            case PAM_PROMPT_ECHO_OFF: {
                if (password) {
                    replies[i].resp = strdup(password);
                }
                break;
            }
            case PAM_PROMPT_ECHO_ON: {
                // Not expected; return empty
                replies[i].resp = strdup("");
                break;
            }
            case PAM_ERROR_MSG:
            case PAM_TEXT_INFO: {
                // No response needed
                break;
            }
            default: {
                free(replies);
                return PAM_CONV_ERR;
            }
        }
    }
    *resp = replies;
    return PAM_SUCCESS;
}

static void worker_loop()
{
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
        struct pam_conv conv { conv_func, (void*)req.password.c_str() };
        int rc = pam_start(req.service.c_str(), req.user.c_str(), &conv, &pamh);
        int finalCode = rc;
        std::string message;
        if (rc == PAM_SUCCESS) {
            rc = pam_authenticate(pamh, 0);
            finalCode = rc;
            if (rc == PAM_SUCCESS) {
                rc = pam_acct_mgmt(pamh, 0);
                finalCode = rc;
            }
        }
        if (pamh) {
            pam_end(pamh, finalCode);
        }
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
}

static std::string js_to_string(JSContext* ctx, JSValueConst v) {
    const char* c = JS_ToCString(ctx, v);
    std::string s = c ? c : "";
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
    if (user.empty()) return JS_ThrowTypeError(ctx, "authenticate: 'user' is required");

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

} // namespace

extern "C" {
JSModuleDef* integrateV1(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook, const KoyaRendererV1*)
{
    g_ctx = ctx;
    if (!g_running.exchange(true)) {
        g_worker = std::thread(worker_loop);
        g_worker.detach();
    }
    registerHook("update", pam_update);
    registerHook("cleanup", [](void*){
        g_running.store(false);
        g_reqCv.notify_all();
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
            while (!g_requests.empty()) g_requests.pop();
        }
        g_ctx = nullptr;
    });
    JSModuleDef* m = JS_NewCModule(ctx, module_name, pam_module_init);
    if (!m) return nullptr;
    JS_AddModuleExport(ctx, m, "authenticate");
    return m;
}
}


