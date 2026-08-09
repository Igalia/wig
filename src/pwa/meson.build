pwa_sources = files(
    'wig-pwa-launch-handler.c',
    'wig-pwa-navigation-policy.c',
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
