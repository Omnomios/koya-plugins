process_inc = include_directories(
  'src',
  '..',
  '../sdk',
)

shared_library(
  'sm-process',
  files('src/module.cpp'),
  include_directories: process_inc,
  dependencies: [threads_dep],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
