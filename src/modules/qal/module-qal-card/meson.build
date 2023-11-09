name = 'module-qal-card'

sources = [
  'module-qal-card.c',
  'qal-sink.c',
  'qal-source.c',
  'qal-utils.c',
  'qal-config-parser.c',
  'module-qal-card-extn.c',
  'qal-jack-hdmi-out.c',
  'qal-jack.c',
  'qal-format-detection.c',
]

headers = [
  'qal-card.h',
  'qal-sink.h',
  'qal-source.h',
  'qal-utils.h',
  'qal-config-parser.h',
  'qal-jack-common.h',
  'qal-jack.h',
  'qal-jack-format.h',
]

extra_inc = [include_directories(qal_inc)]
extra_flags = []
extra_deps = [ dbus_dep,
               meson.get_compiler('c').find_library('pal'),
               meson.get_compiler('c').find_library('agm')
             ]
if get_option('enable-pal-service') == 'yes'
extra_deps += [ meson.get_compiler('c').find_library('palserverwrapper'),
                meson.get_compiler('c').find_library('utils'),
                meson.get_compiler('c').find_library('binder')
              ]
endif

if get_option('pal-support-card-status')
extra_flags += ['-DPAL_CARD_STATUS_SUPPORTED']
endif

if get_option('pal-support-cutils')
extra_flags += ['-DPAL_USES_CUTILS']
extra_deps += [meson.get_compiler('c').find_library('cutils')]
endif

extra_libs = []

# build shared module
mod = shared_module(name,
  sources,
  headers,
  include_directories : [configinc, topinc, include_directories('.')] + extra_inc,
  c_args : [pa_c_args, server_c_args, '-DPA_MODULE_NAME=' + name.underscorify()] + extra_flags,
  install : true,
  install_rpath : rpath_dirs,
  install_dir : modlibexecdir,
  dependencies : [thread_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libintl_dep, platform_dep, platform_socket_dep] + extra_deps,
  link_args : [nodelete_link_args, '-Wl,--no-undefined' ],
  link_with : extra_libs,
  name_prefix : '',
  implicit_include_directories : false)

# install configs
install_subdir('configs',
  install_dir : qaldatadir
)
