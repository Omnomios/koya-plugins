openssl_dep = dependency('openssl', required: true)

http_inc = include_directories(
  'src',
  'cpp-httplib',
  '..',
  '../sdk',
)

shared_library(
  'sm-http',
  files(
    'src/module.cpp',
    'src/Http.cpp',
  ),
  include_directories: http_inc,
  dependencies: [threads_dep, openssl_dep],
  cpp_args: ['-DCPPHTTPLIB_OPENSSL_SUPPORT'],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
