cmake_minimum_required(VERSION 3.20)

project(betl
    VERSION     0.1.0
    DESCRIPTION "Better ETL — open-source SSIS replacement"
    LANGUAGES   C
)

# --- Standards & flags -----------------------------------------------------
set(CMAKE_C_STANDARD          11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS        OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Provider plugins are dlopen'd at runtime; build host with default
# visibility so symbol export rules in headers behave consistently.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wshadow -Wstrict-prototypes -Wmissing-prototypes
        -Wno-unused-parameter
    )
endif()

# We use POSIX / XSI features (strdup, dlopen, realpath, ...) under
# -std=c11. The strict standard hides them by default; expose them
# globally. _XOPEN_SOURCE pulls in realpath from the SUS extensions.
add_compile_definitions(_POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)

# Opt-in sanitizers: -DBETL_SANITIZE=address|undefined|thread
if(BETL_SANITIZE)
    add_compile_options(-fsanitize=${BETL_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${BETL_SANITIZE})
endif()

# --- Dependencies ----------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)
# libpq is optional at top-level configure: when present we build the
# Postgres integration tests; when absent the rest of the project still
# builds cleanly. Tests gate themselves on LIBPQ_FOUND.
#
# We deliberately avoid pkg_check_modules here: libpq.pc declares
# Requires.private = libssl libcrypto, which we don't ship in deps/
# (we dynamic-link, so the runtime loader resolves them). pkg-config
# treats those as missing and fails the whole probe. find_path /
# find_library directly is what we actually need.
find_path(LIBPQ_INCLUDE_DIR libpq-fe.h
    HINTS ${CMAKE_SOURCE_DIR}/deps/include
    PATH_SUFFIXES postgresql
)
find_library(LIBPQ_LIBRARY NAMES pq
    HINTS ${CMAKE_SOURCE_DIR}/deps/lib
)
if(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
    set(LIBPQ_FOUND TRUE)
    add_library(betl_libpq INTERFACE)
    target_include_directories(betl_libpq INTERFACE ${LIBPQ_INCLUDE_DIR})
    target_link_libraries(betl_libpq INTERFACE ${LIBPQ_LIBRARY})
    message(STATUS "Found libpq: ${LIBPQ_LIBRARY} (headers: ${LIBPQ_INCLUDE_DIR})")
else()
    set(LIBPQ_FOUND FALSE)
    message(STATUS "libpq not found — Postgres tests will be skipped")
endif()

# unixODBC: same shape as libpq — we look up sql.h + libodbc.so directly.
# Available headers: sql.h, sqlext.h, sqltypes.h.
find_path(ODBC_INCLUDE_DIR sql.h
    HINTS ${CMAKE_SOURCE_DIR}/deps/include
)
find_library(ODBC_LIBRARY NAMES odbc
    HINTS ${CMAKE_SOURCE_DIR}/deps/lib
)
if(ODBC_INCLUDE_DIR AND ODBC_LIBRARY)
    set(ODBC_FOUND TRUE)
    add_library(betl_odbc INTERFACE)
    target_include_directories(betl_odbc INTERFACE ${ODBC_INCLUDE_DIR})
    # sqltypes.h #includes "unixodbc.h", which Debian packages under a
    # multiarch path. Add the shim dir if it exists so the headers
    # resolve under -std=c11.
    if(EXISTS ${CMAKE_SOURCE_DIR}/deps/include/x86_64-linux-gnu/unixodbc.h)
        target_include_directories(betl_odbc INTERFACE
            ${CMAKE_SOURCE_DIR}/deps/include/x86_64-linux-gnu)
    endif()
    target_link_libraries(betl_odbc INTERFACE ${ODBC_LIBRARY})
    message(STATUS "Found unixODBC: ${ODBC_LIBRARY} (headers: ${ODBC_INCLUDE_DIR})")
else()
    set(ODBC_FOUND FALSE)
    message(STATUS "unixODBC not found — MSSQL components will be omitted")
endif()

# --- Generated headers -----------------------------------------------------
configure_file(
    ${CMAKE_SOURCE_DIR}/include/betl/version.h.in
    ${CMAKE_BINARY_DIR}/include/betl/version.h
    @ONLY
)

# --- Core library (engine + loader + yaml). Linked by the CLI and tests. --
add_library(betl_core STATIC
    src/loader/registry.c
    src/pipeline/pipeline.c
    src/runtime/builtins.c
    src/runtime/connections.c
    src/runtime/context.c
    src/runtime/exec.c
    src/runtime/literal_expr.c
    src/runtime/mssql_sql.c
    src/runtime/parameters.c
    src/runtime/pg_sql.c
    src/runtime/substitute.c
    src/runtime/transforms.c
    src/runtime/transforms_common.c
    src/runtime/transform_filter.c
    src/runtime/transform_map.c
    src/runtime/transform_aggregate.c
    src/runtime/transform_sort.c
    src/runtime/transform_join.c
    src/yaml/yaml_load.c
)
target_include_directories(betl_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(betl_core PUBLIC PkgConfig::YAML ${CMAKE_DL_LIBS})

# Pull in postgres.upsert when libpq is available. The component is part
# of the core (not a separate plugin) for v0.1 — same model as csv.read.
if(LIBPQ_FOUND)
    target_sources(betl_core PRIVATE
        src/runtime/postgres_upsert.c
        src/runtime/postgres_lookup.c
    )
    target_link_libraries(betl_core PUBLIC betl_libpq)
    target_compile_definitions(betl_core PUBLIC BETL_HAVE_LIBPQ)
endif()

# Pull in mssql.upsert when unixODBC is available. Same model as the
# postgres integration — the component is part of the core.
if(ODBC_FOUND)
    target_sources(betl_core PRIVATE
        src/runtime/mssql_upsert.c
        src/runtime/mssql_lookup.c
        src/runtime/mssql_read.c
    )
    target_link_libraries(betl_core PUBLIC betl_odbc)
    target_compile_definitions(betl_core PUBLIC BETL_HAVE_ODBC)
endif()

# --- Engine binary ---------------------------------------------------------
add_executable(betl
    src/cli/main.c
    src/cli/cmd_run.c
    src/cli/cmd_validate.c
)
target_link_libraries(betl PRIVATE betl_core)
# Plugins dlopen'd into this binary need to resolve betl_log / betl_set_error
# / betl_should_cancel / betl_get_param / betl_get_connection back into the
# host. ENABLE_EXPORTS is the portable spelling of -rdynamic.
set_target_properties(betl PROPERTIES ENABLE_EXPORTS ON)

# --- Provider plugins ------------------------------------------------------
# Each provider is a separate shared library, dlopen'd by the host at
# registry load. The subdirectory may set BETL_LUA_FOUND / BETL_LUA_TARGET
# in the parent scope so tests can gate on availability.
add_subdirectory(providers/betl-lua)

# --- Tests -----------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# --- Install ---------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS betl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/betl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY schemas/     DESTINATION ${CMAKE_INSTALL_DATADIR}/betl/schemas)
