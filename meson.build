project(
  'wig', 'c',
  version: '0.0.1',
  license: 'LGPL2.1+',
  meson_version: '>= 0.60.0',
  default_options: ['c_std=c11']
)

libwpeplatformgtk_dep = dependency('wpe-platform-gtk-1.0')
wpe_webkit_dep = dependency('wpe-webkit-2.0', version: '>= 2.51.3')
adw_dep	= dependency('libadwaita-1', version: '>= 1.6')

subdir('src')
