cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(luna LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

# LPeg 1.1.0 (deps/lpeg): pattern-matching engine required by the
# scintillua lexers; exposed to Lua as require "lpeg" from luna_main.
file(GLOB LPEG_SOURCES "${PROJECT_SOURCE_DIR}/deps/lpeg/*.c")
list(LENGTH LPEG_SOURCES LPEG_SOURCE_COUNT)
add_library(lpeg STATIC ${LPEG_SOURCES})
target_include_directories(lpeg PUBLIC "${PROJECT_SOURCE_DIR}/deps/lpeg")

# replxx (deps/replxx, release-0.0.4): line editing + history for the
# interactive console; the C bridge lives in src/luna_line.c.
file(GLOB REPLXX_SOURCES "${PROJECT_SOURCE_DIR}/deps/replxx/src/*.cxx"
     "${PROJECT_SOURCE_DIR}/deps/replxx/src/*.cpp")
list(FILTER REPLXX_SOURCES EXCLUDE REGEX "/windows\\.cxx$")
list(LENGTH REPLXX_SOURCES REPLXX_SOURCE_COUNT)
add_library(replxx STATIC ${REPLXX_SOURCES})
target_include_directories(replxx PUBLIC "${PROJECT_SOURCE_DIR}/deps/replxx/include")

add_library(luna_line STATIC src/luna_line.c src/luna_line.h)
target_link_libraries(luna_line PUBLIC replxx lualib)

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
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/highlight.lua" LUNA_LUA_HIGHLIGHT)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/magic.lua" LUNA_LUA_MAGIC)
# re-configure when the embedded sources change
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/lua/luna.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/repl.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/complete.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/introspect.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/highlight.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/magic.lua")
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
target_link_libraries(luna lualib lpeg luna_line luna_kernel)
set_target_properties(luna PROPERTIES LINKER_LANGUAGE CXX)

# bundled modules: argparse is vendored as a submodule and staged into
# the build tree under luna_modules/ so require "argparse" resolves the
# same way user project modules will.
set(LUNA_MODULES_DIR "${CMAKE_BINARY_DIR}/luna_modules")
file(MAKE_DIRECTORY "${LUNA_MODULES_DIR}/argparse")
file(COPY "${PROJECT_SOURCE_DIR}/deps/argparse/src/argparse.lua"
     DESTINATION "${LUNA_MODULES_DIR}/argparse")
file(WRITE "${LUNA_MODULES_DIR}/argparse/init.lua"
     "return require \"argparse.argparse\"\n")

# scintillua's lexer engine and the Lua lexer are staged the same way:
# require "lexer" resolves through luna_modules/?.lua, and highlight
# adds luna_modules/lexers to package.path — scintillua derives its
# lexer-file search from package.path entries ending in /?.lua, so that
# one element serves both the engine and the lexer definitions.
file(MAKE_DIRECTORY "${LUNA_MODULES_DIR}/lexers")
file(COPY "${PROJECT_SOURCE_DIR}/deps/scintillua/lexers/lexer.lua"
     DESTINATION "${LUNA_MODULES_DIR}")
file(COPY "${PROJECT_SOURCE_DIR}/deps/scintillua/lexers/lexer.lua"
     DESTINATION "${LUNA_MODULES_DIR}/lexers")
file(COPY "${PROJECT_SOURCE_DIR}/deps/scintillua/lexers/lua.lua"
     DESTINATION "${LUNA_MODULES_DIR}/lexers")

target_compile_definitions(luna PRIVATE
        LUNA_MODULES_DIR="${LUNA_MODULES_DIR}"
        LUNA_LEXERS_DIR="${LUNA_MODULES_DIR}/lexers"
        LUNA_SO_EXT="${CMAKE_SHARED_LIBRARY_SUFFIX}")

if (ENABLE_UNIT_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
