project(
  'koya-plugin',
  'cpp',
  version: '0.0.1',
  default_options: [
    'cpp_std=c++20',
    'b_lundef=false',
  ],
)

cpp = meson.get_compiler('cpp')
plugin_link_args = []
if cpp.get_id() == 'gcc' or cpp.get_id() == 'clang'
  plugin_link_args += ['-Wl,--allow-shlib-undefined']
endif
plugin_override_options = ['b_lundef=false']

threads_dep = dependency('threads', required: true)

subdir('dbus')
subdir('ffmpeg')
subdir('http')
subdir('hypr')
subdir('pam')
subdir('process')
subdir('sqlite')
subdir('ws')
