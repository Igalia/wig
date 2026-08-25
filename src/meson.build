subdir('bookmarks')
subdir('history')
subdir('settings')
subdir('tabs')
subdir('entry')
subdir('downloads')
subdir('flatpak')

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
    'wig-emoji-chooser.c',
    'wig-favicon.c',
    'wig-key-file-utils.c',
    'wig-network-monitor.c',
    'wig-permissions-manager.c',
    'wig-permissions.c',
    'wig-search-bar.c',
    'wig-session.c',
    'wig-settings.c',
    'wig-utils.c',
    'wig-window-base.c',
    'wig-window.c',
) + wig_bookmarks_sources + wig_history_sources + wig_settings_sources + wig_tab_sources + wig_entry_sources + wig_download_sources + wig_flatpak_sources + wig_resources + wig_icon_resources

executable(
    'wig',
    sources: wig_sources,
    dependencies: [
        libwpeplatformgtk_dep,
        wpe_webkit_dep,
        adw_dep,
        libpsl_dep,
        sqlite_dep,
    ],
    include_directories: [
        '.',
        'bookmarks',
        'history',
        'settings',
        'tabs',
        'entry',
        'downloads',
        'flatpak',
    ],
    c_args: ['-DG_LOG_DOMAIN="wig"'],
    link_depends: wig_compiled_schemas,
    install: true,
)

executable(
    'wig-restart-helper',
    sources: 'wig-restart-helper.c',
    dependencies: [dependency('gio-2.0')],
    c_args: ['-DG_LOG_DOMAIN="wig-restart-helper"'],
    install: true,
    install_dir: wig_libexecdir,
)
