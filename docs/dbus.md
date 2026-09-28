# D-Bus API

Import the plugin as `Module/dbus`. The engine delivers replies and signals
on its script thread through the update hook.

## Independent session and system handles

```js
import { session, system } from 'Module/dbus';

await Promise.all([session.connect(), system.connect()]);

session.addMatch("type='signal',interface='org.example.Desktop'");
session.onSignal(sig => {
  if (sig.member === 'HardwareButton') {
    const button = sig.args[0]; // decoded a{sv} dictionary
  }
});

const settings = await system.call(
  'org.freedesktop.NetworkManager',
  '/org/freedesktop/NetworkManager/Settings/1',
  'org.freedesktop.NetworkManager.Settings.Connection',
  'GetSettings'
); // decoded a{sa{sv}} object
```

Each handle exposes `connect()`, `addMatch`, `call`, `callComplex`, `onSignal`,
and `offSignal`. Connections, pending calls, match rules, and callbacks are
independent of each other and of the module-level connection. Await `connect()`
before sending calls or adding match rules. A handle always connects to its
named bus.

The original module-level API remains available:

```js
import * as DBus from 'Module/dbus';
await DBus.connect('session'); // defaults to session when omitted
```

Module-level functions share one legacy connection. Once connected or connecting,
requesting another bus raises an error. Use the independent handles for concurrent
access to both buses.

## Methods

| Method | Behavior |
| --- | --- |
| `connect()` | Connect a handle asynchronously; returns a promise. |
| `addMatch(rule)` | Add a signal match rule. |
| `call(dest, path, interface, method, signature?, ...args)` | Send a method call with basic arguments; returns a promise. |
| `callComplex(dest, path, interface, method, signature, ...args)` | Send a method call with containers and variants; returns a promise. |
| `onSignal(callback)` | Register a callback for incoming signals on this connection. |
| `offSignal(callback?)` | Remove a callback, or clear all callbacks on this connection. |

Basic method replies resolve to strings. Arrays, structs, and dictionaries resolve
to decoded JavaScript values. Byte arrays (`ay`), including variant-wrapped arrays,
resolve to arrays of numbers from 0 to 255. Variants are unwrapped, and nested
dictionaries such as `a{sa{sv}}` decode recursively. Method errors reject with an
error string.

`callComplex` accepts `a{sv}`, `a{sa{sv}}`, `v`, `ay`, and `as`. Variant types are
inferred from JavaScript values; to force a supported type, use a wrapper such as
`{ _t: 'ay', _v: [83, 83, 73, 68] }`.

## Signals

Callbacks receive:

```js
{
  sender: string,
  path: string,
  interface: string,
  member: string,
  signature: string,
  body: string,
  args: unknown[]
}
```

`args` contains every argument in wire order. Basic values retain their JavaScript
types, variants are unwrapped, dictionaries become objects, and arrays and structs
become arrays. For `HardwareButton(a{sv})`, the dictionary is `sig.args[0]`.
For `PropertiesChanged(sa{sv}as)`, the interface name, changed properties, and
invalidated property names are `sig.args[0]`, `sig.args[1]`, and `sig.args[2]`.

`body` preserves the first argument as text for compatibility. Composite values
are JSON text. Empty signals have `args: []` and `body: ''`.

Set `KOYA_DBUS_DEBUG=1` to log send/receive activity to stderr.

## Regression tests

The regression harness needs a matching QuickJS library, Python 3, and
`dbus-daemon`. It starts two private daemons and never contacts your live buses.
For example, using the QuickJS library and lifecycle runner built by Helix:

```sh
meson setup build-dbus-regression -Dplugins=dbus \
  -Dquickjs_library="$HOME/projects/helix/build/helix-base/ext/quickjs/libquickjs.a" \
  -Dlifecycle_test_runner="$HOME/projects/helix/build/helix-plugin/helix_plugin_two_runtime_lifecycle"
meson test -C build-dbus-regression --print-errorlogs
```

The regression test covers byte decoding, nested dictionaries, composite and
multiple signal arguments, independent buses, and runtime isolation.
