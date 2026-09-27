test(
    'mcp-e2e',
    find_program('mcp-e2e.py'),
    args: [wig_exe.full_path(), meson.current_source_dir() / 'fixtures'],
    depends: wig_exe,
    protocol: 'tap',
    timeout: 300,
    suite: 'mcp',
    env: {
        'GTK_A11Y': 'none',
        'NO_AT_BRIDGE': '1',
    },
)
