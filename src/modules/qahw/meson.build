name = 'module-qahw-card'

sources = [
  'module-qahw-card.c',
  'module-qahw-card-extn.c',
  'qahw-config-parser.c',
  'qahw-effect.c',
  'qahw-format-detection.c',
  'qahw-jack.c',
  'qahw-jack-evdev.c',
  'qahw-jack-external.c',
  'qahw-jack-hdmi.c',
  'qahw-jack-hdmi-out.c',
  'qahw-jack-spdif.c',
  'qahw-loopback.c',
  'qahw-sink.c',
  'qahw-sink-extn.c',
  'qahw-source.c',
  'qahw-source-extn.c',
  'qahw-utils.c',
]

headers = [
  'qahw-card-extn.h',
  'qahw-card.h',
  'qahw-config-parser.h',
  'qahw-effect.h',
  'qahw-jack-common.h',
  'qahw-jack-format.h',
  'qahw-jack.h',
  'qahw-loopback.h',
  'qahw-sink-extn.h',
  'qahw-sink.h',
  'qahw-source-extn.h',
  'qahw-source.h',
  'qahw-utils.h',
]

extra_inc = [ include_directories(qahw_inc),
              include_directories(qahw_api_inc),
            ]
extra_flags = []
extra_deps = [ dbus_dep,
               meson.get_compiler('c').find_library('qahw'),
             ]
extra_libs = []
libm_dep = cc.find_library('m', required : true)

# build shared module
mod = shared_module(name,
  sources,
  headers,
  include_directories : [configinc, topinc, include_directories('.')] + extra_inc,
  c_args : [pa_c_args, server_c_args, '-DPA_MODULE_NAME=' + name.underscorify()] + extra_flags,
  install : true,
  install_rpath : rpath_dirs,
  install_dir : modlibexecdir,
  dependencies : [thread_dep, libpulse_dep, libpulsecommon_dep, libpulsecore_dep, libintl_dep, platform_dep, platform_socket_dep, libm_dep] + extra_deps,
  link_args : [nodelete_link_args, '-Wl,--no-undefined' ],
  link_with : extra_libs,
  name_prefix : '',
  implicit_include_directories : false)

# install configs
install_subdir('configs',
  install_dir : qahwdatadir
)
