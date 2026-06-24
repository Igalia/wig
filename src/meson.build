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
    dependencies: [adw_dep, libpsl_dep, webkit_deps],
    include_directories: ['.'],
    c_args: ['-DG_LOG_DOMAIN="wig"'],
    install: true,
)
