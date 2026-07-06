hypr_inc = include_directories(
  'src',
  '..',
  '../sdk',
)

shared_library(
  'sm-hypr',
  files('src/module.cpp'),
  include_directories: hypr_inc,
  dependencies: [threads_dep],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
