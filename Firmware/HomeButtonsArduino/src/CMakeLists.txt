# This fork targets Home Buttons Original (model A1) only, so every source
# file under src/ is compiled. Upstream removed per-variant directories here;
# the mini/pro/industrial variants and src/touch no longer exist.

# NB: this file is also evaluated during component_get_requirements, a phase
# where HOME_BUTTONS_ORIGINAL is not yet defined, so it must not assert on it.
# config.h #errors at compile time if the build flag is missing.
message(STATUS "HOME_BUTTONS_ORIGINAL: ${HOME_BUTTONS_ORIGINAL}")

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)

foreach(src_file ${app_sources})
    message(STATUS "Source file: ${src_file}")
endforeach()

idf_component_register(SRCS ${app_sources})
