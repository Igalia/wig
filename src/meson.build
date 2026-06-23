subdir('internal-pages')

wig_sources = files(
    'main.c',
    'wig-application.c',
    'wig-tab-view.c',
    'wig-utils.c',
    'wig-window.c',
) + internal_pages_sources

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
    include_directories: ['.', 'internal-pages'],
    c_args: ['-DG_LOG_DOMAIN="wig"'],
    install: true,
)
