## Koya Plugins — Build native power into your UI

This repository showcases native plugins that extend the Koya runtime with system capabilities like DBus, HTTP(S), Hyprland IPC, processes, SQLite, and WebSockets. Each plugin is intentionally small and commented to help you author your own.

### Architecture in one minute

- Engine (Koya): JavaScript runtime (QuickJS), module system, and lifecycle hooks so work can be executed on the engine thread.
- Plugin (your code): Performs OS/network I/O and threading, then delivers results back to JS during engine hooks.

Typical shape of a plugin: export `integrateV1(JSContext*, const char*, RegisterHookFunc, const KoyaRendererV1*)` which:

- Registers `update` (and optionally `cleanup`) using the provided `RegisterHookFunc`.
- Creates a JS module via `JS_NewCModule` and exports your API functions.
- Keeps blocking or async work off the engine thread; drains results in `update`.

### Quick usage examples

DBus (minimal):
```js
import * as dbus from 'Module/dbus';
await dbus.connect('session');
dbus.addMatch("type='signal',interface='org.freedesktop.DBus'");
dbus.onSignal(s => console.log('signal', s));
const reply = await dbus.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus', 'ListNames');
```

HTTP:
```js
import * as http from 'Module/http';
const res = await http.request({ url: 'https://example.com', method: 'get' });
console.log(res.status, res.body);
```

Hyprland IPC:
```js
import * as hypr from 'Module/hypr';
await hypr.connect({});
hypr.on('workspace', e => console.log('workspace event', e));
const ws = await hypr.workspaces();
```

Process:
```js
import * as proc from 'Module/process';
const p = proc.spawn({ cmd: 'bash', args: ['-lc', 'echo hi && sleep 1 && echo bye'] });
p.stdout.on('data', chunk => print('out', chunk));
p.on('exit', code => print('exit', code));
const path = proc.getEnv('PATH', '/usr/bin');
const allEnv = proc.env();
```

SQLite:
```js
import * as sql from 'Module/sqlite';
const db = await sql.openInMemory();
await sql.exec(db, 'create table t(x)');
await sql.exec(db, "insert into t values('hello')");
const rows = await sql.query(db, 'select * from t');
```

WebSocket:
```js
import * as ws from 'Module/ws';
const sock = ws.create({ url: 'wss://echo.websocket.org', onMessage: m => print('msg', m) });
sock.start();
sock.send('hello');
```

### Included plugins

- DBus: Background thread pumps messages; promises resolved during `update`.
- HTTP: Async HTTP(S) requests with a worker thread and a small Promise API.
- Hypr: IPC bridge to Hyprland sockets for events and commands/JSON queries.
- Process: Spawn/exec with stdout/stderr streaming, an `exec()` Promise, and access to environment variables.
- SQLite: Async `exec()` and `query()` using a worker thread.
- WebSocket: IXWebSocket wrapper with callbacks marshaled to the engine thread.

Vendored dependencies live under folders like `IXWebSocket/` or `cpp-httplib/`. Comments here focus on the Koya↔plugin boundary, not third‑party internals.

### Build (against the Koya binary distribution)

These are native modules intended to extend a binary distribution of Koya. You don't need Koya's source to build them.

- Ensure you have the Koya SDK bits available at build time: QuickJS headers and the `module_hooks.h` interface (see `sdk/quickjs/` and `module_hooks.h` in this repo).
- Use the provided `CMakeLists.txt` in each plugin to build a shared library.
- The output is a shared object exposing `integrateV1(JSContext*, const char*, RegisterHookFunc, const KoyaRendererV1*)` that Koya can discover and load.
- Consult Koya's distribution docs for where to place the built module and how it is discovered at runtime.

### Create your own plugin

1. Copy a minimal module (e.g., `sqlite/src/module.cpp`).
2. Implement `integrateV1(...)`, create a JS module with `JS_NewCModule`, and export your API with `JS_SetModuleExport` + `JS_AddModuleExport`.
3. Keep blocking/async work off-thread; queue results from workers.
4. Drain queues and settle Promises in the `update` or `render_begin` hook. Release all JS values and OS resources in `cleanup`.
5. Build as a shared library; place it where the Koya binary expects modules.

Koya website: [koya-ui.com](https://www.koya-ui.com)

### License

See third‑party licenses in their folders. Plugin sources follow the main Koya project license unless noted.


