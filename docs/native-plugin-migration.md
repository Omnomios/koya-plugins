# Migrating a Koya native plugin to Helix

JavaScript imports do not change: `Module/example` remains `Module/example`.
The native filename changes from `libsm-example` to `libhx-example`, and the
exported `integrateV1` function becomes `helix_plugin_integrate`.

The old `RegisterHookFunc` callback and named hooks map to fixed fields on a
`HelixPluginInstance`:

| Old hook | New instance callback |
| --- | --- |
| `update` | `scriptUpdate` |
| `solve` | `engineSolve` |
| `render` | `engineRender` |
| `render_begin` | `renderBegin` |
| `script:cleanup` | `scriptShutdown` |
| `cleanup` | split into `scriptShutdown` and `engineShutdown` |

Replace mutable library globals with state allocated for each integration.
Callbacks receive that state directly. Promise tables, JS callbacks, queues,
connections, and worker ownership belong there. Join workers during shutdown,
then release the state in `destroy`.

The former `KoyaRendererV1` table is split into capabilities. Discover
`helix.render` for texture and render requests, `helix.assets` for reads and
temporary-file resolution, and `helix.script` for revocable QuickJS-thread
scheduling. Do not retain or call capabilities after `scriptShutdown` begins.

Build the plugin with Meson, name the target `hx-example`, set
`name_prefix: 'lib'`, include this repository’s `sdk` directory, and export only the canonical
entry point. Helix provides no old ABI or filename fallback.
