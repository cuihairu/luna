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

# official Lua 5.5 (deps/lua, pinned tag v5.5.1)
# core sources live at the repo root; exclude the standalone
# interpreter/amalgamation/debug-driver translation units.
file(GLOB LUA_CORE_SOURCES "${PROJECT_SOURCE_DIR}/deps/lua/*.c")
list(FILTER LUA_CORE_SOURCES EXCLUDE REGEX "/(lua|onelua|ltests)\\.c$")
list(LENGTH LUA_CORE_SOURCES LUA_CORE_COUNT)
message(STATUS "lua core sources: ${LUA_CORE_COUNT}")

add_library(lualib STATIC ${LUA_CORE_SOURCES})
target_include_directories(lualib PUBLIC "${PROJECT_SOURCE_DIR}/deps/lua")
# io.popen: std Lua on POSIX; luna.rocks needs it (luarocks shells out
# to curl/unzip), and scripts get it like any standard Lua. Lua 5.5's
# liolib gates popen behind LUA_USE_POSIX (the old LUA_USE_POPEN knob
# is gone).
target_compile_definitions(lualib PUBLIC LUA_USE_POSIX)
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
# the wake helper thread behind `luna --attach` (see luna_line.c)
find_package(Threads REQUIRED)
target_link_libraries(luna_line PUBLIC Threads::Threads)

# ---- stdlib module backends (mature bindings, batch 8) -----------------
find_package(ZLIB REQUIRED)
#
# fs   -> luafilesystem v1_9_0 (keplerproject), single translation unit
add_library(lfs STATIC deps/luafilesystem/src/lfs.c)
target_link_libraries(lfs PUBLIC lualib)

# net/http -> luasocket v3.1.0 (diegonehab); socket.core and mime.core
# share the support files and live in one static library. The unix
# domain and serial transports are not bundled.
set(LUASOCKET_SOURCES
        deps/luasocket/src/luasocket.c deps/luasocket/src/timeout.c
        deps/luasocket/src/buffer.c deps/luasocket/src/io.c
        deps/luasocket/src/auxiliar.c deps/luasocket/src/compat.c
        deps/luasocket/src/options.c deps/luasocket/src/inet.c
        deps/luasocket/src/usocket.c deps/luasocket/src/except.c
        deps/luasocket/src/select.c deps/luasocket/src/tcp.c
        deps/luasocket/src/udp.c deps/luasocket/src/mime.c
        # unix domain sockets: the attach/serve channel (luna --attach)
        deps/luasocket/src/unix.c deps/luasocket/src/unixstream.c
        deps/luasocket/src/unixdgram.c)
add_library(luasocket STATIC ${LUASOCKET_SOURCES})
target_compile_definitions(luasocket PRIVATE LUASOCKET_INET_PTON)
target_link_libraries(luasocket PUBLIC lualib)

# zlib -> lua-zlib v1.4 (brimworks), bound to the system zlib
add_library(lua_zlib STATIC deps/lua-zlib/lua_zlib.c)
target_link_libraries(lua_zlib PUBLIC lualib ZLIB::ZLIB)

# crypto -> luaossl rel-20260910 (wahern): requires OpenSSL development
# headers; when absent, luna builds without crypto and the module
# reports how to enable it.
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    message(STATUS "OpenSSL found: crypto module enabled (${OPENSSL_VERSION})")
    add_library(luaossl STATIC deps/luaossl/src/openssl.c)
    target_link_libraries(luaossl PUBLIC lualib OpenSSL::SSL OpenSSL::Crypto)
    set(LUNA_HAVE_OPENSSL 1)
else()
    message(STATUS "OpenSSL NOT found: building without the crypto module")
endif()

# thin C kernel exposed to Lua as require "kernel"
add_library(luna_kernel STATIC src/luna_kernel.c src/luna_kernel.h)
target_include_directories(luna_kernel PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_BINARY_DIR}/generated")

# libuv (deps/libuv, v1.44.2): the opt-in event loop behind require
# "loop" (src/luna_loop.c). Static only — luna stays a single binary.
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
# libuv declares cmake_minimum_required(3.4); CMake >= 4 refuses to run
# it as-is. Newer CMake reads this variable, older ones ignore it.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("${PROJECT_SOURCE_DIR}/deps/libuv" EXCLUDE_FROM_ALL)

# the loop module: Node-named timers/immediates over uv, with the
# attach poll and ^C handling wired into the prepare hook
add_library(luna_loop STATIC src/luna_loop.c src/luna_loop.h)
target_include_directories(luna_loop PUBLIC
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/deps/libuv/include")
target_link_libraries(luna_loop PUBLIC uv_a)
# net.connectTls rides on OpenSSL when it is around; without it the
# loop builds fine and connectTls reports how to enable TLS
if(LUNA_HAVE_OPENSSL)
    target_compile_definitions(luna_loop PUBLIC LUNA_LOOP_HAVE_OPENSSL=1)
    target_link_libraries(luna_loop PUBLIC OpenSSL::SSL)
endif()

# embed the entry plus policy modules into the binary (raw strings)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna.lua" LUNA_LUA_ENTRY)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/repl.lua" LUNA_LUA_REPL)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/complete.lua" LUNA_LUA_COMPLETE)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/introspect.lua" LUNA_LUA_INTROSPECT)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/highlight.lua" LUNA_LUA_HIGHLIGHT)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/magic.lua" LUNA_LUA_MAGIC)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/modules.lua" LUNA_LUA_MODULES)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/plugins.lua" LUNA_LUA_PLUGINS)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/rocks.lua" LUNA_LUA_ROCKS)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/serve.lua" LUNA_LUA_SERVE)
# re-configure when the embedded sources change
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/lua/luna.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/repl.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/complete.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/introspect.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/highlight.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/magic.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/modules.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/plugins.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/rocks.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/serve.lua")
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
target_link_libraries(luna lualib lpeg luna_line luna_kernel luna_loop
        lfs luasocket lua_zlib)
if(LUNA_HAVE_OPENSSL)
    target_link_libraries(luna luaossl)
endif()
set_target_properties(luna PROPERTIES LINKER_LANGUAGE CXX)

# vendored LuaRocks sources for `luna install/search/list/update`:
# staged into the build tree (configure-time copy, like luna_modules/)
# and located through kernel.vendor_dir at runtime. The wrapper embeds
# them into luna's own VM - no system luarocks needed.
set(LUNA_VENDOR_DIR "${CMAKE_BINARY_DIR}/luna_vendor/luarocks/src")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/luna_vendor/luarocks")
file(COPY "${PROJECT_SOURCE_DIR}/deps/luarocks/src"
     DESTINATION "${CMAKE_BINARY_DIR}/luna_vendor/luarocks")
# one src/?.lua path element resolves both the core (luarocks/...) and
# everything vendored inside it (luarocks/vendor/argparse, dkjson,
# compat53) — the release tree keeps them under src/
#
# luarocks also insists on a real `lua` interpreter (its
# cfg.variables.LUA): build the standalone one from the same vendored
# sources, so `luna install` works on machines with no system lua.
add_executable(lua_host ${LUA_CORE_SOURCES} "${PROJECT_SOURCE_DIR}/deps/lua/lua.c")
set_target_properties(lua_host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/luna_vendor/bin"
        OUTPUT_NAME lua
        LINKER_LANGUAGE CXX)
target_link_libraries(lua_host PRIVATE lualib)
# on the kernel target: that is the translation unit that publishes
# both paths to Lua as kernel.vendor_dir / kernel.lua_host
target_compile_definitions(luna_kernel PUBLIC
    LUNA_VENDOR_DIR="${LUNA_VENDOR_DIR}"
    LUNA_LUA_HOST="${CMAKE_BINARY_DIR}/luna_vendor/bin/lua")

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

# stdlib modules (batch 8). Thin wrappers from lua/modules/<name>/ are
# staged as luna_modules/<name>/; vendored pure-Lua backends sit beside
# them so require "json"/"fs"/... resolve through the same tree user
# packages will. The wrapper sources are declared as configure
# dependencies: file(COPY) runs at configure time only, so edits must
# re-configure to land in the build tree. Nested files (loop/http.lua,
# batch 33) stage into luna_modules/loop/ the same way — require
# "loop.http" resolves through the ? -> / translation.
file(GLOB LUNA_STDLIB_WRAPPERS CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/lua/modules/*/init.lua"
        "${PROJECT_SOURCE_DIR}/lua/modules/*/*.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${LUNA_STDLIB_WRAPPERS})
file(COPY "${PROJECT_SOURCE_DIR}/lua/modules/"
     DESTINATION "${LUNA_MODULES_DIR}")

# dkjson is vendored as a single file (see deps/dkjson/README.md)
file(COPY "${PROJECT_SOURCE_DIR}/deps/dkjson/dkjson.lua"
     DESTINATION "${LUNA_MODULES_DIR}")

# luasocket's Lua layer in the upstream layout: socket.lua/ltn12.lua/
# mime.lua at the top, socket/{http,url,...} in a subdirectory. The C
# halves (socket.core, mime.core) resolve from package.loaded,
# registered in luna_main.
foreach(_f socket ltn12 mime)
    configure_file("${PROJECT_SOURCE_DIR}/deps/luasocket/src/${_f}.lua"
                   "${LUNA_MODULES_DIR}/${_f}.lua" COPYONLY)
endforeach()
file(MAKE_DIRECTORY "${LUNA_MODULES_DIR}/socket")
foreach(_f http url headers tp ftp smtp)
    configure_file("${PROJECT_SOURCE_DIR}/deps/luasocket/src/${_f}.lua"
                   "${LUNA_MODULES_DIR}/socket/${_f}.lua" COPYONLY)
endforeach()

# luaossl's Lua layer: openssl.lua becomes the package entry, the
# openssl.*.lua submodules map to openssl/<sub>.lua.
if(LUNA_HAVE_OPENSSL)
    file(MAKE_DIRECTORY "${LUNA_MODULES_DIR}/openssl")
    configure_file("${PROJECT_SOURCE_DIR}/deps/luaossl/src/openssl.lua"
                   "${LUNA_MODULES_DIR}/openssl/init.lua" COPYONLY)
    file(GLOB LUAOSSL_LUA_SOURCES
            "${PROJECT_SOURCE_DIR}/deps/luaossl/src/openssl.*.lua")
    foreach(_f ${LUAOSSL_LUA_SOURCES})
        get_filename_component(_name "${_f}" NAME)
        string(REGEX REPLACE "^openssl\\.(.*)\\.lua$" "\\1" _sub "${_name}")
        configure_file("${_f}"
                       "${LUNA_MODULES_DIR}/openssl/${_sub}.lua" COPYONLY)
    endforeach()
endif()

target_compile_definitions(luna PRIVATE
        LUNA_MODULES_DIR="${LUNA_MODULES_DIR}"
        LUNA_LEXERS_DIR="${LUNA_MODULES_DIR}/lexers"
        LUNA_SO_EXT="${CMAKE_SHARED_LIBRARY_SUFFIX}")
if(LUNA_HAVE_OPENSSL)
    target_compile_definitions(luna PRIVATE LUNA_HAVE_OPENSSL=1)
endif()

if (ENABLE_UNIT_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
