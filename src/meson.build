subdir('internal-pages')
subdir('tabs')
subdir('entry')

wig_resources = gnome.compile_resources(
    'wig-resources',
    'wig-application.gresource.xml',
    source_dir: meson.current_source_dir(),
)

wig_sources = files(
    'main.c',
    'wig-application.c',
    'wig-permissions-manager.c',
    'wig-permissions.c',
    'wig-utils.c',
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
        template_glib_dep,
    ],
    include_directories: ['.', 'internal-pages', 'tabs', 'entry'],
    c_args: ['-DG_LOG_DOMAIN="wig"'],
    install: true,
)
