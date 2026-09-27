wig_mcp_resources = gnome.compile_resources(
    'wig-mcp-resources',
    'wig-mcp.gresource.xml',
    source_dir: meson.current_source_dir(),
)

wig_mcp_sources = files(
    'wig-mcp-json.c',
    'wig-mcp-navigation.c',
    'wig-mcp-network.c',
    'wig-mcp-result.c',
    'wig-mcp-scripts.c',
    'wig-mcp-server.c',
    'wig-mcp-tool-table.c',
    'wig-mcp-tools.c',
    'wig-mcp-transport.c',
) + wig_mcp_resources
