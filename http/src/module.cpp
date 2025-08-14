/*
   Koya Plugin: HTTP

   What this is:
   - Application code that exposes an async HTTP client to Koya's JS runtime.

   Koya vs Application responsibilities:
   - Koya provides the QuickJS context and the hook system (`registerHook`).
   - The plugin (application) provides:
     - A worker (`Http` class) that does blocking I/O off-thread
     - A small JS API exported from `integrate(...)`:
       - request({ url, method }) -> Promise<{ status, headers, body }>
       - drain() to run queued completions (also wired to the `update` hook)

   Integration pattern:
   - Create a module with `JS_NewCModule` and expose C functions as exports.
   - Use `registerHook("update", ...)` to marshal results back onto the JS thread.
*/
#include "Http.hpp"
#include "httplib.h"
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>

#include "../../sdk/quickjs/quickjs.h"
#include <cstring>
#include <cstdio>

// Include the hook system interface
#include "../../module_hooks.h"

// Static Http instance for the module
static Http http_instance;

// Helper to convert QuickJS JSValue to std::string
// Convert a QuickJS value to std::string (utility for argument parsing).
static std::string jsvalue_to_string (JSContext* ctx, JSValueConst val)
{
    const char* cstr = JS_ToCString(ctx, val);
    std::string result = cstr ? cstr : "";
    if (cstr) JS_FreeCString(ctx, cstr);
    return result;
}

// Helper to convert Http::Method from string
// Parse HTTP method from string; defaults to GET.
static Http::Method method_from_string (const std::string& m)
{
    if (m == "get" || m == "GET") return Http::Method::Get;
    if (m == "post" || m == "POST") return Http::Method::Post;
    if (m == "put" || m == "PUT") return Http::Method::Put;
    if (m == "patch" || m == "PATCH") return Http::Method::Patch;
    if (m == "delete" || m == "DELETE") return Http::Method::Delete;
    return Http::Method::Get;
}

// Structure to hold the callback context for the async request
struct RequestPromiseContext {
    JSContext* ctx;
    JSValue resolve;
    JSValue reject;
};

// This will be called from the Http worker thread, so we must schedule the JS callback on the main thread.
// For now, we resolve the Promise immediately in the callback (assuming JS is single-threaded in this engine).
// Runs on engine thread (via `drain()`): settles the Promise with
// { status, headers, body } and frees captured JS values.
static void request_callback (Http::Job job, RequestPromiseContext* pctx)
{
    JSContext* ctx = pctx->ctx;
    JSValue result = JS_NewObject(ctx);

    // status
    JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, job.status));

    // headers
    JSValue headers = JS_NewObject(ctx);
    for (const auto& h : job.headers) {
        JS_SetPropertyStr(ctx, headers, h.first.c_str(), JS_NewString(ctx, h.second.c_str()));
    }
    JS_SetPropertyStr(ctx, result, "headers", headers);

    // body
    JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, job.body.c_str()));

    if (job.succeed) {
        JS_Call(ctx, pctx->resolve, JS_UNDEFINED, 1, &result);
    } else {
        JS_Call(ctx, pctx->reject, JS_UNDEFINED, 1, &result);
    }
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, pctx->resolve);
    JS_FreeValue(ctx, pctx->reject);
    delete pctx;
}

// JS: request({ url, method }) -> Promise<{ status, headers, body }>
static JSValue js_request (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if (argc < 1 || !JS_IsObject(argv[0])) {
        return JS_ThrowTypeError(ctx, "request expects an options object");
    }

    // Extract url and method
    JSValue url_val = JS_GetPropertyStr(ctx, argv[0], "url");
    std::string url = jsvalue_to_string(ctx, url_val);
    JS_FreeValue(ctx, url_val);
    if (url.empty()) {
        return JS_ThrowTypeError(ctx, "request: 'url' is required");
    }
    JSValue method_val = JS_GetPropertyStr(ctx, argv[0], "method");
    std::string method = jsvalue_to_string(ctx, method_val);
    JS_FreeValue(ctx, method_val);
    Http::Method http_method = method_from_string(method);

    // Create a Promise
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    JSValue resolve = resolving_funcs[0];
    JSValue reject  = resolving_funcs[1];

    // Allocate context for the callback
    auto* pctx = new RequestPromiseContext{ctx, resolve, reject};

    // Queue the request
    http_instance.request(http_method, url, [pctx](Http::Job job) {
        request_callback(job, pctx);
    });

    return promise;
}

// JS: drain() — manual pump; also wired to Koya's update hook.
static JSValue js_drain (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    http_instance.drain();
    return JS_UNDEFINED;
}

// Bind exports for the JS module.
static int js_http_init (JSContext* ctx, JSModuleDef* m)
{
    JS_SetModuleExport(ctx, m, "request", JS_NewCFunction(ctx, js_request, "request", 1));
    JS_SetModuleExport(ctx, m, "drain", JS_NewCFunction(ctx, js_drain, "drain", 0));
    return 0;
}

// Update hook callback for the HTTP module (handles drain)
void http_update_callback(void* data)
{
    http_instance.drain();
}

extern "C" {
// How to extend:
// - Export `integrate` from your shared library.
// - Register hooks you need (at least `update` if you queue work).
// - Build your JS module and add exports.
JSModuleDef* integrate (JSContext* ctx, const char* module_name, RegisterHookFunc registerHook)
{
    // Register our update hook callback with the engine
    registerHook("update", http_update_callback);

    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_http_init);
    if(!m)
    {
        printf("Failed to create module for %s\n", module_name);
        return nullptr;
    }
    JS_AddModuleExport(ctx, m, "request");
    JS_AddModuleExport(ctx, m, "drain");
    return m;
}
}