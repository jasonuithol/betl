# betl-lua — Lua 5.4 language-host provider, built as a runtime-loaded
# MODULE library. Picked up by the host via dlopen at registry load.
#
# We deliberately bypass pkg_check_modules: the lua5.4.pc shipped by the
# Debian liblua5.4-dev package hardcodes a multiarch libdir
# (${prefix}/lib/x86_64-linux-gnu) that doesn't exist after install_dep
# unpacks the .deb into deps/, where libraries land flat in deps/lib.
# find_path / find_library with explicit hints points at what's there.
find_path(LUA_INCLUDE_DIR lua.h
    HINTS ${CMAKE_SOURCE_DIR}/deps/include
    PATH_SUFFIXES lua5.4
)
find_library(LUA_LIBRARY NAMES lua5.4
    HINTS ${CMAKE_SOURCE_DIR}/deps/lib
)

if(NOT LUA_INCLUDE_DIR OR NOT LUA_LIBRARY)
    message(STATUS "Lua 5.4 not found in deps/ — betl-lua provider will be skipped")
    set(BETL_LUA_FOUND FALSE PARENT_SCOPE)
    return()
endif()
set(BETL_LUA_FOUND TRUE PARENT_SCOPE)
set(BETL_LUA_TARGET betl-lua PARENT_SCOPE)

message(STATUS "Found Lua 5.4: ${LUA_LIBRARY} (headers: ${LUA_INCLUDE_DIR})")

add_library(betl-lua MODULE lua_provider.c)
# Debian's liblua5.4-dev splits its config across luaconf.h and a
# multiarch shim (lua5.4-deb-multiarch.h) under /include/x86_64-linux-gnu.
# install_dep extracts that file too, so just put the multiarch dir on
# the include path; on systems without it the find_path above would
# already have failed.
target_include_directories(betl-lua PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LUA_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/deps/include/x86_64-linux-gnu
)
target_link_libraries(betl-lua PRIVATE ${LUA_LIBRARY})
set_target_properties(betl-lua PROPERTIES
    PREFIX ""                       # produces betl-lua.so, not libbetl-lua.so
    C_VISIBILITY_PRESET hidden
)
