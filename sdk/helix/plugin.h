#ifndef HELIX_PLUGIN_H
#define HELIX_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JSContext JSContext;
typedef struct JSModuleDef JSModuleDef;

#define HELIX_PLUGIN_ABI_VERSION 1u
#define HELIX_PLUGIN_CAPABILITY_SCRIPT "helix.script"
#define HELIX_PLUGIN_CAPABILITY_ASSETS "helix.assets"
#define HELIX_PLUGIN_CAPABILITY_RENDER "helix.render"

typedef struct HelixPluginUpdate {
    double engineTime;
    double delta;
} HelixPluginUpdate;

typedef struct HelixPluginRenderBegin {
    uint32_t windowId;
    double engineTime;
    double integration;
} HelixPluginRenderBegin;

typedef void (*HelixPluginTask)(void* data);

typedef struct HelixScriptCapabilityV1 {
    uint32_t version;
    void* context;
    int (*schedule)(void* context, HelixPluginTask task, void* data);
} HelixScriptCapabilityV1;

typedef struct HelixAssetsCapabilityV1 {
    uint32_t version;
    void* context;
    int (*resolveTemporaryFile)(void* context, const char* virtualPath, char* outputPath, size_t outputCapacity);
    int (*readAll)(void* context, const char* virtualPath, const unsigned char** outputData, size_t* outputSize);
    void (*releaseBuffer)(void* context, const unsigned char* data);
} HelixAssetsCapabilityV1;

enum {
    HELIX_RENDER_BLEND_PREMULTIPLIED_ALPHA = 0,
    HELIX_RENDER_BLEND_OPAQUE = 1,
    HELIX_RENDER_BLEND_ADDITIVE = 2,
    HELIX_RENDER_BLEND_ALPHA = 3
};

enum {
    HELIX_RENDER_TEXTURE_COLOR_SRGB_PREMUL = 0,
    HELIX_RENDER_TEXTURE_DATA_LINEAR_NO_PREMUL = 1
};

typedef struct HelixRenderCapabilityV1 {
    uint32_t version;
    void* context;
    int (*createTextureRgba)(void* context, uint32_t windowId, const char* key, int width, int height, int mipmaps, int textureSemantic);
    int (*updateTextureRgba)(void* context, uint32_t windowId, const char* key, const unsigned char* data, int strideBytes, int width, int height,
                             int mipmaps, int textureSemantic);
    int (*destroyTexture)(void* context, uint32_t windowId, const char* key);
    int (*requestWindowRender)(void* context, uint32_t windowId);
    int (*setRenderableShaderPaths)(void* context, uint32_t windowId, uint32_t renderableHandle, const char* vertexPath, const char* fragmentPath,
                                    int blendMode);
    int (*setUiShaderPaths)(void* context, uint32_t windowId, int elementId, const char* vertexPath, const char* fragmentPath, int blendMode);
} HelixRenderCapabilityV1;

typedef struct HelixPluginHost {
    uint32_t abiVersion;
    void* context;
    const void* (*findCapability)(void* context, const char* identifier, uint32_t minimumVersion);
} HelixPluginHost;

typedef struct HelixPluginInstance {
    uint32_t abiVersion;
    JSModuleDef* module;
    void* state;
    void (*scriptUpdate)(void* state, const HelixPluginUpdate* update);
    void (*engineSolve)(void* state);
    void (*engineRender)(void* state);
    void (*renderBegin)(void* state, const HelixPluginRenderBegin* render);
    void (*scriptShutdown)(void* state);
    void (*engineShutdown)(void* state);
    void (*destroy)(void* state);
} HelixPluginInstance;

/* Each integration returns a distinct instance. A non-zero result is failure. */
int helix_plugin_integrate(JSContext* context, const char* moduleName, const HelixPluginHost* host, HelixPluginInstance** outInstance);

#if defined(_WIN32)
#define HELIX_PLUGIN_EXPORT __declspec(dllexport)
#else
#define HELIX_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif

#endif
