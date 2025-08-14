/*
   Koya Plugin: WebSocket

   What this is:
   - Application code that wraps IXWebSocket to provide a simple WebSocket API
     to Koya's JS runtime (QuickJS), with callbacks delivered on the engine
     thread via the `update` hook.

   Koya vs Application responsibilities:
   - Koya provides the JS engine and the hook wiring (`registerHook`).
   - The plugin (application) owns IXWebSocket instances, queues messages from
     network threads, and drains them during `update` to call JS handlers.

   Exposed JS API:
   - create({ url, onMessage?, onOpen?, onClose?, onError? }) -> { start(), send(), close(), stop(), destroy(), setAutoReconnect(bool) }
   - drain() (also executed every `update`)

   Reference notes for building your own plugin:
   - Keep external libs under vendor folders; do not mix with Koya SDK.
   - All JS values must be created/freed on the engine thread.
*/
// Syncromesh WebSocket QuickJS Module
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <queue>
#include <iostream>
#include <unordered_map>
#include <iomanip> // Required for std::hex and std::setw

#include "../IXWebSocket/ixwebsocket/IXWebSocket.h"
#include "../IXWebSocket/ixwebsocket/IXNetSystem.h"
#include "../IXWebSocket/ixwebsocket/IXWebSocketMessage.h"
#include "../IXWebSocket/ixwebsocket/IXWebSocketMessageType.h"
#include "../../sdk/quickjs/quickjs.h"

// Include the hook system interface
#include "../../module_hooks.h"

// WebSocket state
// JS callbacks tied to a WebSocket instance.
struct WsCallbacks
{
    JSContext* ctx;
    JSValue onMessage;
    JSValue onOpen;
    JSValue onClose;
    JSValue onError;
};

// Custom struct for safe message passing to JS
// Message container moved from network threads to the engine thread.
struct JsWebSocketMessage {
    ix::WebSocketMessageType type;
    std::string str;   // Used for message data or error reason
    bool binary;       // Only relevant for Message
};

// A single WebSocket: native socket, message queue, and callbacks.
struct WsInstance
{
    std::unique_ptr<ix::WebSocket> socket;
    std::queue<std::unique_ptr<JsWebSocketMessage>> messageQueue;
    WsCallbacks callbacks;
};

static std::mutex ws_mutex;
static std::unordered_map<uint32_t, std::unique_ptr<WsInstance>> ws_instances;
static std::atomic<uint32_t> ws_next_id{1};
static bool ws_net_initialized = false;

static std::string jsvalue_to_string ( JSContext* ctx, JSValueConst val )
{
    const char* cstr = JS_ToCString(ctx, val);
    std::string result = cstr ? cstr : "";
    if(cstr) JS_FreeCString(ctx, cstr);
    return result;
}

static void ws_free_callbacks ( WsCallbacks& cbs )
{
    if(cbs.ctx)
    {
        if(!JS_IsNull(cbs.onMessage)) JS_FreeValue(cbs.ctx, cbs.onMessage);
        if(!JS_IsNull(cbs.onOpen))    JS_FreeValue(cbs.ctx, cbs.onOpen);
        if(!JS_IsNull(cbs.onClose))   JS_FreeValue(cbs.ctx, cbs.onClose);
        if(!JS_IsNull(cbs.onError))   JS_FreeValue(cbs.ctx, cbs.onError);
    }
    cbs.onMessage = JS_NULL;
    cbs.onOpen = JS_NULL;
    cbs.onClose = JS_NULL;
    cbs.onError = JS_NULL;
}

// Helper to extract the id from JSValueConst* data
static uint32_t ws_id_from_data (JSContext* ctx, JSValueConst* data)
{
    int64_t id = 0;
    JS_ToInt64(ctx, &id, data[0]);
    return static_cast<uint32_t>(id);
}

// JS: start() — starts the connection.
static JSValue ws_start_func (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    uint32_t id = ws_id_from_data(ctx, (JSValueConst*)data);
    std::lock_guard<std::mutex> lock(ws_mutex);
    if(ws_instances.count(id)) ws_instances[id]->socket->start();
    return JS_UNDEFINED;
}

// JS: send(text) — sends a text frame.
static JSValue ws_send_func (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    uint32_t id = ws_id_from_data(ctx, (JSValueConst*)data);
    std::string msg = argc > 0 ? jsvalue_to_string(ctx, argv[0]) : "";
    std::lock_guard<std::mutex> lock(ws_mutex);
    if(ws_instances.count(id)) ws_instances[id]->socket->sendText(msg);
    return JS_UNDEFINED;
}

// JS: close() — closes the connection gracefully.
static JSValue ws_close_func (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    uint32_t id = ws_id_from_data(ctx, (JSValueConst*)data);
    std::lock_guard<std::mutex> lock(ws_mutex);
    if(ws_instances.count(id)) ws_instances[id]->socket->close();
    return JS_UNDEFINED;
}

// JS: stop() — stops any background activity.
static JSValue ws_stop_func (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    uint32_t id = ws_id_from_data(ctx, (JSValueConst*)data);
    std::lock_guard<std::mutex> lock(ws_mutex);
    if(ws_instances.count(id)) ws_instances[id]->socket->stop();
    return JS_UNDEFINED;
}

// JS: destroy() — closes and frees the native instance and callbacks.
static JSValue ws_destroy_func (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    uint32_t id = ws_id_from_data(ctx, (JSValueConst*)data);
    std::lock_guard<std::mutex> lock(ws_mutex);
    auto it = ws_instances.find(id);
    if(it != ws_instances.end())
    {
        it->second->socket->close();
        ws_free_callbacks(it->second->callbacks);
        ws_instances.erase(it);
    }
    return JS_UNDEFINED;
}

// JS: setAutoReconnect(bool)
static JSValue ws_set_auto_reconnect_func (JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValueConst* data)
{
    uint32_t id = ws_id_from_data(ctx, (JSValueConst*)data);
    int enable = 0;
    if(argc > 0) enable = JS_ToBool(ctx, argv[0]);
    std::lock_guard<std::mutex> lock(ws_mutex);
    if(ws_instances.count(id))
    {
        if(enable)
            ws_instances[id]->socket->enableAutomaticReconnection();
        else
            ws_instances[id]->socket->disableAutomaticReconnection();
    }
    return JS_UNDEFINED;
}

// JS: create({ url, onMessage?, onOpen?, onClose?, onError? }) -> object with methods.
static JSValue ws_create ( JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv )
{
    if(argc < 1 || !JS_IsObject(argv[0]))
    {
        return JS_ThrowTypeError(ctx, "create expects an options object");
    }

    if(!ws_net_initialized)
    {
        ix::initNetSystem();
        ws_net_initialized = true;
    }
    uint32_t id = ws_next_id++;
    auto ws = std::make_unique<WsInstance>();
    ws->socket = std::make_unique<ix::WebSocket>();
    ws->socket->disableAutomaticReconnection(); // Disable auto-reconnect by default
    ws->callbacks.ctx = ctx;
    ws->callbacks.onMessage = JS_GetPropertyStr(ctx, argv[0], "onMessage");
    ws->callbacks.onOpen    = JS_GetPropertyStr(ctx, argv[0], "onOpen");
    ws->callbacks.onClose   = JS_GetPropertyStr(ctx, argv[0], "onClose");
    ws->callbacks.onError   = JS_GetPropertyStr(ctx, argv[0], "onError");

    JSValue url_val = JS_GetPropertyStr(ctx, argv[0], "url");
    std::string url = jsvalue_to_string(ctx, url_val);
    JS_FreeValue(ctx, url_val);
    ws->socket->setUrl(url);

    // Message callback
    ws->socket->setOnMessageCallback([id](const ix::WebSocketMessagePtr& msg) {
        std::lock_guard<std::mutex> lock(ws_mutex);
        if(ws_instances.count(id))
        {
            auto jsmsg = std::make_unique<JsWebSocketMessage>();
            jsmsg->type = msg->type;
            jsmsg->str = msg->str;
            jsmsg->binary = msg->binary;
            if (msg->type == ix::WebSocketMessageType::Error) {
                jsmsg->str = msg->errorInfo.reason;
            }
            ws_instances[id]->messageQueue.push(std::move(jsmsg));
        }
    });
    {
        std::lock_guard<std::mutex> lock(ws_mutex);
        ws_instances[id] = std::move(ws);
    }
    // Return JS object with methods
    JSValue obj = JS_NewObject(ctx);
    JSValue id_val = JS_NewInt32(ctx, id);
    JS_SetPropertyStr(ctx, obj, "start", JS_NewCFunctionData(ctx, ws_start_func, 0, 0, 1, &id_val));
    JS_SetPropertyStr(ctx, obj, "send",  JS_NewCFunctionData(ctx, ws_send_func, 1, 0, 1, &id_val));
    JS_SetPropertyStr(ctx, obj, "close", JS_NewCFunctionData(ctx, ws_close_func, 0, 0, 1, &id_val));
    JS_SetPropertyStr(ctx, obj, "stop",  JS_NewCFunctionData(ctx, ws_stop_func, 0, 0, 1, &id_val));
    JS_SetPropertyStr(ctx, obj, "destroy", JS_NewCFunctionData(ctx, ws_destroy_func, 0, 0, 1, &id_val));
    JS_SetPropertyStr(ctx, obj, "setAutoReconnect", JS_NewCFunctionData(ctx, ws_set_auto_reconnect_func, 1, 0, 1, &id_val));
    JS_FreeValue(ctx, id_val); // After all uses
    return obj;
}

// JS: drain() — deliver queued messages to JS; also wired to `update`.
static JSValue ws_drain ( JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv )
{
    std::unordered_map<uint32_t, std::vector<std::unique_ptr<JsWebSocketMessage>>> local_queues;
    {
        std::lock_guard<std::mutex> lock(ws_mutex);
        for(auto& [id, ws] : ws_instances)
        {
            while(!ws->messageQueue.empty())
            {
                local_queues[id].push_back(std::move(ws->messageQueue.front()));
                ws->messageQueue.pop();
            }
        }
    }
    // Now process local_queues outside the lock
    for(auto& [id, messages] : local_queues)
    {
        auto it = ws_instances.find(id);
        if(it == ws_instances.end()) continue;
        auto& ws = it->second;
        for(auto& msg : messages)
        {
            switch (msg->type)
            {
                case ix::WebSocketMessageType::Message:
                    if(!JS_IsNull(ws->callbacks.onMessage))
                    {
                        JSValue arg;
                        if(msg->binary)
                        {
                            arg = JS_NewArrayBufferCopy(ctx, (const uint8_t*)msg->str.data(), msg->str.size());
                        }
                        else
                        {
                            arg = JS_NewString(ctx, msg->str.c_str());
                        }
                        JS_Call(ctx, ws->callbacks.onMessage, JS_UNDEFINED, 1, &arg);
                        JS_FreeValue(ctx, arg);
                    }
                    break;
                case ix::WebSocketMessageType::Open:
                    if(!JS_IsNull(ws->callbacks.onOpen))
                    {
                        JS_Call(ctx, ws->callbacks.onOpen, JS_UNDEFINED, 0, nullptr);
                    }
                    break;
                case ix::WebSocketMessageType::Close:
                    if(!JS_IsNull(ws->callbacks.onClose))
                    {
                        JS_Call(ctx, ws->callbacks.onClose, JS_UNDEFINED, 0, nullptr);
                    }
                    break;
                case ix::WebSocketMessageType::Error:
                    if(!JS_IsNull(ws->callbacks.onError))
                    {
                        JSValue arg = JS_NewString(ctx, msg->str.c_str());
                        JS_Call(ctx, ws->callbacks.onError, JS_UNDEFINED, 1, &arg);
                        JS_FreeValue(ctx, arg);
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return JS_UNDEFINED;
}

static int ws_module_init ( JSContext* ctx, JSModuleDef* m )
{
    JS_SetModuleExport(ctx, m, "create", JS_NewCFunction(ctx, ws_create, "create", 1));
    JS_SetModuleExport(ctx, m, "drain", JS_NewCFunction(ctx, ws_drain, "drain", 0));
    return 0;
}

// Global context for the WebSocket module
static JSContext* g_ws_ctx = nullptr;

// Update hook callback for the WebSocket module (handles drain)
// Hook: update — same behavior as ws_drain(), invoked automatically each tick.
void ws_update_callback(void* data)
{
    if (g_ws_ctx)
    {
        // Process the message queues (same logic as ws_drain)
        std::unordered_map<uint32_t, std::vector<std::unique_ptr<JsWebSocketMessage>>> local_queues;
        {
            std::lock_guard<std::mutex> lock(ws_mutex);
            for(auto& [id, ws] : ws_instances)
            {
                while(!ws->messageQueue.empty())
                {
                    local_queues[id].push_back(std::move(ws->messageQueue.front()));
                    ws->messageQueue.pop();
                }
            }
        }
        // Process the messages
        for(auto& [id, messages] : local_queues)
        {
            auto it = ws_instances.find(id);
            if(it == ws_instances.end()) continue;
            auto& ws = it->second;
            for(auto& msg : messages)
            {
                switch (msg->type)
                {
                    case ix::WebSocketMessageType::Message:
                        if(!JS_IsNull(ws->callbacks.onMessage))
                        {
                            JSValue arg;
                            if(msg->binary)
                            {
                                arg = JS_NewArrayBufferCopy(g_ws_ctx, (const uint8_t*)msg->str.data(), msg->str.size());
                            }
                            else
                            {
                                arg = JS_NewString(g_ws_ctx, msg->str.c_str());
                            }
                            JS_Call(g_ws_ctx, ws->callbacks.onMessage, JS_UNDEFINED, 1, &arg);
                            JS_FreeValue(g_ws_ctx, arg);
                        }
                        break;
                    case ix::WebSocketMessageType::Open:
                        if(!JS_IsNull(ws->callbacks.onOpen))
                        {
                            JS_Call(g_ws_ctx, ws->callbacks.onOpen, JS_UNDEFINED, 0, nullptr);
                        }
                        break;
                    case ix::WebSocketMessageType::Close:
                        if(!JS_IsNull(ws->callbacks.onClose))
                        {
                            JS_Call(g_ws_ctx, ws->callbacks.onClose, JS_UNDEFINED, 0, nullptr);
                        }
                        break;
                    case ix::WebSocketMessageType::Error:
                        if(!JS_IsNull(ws->callbacks.onError))
                        {
                            JSValue arg = JS_NewString(g_ws_ctx, msg->str.c_str());
                            JS_Call(g_ws_ctx, ws->callbacks.onError, JS_UNDEFINED, 1, &arg);
                            JS_FreeValue(g_ws_ctx, arg);
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }
}

extern "C"
{
// How to extend:
// - Export `integrate` and register `update` if you queue network messages.
// - Build a JS module and expose your functions; avoid calling JS off-thread.
JSModuleDef* integrate ( JSContext* ctx, const char* module_name, RegisterHookFunc registerHook )
{
    // Store the context for use in update callback
    g_ws_ctx = ctx;

    // Register our update hook callback with the engine
    registerHook("update", ws_update_callback);

    JSModuleDef* m = JS_NewCModule(ctx, module_name, ws_module_init);
    if(!m) return nullptr;
    JS_AddModuleExport(ctx, m, "create");
    JS_AddModuleExport(ctx, m, "drain");
    return m;
}
}