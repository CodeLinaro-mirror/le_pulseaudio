cmake_minimum_required(VERSION 3.1)


project(module-pulseaudio-effects-sink
    VERSION 2.0
    LANGUAGES CXX C
    )
add_subdirectory(gstpostaudiorate)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


include(GNUInstallDirs)
set(ENV{PKG_CONFIG_PATH} "${PKG_CONFIG_SYSROOT_DIR}/${libdir}/pkgconfig/")
# !! Thread detection must be done before adding the asan parameters.
# pthread detection doesn't work correctly with "-fsanitize=address" because
# libasan provides its own (but incomplete) pthread implementation (GCC 5.4.0
# at least).
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(LIBPULSE REQUIRED libpulse)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-plugins-base-1.0 gstreamer-app-1.0 gstreamer-audio-1.0)
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
include_directories(${GSTREAMER_INCLUDE_DIRS})

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

# Define the module name
string(REPLACE - _ PA_MODULE_NAME ${PROJECT_NAME})
add_definitions(-DPA_MODULE_NAME=${PA_MODULE_NAME})

add_compile_options(-Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)
link_libraries("-Wl,--as-needed")


add_library(${PROJECT_NAME} SHARED
    src/module-filter-sink.c
    )
target_include_directories(${PROJECT_NAME} SYSTEM PUBLIC
    ${DBUS_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${PA_INCLUDE_DIRS}
    )

set(PROJECT_LIBNAME lib${PROJECT_NAME})
set(PROJECT_LIBNAME_LDFLAGS
    ${GSTREAMER_LIBRARIES}
    )

target_link_libraries(${PROJECT_NAME} ${CMAKE_THREAD_LIBS_INIT} ${PROJECT_LIBNAME_LDFLAGS} ${PA_LIBRARIES} ${DBUS_LIBRARIES} ${LIBPULSE_LIBRARIES} -ldl Threads::Threads)

set_target_properties( PROPERTIES PREFIX "")
install(TARGETS module-pulseaudio-effects-sink DESTINATION lib)


