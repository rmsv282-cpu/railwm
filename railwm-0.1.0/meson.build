project(
  'railwm',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=release',
  ],
)

x11     = dependency('x11')
xcursor = dependency('xcursor')
cairo   = dependency('cairo')
dbus    = dependency('dbus-1')
m_dep   = meson.get_compiler('c').find_library('m', required: false)

# Window manager
railwm = executable(
  'railwm',
  ['src/wm.c', 'src/ewmh.c', 'src/layout.c'],
  dependencies: [x11, xcursor],
  install: true,
)

# Tools (launched by railwm via PATH, so they install next to it)
executable(
  'railbar',
  'tools/railbar/main.c',
  dependencies: [x11, cairo, m_dep],
  install: true,
)

executable(
  'raillauncher',
  'tools/raillauncher/main.c',
  dependencies: [x11, cairo],
  install: true,
)

executable(
  'railnotify',
  'tools/railnotify/main.c',
  dependencies: [x11, cairo, dbus],
  install: true,
)

# Layout unit test (no X needed): meson test -C build
test_layout = executable(
  'test_layout',
  ['tests/test_layout.c', 'src/layout.c'],
  include_directories: include_directories('src'),
)
test('layout', test_layout)
