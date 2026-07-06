subdir('IXWebSocket')

ws_inc = include_directories(
  'src',
  'IXWebSocket',
  '..',
  '../sdk',
)

shared_library(
  'sm-ws',
  files('src/module.cpp'),
  include_directories: ws_inc,
  dependencies: [threads_dep, ixwebsocket_dep],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
