/*
   Koya SDK: Hook registration interface

   What this is:
   - A small C interface provided by Koya that your native plugin will use to
     register callbacks for engine lifecycle hooks like `update` and `cleanup`.

   How plugins use it (application side):
   - In your `integrate(JSContext*, const char*, RegisterHookFunc)` function,
     call `registerHook("update", yourUpdateCallback)` to deliver queued work
     onto the JS thread, and `registerHook("cleanup", yourCleanupCallback)` to
     release resources.

   This header is part of the Koya surface. Your plugin should treat it as an
   SDK file rather than application logic.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Function pointer type for hook callbacks
typedef void (*HookCallback)(void* data);

// Function pointer type for the registration function passed to integrate()
typedef void (*RegisterHookFunc)(const char* hookName, HookCallback callback);

// Include this in your native modules to get the hook system interface
// #include "ext/module_hooks.h"

// Example integrate function signature that native modules should implement:
// JSModuleDef* integrate(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook);
//
// Usage example:
// void myUpdateCallback(void* data) {
//     // Handle the update hook call
//     printf("Update hook triggered\n");
// }
//
// void myRenderCallback(void* data) {
//     // Handle the render hook call
//     printf("Render hook triggered\n");
// }
//
// JSModuleDef* integrate(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook) {
//     // Register your hooks
//     registerHook("update", myUpdateCallback);
//     registerHook("render", myRenderCallback);
//
//     // Return your module definition
//     // ...
// }

#ifdef __cplusplus
}
#endif