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
# is gone). POSIX-only: MSVC's luaconf.h #errors on the define
# ("POSIX is not compatible with C89"), so Windows builds without popen
# until the port decides its story (windows probe, nightly matrix).
if(NOT WIN32)
    target_compile_definitions(lualib PUBLIC LUA_USE_POSIX)
endif()
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
# replxx_impl comes from the patched copy next door: upstream turns a
# 0-byte read (a ^D the line discipline ate between reads, owed back to
# the next one) into an empty-line commit, so the user's end of input
# vanishes and a console closed from its prompt never closes. The
# patched copy bails with EOF instead when the buffer is empty — see
# the note in that file.
list(REMOVE_ITEM REPLXX_SOURCES
     "${PROJECT_SOURCE_DIR}/deps/replxx/src/replxx_impl.cxx")
list(APPEND REPLXX_SOURCES
     "${PROJECT_SOURCE_DIR}/deps/replxx-patched/replxx_impl.cxx")
list(LENGTH REPLXX_SOURCES REPLXX_SOURCE_COUNT)
add_library(replxx STATIC ${REPLXX_SOURCES})
target_include_directories(replxx PUBLIC "${PROJECT_SOURCE_DIR}/deps/replxx/include"
                          PRIVATE "${PROJECT_SOURCE_DIR}/deps/replxx/src")
# replxx.h decorates its API with __declspec(dllimport) on _WIN32 unless
# REPLXX_STATIC is set — without it MSVC consumers link against
# __imp_replxx_* import thunks that a static lib never defines (and the
# lib's own sources emit C4273 "inconsistent dll linkage"). PUBLIC so
# luna_line and the test shim see the same plain declarations.
# windows.cxx (win_write) is #ifdef _WIN32 all the way through and is
# referenced by terminal.cxx's _WIN32 branch, so it must stay in the
# source list for Windows links (empty TU elsewhere).
target_compile_definitions(replxx PUBLIC REPLXX_STATIC)

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
# share the support files and live in one static library. The serial
# transport is not bundled; the socket transport is platform-selected
# the way upstream's own socket.vcxproj does it: winsock (wsocket.c)
# on Windows, usocket + the unix domain transport elsewhere —
# socket.unix is the attach/serve channel (luna --attach) and has no
# Windows counterpart, so those three translation units are skipped
# wholesale rather than patched.
set(LUASOCKET_SOURCES
        deps/luasocket/src/luasocket.c deps/luasocket/src/timeout.c
        deps/luasocket/src/buffer.c deps/luasocket/src/io.c
        deps/luasocket/src/auxiliar.c deps/luasocket/src/compat.c
        deps/luasocket/src/options.c deps/luasocket/src/inet.c
        deps/luasocket/src/except.c deps/luasocket/src/select.c
        deps/luasocket/src/tcp.c
        deps/luasocket/src/udp.c deps/luasocket/src/mime.c)
if(WIN32)
    list(APPEND LUASOCKET_SOURCES deps/luasocket/src/wsocket.c)
else()
    list(APPEND LUASOCKET_SOURCES
            deps/luasocket/src/usocket.c
            # unix domain sockets: the attach/serve channel (luna --attach).
            # unixstream comes from the patched copy next door: upstream
            # binds with sizeof(sun_family) + len, one byte short on macOS
            # (1-byte sa_family_t), landing the socket file one character
            # before the path — see the note in that file
            deps/luasocket/src/unix.c
            deps/luasocket-patched/unixstream.c
            deps/luasocket/src/unixdgram.c)
endif()
add_library(luasocket STATIC ${LUASOCKET_SOURCES})
# the patched unixstream copy still compiles against the vendored headers
target_include_directories(luasocket PRIVATE "${PROJECT_SOURCE_DIR}/deps/luasocket/src")
if(NOT WIN32)
    # inet_pton/inet_ntop fallback for platforms without them. Upstream's
    # Windows build leaves this unset: ws2tcpip.h declares inet_pton with
    # WinSock linkage, and a cdecl redefinition here would clash.
    target_compile_definitions(luasocket PRIVATE LUASOCKET_INET_PTON)
endif()
target_link_libraries(luasocket PUBLIC lualib)
if(WIN32)
    # wsocket.c talks to the WinSock DLL (upstream socket.vcxproj links it)
    target_link_libraries(luasocket PUBLIC ws2_32)
endif()

# zlib -> lua-zlib v1.4 (brimworks), bound to the system zlib
add_library(lua_zlib STATIC deps/lua-zlib/lua_zlib.c)
target_link_libraries(lua_zlib PUBLIC lualib ZLIB::ZLIB)

# toml -> tomlc17 R260821 (cktan/tomlc17, submodule pinned by SHA):
# TOML 1.0 parser as a single C17 translation unit, no encoder — the
# Lua-facing decode binding lives in src/luna_toml.c and encode is the
# wrapper's own Lua face (scalar-first key ordering). No system deps.
add_library(tomlc17 STATIC deps/tomlc17/src/tomlc17.c)
target_include_directories(tomlc17 PUBLIC
        "${PROJECT_SOURCE_DIR}/deps/tomlc17/src")

# the decode binding over it (luaopen_toml_core)
add_library(luna_toml STATIC src/luna_toml.c)
target_link_libraries(luna_toml PUBLIC lualib tomlc17)

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

# yaml -> libyaml 0.2.5 + lyaml 6.2.9 (both submodules pinned by SHA):
# the reference C implementation of YAML behind lyaml's Lua binding.
# libyaml's own CMake builds the static lib (BUILD_SHARED_LIBS defaults
# off); its tests stay out of our ctest (BUILD_TESTING forced off for
# the subdirectory, CMP0077-observing), and its
# cmake_minimum_required(3.0) is grandfathered by the
# CMAKE_POLICY_VERSION_MINIMUM set for libuv above.
set(BUILD_TESTING OFF)
set(LUNA_LYAML_VERSION "6.2.9")
add_subdirectory("${PROJECT_SOURCE_DIR}/deps/libyaml" EXCLUDE_FROM_ALL)

# lyaml's C face (luaopen_yaml): four translation units over libyaml.
# Lua 5.5 upstream dropped lua_objlen/lua_strlen and lyaml.h only shims
# 5.2-5.4 — but the bundled luaconf.h still restores both as compat
# macros over lua_rawlen, so the binding compiles unpatched (zero
# vendor changes; only the lukefile's VERSION define is supplied here,
# pinned to the vendored tag for the MYVERSION string in yaml.c). The
# lukefile's NDEBUG/_FORTIFY_SOURCE are release flags, not reproduced.
add_library(luna_yaml STATIC
        deps/lyaml/ext/yaml/yaml.c
        deps/lyaml/ext/yaml/emitter.c
        deps/lyaml/ext/yaml/parser.c
        deps/lyaml/ext/yaml/scanner.c)
target_include_directories(luna_yaml PRIVATE
        "${PROJECT_SOURCE_DIR}/deps/lyaml/ext/yaml")
target_compile_definitions(luna_yaml PRIVATE
        VERSION="${LUNA_LYAML_VERSION}")
target_link_libraries(luna_yaml PUBLIC lualib yaml)

# xml -> expat 2.8.5 + lua-expat 1.5.2 (both submodules pinned by SHA):
# the reference SAX parser behind lxp's Lua binding. expat's upstream
# CMake comes in EXCLUDE_FROM_ALL like libuv; every public build knob
# (tests/tools/examples/docs/pkgconfig) is forced off before
# add_subdirectory so the xmlwf tool and expat's own ctest targets stay
# out of ours — expat_shy_set is shy toward pre-defined variables, so
# plain set() lands (no cache FORCE needed). EXPAT_SHARED_LIBS off
# pins the static lib (its default follows BUILD_SHARED_LIBS, which a
# fresh tree may not have defined — a shared expat would be a system
# dependency luna must not gain). EXPAT_ENABLE_INSTALL off keeps
# vendored install rules out of `cmake --install`.
set(EXPAT_SHARED_LIBS OFF)
set(EXPAT_BUILD_TESTS OFF)
set(EXPAT_BUILD_TOOLS OFF)
set(EXPAT_BUILD_EXAMPLES OFF)
set(EXPAT_BUILD_DOCS OFF)
set(EXPAT_BUILD_PKGCONFIG OFF)
set(EXPAT_ENABLE_INSTALL OFF)
add_subdirectory("${PROJECT_SOURCE_DIR}/deps/expat/expat" EXCLUDE_FROM_ALL)

# lua-expat's C face (luaopen_lxp): lxplib.c over expat's headers, which
# the expat target exposes (lib/ + the generated expat_config.h dir).
# Registered bare as "lxp" — the C module's own name; the user-facing
# "xml" module is the staged wrapper in luna_modules/xml/.
add_library(luna_xml STATIC deps/luaexpat/src/lxplib.c)
target_include_directories(luna_xml PRIVATE
        "${PROJECT_SOURCE_DIR}/deps/luaexpat/src")
target_link_libraries(luna_xml PUBLIC lualib expat)

# embed the entry plus policy modules into the binary
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
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/new.lua" LUNA_LUA_NEW)
file(READ "${PROJECT_SOURCE_DIR}/lua/luna/sandbox.lua" LUNA_LUA_SANDBOX)
# Each source becomes ordinary C string literals — one quoted line per
# source line, concatenated by the compiler. This header used to emit
# C++ raw string literals (R"…") instead: a C++-only feature that
# MSVC rejects the moment a C translation unit includes the header
# (612 of the 619 Windows CI errors, todo "新层 1"). The round trip
# must be byte-identical to the file on disk: escape backslashes
# first, then quotes, then CRs, and turn each source line break into
# "\n" between adjacent literals. (Raw content reaching configure_file
# was never transformed, so @VAR@ substitution exposure is unchanged.)
function(luna_c_string_literal out content)
    set(s "${content}")
    string(REPLACE "\\" "\\\\" s "${s}")
    string(REPLACE "\"" "\\\"" s "${s}")
    string(REPLACE "\r" "\\r" s "${s}")
    string(REPLACE "\n" "\\n\"\n\"" s "${s}")
    set(${out} "\"${s}\"" PARENT_SCOPE)
endfunction()
luna_c_string_literal(LUNA_LUA_ENTRY "${LUNA_LUA_ENTRY}")
luna_c_string_literal(LUNA_LUA_REPL "${LUNA_LUA_REPL}")
luna_c_string_literal(LUNA_LUA_COMPLETE "${LUNA_LUA_COMPLETE}")
luna_c_string_literal(LUNA_LUA_INTROSPECT "${LUNA_LUA_INTROSPECT}")
luna_c_string_literal(LUNA_LUA_HIGHLIGHT "${LUNA_LUA_HIGHLIGHT}")
luna_c_string_literal(LUNA_LUA_MAGIC "${LUNA_LUA_MAGIC}")
luna_c_string_literal(LUNA_LUA_MODULES "${LUNA_LUA_MODULES}")
luna_c_string_literal(LUNA_LUA_PLUGINS "${LUNA_LUA_PLUGINS}")
luna_c_string_literal(LUNA_LUA_ROCKS "${LUNA_LUA_ROCKS}")
luna_c_string_literal(LUNA_LUA_SERVE "${LUNA_LUA_SERVE}")
luna_c_string_literal(LUNA_LUA_NEW "${LUNA_LUA_NEW}")
luna_c_string_literal(LUNA_LUA_SANDBOX "${LUNA_LUA_SANDBOX}")
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
        "${PROJECT_SOURCE_DIR}/lua/luna/serve.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/new.lua"
        "${PROJECT_SOURCE_DIR}/lua/luna/sandbox.lua")
configure_file("${PROJECT_SOURCE_DIR}/cmake/luna_lua.h.in"
               "${PROJECT_BINARY_DIR}/generated/luna_lua.h" @ONLY)
include_directories("${PROJECT_BINARY_DIR}/generated")

# luacov wiring for coverage builds: the runner/reporter config plus the
# absolute source root and the luacov tree get compiled into luna and the
# test harnesses. Collection itself only activates when LUNA_COVERAGE is
# set in the environment — test/CMakeLists exports it for Profiling
# builds — so plain builds run exactly as before.
configure_file("${PROJECT_SOURCE_DIR}/cmake/luacov.config.in"
               "${PROJECT_BINARY_DIR}/luacov.config.lua" @ONLY)

list(APPEND luna_sources
        src/luna.c
        src/luna.h
        src/luna_main.c
        src/luna_hook.c
        src/luna_hook.h
)

add_executable(luna ${luna_sources})
target_link_libraries(luna lualib lpeg luna_line luna_kernel luna_loop
        lfs luasocket lua_zlib luna_toml luna_yaml luna_xml)
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

# C-side coverage report over the Profiling tree's .gcda (opt-in tool).
# The white-box main test's profiles are redirected into gcov-discarded/
# (see test/CMakeLists.txt) so the real binary stays the single source
# of truth for src/luna_main.c. Sweeping the test tree's coverage
# artifacts before the scan finishes the job: gcovr's filters cannot
# keep the white-box object out (its .gcno alone makes gcov emit an
# all-zero second line set for src/luna_main.c), and the harnesses'
# own gcda would merge their dependency-lib counts into the report.
# Harness binaries exist to pass/fail, not to report on src/.
# The scan root is the BINARY dir, not the source tree: sibling build
# trees (a plain `build/` is coverage-instrumented here too) carry their
# own white-box gcda for src/*.c, and a source-tree walk merges those
# ghost line sets into this report — observed 2026-09-28 as
# src/luna_main.c reading 64% (128/198, whole of main() dark) instead
# of the honest 90% (123/136) and dragging TOTAL to 93.4%.
add_custom_target(gcovr_summary
        COMMAND ${CMAKE_COMMAND} -E rm -rf
                ${PROJECT_BINARY_DIR}/gcov-discarded
        COMMAND find ${PROJECT_BINARY_DIR}/test -name *.gcda -delete
        COMMAND find ${PROJECT_BINARY_DIR}/test -name *.gcno -delete
        COMMAND gcovr -r ${PROJECT_BINARY_DIR}
                --object-directory ${PROJECT_BINARY_DIR}
                --filter ${PROJECT_SOURCE_DIR}/src/ --print-summary
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        VERBATIM)

# Lua-layer coverage report: merge + render the stats the ctest run left
# in the shared stats file (only produced by Profiling builds with
# LUNA_COVERAGE=1; an absent file makes the reporter fail loudly).
add_custom_target(lua_coverage
        COMMAND ${CMAKE_COMMAND} -E env
                "LUA_PATH=${PROJECT_SOURCE_DIR}/deps/luacov/src/?.lua;;"
                $<TARGET_FILE:lua_host>
                ${PROJECT_SOURCE_DIR}/deps/luacov/src/bin/luacov
                -c ${PROJECT_BINARY_DIR}/luacov.config.lua
        COMMAND ${CMAKE_COMMAND} -E env
                "LUA_PATH=${PROJECT_SOURCE_DIR}/deps/luacov/src/?.lua;;"
                $<TARGET_FILE:lua_host>
                ${PROJECT_SOURCE_DIR}/cmake/lua_cov_summary.lua
                ${PROJECT_BINARY_DIR}/luacov.report.out
        DEPENDS lua_host
        WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
        VERBATIM
        SOURCES ${PROJECT_SOURCE_DIR}/cmake/luacov.config.in
                ${PROJECT_SOURCE_DIR}/cmake/lua_cov_summary.lua)
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

# lualogging (deps/lualogging, tag v1.8.2): the `logging` core plus its
# log4j-style appenders, staged the same way so require "logging" and
# require "logging.file" resolve out of the box. Pure Lua; only
# logging.socket / logging.rsyslog touch LuaSocket (already bundled),
# and the optional drivers (sql's DBI, copas, ngx) are required lazily
# inside the functions that use them, so the staged set loads clean.
file(COPY "${PROJECT_SOURCE_DIR}/deps/lualogging/src/logging.lua"
     DESTINATION "${LUNA_MODULES_DIR}")
file(COPY "${PROJECT_SOURCE_DIR}/deps/lualogging/src/logging/"
     DESTINATION "${LUNA_MODULES_DIR}/logging")

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
        LUNA_SO_EXT="${CMAKE_SHARED_LIBRARY_SUFFIX}"
        LUNA_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
        LUNA_COV_CONFIG="${PROJECT_BINARY_DIR}/luacov.config.lua"
        LUNA_LUACOV_SRC="${PROJECT_SOURCE_DIR}/deps/luacov/src")
if(LUNA_HAVE_OPENSSL)
    target_compile_definitions(luna PRIVATE LUNA_HAVE_OPENSSL=1)
endif()

if (ENABLE_UNIT_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
