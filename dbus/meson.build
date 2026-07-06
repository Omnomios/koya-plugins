dbus_dep = dependency('dbus-1', required: true)

dbus_inc = include_directories(
  'src',
  '..',
  '../sdk',
)

shared_library(
  'sm-dbus',
  files('src/module.cpp'),
  include_directories: dbus_inc,
  dependencies: [threads_dep, dbus_dep],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
