cmake_minimum_required(VERSION 3.1)

project(pulseaudio-group-modules
    VERSION 2.0
    LANGUAGES CXX C
    )

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

# !! Thread detection must be done before adding the asan parameters.
# pthread detection doesn't work correctly with "-fsanitize=address" because
# libasan provides its own (but incomplete) pthread implementation (GCC 5.4.0
# at least).
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(LIBPULSE REQUIRED libpulse)
pkg_get_variable(LIBPULSE_MODDIR libpulse modlibexecdir)

set(PA_SRC_ROOT "" CACHE PATH "Location of the PulseAudio project. If unset,
    expect the pulsecore headers to be in the default include paths, and
    PulseAudio's config.h along them")
set(PA_CONFIG_DIR "" CACHE PATH "Location of the PulseAudio config.h. If unset
    but PA_SRC_ROOT is set, expect the header to be either directly in
    PA_SRC_ROOT or in PA_SRC_ROOT/build. If unset and PA_SRC_ROOT is not set
    either, expect it to be in the pulsecore subdirectory of one of the default
    include paths.")

if (PA_SRC_ROOT)
    if (PA_CONFIG_DIR)
        if (NOT EXISTS "${PA_CONFIG_DIR}/config.h")
            message(FATAL_ERROR "Could not find pulse audio config at ${PA_CONFIG_DIR}")
        endif()
    else()
        if (EXISTS "${PA_SRC_ROOT}/config.h")
            set(PA_CONFIG_DIR "${PA_SRC_ROOT}")
        elseif (EXISTS "${PA_SRC_ROOT}/build/config.h")
            set(PA_CONFIG_DIR "${PA_SRC_ROOT}/build")
        else()
            message(FATAL_ERROR "Could not find pulse audio config at ${PA_SRC_ROOT}")
        endif()
    endif()
    if (NOT EXISTS "${PA_SRC_ROOT}/src/pulsecore")
        message(FATAL_ERROR "Could not find pulsecore source at ${PA_SRC_ROOT}")
    endif()
    set(PA_INCLUDE_DIRS
        "${PA_CONFIG_DIR}"
        "${PA_SRC_ROOT}/src")
else()
    find_path(PULSECORE_DIR pulsecore/config.h)
    if (NOT PULSECORE_DIR)
        message(FATAL_ERROR "Could not find pulsecore header in default path")
    endif()
    set(PA_INCLUDE_DIRS ${PULSECORE_DIR})
endif()


include_directories(${DBUS_INCLUDE_DIRS})

option(ENABLE_CODE_COVERAGE "Enable code coverage" OFF)
if (ENABLE_CODE_COVERAGE)
    add_compile_options(-O0 --coverage)
    link_libraries(--coverage)
endif()

option(ENABLE_ASAN "Enable address sanitizer" OFF)
if (ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    link_libraries(-fsanitize=address)
endif()

# g++ doesn't define that macro, but pulseaudio make uses of it, in particular
# for the log functions.
add_definitions(-D__STDC_VERSION__=201112L)

# Allow "server side" definitions
add_definitions(-D__INCLUDED_FROM_PULSE_AUDIO)

add_compile_options(-Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)
link_libraries("-Wl,--as-needed")

# Create extra variables for convience to distinguish between the "object
# library" and the executable
set(PROJECT_LIBNAME lib${PROJECT_NAME})

# Create the "object library" and executable (without sources yet)
add_library(${PROJECT_LIBNAME} OBJECT
    clock.h
    enums.h
    group_manager.cc
    group_manager.h
    group_sink.h
    group_sink_ctrl.cc
    group_sink_ctrl.h
    ts_renderer.h
    ts_renderer_ctrl.cc
    ts_renderer_ctrl.h
    pulsecore_config.h
    ts_logging.cc
    ts_logging.h
    )
target_include_directories(${PROJECT_LIBNAME} SYSTEM PUBLIC
    ${PA_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
    )
install(FILES group_sink.h ts_renderer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/pulsemodules)

# Group Sink module
add_library(module-group-sink SHARED
    module_group_sink.cc
    $<TARGET_OBJECTS:${PROJECT_LIBNAME}>
    )
target_include_directories(module-group-sink PRIVATE $<TARGET_PROPERTY:${PROJECT_LIBNAME},INCLUDE_DIRECTORIES>)
target_link_libraries(module-group-sink
    Threads::Threads
    )
target_compile_definitions(module-group-sink PRIVATE PA_MODULE_NAME=module_group_sink)
set_target_properties(module-group-sink PROPERTIES PREFIX "")
install(TARGETS module-group-sink DESTINATION ${LIBPULSE_MODDIR})

# TS Renderer module
add_library(module-ts-renderer SHARED
    module_ts_renderer.cc
    $<TARGET_OBJECTS:${PROJECT_LIBNAME}>
    )
target_include_directories(module-ts-renderer PRIVATE $<TARGET_PROPERTY:${PROJECT_LIBNAME},INCLUDE_DIRECTORIES>)
target_link_libraries(module-ts-renderer
    Threads::Threads
    )
target_compile_definitions(module-ts-renderer PRIVATE PA_MODULE_NAME=module_ts_renderer)
set_target_properties(module-ts-renderer PROPERTIES PREFIX "")
install(TARGETS module-ts-renderer DESTINATION ${LIBPULSE_MODDIR})

# Group Manager module
add_library(module-group-manager SHARED
    module_group_manager.cc
    $<TARGET_OBJECTS:${PROJECT_LIBNAME}>
    )
target_include_directories(module-group-manager PRIVATE $<TARGET_PROPERTY:${PROJECT_LIBNAME},INCLUDE_DIRECTORIES>)
target_link_libraries(module-group-manager
    Threads::Threads
    )
target_compile_definitions(module-group-manager PRIVATE PA_MODULE_NAME=module_group_manager)
set_target_properties(module-group-manager PROPERTIES PREFIX "")
install(TARGETS module-group-manager DESTINATION ${LIBPULSE_MODDIR})
