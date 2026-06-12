wig_sources = files(
    'main.c',
    'wig-application.c',
    'wig-tab-view.c',
    'wig-utils.c',
    'wig-window.c',
)

executable(
    'wig',
    sources: wig_sources,
    dependencies: [libwpeplatformgtk_dep, wpe_webkit_dep, adw_dep, libpsl_dep],
    include_directories: ['.'],
    c_args: ['-DG_LOG_DOMAIN="wig"'],
    install: true,
)
