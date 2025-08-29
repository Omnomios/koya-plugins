#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Function pointer type for hook callbacks
typedef void (*HookCallback)(void* data);

// Function pointer type for the registration function passed to integrate()
typedef void (*RegisterHookFunc)(const char* hookName, HookCallback callback);

typedef struct KoyaRendererV1 {
    void* ctx;  // Opaque engine context

    int (*create_texture_rgba)(void* ctx,
                               unsigned int windowId,
                               const char* key,
                               int width,
                               int height,
                               int mipmaps);

    int (*update_texture_rgba)(void* ctx,
                               unsigned int windowId,
                               const char* key,
                               const unsigned char* data,
                               int strideBytes,
                               int width,
                               int height,
                               int mipmaps);

    int (*destroy_texture)(void* ctx,
                           unsigned int windowId,
                           const char* key);

    int (*asset_resolve_to_tempfile)(void* ctx,
                                     const char* vpath,
                                     char* outPath,
                                     size_t outPathCap);
    int (*asset_read_all)(void* ctx,
                          const char* vpath,
                          const unsigned char** outData,
                          size_t* outSize);
    void (*asset_free_buffer)(void* ctx, const unsigned char* data);

    int (*request_window_render)(void* ctx, unsigned int windowId);
} KoyaRendererV1;

typedef struct KoyaRenderBeginDataV1 {
    unsigned int windowId;
    double engineTime;
    double integration;
} KoyaRenderBeginDataV1;

typedef struct KoyaUpdateDataV1 {
    double engineTime;
    double delta;
} KoyaUpdateDataV1;

#ifdef __cplusplus
}
#endif

