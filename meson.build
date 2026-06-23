project(
    'wig',
    'c',
    version: '0.0.1',
    license: 'LGPL2.1+',
    meson_version: '>= 0.60.0',
    default_options: ['c_std=c11', 'warning_level=2'],
)

libwpeplatformgtk_dep = dependency('wpe-platform-gtk-1.0')
wpe_webkit_dep = dependency('wpe-webkit-2.0', version: '>= 2.51.3')
adw_dep = dependency('libadwaita-1', version: '>= 1.6')
libpsl_dep = dependency('libpsl')
template_glib_dep = dependency(
    'template-glib-1.0',
    version: '>= 3.41.0',
    allow_fallback: true,
    default_options: [
        'package_subdir=' + meson.project_name(),
        'vapi=false',
        'introspection=disabled',
    ],
)

cc = meson.get_compiler('c')
global_cflags = []
if get_option('optimization') not in ['0', 'g']
    global_cflags += '-D_FORTIFY_SOURCE=3'
endif
test_cflags = [
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-funsigned-char',
    '-Wshadow',
    '-Wundef',
    '-Wcast-align',
    '-Wcast-qual',
    '-Wwrite-strings',
    '-Wmissing-prototypes',
    '-Wmissing-declarations',
    '-Wredundant-decls',
    '-Wnull-dereference',
    '-Wfloat-conversion',
    '-Wsign-conversion',
    '-Wlogical-op',
    '-Wjump-misses-init',
    '-Wvla',
    '-Winline',
    '-Werror=implicit-function-declaration',
    '-Werror=pointer-arith',
    '-Werror=init-self',
    ['-Werror=format-security', '-Werror=format=1'],
    '-Werror=missing-include-dirs',
    '-Werror=date-time',
    '-Werror=incompatible-pointer-types',
    '-Werror=int-conversion',
]
foreach cflag : test_cflags
    if cc.has_multi_arguments(cflag)
        global_cflags += cflag
    endif
endforeach
if get_option('buildtype') != 'plain'
    if cc.has_argument('-fstack-protector-strong') and cc.links(
        '''
     int main (void) {
       char buffer[16];
       strcpy(buffer, "foo");
       return 0;
     }
     ''',
        args: '-fstack-protector-all',
    )
        global_cflags += '-fstack-protector-strong'
    endif
endif
add_project_arguments(global_cflags, language: 'c')

global_ldflags = []
test_ldflags = [
    '-Wl,-z,relro',
    '-Wl,-z,now',
    '-Wl,-z,noexecstack',
    '-Wl,--no-undefined',
]
foreach ldflag : test_ldflags
    has_arg = cc.has_link_argument(ldflag)

    if has_arg and cc.links('int main (void) { return 0; }', args: ldflag)
        global_ldflags += ldflag
    endif
endforeach
add_project_link_arguments(global_ldflags, language: 'c')

subdir('src')
subdir('data')
