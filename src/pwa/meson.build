pwa_sources = files(
    'wig-app-installer-native.c',
    'wig-app-installer-portal.c',
    'wig-app-installer.c',
    'wig-pwa-desktop-integration.c',
    'wig-pwa-icon.c',
    'wig-pwa-launch-handler.c',
    'wig-pwa-manager.c',
    'wig-pwa-navigation-policy.c',
    'wig-pwa-protocol-handlers.c',
    'wig-pwa-synthetic-manifest.c',
    'wig-pwa-window.c',
)

pwa_navigation_policy_test = executable(
    'test-pwa-navigation-policy',
    sources: files('test-pwa-navigation-policy.c', 'wig-pwa-navigation-policy.c'),
    dependencies: [wpe_webkit_dep],
)
test('pwa-navigation-policy', pwa_navigation_policy_test)

pwa_launch_handler_test = executable(
    'test-pwa-launch-handler',
    sources: files('test-pwa-launch-handler.c', 'wig-pwa-launch-handler.c'),
    dependencies: [wpe_webkit_dep],
)
test('pwa-launch-handler', pwa_launch_handler_test)

pwa_protocol_handlers_test = executable(
    'test-pwa-protocol-handlers',
    sources: files(
        'test-pwa-protocol-handlers.c',
        'wig-pwa-navigation-policy.c',
        'wig-pwa-protocol-handlers.c',
    ),
    dependencies: [wpe_webkit_dep, json_glib_dep],
)
test('pwa-protocol-handlers', pwa_protocol_handlers_test)

pwa_synthetic_manifest_test = executable(
    'test-pwa-synthetic-manifest',
    sources: files(
        'test-pwa-synthetic-manifest.c',
        'wig-pwa-synthetic-manifest.c',
    ),
    dependencies: [gio_dep],
)
test('pwa-synthetic-manifest', pwa_synthetic_manifest_test)

pwa_desktop_integration_test = executable(
    'test-pwa-desktop-integration',
    sources: files(
        'test-pwa-desktop-integration.c',
        'wig-app-installer-native.c',
        'wig-app-installer.c',
        'wig-pwa-desktop-integration.c',
    ),
    dependencies: [gio_dep],
)
test('pwa-desktop-integration', pwa_desktop_integration_test)
