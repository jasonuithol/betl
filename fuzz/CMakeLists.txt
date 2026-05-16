# Fuzz harness build. Driven by clang via add_custom_command so the
# main gcc build is unaffected. Each fuzz binary links against the
# gcc-built libbetl_core.a — ASan from clang's runtime still catches
# heap bugs across that boundary because the AddressSanitizer hooks
# global malloc, regardless of which compiler instrumented the caller.
# Coverage-guided mutation is limited to the harness file itself
# (since betl_core isn't compiled with -fsanitize=fuzzer-no-link),
# but the bug oracle still fires on any out-of-bounds / use-after-free
# / leak the harness drags out of the parser.
#
# BETL_CLANG_BIN is set by the parent CMakeLists.

set(FUZZ_INCLUDES
    -I${CMAKE_SOURCE_DIR}/include
    -I${CMAKE_SOURCE_DIR}/src
    -I${CMAKE_BINARY_DIR}/include
)

set(FUZZ_FLAGS
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -O1 -g
    -D_POSIX_C_SOURCE=200809L
    -D_XOPEN_SOURCE=700
)

# Link flags — pull in the static archive directly. The order matters:
# fuzz harness object → libbetl_core.a → external libs. We list every
# library libbetl_core may have been built against; -Wl,--as-needed
# drops the unused ones so this is safe even when LIBPQ_FOUND etc. is
# false.
set(FUZZ_LINK_LIBS
    ${CMAKE_BINARY_DIR}/libbetl_core.a
    -Wl,--as-needed
    -lyaml
    -ldl
    -lpthread
    -lm
)
# Check the find_library cache variables directly — those are globally
# visible. The companion `*_FOUND` flags are set in the parent dir with
# `set(VAR TRUE)` (no CACHE), so they're invisible here.
if(LIBPQ_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${LIBPQ_LIBRARY})
endif()
if(ODBC_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${ODBC_LIBRARY})
endif()
if(SYBDB_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${SYBDB_LIBRARY})
endif()
if(XLSXWRITER_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${XLSXWRITER_LIBRARY})
endif()
if(XLSXIO_READ_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${XLSXIO_READ_LIBRARY})
endif()
if(LIBXML2_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${LIBXML2_LIBRARY})
endif()
if(CJSON_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${CJSON_LIBRARY})
endif()
if(LIBCURL_LIBRARY)
    list(APPEND FUZZ_LINK_LIBS ${LIBCURL_LIBRARY})
endif()

# Macro: build one fuzz binary. `name` is the harness basename (no
# extension); the source is `<name>.c` in this directory.
# Custom-command output path must differ from the custom-target name,
# otherwise ninja sees two rules generating the same node. We name the
# binary `<name>` (run-fuzz.sh expects build/fuzz/yaml_fuzz etc.) and
# the alias target `build_<name>`.
function(betl_add_fuzz_target name)
    set(_src ${CMAKE_CURRENT_SOURCE_DIR}/${name}.c)
    set(_out ${CMAKE_CURRENT_BINARY_DIR}/${name})
    add_custom_command(
        OUTPUT ${_out}
        COMMAND ${BETL_CLANG_BIN}
                ${FUZZ_FLAGS}
                ${FUZZ_INCLUDES}
                ${_src}
                ${FUZZ_LINK_LIBS}
                -o ${_out}
        DEPENDS ${_src} betl_core
        COMMENT "[fuzz] clang ${name}"
        VERBATIM
    )
    add_custom_target(build_${name} ALL DEPENDS ${_out})
endfunction()

betl_add_fuzz_target(yaml_fuzz)
betl_add_fuzz_target(csv_fuzz)
if(CJSON_LIBRARY)
    betl_add_fuzz_target(json_fuzz)
endif()
