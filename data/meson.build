install_data(
    'com.igalia.wig.desktop',
    install_dir: join_paths(get_option('datadir'), 'applications'),
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
