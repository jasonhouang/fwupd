host_machine.system() == 'linux' or subdir_done()

cargs = ['-DG_LOG_DOMAIN="FuPluginElantp"']
plugins += {meson.current_source_dir().split('/')[-1]: true}

plugin_quirks += files('elantp.quirk')
plugin_builtin_elantp = static_library('fu_plugin_elantp',
  rustgen.process(
    'fu-elantp.rs',         # fuzzing
  ),
  sources: [
    'fu-elantp-plugin.c',
    'fu-elantp-firmware.c',   # fuzzing
    'fu-elantp-haptic-firmware.c',
    'fu-elantp-hid-device.c',
    'fu-elantp-i2c-device.c',
    'fu-elantp-hid-haptic-device.c',
  ],
  include_directories: plugin_incdirs,
  c_args: [
    cargs,
    '-DLOCALSTATEDIR="' + localstatedir + '"',
  ],
  link_with: plugin_libs,
  dependencies: plugin_deps,
)
plugin_builtins += plugin_builtin_elantp

enumeration_data += files('tests/lenovo-nano-g1-setup.json')
device_tests += files('tests/lenovo-nano-g1.json')

if get_option('tests')
  install_data(['tests/elantp.builder.xml'],
    install_dir: join_paths(installed_test_datadir, 'tests'))
  env = environment()
  env.set('G_TEST_SRCDIR', meson.current_source_dir())
  env.set('G_TEST_BUILDDIR', meson.current_build_dir())
  e = executable(
    'elantp-self-test',
    sources: [
      'fu-self-test.c',
    ],
    include_directories: plugin_incdirs,
    dependencies: plugin_deps,
    link_with: [
      plugin_libs,
      plugin_builtin_elantp,
    ],
    install: true,
    install_rpath: libdir_pkg,
    install_dir: installed_test_bindir,
    c_args: [
      '-DSRCDIR="' + meson.current_source_dir() + '"',
    ],
  )
  test('elantp-self-test', e, env: env)
endif
