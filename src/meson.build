subdir('internal-pages')
subdir('tabs')

wig_sources = files('main.c', 'wig-application.c', 'wig-utils.c', 'wig-window.c') + internal_pages_sources + wig_tab_sources

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
    include_directories: ['.', 'internal-pages', 'tabs'],
    c_args: ['-DG_LOG_DOMAIN="wig"'],
    install: true,
)
