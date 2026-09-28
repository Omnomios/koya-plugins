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
   - Use the update hook to drain native results, then enqueue Promise settlement
     onto the owning scripting thread.
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
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "../../sdk/quickjs/quickjs.h"
#include <cstring>
#include <cstdio>

// Include the hook system interface
#include "../../sdk/plugin_support.hpp"

struct RequestPromiseContext;

struct HttpContext
{
    void* rendererContext = nullptr;
    int (*enqueueScriptThreadTask)(void*, void (*)(void*), void*) = nullptr;
    std::mutex mutex;
    bool active = true;
    std::uint64_t nextRequestId = 1;
    std::unordered_map<std::uint64_t, std::shared_ptr<RequestPromiseContext>> pending;
    Http http;
    std::atomic<std::uint64_t> multipartSequence{1};
};

static thread_local std::shared_ptr<HttpContext> thread_http_context;

#define http_instance (thread_http_context->http)
#define multipart_sequence (thread_http_context->multipartSequence)

struct HttpPlugin : HelixPluginSupport
{
    std::shared_ptr<HttpContext> context;
};

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
struct RequestPromiseContext
{
    std::weak_ptr<HttpContext> owner;
    std::uint64_t id = 0;
    JSContext* ctx = nullptr;
    JSValue resolve = JS_UNDEFINED;
    JSValue reject = JS_UNDEFINED;
    bool released = false;
};

struct RequestCompletion
{
    Http::Job job;
    std::shared_ptr<RequestPromiseContext> promise;
};

static void release_promise (RequestPromiseContext& promise)
{
    if(promise.released) return;
    JS_FreeValue(promise.ctx, promise.resolve);
    JS_FreeValue(promise.ctx, promise.reject);
    promise.resolve = JS_UNDEFINED;
    promise.reject = JS_UNDEFINED;
    promise.released = true;
}

static void settle_request (void* data)
{
    std::unique_ptr<RequestCompletion> completion(static_cast<RequestCompletion*>(data));
    auto promise = completion->promise;
    auto owner = promise->owner.lock();
    if(!owner) return;

    {
        std::lock_guard<std::mutex> lock(owner->mutex);
        if(!owner->active || promise->released) return;
        owner->pending.erase(promise->id);
    }

    JSContext* ctx = promise->ctx;
    const Http::Job& job = completion->job;

    if(job.succeed)
    {
        JSValue result = JS_NewObject(ctx);

        // status
        JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, job.status));

        // headers
        JSValue headers = JS_NewObject(ctx);
        for (const auto& h : job.headers) {
            JS_SetPropertyStr(ctx, headers, h.first.c_str(), JS_NewString(ctx, h.second.c_str()));
        }
        JS_SetPropertyStr(ctx, result, "headers", headers);

        // body (text or binary based on job.binary)
        if (job.binary) {
            // Return binary data as ArrayBuffer
            if (!job.bodyBinary.empty()) {
                JSValue arrayBuffer = JS_NewArrayBufferCopy(ctx, job.bodyBinary.data(), job.bodyBinary.size());
                JS_SetPropertyStr(ctx, result, "body", arrayBuffer);
            } else {
                JS_SetPropertyStr(ctx, result, "body", JS_NULL);
            }
        } else {
            // Return text data as string
            JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, job.body.c_str()));
        }

        JSValue ret = JS_Call(ctx, promise->resolve, JS_UNDEFINED, 1, &result);
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, ret);
    }
    else
    {
        std::string errorMsg = job.errorMessage.empty() ? "HTTP request failed" : job.errorMessage;
        JS_ThrowInternalError(ctx, "%s", errorMsg.c_str());
        JSValue error = JS_GetException(ctx);
        JSValue ret = JS_Call(ctx, promise->reject, JS_UNDEFINED, 1, &error);
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, ret);
    }
    release_promise(*promise);
}

// Runs from the native update hook. It may move C++ data and queue work, but it
// must never touch the QuickJS context owned by the scripting thread.
static void request_callback (Http::Job job, const std::shared_ptr<RequestPromiseContext>& promise)
{
    auto owner = promise->owner.lock();
    if(!owner) return;

    auto completion = std::make_unique<RequestCompletion>();
    completion->job = std::move(job);
    completion->promise = promise;

    // Helix may run a script-thread task immediately when this callback is
    // already executing on the owning script thread. Do not call the host
    // scheduler while holding the context mutex: settle_request() also needs
    // it, so an inline dispatch would otherwise self-deadlock.
    {
        std::lock_guard<std::mutex> lock(owner->mutex);
        if(!owner->active || promise->released) return;
    }
    if(!owner->enqueueScriptThreadTask(owner->rendererContext, &settle_request, completion.get())) return;
    completion.release();
}

static void cleanup_http_context (void*)
{
    auto owner = thread_http_context;
    if(!owner) return;

    std::vector<std::shared_ptr<RequestPromiseContext>> pending;
    {
        std::lock_guard<std::mutex> lock(owner->mutex);
        owner->active = false;
        pending.reserve(owner->pending.size());
        for(auto& [id, promise] : owner->pending)
        {
            (void)id;
            pending.push_back(std::move(promise));
        }
        owner->pending.clear();
    }
    for(auto& promise : pending) release_promise(*promise);
    thread_http_context.reset();
}

// Helper to convert JS object to httplib::Headers
static httplib::Headers jsvalue_to_headers(JSContext* ctx, JSValueConst headers_val)
{
    httplib::Headers headers;
    
    if (JS_IsObject(headers_val)) {
        JSPropertyEnum* props = nullptr;
        uint32_t prop_count = 0;
        
        if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, headers_val, JS_GPN_STRING_MASK) == 0) {
            for (uint32_t i = 0; i < prop_count; i++) {
                JSValue key_val = JS_AtomToValue(ctx, props[i].atom);
                JSValue value_val = JS_GetProperty(ctx, headers_val, props[i].atom);
                
                std::string key = jsvalue_to_string(ctx, key_val);
                std::string value = jsvalue_to_string(ctx, value_val);
                
                headers.insert({key, value});
                
                JS_FreeValue(ctx, key_val);
                JS_FreeValue(ctx, value_val);
            }
            js_free(ctx, props);
        }
    }
    
    return headers;
}

static bool jsvalue_to_body (JSContext* ctx, JSValueConst value, std::string& body)
{
    size_t buffer_size = 0;
    uint8_t* buffer = JS_GetArrayBuffer(ctx, &buffer_size, value);
    if(buffer)
    {
        body.assign(reinterpret_cast<const char*>(buffer), buffer_size);
        return true;
    }

    if(JS_IsObject(value))
    {
        JSValue buffer_value = JS_GetPropertyStr(ctx, value, "buffer");
        size_t underlying_size = 0;
        uint8_t* underlying = JS_GetArrayBuffer(ctx, &underlying_size, buffer_value);
        if(underlying)
        {
            JSValue offset_value = JS_GetPropertyStr(ctx, value, "byteOffset");
            JSValue length_value = JS_GetPropertyStr(ctx, value, "byteLength");
            int64_t offset = 0;
            int64_t length = 0;
            const bool valid = JS_ToInt64(ctx, &offset, offset_value) == 0
                && JS_ToInt64(ctx, &length, length_value) == 0
                && offset >= 0
                && length >= 0
                && static_cast<size_t>(offset) <= underlying_size
                && static_cast<size_t>(length) <= underlying_size - static_cast<size_t>(offset);
            JS_FreeValue(ctx, offset_value);
            JS_FreeValue(ctx, length_value);
            JS_FreeValue(ctx, buffer_value);
            if(!valid) return false;
            body.assign(reinterpret_cast<const char*>(underlying + offset), static_cast<size_t>(length));
            return true;
        }
        JS_FreeValue(ctx, buffer_value);
    }

    if(JS_IsUndefined(value) || JS_IsNull(value)) return true;
    size_t text_size = 0;
    const char* text = JS_ToCStringLen(ctx, &text_size, value);
    if(!text) return false;
    body.assign(text, text_size);
    JS_FreeCString(ctx, text);
    return true;
}

static bool jsvalue_to_multipart (JSContext* ctx, JSValueConst value, std::vector<Http::MultipartPart>& multipart)
{
    if(JS_IsUndefined(value) || JS_IsNull(value)) return true;
    if(!JS_IsArray(ctx, value)) return false;

    JSValue length_value = JS_GetPropertyStr(ctx, value, "length");
    uint32_t length = 0;
    const bool valid_length = JS_ToUint32(ctx, &length, length_value) == 0;
    JS_FreeValue(ctx, length_value);
    if(!valid_length) return false;

    multipart.reserve(length);
    for(uint32_t index = 0; index < length; ++index)
    {
        JSValue part_value = JS_GetPropertyUint32(ctx, value, index);
        if(!JS_IsObject(part_value))
        {
            JS_FreeValue(ctx, part_value);
            return false;
        }

        JSValue name_value = JS_GetPropertyStr(ctx, part_value, "name");
        JSValue filename_value = JS_GetPropertyStr(ctx, part_value, "filename");
        JSValue content_type_value = JS_GetPropertyStr(ctx, part_value, "contentType");
        JSValue body_value = JS_GetPropertyStr(ctx, part_value, "body");
        Http::MultipartPart part;
        part.name = jsvalue_to_string(ctx, name_value);
        if(!JS_IsUndefined(filename_value)) part.filename = jsvalue_to_string(ctx, filename_value);
        if(!JS_IsUndefined(content_type_value)) part.contentType = jsvalue_to_string(ctx, content_type_value);
        const bool valid_body = jsvalue_to_body(ctx, body_value, part.body);
        JS_FreeValue(ctx, name_value);
        JS_FreeValue(ctx, filename_value);
        JS_FreeValue(ctx, content_type_value);
        JS_FreeValue(ctx, body_value);
        JS_FreeValue(ctx, part_value);
        if(part.name.empty() || !valid_body) return false;
        multipart.push_back(std::move(part));
    }
    return true;
}

// JS: request({ url, method, headers, body, binary?, multipart? }) -> Promise<{ status, headers, body }>
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

    // Extract headers
    JSValue headers_val = JS_GetPropertyStr(ctx, argv[0], "headers");
    httplib::Headers headers = jsvalue_to_headers(ctx, headers_val);
    JS_FreeValue(ctx, headers_val);

    // Extract body
    JSValue body_val = JS_GetPropertyStr(ctx, argv[0], "body");
    std::string body;
    const bool valid_body = jsvalue_to_body(ctx, body_val, body);
    JS_FreeValue(ctx, body_val);
    if(!valid_body) {
        return JS_ThrowTypeError(ctx, "request: body must be text, an ArrayBuffer, or a typed array");
    }

    JSValue multipart_val = JS_GetPropertyStr(ctx, argv[0], "multipart");
    std::vector<Http::MultipartPart> multipart;
    const bool valid_multipart = jsvalue_to_multipart(ctx, multipart_val, multipart);
    JS_FreeValue(ctx, multipart_val);
    if(!valid_multipart) {
        return JS_ThrowTypeError(ctx, "request: multipart must be an array of named parts");
    }
    std::string multipart_boundary;
    if(!multipart.empty()) {
        multipart_boundary = "----SyncromeshBoundary" + std::to_string(multipart_sequence.fetch_add(1));
        headers.insert({"Content-Type", "multipart/form-data; boundary=" + multipart_boundary});
    }

    // Extract binary option
    JSValue binary_val = JS_GetPropertyStr(ctx, argv[0], "binary");
    bool binary = JS_ToBool(ctx, binary_val);
    JS_FreeValue(ctx, binary_val);

    // Create a Promise
    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    JSValue resolve = resolving_funcs[0];
    JSValue reject  = resolving_funcs[1];

    auto owner = thread_http_context;
    if(!owner)
    {
        JS_FreeValue(ctx, resolve);
        JS_FreeValue(ctx, reject);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "request: HTTP scripting context is unavailable");
    }
    auto promise_context = std::make_shared<RequestPromiseContext>();
    promise_context->owner = owner;
    promise_context->ctx = ctx;
    promise_context->resolve = resolve;
    promise_context->reject = reject;
    {
        std::lock_guard<std::mutex> lock(owner->mutex);
        promise_context->id = owner->nextRequestId++;
        owner->pending.emplace(promise_context->id, promise_context);
    }

    // Queue the request
    http_instance.request(http_method, url, [promise_context](Http::Job job) {
        request_callback(std::move(job), promise_context);
    }, headers, body, binary, multipart_boundary, std::move(multipart));

    return promise;
}

static JSValue js_fnv1a (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if(argc < 1) return JS_ThrowTypeError(ctx, "fnv1a expects an ArrayBuffer or typed array");

    size_t buffer_size = 0;
    uint8_t* buffer = JS_GetArrayBuffer(ctx, &buffer_size, argv[0]);
    JSValue buffer_value = JS_UNDEFINED;
    if(!buffer && JS_IsObject(argv[0]))
    {
        buffer_value = JS_GetPropertyStr(ctx, argv[0], "buffer");
        size_t underlying_size = 0;
        uint8_t* underlying = JS_GetArrayBuffer(ctx, &underlying_size, buffer_value);
        if(underlying)
        {
            JSValue offset_value = JS_GetPropertyStr(ctx, argv[0], "byteOffset");
            JSValue length_value = JS_GetPropertyStr(ctx, argv[0], "byteLength");
            int64_t offset = 0;
            int64_t length = 0;
            const bool valid = JS_ToInt64(ctx, &offset, offset_value) == 0
                && JS_ToInt64(ctx, &length, length_value) == 0
                && offset >= 0
                && length >= 0
                && static_cast<size_t>(offset) <= underlying_size
                && static_cast<size_t>(length) <= underlying_size - static_cast<size_t>(offset);
            JS_FreeValue(ctx, offset_value);
            JS_FreeValue(ctx, length_value);
            if(valid)
            {
                buffer = underlying + offset;
                buffer_size = static_cast<size_t>(length);
            }
        }
    }
    if(!buffer)
    {
        JS_FreeValue(ctx, buffer_value);
        return JS_ThrowTypeError(ctx, "fnv1a expects an ArrayBuffer or typed array");
    }

    std::uint32_t checksum = 0x811c9dc5;
    for(size_t index = 0; index < buffer_size; ++index)
    {
        checksum ^= buffer[index];
        checksum *= 0x01000193;
    }
    JS_FreeValue(ctx, buffer_value);
    char result[9];
    std::snprintf(result, sizeof(result), "%08x", checksum);
    return JS_NewString(ctx, result);
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
    JS_SetModuleExport(ctx, m, "fnv1a", JS_NewCFunction(ctx, js_fnv1a, "fnv1a", 1));
    JS_SetModuleExport(ctx, m, "drain", JS_NewCFunction(ctx, js_drain, "drain", 0));
    return 0;
}

// Update hook callback for the HTTP module (handles drain)
void http_update_callback(void* data)
{
    http_instance.drain();
}

extern "C" {
HELIX_PLUGIN_EXPORT int helix_plugin_integrate (JSContext* ctx, const char* module_name, const HelixPluginHost* host, HelixPluginInstance** out)
{
    if(!out) return 1;
    auto* owner = new HttpPlugin();
    if(!owner->initialize(host) || !owner->services.enqueue_script_thread_task)
    {
        delete owner;
        JS_ThrowInternalError(ctx, "HTTP module requires script-thread task dispatch");
        return 1;
    }
    owner->context = std::make_shared<HttpContext>();
    thread_http_context = owner->context;
    thread_http_context->rendererContext = owner->services.ctx;
    thread_http_context->enqueueScriptThreadTask = owner->services.enqueue_script_thread_task;

    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_http_init);
    if(!m)
    {
        printf("Failed to create module for %s\n", module_name);
        thread_http_context.reset();
        owner->context.reset();
        delete owner;
        return 1;
    }
    JS_AddModuleExport(ctx, m, "request");
    JS_AddModuleExport(ctx, m, "fnv1a");
    JS_AddModuleExport(ctx, m, "drain");
    owner->instance.abiVersion = HELIX_PLUGIN_ABI_VERSION;
    owner->instance.module = m;
    owner->instance.state = owner;
    owner->instance.scriptUpdate = [](void* state, const HelixPluginUpdate*) {
        auto* plugin = static_cast<HttpPlugin*>(state);
        thread_http_context = plugin->context;
        http_update_callback(nullptr);
    };
    owner->instance.scriptShutdown = [](void* state) {
        auto* plugin = static_cast<HttpPlugin*>(state);
        thread_http_context = plugin->context;
        cleanup_http_context(nullptr);
        plugin->context.reset();
    };
    owner->instance.destroy = [](void* state) { delete static_cast<HttpPlugin*>(state); };
    *out = &owner->instance;
    return 0;
}
}
