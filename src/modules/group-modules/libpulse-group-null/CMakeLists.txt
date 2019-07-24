cmake_minimum_required(VERSION 3.1)

project(pulse-group-null
    VERSION 2.0
    LANGUAGES CXX C
    )

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPULSE REQUIRED libpulse)
pkg_get_variable(LIBPULSE_MODDIR libpulse modlibexecdir)

find_path(PULSECORE_DIR pulsecore/config.h)
if (NOT PULSECORE_DIR)
    message(FATAL_ERROR "Could not find pulsecore headers in default path")
endif()
set(PA_INCLUDE_DIRS ${PULSECORE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../modules)

# Allow "server side" definitions
add_definitions(-D__INCLUDED_FROM_PULSE_AUDIO)

add_compile_options(-Wall -Wextra -fvisibility=hidden)
link_libraries("-Wl,--as-needed")

# Create extra variables for convience to distinguish between the "object
# library" and the executable
set(PROJECT_LIBNAME lib${PROJECT_NAME})

# Create the "object library" and executable (without sources yet)
add_library(${PROJECT_LIBNAME} OBJECT
    ts_renderer_impl.h
    ts_renderer_impl.c
    group_sink_impl.h
    group_sink_impl.c
    )
target_include_directories(${PROJECT_LIBNAME} SYSTEM PUBLIC
    ${PA_INCLUDE_DIRS}
    )

# Shared library
add_library(${PROJECT_NAME} SHARED
    $<TARGET_OBJECTS:${PROJECT_LIBNAME}>
    )
target_include_directories(${PROJECT_NAME} PRIVATE $<TARGET_PROPERTY:${PROJECT_LIBNAME},INCLUDE_DIRECTORIES>)
install(TARGETS ${PROJECT_NAME} DESTINATION ${LIBPULSE_MODDIR})
