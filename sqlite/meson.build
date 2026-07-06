sqlite_dep = dependency('sqlite3', required: true)

sqlite_inc = include_directories(
  'src',
  '..',
  '../sdk',
)

shared_library(
  'sm-sqlite',
  files('src/module.cpp'),
  include_directories: sqlite_inc,
  dependencies: [threads_dep, sqlite_dep],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
