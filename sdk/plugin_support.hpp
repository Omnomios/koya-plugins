#pragma once

#include "helix/plugin.h"

struct PluginServices
{
    void* ctx = nullptr;
    int (*create_texture_rgba)(void*, unsigned int, const char*, int, int, int, int) = nullptr;
    int (*update_texture_rgba)(void*, unsigned int, const char*, const unsigned char*, int, int, int, int, int) = nullptr;
    int (*destroy_texture)(void*, unsigned int, const char*) = nullptr;
    int (*asset_resolve_to_tempfile)(void*, const char*, char*, size_t) = nullptr;
    int (*asset_read_all)(void*, const char*, const unsigned char**, size_t*) = nullptr;
    void (*asset_free_buffer)(void*, const unsigned char*) = nullptr;
    int (*request_window_render)(void*, unsigned int) = nullptr;
    int (*set_renderable_shader_paths)(void*, unsigned int, unsigned int, const char*, const char*, int) = nullptr;
    int (*set_ui_shader_paths)(void*, unsigned int, int, const char*, const char*, int) = nullptr;
    int (*enqueue_script_thread_task)(void*, HelixPluginTask, void*) = nullptr;
};

struct HelixPluginSupport
{
    HelixPluginInstance instance{};
    PluginServices services{};
    const HelixScriptCapabilityV1* script = nullptr;
    const HelixAssetsCapabilityV1* assets = nullptr;
    const HelixRenderCapabilityV1* render = nullptr;

    bool initialize (const HelixPluginHost* host)
    {
        if(!host || host->abiVersion != HELIX_PLUGIN_ABI_VERSION || !host->findCapability) return false;
        script = static_cast<const HelixScriptCapabilityV1*>(host->findCapability(host->context, HELIX_PLUGIN_CAPABILITY_SCRIPT, 1));
        assets = static_cast<const HelixAssetsCapabilityV1*>(host->findCapability(host->context, HELIX_PLUGIN_CAPABILITY_ASSETS, 1));
        render = static_cast<const HelixRenderCapabilityV1*>(host->findCapability(host->context, HELIX_PLUGIN_CAPABILITY_RENDER, 1));
        services.ctx = this;
        services.create_texture_rgba = &createTexture;
        services.update_texture_rgba = &updateTexture;
        services.destroy_texture = &destroyTexture;
        services.asset_resolve_to_tempfile = &resolveTemporaryFile;
        services.asset_read_all = &readAll;
        services.asset_free_buffer = &releaseBuffer;
        services.request_window_render = &requestRender;
        services.set_renderable_shader_paths = &setRenderableShaders;
        services.set_ui_shader_paths = &setUiShaders;
        services.enqueue_script_thread_task = &schedule;
        return script != nullptr;
    }

    static HelixPluginSupport* self (void* context) { return static_cast<HelixPluginSupport*>(context); }
    static int schedule (void* context, HelixPluginTask task, void* data)
    {
        auto* owner = self(context);
        return owner && owner->script ? owner->script->schedule(owner->script->context, task, data) : 0;
    }
    static int resolveTemporaryFile (void* context, const char* path, char* output, size_t capacity)
    {
        auto* owner = self(context);
        return owner && owner->assets ? owner->assets->resolveTemporaryFile(owner->assets->context, path, output, capacity) : 0;
    }
    static int readAll (void* context, const char* path, const unsigned char** output, size_t* size)
    {
        auto* owner = self(context);
        return owner && owner->assets ? owner->assets->readAll(owner->assets->context, path, output, size) : 0;
    }
    static void releaseBuffer (void* context, const unsigned char* data)
    {
        auto* owner = self(context);
        if(owner && owner->assets) owner->assets->releaseBuffer(owner->assets->context, data);
    }
    static int createTexture (void* context, unsigned int window, const char* key, int width, int height, int mipmaps, int semantic)
    {
        auto* owner = self(context);
        return owner && owner->render ? owner->render->createTextureRgba(owner->render->context, window, key, width, height, mipmaps, semantic) : 0;
    }
    static int updateTexture (void* context, unsigned int window, const char* key, const unsigned char* data, int stride, int width, int height, int mipmaps,
                              int semantic)
    {
        auto* owner = self(context);
        return owner && owner->render ? owner->render->updateTextureRgba(owner->render->context, window, key, data, stride, width, height, mipmaps, semantic) : 0;
    }
    static int destroyTexture (void* context, unsigned int window, const char* key)
    {
        auto* owner = self(context);
        return owner && owner->render ? owner->render->destroyTexture(owner->render->context, window, key) : 0;
    }
    static int requestRender (void* context, unsigned int window)
    {
        auto* owner = self(context);
        return owner && owner->render ? owner->render->requestWindowRender(owner->render->context, window) : 0;
    }
    static int setRenderableShaders (void* context, unsigned int window, unsigned int handle, const char* vertex, const char* fragment, int blend)
    {
        auto* owner = self(context);
        return owner && owner->render && owner->render->setRenderableShaderPaths
            ? owner->render->setRenderableShaderPaths(owner->render->context, window, handle, vertex, fragment, blend) : 0;
    }
    static int setUiShaders (void* context, unsigned int window, int element, const char* vertex, const char* fragment, int blend)
    {
        auto* owner = self(context);
        return owner && owner->render && owner->render->setUiShaderPaths
            ? owner->render->setUiShaderPaths(owner->render->context, window, element, vertex, fragment, blend) : 0;
    }
};
