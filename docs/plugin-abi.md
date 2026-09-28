# Helix native plugin ABI

`sdk/helix/plugin.h` is the only public native plugin ABI header. It is C
compatible and compile-checked as both C and C++ by the standalone Meson build.

JavaScript imports use `Module/<name>`. Names must be a single component made
from ASCII letters, digits, `_`, or `-`. The host resolves that import to
`libhx-<name>.so` or `libhx-<name>.dll`, searching each `-n` directory first,
then the executable directory, then system library paths. There is no fallback
filename or entry point.

## Integration and capabilities

A library exports:

```c
HELIX_PLUGIN_EXPORT int helix_plugin_integrate(
    JSContext *context,
    const char *module_name,
    const HelixPluginHost *host,
    HelixPluginInstance **output);
```

Every call must create a distinct instance. The instance owns its module,
opaque state, and callback table. Integration is committed only when the
function succeeds and returns a version-matched instance with a module.

Runtime services are discovered with `host->findCapability`. The core names
are `helix.script`, `helix.assets`, and `helix.render`. Consumers may expose
additional namespaced capabilities without revising the core ABI. Capability
pointers are runtime-bound and must not be used after script shutdown begins.

## Threading and teardown

- `scriptUpdate` runs on the QuickJS owner thread and receives a copied timing payload.
- `engineSolve`, `engineRender`, and `renderBegin` run on the consumer's documented engine/render thread.
- Work submitted through `helix.script` runs on the QuickJS owner thread. It is revoked when shutdown starts.
- `scriptShutdown` runs before the QuickJS thread exits.
- `engineShutdown` runs after that thread joins while consumer resources still exist.
- After in-flight callbacks drain, `destroy` runs exactly once and the library handle closes.

Shutdown callbacks and explicit plugin shutdown functions must be idempotent.
Workers must be joinable and stopped before instance destruction.

## Minimal Meson plugin

```meson
project('example-helix-plugin', 'c', default_options: ['c_std=c11'])
shared_library(
  'hx-example',
  'plugin.c',
  name_prefix: 'lib',
  include_directories: include_directories('/path/to/helix-plugins/sdk'),
  c_args: ['-fvisibility=hidden'],
)
```

```c
#include <stdlib.h>
#include <helix/plugin.h>

struct Example { HelixPluginInstance instance; };

HELIX_PLUGIN_EXPORT int helix_plugin_integrate(
    JSContext *ctx, const char *name, const HelixPluginHost *host,
    HelixPluginInstance **out)
{
    struct Example *self = calloc(1, sizeof *self);
    if (!self || !out || host->abiVersion != HELIX_PLUGIN_ABI_VERSION) {
        free(self);
        return 1;
    }
    /* Create self->instance.module with the host's QuickJS API here. */
    self->instance.abiVersion = HELIX_PLUGIN_ABI_VERSION;
    self->instance.state = self;
    self->instance.destroy = free;
    *out = &self->instance;
    return self->instance.module ? 0 : 1;
}
```

The Helix source tree contains a complete example in
`helix-base/tests/native_plugin_fixture.cpp`, including a real QuickJS module
and lifecycle callbacks.
