name = 'module-qal-voiceui-card'

sources = [
  'module-qal-voiceui-card.c',
]

headers = [
  'qal-voiceui-utils.h',
]

extra_inc = [include_directories(qal_voiceui_inc)]
extra_flags = []
extra_deps = [ dbus_dep,
               meson.get_compiler('c').find_library('cutils'),
               meson.get_compiler('c').find_library('pal'),
               meson.get_compiler('c').find_library('agm')
             ]
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
