project('Plugin', 'c', 'cpp',
  default_options: ['buildtype=release', 'b_ndebug=if-release', 'c_std=c99', 'cpp_std=c++17'],
  meson_version: '>=0.48.0',
  version: '8'
)

vapoursynth_dep = dependency('vapoursynth').partial_dependency(compile_args: true, includes: true)

libs = []

shared_module('api3', [ 'api3.cc' ],
  cpp_args: '-DSTANDALONE',
  dependencies: vapoursynth_dep,
  link_with: [],
  install: false,
  include_directories: [],
  gnu_symbol_visibility: 'hidden'
)
