# Fuzz harnesses. Reaches here only when BETL_FUZZ=ON, which forces the
# whole build to use clang + coverage + ASan + UBSan via the top-level
# CMakeLists. Fuzz binaries link with -fsanitize=fuzzer (no -no-link)
# to pull in libFuzzer's driver; everything else uses fuzzer-no-link
# (instrumentation only, no driver).

function(betl_add_fuzz_target name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE betl_core)
    target_link_options  (${name} PRIVATE -fsanitize=fuzzer)
endfunction()

betl_add_fuzz_target(yaml_fuzz)
betl_add_fuzz_target(csv_fuzz)
if(CJSON_LIBRARY)
    betl_add_fuzz_target(json_fuzz)
endif()
