# tomlc17 is only needed by xal-server, so a missing one skips the server rather than failing the
# whole build; -Dxal-server=enabled turns that skip into an error.
xal_server_feature = get_option('xal-server')

if xal_server_feature.disabled()
  subdir_done()
endif

toml_proj = subproject('tomlc17', required: xal_server_feature.enabled())
if not toml_proj.found()
  message('xal-server: tomlc17 unavailable, skipping')
  subdir_done()
endif

toml_dep = toml_proj.get_variable('toml_dep')

systemd_dep = dependency('systemd', required: false)

xal_server_sources = files(
  'xal_server.c',
  'xal_server_conf.c',
)

xal_server_exe = executable(
  'xal-server',
  sources: xal_server_sources,
  dependencies: xallib_deps + [toml_dep],
  link_with: xal_library,
  include_directories: [include_dirs, include_directories('.')],
  install_rpath: xallib_rpath,
  install: true,
)

xal_server_bindir = join_paths(get_option('prefix'), get_option('bindir'))
xal_server_confdir = join_paths(get_option('prefix'), get_option('sysconfdir'), 'xal')

# Installed under .example because `make` re-runs install on every build; writing to
# xal-server.conf itself would discard the operator's edits each time.
configure_file(
  input: 'xal-server.conf.in',
  output: 'xal-server.conf.example',
  copy: true,
  install: true,
  install_dir: xal_server_confdir,
)

# The unit is only installed where systemd can say where units belong; the server itself runs
# fine in the foreground without it.
if systemd_dep.found()
  configure_file(
    input: 'xal-server.service.in',
    output: 'xal-server.service',
    install: true,
    install_dir: systemd_dep.get_variable(pkgconfig: 'systemdsystemunitdir'),
    configuration: {
      'bindir': xal_server_bindir,
      'config': join_paths(xal_server_confdir, 'xal-server.conf'),
    },
  )
else
  message('xal-server: systemd not found, skipping the service unit')
endif
