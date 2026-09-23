cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(luna LANGUAGES C)
set(CMAKE_C_STANDARD 11)

# custom modules
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
message(STATUS "CMAKE_MODULE_PATH:${CMAKE_MODULE_PATH}")
# option
option(ENABLE_UNIT_TESTS "Enable unit tests" ON)
message(STATUS "Enable testing :${ENABLE_UNIT_TESTS}")
# git submodules
option(CHECKOUT_GIT_SUBMODULE "Check out git submodules during build" ON)
message(STATUS "CHECKOUT_GIT_SUBMODULE:${CHECKOUT_GIT_SUBMODULE}")
include(FetchSubmodules)

# official Lua 5.4 (deps/lua, pinned tag v5.4.9)
# core sources live at the repo root; exclude the standalone
# interpreter/amalgamation/debug-driver translation units.
file(GLOB LUA_CORE_SOURCES "${PROJECT_SOURCE_DIR}/deps/lua/*.c")
list(FILTER LUA_CORE_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")
list(LENGTH LUA_CORE_SOURCES LUA_CORE_COUNT)
message(STATUS "lua core sources: ${LUA_CORE_COUNT}")

add_library(lualib STATIC ${LUA_CORE_SOURCES})
target_include_directories(lualib PUBLIC "${PROJECT_SOURCE_DIR}/deps/lua")
# libm: implicit on macOS, explicit on Linux (pow/log/...); dl: loadlib.c
if(UNIX)
    target_link_libraries(lualib PUBLIC m ${CMAKE_DL_LIBS})
endif()

include_directories("${PROJECT_SOURCE_DIR}/deps/lua")

# thin C kernel exposed to Lua as require "kernel"
add_library(luna_kernel STATIC src/luna_kernel.c src/luna_kernel.h)
target_include_directories(luna_kernel PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_BINARY_DIR}/generated")

# embed the entry plus policy modules into the binary (raw strings)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna.lua" LUNA_LUA_ENTRY)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/repl.lua" LUNA_LUA_REPL)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/complete.lua" LUNA_LUA_COMPLETE)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/introspect.lua" LUNA_LUA_INTROSPECT)
# re-configure when the embedded sources change
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/lua/luna.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/repl.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/complete.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/introspect.lua")
configure_file("${PROJECT_SOURCE_DIR}/cmake/luna_lua.h.in"
               "${PROJECT_BINARY_DIR}/generated/luna_lua.h" @ONLY)
include_directories("${PROJECT_BINARY_DIR}/generated")

list(APPEND luna_sources
        src/luna.c
        src/luna.h
        src/luna_main.c
        src/luna_hook.c
        src/luna_hook.h
)

add_executable(luna ${luna_sources})
target_link_libraries(luna lualib luna_kernel)

# bundled modules: argparse is vendored as a submodule and staged into
# the build tree under luna_modules/ so require "argparse" resolves the
# same way user project modules will.
set(LUNA_MODULES_DIR "${CMAKE_BINARY_DIR}/luna_modules")
file(MAKE_DIRECTORY "${LUNA_MODULES_DIR}/argparse")
file(COPY "${PROJECT_SOURCE_DIR}/deps/argparse/src/argparse.lua"
     DESTINATION "${LUNA_MODULES_DIR}/argparse")
file(WRITE "${LUNA_MODULES_DIR}/argparse/init.lua"
     "return require \"argparse.argparse\"\n")

target_compile_definitions(luna PRIVATE
        LUNA_MODULES_DIR="${LUNA_MODULES_DIR}"
        LUNA_SO_EXT="${CMAKE_SHARED_LIBRARY_SUFFIX}")

if (ENABLE_UNIT_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
