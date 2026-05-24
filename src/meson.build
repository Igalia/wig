gnome = import('gnome')

wig_resources = gnome.compile_resources(
  'wig-resources',
  'wig.gresource.xml',
  source_dir: '.',
  c_name: 'wig'
)

wig_sources = files(
  'main.c',
  'wig-application.c',
  'wig-popup-menu.c',
  'wig-tab-view.c',
  'wig-window.c'
)

executable('wig',
  sources: [ wig_sources, wig_resources ],
  dependencies: [ libwpeplatformgtk_dep, wpe_webkit_dep, adw_dep ],
  include_directories: [ '.' ],
  install: true
)
