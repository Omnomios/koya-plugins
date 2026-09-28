project(
  'helix-plugins',
  ['c', 'cpp'],
  version: '0.0.1',
  default_options: ['cpp_std=c++20', 'b_lundef=false'],
)

threads_dep = dependency('threads', required: true)
# Unix hosts export QuickJS symbols. Windows needs an explicit matching library.
quickjs_library = get_option('quickjs_library')
quickjs_dep = declare_dependency()
if quickjs_library != ''
  quickjs_dep = declare_dependency(link_args: [quickjs_library])
elif host_machine.system() == 'windows' and get_option('plugins').length() > 0
  error('Windows builds require -Dquickjs_library=<matching QuickJS library>')
endif

plugin_header_args = ['-I' + meson.current_source_dir() / 'sdk']
if not meson.get_compiler('c').compiles(
  '#include <helix/plugin.h>\nint main(void) { HelixPluginInstance value = {0}; return (int)value.abiVersion; }',
  args: plugin_header_args,
  name: 'Helix plugin ABI is C compatible',
)
  error('Helix plugin ABI header failed the C compile check')
endif
if not meson.get_compiler('cpp').compiles(
  '#include <helix/plugin.h>\nint main() { HelixPluginInstance value{}; return static_cast<int>(value.abiVersion); }',
  args: plugin_header_args,
  name: 'Helix plugin ABI is C++ compatible',
)
  error('Helix plugin ABI header failed the C++ compile check')
endif

cpp = meson.get_compiler('cpp')
plugin_selection = get_option('plugins')
if 'all' in plugin_selection
  plugin_selection = ['dbus', 'ffmpeg', 'http', 'hypr', 'pam', 'process', 'sqlite', 'ws']
endif

plugin_inc = include_directories(
  'sdk',
  'sdk/quickjs',
)
plugin_common_deps = [threads_dep, quickjs_dep]
plugin_cpp_args = []
plugin_test_names = []
plugin_test_targets = []
if cpp.get_id() == 'gcc' or cpp.get_id() == 'clang'
  plugin_cpp_args += ['-fvisibility=hidden']
endif

if 'dbus' in plugin_selection
  dbus_dep = dependency('dbus-1', required: true)
  dbus_plugin = shared_library(
    'hx-dbus',
    files('dbus/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: plugin_inc,
    dependencies: plugin_common_deps + [dbus_dep],
    cpp_args: plugin_cpp_args,
  )
  plugin_test_names += ['dbus']
  plugin_test_targets += [dbus_plugin]
  # The standalone test harness needs matching QuickJS symbols of its own.
  if quickjs_library != '' and not meson.is_cross_build()
    dbus_regression = executable(
      'helix_dbus_regression',
      files('tests/dbus_regression.cpp'),
      link_with: dbus_plugin,
      include_directories: plugin_inc,
      dependencies: plugin_common_deps + [dbus_dep, cpp.find_library('m')],
    )
    test(
      'hx-dbus-regression',
      find_program('python3'),
      args: [files('tests/run_dbus_regression.py'), find_program('dbus-daemon').full_path(), dbus_regression],
      timeout: 40,
    )
  endif
endif

if 'ffmpeg' in plugin_selection
  ffmpeg_deps = [
    dependency('libavformat', required: true),
    dependency('libavcodec', required: true),
    dependency('libavutil', required: true),
    dependency('libswscale', required: true),
  ]
  ffmpeg_plugin = shared_library(
    'hx-ffmpeg',
    files('ffmpeg/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: plugin_inc,
    dependencies: plugin_common_deps + ffmpeg_deps,
    cpp_args: plugin_cpp_args,
  )
  plugin_test_names += ['ffmpeg']
  plugin_test_targets += [ffmpeg_plugin]
endif

if 'http' in plugin_selection
  http_deps = plugin_common_deps
  http_args = plugin_cpp_args
  if host_machine.system() == 'windows'
    http_args += ['-DUNICODE', '-D_UNICODE']
    http_deps += [
      cpp.find_library('winhttp', required: true),
      cpp.find_library('ws2_32', required: true),
    ]
  elif not get_option('http_tls').disabled()
    openssl_dep = dependency('openssl', required: get_option('http_tls'))
    if openssl_dep.found()
      http_args += ['-DCPPHTTPLIB_OPENSSL_SUPPORT']
      http_deps += [openssl_dep]
    endif
  endif
  http_plugin = shared_library(
    'hx-http',
    files('http/src/Http.cpp', 'http/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: [plugin_inc, include_directories('http/src', 'http/cpp-httplib')],
    dependencies: http_deps,
    cpp_args: http_args,
  )
  plugin_test_names += ['http']
  plugin_test_targets += [http_plugin]
endif

if 'hypr' in plugin_selection
  if host_machine.system() == 'windows'
    error('hx-hypr is only supported on Unix-like systems')
  endif
  hypr_plugin = shared_library(
    'hx-hypr',
    files('hypr/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: plugin_inc,
    dependencies: plugin_common_deps,
    cpp_args: plugin_cpp_args,
  )
  plugin_test_names += ['hypr']
  plugin_test_targets += [hypr_plugin]
endif

if 'pam' in plugin_selection
  if host_machine.system() == 'windows'
    error('hx-pam is only supported on Unix-like systems')
  endif
  pam_dep = cpp.find_library('pam', required: true)
  pam_plugin = shared_library(
    'hx-pam',
    files('pam/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: plugin_inc,
    dependencies: plugin_common_deps + [pam_dep],
    cpp_args: plugin_cpp_args,
  )
  plugin_test_names += ['pam']
  plugin_test_targets += [pam_plugin]
endif

if 'process' in plugin_selection
  if host_machine.system() == 'windows'
    error('hx-process is only supported on Unix-like systems')
  endif
  process_plugin = shared_library(
    'hx-process',
    files('process/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: plugin_inc,
    dependencies: plugin_common_deps,
    cpp_args: plugin_cpp_args,
  )
  plugin_test_names += ['process']
  plugin_test_targets += [process_plugin]
endif

if 'sqlite' in plugin_selection
  sqlite_dep = dependency('sqlite3', required: true)
  sqlite_plugin = shared_library(
    'hx-sqlite',
    files('sqlite/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: plugin_inc,
    dependencies: plugin_common_deps + [sqlite_dep],
    cpp_args: plugin_cpp_args,
  )
  plugin_test_names += ['sqlite']
  plugin_test_targets += [sqlite_plugin]
endif

if 'ws' in plugin_selection
  ix_root = 'ws/IXWebSocket/ixwebsocket'
  ix_sources = files(
    ix_root / 'IXBench.cpp',
    ix_root / 'IXCancellationRequest.cpp',
    ix_root / 'IXConnectionState.cpp',
    ix_root / 'IXDNSLookup.cpp',
    ix_root / 'IXExponentialBackoff.cpp',
    ix_root / 'IXGetFreePort.cpp',
    ix_root / 'IXGzipCodec.cpp',
    ix_root / 'IXHttp.cpp',
    ix_root / 'IXHttpClient.cpp',
    ix_root / 'IXHttpServer.cpp',
    ix_root / 'IXNetSystem.cpp',
    ix_root / 'IXSelectInterrupt.cpp',
    ix_root / 'IXSelectInterruptFactory.cpp',
    ix_root / 'IXSelectInterruptPipe.cpp',
    ix_root / 'IXSelectInterruptEvent.cpp',
    ix_root / 'IXSetThreadName.cpp',
    ix_root / 'IXSocket.cpp',
    ix_root / 'IXSocketConnect.cpp',
    ix_root / 'IXSocketFactory.cpp',
    ix_root / 'IXSocketServer.cpp',
    ix_root / 'IXSocketTLSOptions.cpp',
    ix_root / 'IXStrCaseCompare.cpp',
    ix_root / 'IXUdpSocket.cpp',
    ix_root / 'IXUrlParser.cpp',
    ix_root / 'IXUuid.cpp',
    ix_root / 'IXUserAgent.cpp',
    ix_root / 'IXWebSocket.cpp',
    ix_root / 'IXWebSocketCloseConstants.cpp',
    ix_root / 'IXWebSocketHandshake.cpp',
    ix_root / 'IXWebSocketHttpHeaders.cpp',
    ix_root / 'IXWebSocketPerMessageDeflate.cpp',
    ix_root / 'IXWebSocketPerMessageDeflateCodec.cpp',
    ix_root / 'IXWebSocketPerMessageDeflateOptions.cpp',
    ix_root / 'IXWebSocketProxyServer.cpp',
    ix_root / 'IXWebSocketServer.cpp',
    ix_root / 'IXWebSocketTransport.cpp',
  )
  ix_deps = [threads_dep]
  ix_args = []
  if host_machine.system() == 'windows'
    ix_deps += [cpp.find_library('ws2_32', required: true), cpp.find_library('shlwapi', required: true)]
    ix_args += ['-D_CRT_SECURE_NO_WARNINGS']
  else
    ix_sources += files(ix_root / 'IXSocketOpenSSL.cpp')
    ix_deps += [dependency('openssl', required: true)]
    ix_args += ['-DIXWEBSOCKET_USE_TLS', '-DIXWEBSOCKET_USE_OPEN_SSL']
  endif
  ixwebsocket = static_library(
    'helix-ixwebsocket',
    ix_sources,
    include_directories: include_directories('ws/IXWebSocket'),
    dependencies: ix_deps,
    cpp_args: ix_args,
    pic: true,
  )
  ix_dep = declare_dependency(
    link_with: ixwebsocket,
    include_directories: include_directories('ws/IXWebSocket'),
    dependencies: ix_deps,
    compile_args: ix_args,
  )
  websocket_plugin = shared_library(
    'hx-ws',
    files('ws/src/module.cpp'),
    name_prefix: 'lib',
    include_directories: plugin_inc,
    dependencies: plugin_common_deps + [ix_dep],
    cpp_args: plugin_cpp_args,
  )
  plugin_test_names += ['ws']
  plugin_test_targets += [websocket_plugin]
endif

# The lifecycle harness uses Helix's private runtime services. Reuse a harness
# built in the Helix source tree to test these standalone plugin artifacts.
lifecycle_runner = get_option('lifecycle_test_runner')
if lifecycle_runner != '' and plugin_test_names.length() > 0 and not meson.is_cross_build()
  plugin_lifecycle_test = find_program(lifecycle_runner, required: true)
  foreach index : range(plugin_test_names.length())
    name = plugin_test_names[index]
    test(
      'hx-' + name + '-two-runtime-lifecycle',
      plugin_lifecycle_test,
      args: [meson.current_build_dir(), name],
      depends: plugin_test_targets[index],
      is_parallel: false,
    )
  endforeach
endif
