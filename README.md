# Native plugin SDK for Koya and Syncromesh

SDK headers and C++ examples for adding native JavaScript modules to Koya and
Syncromesh. The included plugins cover networking, databases, media, and system
services, and can be used directly or as starting points for your own modules.

Follow the implementations to learn module exports, asynchronous work,
script-thread callbacks, and resource cleanup.

## Included plugins

| Plugin | What it provides | Development dependencies |
| --- | --- | --- |
| `dbus` | DBus connections, method calls, and signals | `dbus-1` |
| `ffmpeg` | Video decoding and frame delivery to engine textures | `libavformat`, `libavcodec`, `libavutil`, `libswscale` |
| `http` | Asynchronous HTTP(S) requests and binary responses | OpenSSL for HTTPS on Unix; WinHTTP and Winsock on Windows |
| `hypr` | Hyprland IPC events, commands, and queries | Unix sockets; Unix only |
| `pam` | Authentication through PAM | PAM; Unix only |
| `process` | Child processes, streamed output, and environment access | POSIX process APIs; Unix only |
| `sqlite` | SQLite databases with asynchronous SQL execution and queries | `sqlite3` |
| `ws` | WebSocket connections and message callbacks | OpenSSL on Unix; Winsock and Shlwapi on Windows |

## Build

Install Meson, Ninja, a C++20 compiler, and the development dependencies for
the plugins you want to build. The plugin interface and QuickJS headers
are included under `sdk/`.

Build a subset:

```sh
meson setup build --buildtype=release -Dplugins=http,sqlite
meson compile -C build
```

Or build all eight plugins, which is also the default selection:

```sh
meson setup build --buildtype=release -Dplugins=all
meson compile -C build
```

Shared libraries are produced directly in `build/`, for example
`build/libhx-http.so` on Linux or `build/libhx-http.dll` on Windows. Add that
directory to the engine's native module search path, or place the libraries
beside the engine executable. Koya accepts `-n <directory>` for native modules.

HTTP enables OpenSSL when `-Dhttp_tls=enabled`, or when the default `auto`
detection finds it. Use `-Dhttp_tls=disabled` for an HTTP-only Unix build.
Windows uses WinHTTP for HTTPS.

On Unix, plugins resolve QuickJS symbols from the engine by default. Its
QuickJS version and configuration must match the bundled headers. If you need
to link a matching QuickJS library explicitly, pass
`-Dquickjs_library=/absolute/path/to/libquickjs.a`. Windows requires a matching
QuickJS library supported by the selected compiler and an explicit selection
of supported plugins, such as `-Dplugins=http,sqlite,ws`.

## Use from JavaScript

Import native plugins with `Module/<name>` in either engine:

```js
import * as http from 'Module/http';

const response = await http.request({
  url: 'https://example.com',
  method: 'get',
});
console.log(response.status, response.body);
```

For a local database:

```js
import * as sqlite from 'Module/sqlite';

const db = sqlite.openInMemory();
try {
  await sqlite.exec(db, 'CREATE TABLE messages (text TEXT)');
  await sqlite.exec(db, "INSERT INTO messages VALUES ('Hello from a native plugin')");
  const rows = await sqlite.query(db, 'SELECT text FROM messages');
  console.log(rows);
} finally {
  sqlite.close(db);
}
```

The engine drives asynchronous completions and callbacks on its script thread.

For independent session/system D-Bus connections, typed signal arguments, and
regression test instructions, see the [D-Bus API guide](docs/dbus.md).

## Write your own native plugin

Start with the [SQLite implementation](sqlite/src/module.cpp) for a compact
example of module exports, a worker queue, promises, and cleanup. The
[HTTP implementation](http/src/module.cpp) shows script-thread task dispatch;
[FFmpeg](ffmpeg/src/module.cpp) shows asset and rendering capabilities.

1. Create a C++ module and include `sdk/quickjs/quickjs.h` and
   `sdk/helix/plugin.h`. The optional `sdk/plugin_support.hpp` helper wraps
   capability discovery for the examples in this repository.
2. Export `helix_plugin_integrate`, create the JavaScript module with
   `JS_NewCModule`, and declare its exports with `JS_AddModuleExport`.
   Populate those exports in the module initializer with `JS_SetModuleExport`.
3. Allocate state for each integration and return a `HelixPluginInstance`.
   Keep connections, queues, promises, callbacks, and workers in that instance
   so multiple runtimes can use the same library independently.
4. Keep blocking work on worker threads. Settle promises and invoke JavaScript
   callbacks on the owning script thread through `scriptUpdate` or the script
   scheduling capability. Discover asset and rendering services when needed.
5. Stop and join workers during shutdown, free JavaScript values on the script
   thread, and release the instance in `destroy`. Make shutdown idempotent.
6. Build a shared library named `libhx-<name>` with the `sdk/` include directory,
   then load it from JavaScript as `Module/<name>` in the target engine.

The [native plugin ABI guide](docs/plugin-abi.md) documents the entry point,
capabilities, callback threads, and teardown order. For existing plugins, see
the [migration guide](docs/native-plugin-migration.md).

Meson checks the public ABI header with both C and C++ compilers. Test your
plugin in the target engine as well: exercise its exports and asynchronous
operations, shut down while work is pending, and check that separate runtimes
can release their resources independently.

See the [Koya documentation](https://developer.koya-ui.com/) for engine APIs
and application examples.

## Third-party dependencies

cpp-httplib and IXWebSocket are vendored and built directly by Meson. Their
upstream build files remain as vendor documentation. See the
[cpp-httplib license](http/cpp-httplib/LICENSE) and
[IXWebSocket license](ws/IXWebSocket/LICENSE.txt), along with the notices in
other vendored sources.
