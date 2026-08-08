install_data(
    'com.igalia.wig.desktop',
    install_dir: join_paths(get_option('datadir'), 'applications'),
)

service_configuration = configuration_data()
service_configuration.set('bindir', get_option('prefix') / get_option('bindir'))
configure_file(
    input: 'com.igalia.wig.service.in',
    output: 'com.igalia.wig.service',
    configuration: service_configuration,
    install: true,
    install_dir: get_option('datadir') / 'dbus-1/services',
)

install_data(
    wig_gschema,
    install_dir: join_paths(get_option('datadir'), 'glib-2.0', 'schemas'),
)

glib_compile_schemas = find_program('glib-compile-schemas')
test(
    'validate-gsettings-schema',
    glib_compile_schemas,
    args: ['--strict', '--dry-run', meson.current_source_dir()],
)

gnome.post_install(glib_compile_schemas: true)
