gnome = import('gnome')

internal_pages_resources = gnome.compile_resources(
    'wig-internal-pages-resources',
    'wig-internal-pages.gresource.xml',
    source_dir: meson.current_source_dir(),
)

internal_pages_sources = files(
    'wig-history.c',
    'wig-internal-page.c',
    'wig-user-scripts.c',
    'wig-user-styles.c',
    'wig-website-data.c',
) + internal_pages_resources
