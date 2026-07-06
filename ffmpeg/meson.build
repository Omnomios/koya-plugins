avformat_dep = dependency('libavformat', required: true, method: 'pkg-config')
avcodec_dep = dependency('libavcodec', required: true, method: 'pkg-config')
avutil_dep = dependency('libavutil', required: true, method: 'pkg-config')
swscale_dep = dependency('libswscale', required: true, method: 'pkg-config')

ffmpeg_inc = include_directories(
  'src',
  '..',
  '../sdk',
)

shared_library(
  'sm-ffmpeg',
  files('src/module.cpp'),
  include_directories: ffmpeg_inc,
  dependencies: [threads_dep, avformat_dep, avcodec_dep, avutil_dep, swscale_dep],
  link_args: plugin_link_args,
  override_options: plugin_override_options,
  install: false,
)
