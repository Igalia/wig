pwa_sources = files(
    'wig-pwa-launch-handler.c',
    'wig-pwa-navigation-policy.c',
    'wig-pwa-protocol-handlers.c',
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
