mcp_glib_inc = include_directories('.')

mcp_glib_enums = gnome.mkenums_simple(
    'mcp-enums',
    sources: files('mcp-session.h'),
)

mcp_glib_lib = static_library(
    'mcp-glib',
    files(
        'mcp-glib.c',
        'mcp-jsonrpc.c',
        'mcp-server-transports.c',
        'mcp-stdio.c',
    ) + mcp_glib_enums,
    dependencies: [
        glib_dep,
        gobject_dep,
        gio_dep,
        json_glib_dep,
        m_dep,
    ],
    include_directories: mcp_glib_inc,
)

mcp_glib_dep = declare_dependency(
    link_with: mcp_glib_lib,
    sources: mcp_glib_enums[1],
    dependencies: [
        glib_dep,
        gobject_dep,
        gio_dep,
        json_glib_dep,
        m_dep,
    ],
    include_directories: mcp_glib_inc,
)

subdir('tests')
