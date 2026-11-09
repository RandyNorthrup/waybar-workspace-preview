project('waybar-workspace-preview', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 0.60',
  default_options: ['c_std=c11', 'warning_level=3', 'buildtype=debugoptimized', 'b_ndebug=if-release'])

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-Wshadow', '-Wformat=2', '-Wstrict-prototypes', '-Wmissing-prototypes', '-Wnull-dereference',
  '-Wno-unused-parameter', '-Wno-pedantic',
  '-Werror=implicit-function-declaration', '-Werror=return-type',
  '-fno-strict-aliasing',
]), '-D_GNU_SOURCE', '-DG_LOG_DOMAIN="waybar-workspace-preview"', language: 'c')

gtk = dependency('gtk+-3.0')
gio_unix = dependency('gio-unix-2.0')
json_glib = dependency('json-glib-1.0')
m = cc.find_library('m', required: false)

core = static_library('wsp-core',
  'src/naming.c', 'src/ppm.c', 'src/layout.c', 'src/hyprland.c', 'src/capture.c', 'src/apps.c', 'src/store.c',
  dependencies: [gtk, gio_unix, json_glib, m],
  gnu_symbol_visibility: 'hidden',
  pic: true)

plugin = shared_module('waybar-workspace-preview',
  'src/plugin.c',
  include_directories: include_directories('include', 'src'),
  link_whole: core,
  dependencies: [gtk, gio_unix, json_glib, m],
  name_prefix: '',
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: get_option('libdir') / 'waybar')

install_data('README.md', 'LICENSE', install_dir: get_option('datadir') / 'doc' / meson.project_name())

if get_option('tests')
  foreach name : ['naming', 'ppm', 'layout', 'hyprland']
    t = executable('test-' + name, 'tests/test-@0@.c'.format(name),
      include_directories: include_directories('src'),
      c_args: ['-Wno-null-dereference'],
      link_with: core,
      dependencies: [gtk, gio_unix, json_glib, m])
    test(name, t, env: ['FIXTURES=' + meson.current_source_dir() / 'tests' / 'fixtures'])
  endforeach
endif
