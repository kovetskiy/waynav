project(
  'waynav',
  'c',
  meson_version: '>=1.1',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'werror=true',
  ],
  license: 'MIT',
  version: '0.1.0',
)

add_project_arguments(
  '-D_GNU_SOURCE=200809L',
  '-DVERSION="@0@"'.format(meson.project_version()),
  language: 'c',
)

cc = meson.get_compiler('c')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')
xkbcommon = dependency('xkbcommon')
cairo = dependency('cairo')
math = cc.find_library('m')

subdir('protocol')

sources = [
  'src/main.c',
  'src/config.c',
  'src/grid.c',
  'src/overlay.c',
  'src/input.c',
  'src/log.c',
  protos_src,
]

executable(
  'waynav',
  sources,
  dependencies: [
    wayland_client,
    xkbcommon,
    cairo,
    math,
  ],
  install: true,
)

# Tests
grid_test = executable(
  'test_grid',
  ['tests/test_grid.c', 'src/grid.c'],
  dependencies: [math],
)
test('grid', grid_test)

config_test = executable(
  'test_config',
  ['tests/test_config.c', 'src/config.c', 'src/log.c'],
  dependencies: [math, xkbcommon],
)
test('config', config_test)

input_test = executable(
  'test_input',
  ['tests/test_input.c', 'src/input.c', 'src/grid.c', 'src/log.c'],
  dependencies: [math],
)
test('input', input_test)
