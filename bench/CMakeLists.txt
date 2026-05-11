# Benchmark driver — single executable that runs one (shape, mode,
# iterations) trial and prints a one-line CSV summary. The companion
# run.sh script forks this binary across shapes × modes and formats
# the results as a markdown table.
#
# Not registered as a ctest target: benchmark timings are too noisy to
# gate releases on. Invoke via `make bench` (or run.sh directly).

add_executable(betl_bench bench.c)
target_link_libraries(betl_bench PRIVATE betl_core)
# Plugins dlopen'd at runtime expect betl_log / betl_get_param /
# betl_set_error to resolve back to symbols in the host binary —
# same model as the main `betl` CLI.
set_target_properties(betl_bench PROPERTIES ENABLE_EXPORTS ON)

# `make bench` — run the full sweep and write docs/BENCHMARKS.md.
add_custom_target(bench
    COMMAND ${CMAKE_SOURCE_DIR}/bench/run.sh "$<TARGET_FILE:betl_bench>"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS betl_bench
    USES_TERMINAL
    COMMENT "Running benchmark sweep — see docs/BENCHMARKS.md"
)
