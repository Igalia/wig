subdir('internal-pages')
subdir('tabs')
subdir('entry')

wig_compiled_schemas = custom_target(
    'gschemas.compiled',
    input: wig_gschema,
    output: 'gschemas.compiled',
    command: [
        find_program('glib-compile-schemas'),
        '--targetdir=@OUTDIR@',
        meson.project_source_root() / 'data',
    ],
    build_by_default: true,
)

wig_resources = gnome.compile_resources(
    'wig-resources',
    'wig-application.gresource.xml',
    source_dir: meson.current_source_dir(),
)

wig_sources = files(
    'main.c',
    'wig-application.c',
    'wig-context-menu.c',
    'wig-favicon.c',
    'wig-history-item.c',
    'wig-history-store.c',
    'wig-key-file-utils.c',
    'wig-permissions-manager.c',
    'wig-permissions.c',
    'wig-search-bar.c',
    'wig-session.c',
    'wig-settings.c',
    'wig-utils.c',
    'wig-window-base.c',
    'wig-window.c',
) + internal_pages_sources + wig_tab_sources + wig_entry_sources + wig_resources + wig_icon_resources

executable(
    'wig',
    sources: wig_sources,
    dependencies: [
        libwpeplatformgtk_dep,
        wpe_webkit_dep,
        adw_dep,
        libpsl_dep,
        sqlite_dep,
        template_glib_dep,
    ],
    include_directories: ['.', 'internal-pages', 'tabs', 'entry'],
    c_args: ['-DG_LOG_DOMAIN="wig"'],
    link_depends: wig_compiled_schemas,
    install: true,
)
