cpp = meson.get_compiler('cpp')
pam_lib = cpp.find_library('pam', required: true)

pam_inc = include_directories(
  'src',
  '..',
  '../sdk',
)

shared_library(
  'sm-pam',
  files('src/module.cpp'),
  include_directories: pam_inc,
  dependencies: [threads_dep, pam_lib],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
