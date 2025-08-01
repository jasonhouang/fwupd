host_machine.system() == 'linux' or subdir_done()
gnutls.found() or subdir_done()

cargs = ['-DG_LOG_DOMAIN="FuPluginSynapticsRmi"']
plugins += {meson.current_source_dir().split('/')[-1]: true}

plugin_quirks += files('synaptics-rmi.quirk')
plugin_builtin_synaptics_rmi = static_library('fu_plugin_synaptics_rmi',
  rustgen.process(
    'fu-synaptics-rmi.rs',        # fuzzing
  ),
  sources: [
    'fu-synaptics-rmi-plugin.c',
    'fu-synaptics-rmi-common.c',      # fuzzing
    'fu-synaptics-rmi-device.c',
    'fu-synaptics-rmi-hid-device.c',
    'fu-synaptics-rmi-ps2-device.c',
    'fu-synaptics-rmi-v5-device.c',
    'fu-synaptics-rmi-v6-device.c',
    'fu-synaptics-rmi-v7-device.c',
    'fu-synaptics-rmi-firmware.c',    # fuzzing
  ],
  include_directories: plugin_incdirs,
  c_args: cargs,
  dependencies: plugin_deps,
  link_with: plugin_libs,
)
plugin_builtins += plugin_builtin_synaptics_rmi

if get_option('tests')
  install_data(['tests/synaptics-rmi-0x.builder.xml','tests/synaptics-rmi-10.builder.xml'],
    install_dir: join_paths(installed_test_datadir, 'tests'))
  env = environment()
  env.set('G_TEST_SRCDIR', meson.current_source_dir())
  env.set('G_TEST_BUILDDIR', meson.current_build_dir())
  e = executable(
    'synaptics-rmi-self-test',
    sources: [
      'fu-self-test.c',
    ],
    include_directories: plugin_incdirs,
    dependencies: plugin_deps,
    link_with: [
      plugin_libs,
      plugin_builtin_synaptics_rmi,
    ],
    install: true,
    install_rpath: libdir_pkg,
    install_dir: installed_test_bindir,
    c_args: [
      '-DSRCDIR="' + meson.current_source_dir() + '"',
    ],
  )
  test('synaptics-rmi-self-test', e, env: env)
endif
