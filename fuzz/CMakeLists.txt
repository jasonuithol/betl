# Fuzz harnesses. Reaches here only when BETL_FUZZ=ON, which forces the
# whole build to use clang + coverage + ASan + UBSan via the top-level
# CMakeLists. Fuzz binaries link with -fsanitize=fuzzer (no -no-link)
# to pull in libFuzzer's driver; everything else uses fuzzer-no-link
# (instrumentation only, no driver).

function(betl_add_fuzz_target name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} PRIVATE betl_core)
    # -fsanitize=fuzzer pulls in libFuzzer's driver. -rdynamic exports
    # the harness's symbol table so plugin .so's loaded via dlopen
    # (expr_fuzz dlopens betl-ssisexpr.so) can resolve their sanitizer-
    # coverage imports (__sancov_lowest_stack et al). Without it, dlopen
    # fails with "undefined symbol: __sancov_lowest_stack".
    target_link_options  (${name} PRIVATE -fsanitize=fuzzer -rdynamic)
endfunction()

betl_add_fuzz_target(yaml_fuzz)
betl_add_fuzz_target(csv_fuzz)
betl_add_fuzz_target(json_fuzz)

# expr_fuzz drives the betl-ssisexpr provider (loaded at runtime via
# dlopen), so the .so must exist when the harness starts. Add an
# explicit target dependency so `ninja expr_fuzz` rebuilds the plugin
# first.
betl_add_fuzz_target(expr_fuzz)
add_dependencies(expr_fuzz betl-ssisexpr)
