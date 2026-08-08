wig_flatpak_sources = files(
    'wig-flatpak.c',
    'wig-update-monitor.c',
)

executable(
    'flatpak-update-helper',
    sources: files('flatpak-update-helper.c', 'wig-flatpak.c'),
    dependencies: [dependency('gtk4')],
    include_directories: ['.'],
    c_args: ['-DG_LOG_DOMAIN="wig-update-helper"'],
    install: true,
    install_dir: wig_libexecdir,
)
