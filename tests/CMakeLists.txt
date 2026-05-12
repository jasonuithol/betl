# Public-header smoke test: confirm provider.h is self-contained and
# every advertised symbol is reachable.
add_executable(test_provider_header test_provider_header.c)
target_include_directories(test_provider_header PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
add_test(NAME provider_header COMMAND test_provider_header)

# Smoke tests of the betl binary itself.
add_test(NAME cli_version
    COMMAND betl --version
)
set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^betl 0\\."
)

add_test(NAME validate_example_01
    COMMAND betl validate ${CMAKE_SOURCE_DIR}/examples/01-csv-to-postgres/pipeline.betl.yml
)
add_test(NAME validate_example_02
    COMMAND betl validate ${CMAKE_SOURCE_DIR}/examples/02-build-sales-star/pipeline.betl.yml
)
add_test(NAME validate_example_03
    COMMAND betl validate ${CMAKE_SOURCE_DIR}/examples/03-crm-migration/pipeline.betl.yml
)
add_test(NAME validate_example_04
    COMMAND betl validate ${CMAKE_SOURCE_DIR}/examples/04-ssis-orders-by-month/pipeline.betl.yml
)
add_test(NAME validate_connections_bundle
    COMMAND betl validate ${CMAKE_SOURCE_DIR}/examples/03-crm-migration/connections.betl.yml
)

# Negative: a YAML file with no betl discriminator must be rejected.
file(WRITE ${CMAKE_BINARY_DIR}/tests/not_betl.yml "foo: bar\nbaz: 1\n")
add_test(NAME validate_rejects_non_betl
    COMMAND betl validate ${CMAKE_BINARY_DIR}/tests/not_betl.yml
)
set_tests_properties(validate_rejects_non_betl PROPERTIES
    WILL_FAIL TRUE
)

# --- Loader ------------------------------------------------------------------
# A test-only provider that contributes one TASK component. Built as a
# MODULE library — appropriate for dlopen targets, won't be linked against.
add_library(betl-test-echo MODULE plugins/echo_provider.c)
target_include_directories(betl-test-echo PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(betl-test-echo PROPERTIES
    PREFIX ""              # no "lib" prefix on .so
    C_VISIBILITY_PRESET hidden
)

add_executable(test_loader test_loader.c)
target_link_libraries(test_loader PRIVATE betl_core)
# Bake the plugin path as a fallback so the test still has work to do
# when invoked without argv (e.g. by `c-build analyze`).
target_compile_definitions(test_loader PRIVATE
    BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:betl-test-echo>"
)
add_dependencies(test_loader betl-test-echo)

add_test(NAME loader
    COMMAND test_loader $<TARGET_FILE:betl-test-echo>
)

# --- Host context (BetlContext) end-to-end through the loader. --------------
# A test-only provider whose single TASK component calls every betl_*
# extern from inside its task_run, so the test can verify the host-side
# implementations resolve and behave correctly.
add_library(betl-test-ctx-probe MODULE plugins/ctx_probe_provider.c)
target_include_directories(betl-test-ctx-probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(betl-test-ctx-probe PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
)

add_executable(test_context test_context.c)
target_link_libraries(test_context PRIVATE betl_core)
# Plugin loaded into this binary needs to resolve host externs.
set_target_properties(test_context PROPERTIES ENABLE_EXPORTS ON)
target_compile_definitions(test_context PRIVATE
    BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:betl-test-ctx-probe>"
)
add_dependencies(test_context betl-test-ctx-probe)

add_test(NAME context
    COMMAND test_context $<TARGET_FILE:betl-test-ctx-probe>
)

# --- Pipeline parser ---------------------------------------------------------
add_executable(test_pipeline test_pipeline.c)
target_link_libraries(test_pipeline PRIVATE betl_core)
target_compile_definitions(test_pipeline PRIVATE
    BETL_TEST_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples"
    BETL_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
)
add_test(NAME pipeline
    COMMAND test_pipeline
        ${CMAKE_SOURCE_DIR}/examples
        ${CMAKE_SOURCE_DIR}/tests/fixtures
)

# --- Executor end-to-end -----------------------------------------------------
# Runs an in-memory pipeline through the parser, the registry (with
# built-in test providers registered), the runtime context, and the
# executor.
add_executable(test_exec test_exec.c)
target_link_libraries(test_exec PRIVATE betl_core)
target_compile_definitions(test_exec PRIVATE
    BETL_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
)
add_test(NAME exec COMMAND test_exec)

# `betl run` end-to-end via the CLI.
add_test(NAME cli_run
    COMMAND betl run ${CMAKE_SOURCE_DIR}/tests/fixtures/run_demo.yml
)
set_tests_properties(cli_run PROPERTIES
    PASS_REGULAR_EXPRESSION "run ok"
)

# Verifies the CLI auto-loads provider plugins from the dev build tree
# (build/providers/PLUGIN/betl-PLUGIN.so). The pipeline uses `lang:
# ssisexpr` in a `map add:` block — if the plugin didn't load, the run
# would fail with "expression engine 'ssisexpr' not found".
if(BETL_SSISEXPR_FOUND)
    add_test(NAME cli_run_ssisexpr
        COMMAND betl run ${CMAKE_SOURCE_DIR}/tests/fixtures/run_ssisexpr.yml
    )
    set_tests_properties(cli_run_ssisexpr PROPERTIES
        PASS_REGULAR_EXPRESSION "run ok"
        DEPENDS ${BETL_SSISEXPR_TARGET}
    )
endif()

# csv.read end-to-end via the CLI. Demonstrates a real `parameters:`
# block + ${params.X} substitution: --param fixtures_dir=... is passed
# in, the YAML interpolates it into the csv.read step's `path:`.
add_test(NAME cli_run_csv
    COMMAND betl run ${CMAKE_SOURCE_DIR}/tests/fixtures/csv_demo.yml
            --param fixtures_dir=${CMAKE_SOURCE_DIR}/tests/fixtures
)
set_tests_properties(cli_run_csv PROPERTIES
    PASS_REGULAR_EXPRESSION "run ok"
)

# Same pipeline without the required --param: must fail at apply-parameters.
add_test(NAME cli_run_csv_missing_required
    COMMAND betl run ${CMAKE_SOURCE_DIR}/tests/fixtures/csv_demo.yml
)
set_tests_properties(cli_run_csv_missing_required PROPERTIES
    WILL_FAIL TRUE
)

# A --param override naming an undeclared parameter is a typo guard.
add_test(NAME cli_run_csv_unknown_override
    COMMAND betl run ${CMAKE_SOURCE_DIR}/tests/fixtures/csv_demo.yml
            --param fixtures_dir=${CMAKE_SOURCE_DIR}/tests/fixtures
            --param totally_bogus=42
)
set_tests_properties(cli_run_csv_unknown_override PROPERTIES
    WILL_FAIL TRUE
)

# `betl run` against a pipeline whose connection references an unset
# env var. Should exit non-zero with a clear error mentioning the
# variable name. Drives `betl_apply_connections` through the CLI.
file(WRITE ${CMAKE_BINARY_DIR}/tests/missing_env.yml
"betl: 1
name: missing-env
connections:
  warehouse:
    type: postgres
    dsn: \${env.BETL_NONEXISTENT_VAR_FOR_TESTS}
pipeline:
  - id: stage_one
    type: dataflow
    steps:
      - id: source
        type: betl.gen_int64
        row_count: 1
      - id: sink
        type: betl.count_rows
        from: source
")
add_test(NAME cli_run_missing_env
    COMMAND betl run ${CMAKE_BINARY_DIR}/tests/missing_env.yml
)
set_tests_properties(cli_run_missing_env PROPERTIES
    WILL_FAIL TRUE
)

# --- Lua provider end-to-end ------------------------------------------------
# The provider is built in providers/betl-lua. The parent scope sets
# BETL_LUA_FOUND / BETL_LUA_TARGET only when Lua 5.4 was located in deps/.
if(BETL_LUA_FOUND)
    add_executable(test_lua_task test_lua_task.c)
    target_link_libraries(test_lua_task PRIVATE betl_core)
    # The plugin dlopen'd into this binary needs to resolve betl_log /
    # betl_set_error / betl_get_param / betl_get_connection back into
    # the host. ENABLE_EXPORTS is the portable spelling of -rdynamic.
    set_target_properties(test_lua_task PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(test_lua_task PRIVATE
        BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
    )
    add_dependencies(test_lua_task ${BETL_LUA_TARGET})
    add_test(NAME lua_task
        COMMAND test_lua_task $<TARGET_FILE:${BETL_LUA_TARGET}>
    )

    # End-to-end: gen_int64 → lua.map → count_rows.
    add_executable(test_lua_map test_lua_map.c)
    target_link_libraries(test_lua_map PRIVATE betl_core)
    set_target_properties(test_lua_map PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(test_lua_map PRIVATE
        BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
    )
    add_dependencies(test_lua_map ${BETL_LUA_TARGET})
    add_test(NAME lua_map
        COMMAND test_lua_map $<TARGET_FILE:${BETL_LUA_TARGET}>
    )

    # End-to-end: dotnet.task hello-world (v0.2 — first slice of the
    # SSIS Script Component replacement). SKIPs (rc=77) when the .NET
    # SDK isn't installed in deps/dotnet/ — run scripts/install-dotnet.sh
    # to enable.
    if(BETL_DOTNET_FOUND)
        add_executable(test_dotnet_task test_dotnet_task.c)
        target_link_libraries(test_dotnet_task PRIVATE betl_core)
        set_target_properties(test_dotnet_task PROPERTIES ENABLE_EXPORTS ON)
        target_compile_definitions(test_dotnet_task PRIVATE
            BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_DOTNET_TARGET}>"
        )
        add_dependencies(test_dotnet_task ${BETL_DOTNET_TARGET})
        add_test(NAME dotnet_task
            COMMAND test_dotnet_task $<TARGET_FILE:${BETL_DOTNET_TARGET}>
        )
        set_tests_properties(dotnet_task PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "ok: dotnet.task hello-world test passed"
            # AOT compile can be slow on a cold NuGet cache; budget 3 min.
            TIMEOUT 180
        )
    endif()

    # End-to-end: lua.script — the SSIS async script component analogue.
    add_executable(test_lua_script test_lua_script.c)
    target_link_libraries(test_lua_script PRIVATE betl_core)
    set_target_properties(test_lua_script PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(test_lua_script PRIVATE
        BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
    )
    add_dependencies(test_lua_script ${BETL_LUA_TARGET})
    add_test(NAME lua_script
        COMMAND test_lua_script $<TARGET_FILE:${BETL_LUA_TARGET}>
    )
    set_tests_properties(lua_script PROPERTIES
        PASS_REGULAR_EXPRESSION "ok: lua.script test passed"
    )

    # SPEC §7 expression engine: drive compile + evaluate directly.
    add_executable(test_lua_expr test_lua_expr.c)
    target_link_libraries(test_lua_expr PRIVATE betl_core)
    set_target_properties(test_lua_expr PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(test_lua_expr PRIVATE
        BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
    )
    add_dependencies(test_lua_expr ${BETL_LUA_TARGET})
    add_test(NAME lua_expr
        COMMAND test_lua_expr $<TARGET_FILE:${BETL_LUA_TARGET}>
    )

    # Standard transforms — filter + map driven by literal/lua engines.
    add_executable(test_transforms test_transforms.c)
    target_link_libraries(test_transforms PRIVATE betl_core)
    set_target_properties(test_transforms PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(test_transforms PRIVATE
        BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
        BETL_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    )
    add_dependencies(test_transforms ${BETL_LUA_TARGET})
    add_test(NAME transforms
        COMMAND test_transforms $<TARGET_FILE:${BETL_LUA_TARGET}>
    )
endif()

# --- SSIS Expression Language engine ----------------------------------------
# The provider is built in providers/betl-ssisexpr. It is self-contained
# (no external runtime), so the gating is always-true here — kept as a
# parallel form to the lua block so future tooling can ignore it the
# same way.
if(BETL_SSISEXPR_FOUND)
    add_executable(test_ssisexpr test_ssisexpr.c)
    target_link_libraries(test_ssisexpr PRIVATE betl_core)
    set_target_properties(test_ssisexpr PROPERTIES ENABLE_EXPORTS ON)
    target_compile_definitions(test_ssisexpr PRIVATE
        BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_SSISEXPR_TARGET}>"
    )
    add_dependencies(test_ssisexpr ${BETL_SSISEXPR_TARGET})
    add_test(NAME ssisexpr
        COMMAND test_ssisexpr $<TARGET_FILE:${BETL_SSISEXPR_TARGET}>
    )
endif()

# --- pipeline parallelism (async_stream wrapper) ----------------------------
# Same canonical pipeline run with three queue depths to stress the
# producer/consumer sync. Registered three times with distinct env vars
# so the depth-1 case (most synchronization, smallest queue) runs even
# in the default sweep.
add_executable(test_pipeline_parallel test_pipeline_parallel.c)
target_link_libraries(test_pipeline_parallel PRIVATE betl_core)
add_test(NAME pipeline_parallel_default
    COMMAND test_pipeline_parallel)
set_tests_properties(pipeline_parallel_default PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: pipeline_parallel test passed")
add_test(NAME pipeline_parallel_depth1
    COMMAND test_pipeline_parallel)
set_tests_properties(pipeline_parallel_depth1 PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: pipeline_parallel test passed"
    ENVIRONMENT "BETL_PARALLEL=on;BETL_PARALLEL_DEPTH=1")
add_test(NAME pipeline_parallel_off
    COMMAND test_pipeline_parallel)
set_tests_properties(pipeline_parallel_off PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: pipeline_parallel test passed"
    ENVIRONMENT "BETL_PARALLEL=off")

# --- csv.read v2 end-to-end (streaming + RFC 4180 quoting) ------------------
add_executable(test_csv_read test_csv_read.c)
target_link_libraries(test_csv_read PRIVATE betl_core)
add_test(NAME csv_read COMMAND test_csv_read)
set_tests_properties(csv_read PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: csv.read v2 end-to-end test passed"
)

# --- pivot / unpivot transforms ---------------------------------------------
add_executable(test_pivot_unpivot test_pivot_unpivot.c)
target_link_libraries(test_pivot_unpivot PRIVATE betl_core)
add_test(NAME pivot_unpivot COMMAND test_pivot_unpivot)
set_tests_properties(pivot_unpivot PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: pivot/unpivot tests passed"
)

# --- csv.write end-to-end ---------------------------------------------------
# No Lua dependency: the test pipelines are gen_int64 / gen_strings → csv.write,
# and the assertions compare exact file bytes after the pipeline runs.
add_executable(test_csv_write test_csv_write.c)
target_link_libraries(test_csv_write PRIVATE betl_core)
add_test(NAME csv_write COMMAND test_csv_write)
set_tests_properties(csv_write PROPERTIES
    PASS_REGULAR_EXPRESSION "ok: csv.write end-to-end test passed"
)

# --- mssql.upsert SQL (MERGE) builder unit test -----------------------------
# Pure string code — compiles even when ODBC isn't found, since mssql_sql.c
# is always part of betl_core.
add_executable(test_mssql_sql test_mssql_sql.c)
target_link_libraries(test_mssql_sql PRIVATE betl_core)
add_test(NAME mssql_sql COMMAND test_mssql_sql)
set_tests_properties(mssql_sql PROPERTIES
    PASS_REGULAR_EXPRESSION "^ok: mssql_sql unit tests passed"
)

# --- MSSQL connectivity smoke test ------------------------------------------
# Built only when unixODBC was found at configure time. Skips on no DB.
if(ODBC_FOUND)
    add_executable(test_mssql_smoke test_mssql_smoke.c)
    target_link_libraries(test_mssql_smoke PRIVATE betl_odbc)
    add_test(NAME mssql_smoke COMMAND test_mssql_smoke)
    set_tests_properties(mssql_smoke PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "^ok: mssql server says "
    )

    # --- mssql.upsert end-to-end integration test ---------------------------
    add_executable(test_mssql_upsert test_mssql_upsert.c)
    target_link_libraries(test_mssql_upsert PRIVATE betl_core betl_odbc)
    add_test(NAME mssql_upsert COMMAND test_mssql_upsert)
    set_tests_properties(mssql_upsert PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: mssql_upsert integration test passed"
    )

    # --- mssql.lookup end-to-end integration test ---------------------------
    if(BETL_LUA_FOUND)
        add_executable(test_mssql_lookup test_mssql_lookup.c)
        target_link_libraries(test_mssql_lookup PRIVATE betl_core betl_odbc)
        set_target_properties(test_mssql_lookup PROPERTIES ENABLE_EXPORTS ON)
        target_compile_definitions(test_mssql_lookup PRIVATE
            BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
        )
        add_dependencies(test_mssql_lookup ${BETL_LUA_TARGET})
        add_test(NAME mssql_lookup
            COMMAND test_mssql_lookup $<TARGET_FILE:${BETL_LUA_TARGET}>
        )
        set_tests_properties(mssql_lookup PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "ok: mssql_lookup integration test passed"
        )

        # --- mssql.read end-to-end integration test -------------------------
        add_executable(test_mssql_read test_mssql_read.c)
        target_link_libraries(test_mssql_read PRIVATE betl_core betl_odbc)
        set_target_properties(test_mssql_read PROPERTIES ENABLE_EXPORTS ON)
        target_compile_definitions(test_mssql_read PRIVATE
            BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
        )
        add_dependencies(test_mssql_read ${BETL_LUA_TARGET})
        add_test(NAME mssql_read
            COMMAND test_mssql_read $<TARGET_FILE:${BETL_LUA_TARGET}>
        )
        set_tests_properties(mssql_read PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "ok: mssql_read integration test passed"
        )
    endif()

    # --- mssql.read date / datetime2 integration test -----------------------
    add_executable(test_mssql_read_date test_mssql_read_date.c)
    target_link_libraries(test_mssql_read_date PRIVATE betl_core betl_odbc)
    add_test(NAME mssql_read_date COMMAND test_mssql_read_date)
    set_tests_properties(mssql_read_date PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: mssql_read date/timestamp integration test passed"
    )

    # --- mssql.upsert date / datetime2 roundtrip test -----------------------
    add_executable(test_mssql_upsert_date test_mssql_upsert_date.c)
    target_link_libraries(test_mssql_upsert_date PRIVATE betl_core betl_odbc)
    add_test(NAME mssql_upsert_date COMMAND test_mssql_upsert_date)
    set_tests_properties(mssql_upsert_date PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: mssql_upsert date/datetime2 roundtrip test passed"
    )

    # --- mssql.read streaming stress test (no Lua) --------------------------
    # Verifies VmHWM stays bounded across a 2MB result set with a small
    # batch_size — proves nothing along ODBC / FreeTDS silently buffers
    # the whole result. Built as `stress_*` rather than `test_*` so the
    # c-build `analyze` sweep skips it (libtdsodbc keeps internal
    # buffers around that valgrind flags as definite leaks for
    # large-ish result sets — that's a property of the driver, not of
    # our code, and irrelevant to what this test is asserting).
    add_executable(stress_mssql_read_stream test_mssql_read_stream.c)
    target_link_libraries(stress_mssql_read_stream PRIVATE betl_core betl_odbc)
    add_test(NAME mssql_read_stream COMMAND stress_mssql_read_stream)
    set_tests_properties(mssql_read_stream PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: mssql_read_stream test passed"
    )
endif()

# --- Postgres connectivity smoke test ---------------------------------------
# Built only when libpq was found at configure time. The test exits 77 if
# it can't reach the database, so absence of a sibling Postgres turns
# into Skipped rather than Failed.
if(LIBPQ_FOUND)
    add_executable(test_pg_smoke test_pg_smoke.c)
    target_link_libraries(test_pg_smoke PRIVATE betl_libpq)
    add_test(NAME pg_smoke COMMAND test_pg_smoke)
    set_tests_properties(pg_smoke PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "^ok: pg server says PostgreSQL "
    )

    # --- postgres.upsert SQL builder unit test ------------------------------
    # No libpq linkage needed — pure string code in pg_sql.c. We still gate
    # the test on LIBPQ_FOUND to keep it next to the other PG tests.
    add_executable(test_pg_sql test_pg_sql.c)
    target_link_libraries(test_pg_sql PRIVATE betl_core)
    add_test(NAME pg_sql COMMAND test_pg_sql)
    set_tests_properties(pg_sql PROPERTIES
        PASS_REGULAR_EXPRESSION "^ok: pg_sql unit tests passed"
    )

    # --- postgres.upsert end-to-end integration test ------------------------
    # Drives gen_int64 → postgres.upsert against the sibling Postgres,
    # verifies via libpq, drops its scratch schema. Skips on no DB.
    add_executable(test_pg_upsert test_pg_upsert.c)
    target_link_libraries(test_pg_upsert PRIVATE betl_core betl_libpq)
    add_test(NAME pg_upsert COMMAND test_pg_upsert)
    set_tests_properties(pg_upsert PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg_upsert integration test passed"
    )

    # --- postgres `lookup` end-to-end integration test ----------------------
    # Drives gen_int64 → lookup → lua.map (logs) → count_rows against the
    # sibling Postgres. Loads betl-lua.so so the verification step can run.
    if(BETL_LUA_FOUND)
        add_executable(test_pg_lookup test_pg_lookup.c)
        target_link_libraries(test_pg_lookup PRIVATE betl_core betl_libpq)
        set_target_properties(test_pg_lookup PROPERTIES ENABLE_EXPORTS ON)
        target_compile_definitions(test_pg_lookup PRIVATE
            BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
        )
        add_dependencies(test_pg_lookup ${BETL_LUA_TARGET})
        add_test(NAME pg_lookup
            COMMAND test_pg_lookup $<TARGET_FILE:${BETL_LUA_TARGET}>
        )
        set_tests_properties(pg_lookup PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "ok: pg_lookup integration test passed"
        )

        # --- postgres.read end-to-end integration test -----------------------
        add_executable(test_pg_read test_pg_read.c)
        target_link_libraries(test_pg_read PRIVATE betl_core betl_libpq)
        set_target_properties(test_pg_read PROPERTIES ENABLE_EXPORTS ON)
        target_compile_definitions(test_pg_read PRIVATE
            BETL_TEST_PLUGIN_PATH="$<TARGET_FILE:${BETL_LUA_TARGET}>"
        )
        add_dependencies(test_pg_read ${BETL_LUA_TARGET})
        add_test(NAME pg_read
            COMMAND test_pg_read $<TARGET_FILE:${BETL_LUA_TARGET}>
        )
        set_tests_properties(pg_read PROPERTIES
            SKIP_RETURN_CODE 77
            PASS_REGULAR_EXPRESSION "ok: pg_read integration test passed"
        )
    endif()

    # --- postgres.read date / timestamp integration test --------------------
    add_executable(test_pg_read_date test_pg_read_date.c)
    target_link_libraries(test_pg_read_date PRIVATE betl_core betl_libpq)
    add_test(NAME pg_read_date COMMAND test_pg_read_date)
    set_tests_properties(pg_read_date PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg_read date/timestamp integration test passed"
    )

    # --- postgres.upsert date / timestamp roundtrip test --------------------
    add_executable(test_pg_upsert_date test_pg_upsert_date.c)
    target_link_libraries(test_pg_upsert_date PRIVATE betl_core betl_libpq)
    add_test(NAME pg_upsert_date COMMAND test_pg_upsert_date)
    set_tests_properties(pg_upsert_date PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg_upsert date/timestamp roundtrip test passed"
    )

    # --- postgres.upsert NUMERIC roundtrip test -----------------------------
    add_executable(test_pg_upsert_decimal test_pg_upsert_decimal.c)
    target_link_libraries(test_pg_upsert_decimal PRIVATE betl_core betl_libpq)
    add_test(NAME pg_upsert_decimal COMMAND test_pg_upsert_decimal)
    set_tests_properties(pg_upsert_decimal PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg_upsert decimal roundtrip test passed"
    )

    # --- postgres.upsert UUID roundtrip test --------------------------------
    add_executable(test_pg_upsert_uuid test_pg_upsert_uuid.c)
    target_link_libraries(test_pg_upsert_uuid PRIVATE betl_core betl_libpq)
    add_test(NAME pg_upsert_uuid COMMAND test_pg_upsert_uuid)
    set_tests_properties(pg_upsert_uuid PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg_upsert uuid roundtrip test passed"
    )

    # --- postgres.upsert BYTEA roundtrip test -------------------------------
    add_executable(test_pg_upsert_binary test_pg_upsert_binary.c)
    target_link_libraries(test_pg_upsert_binary PRIVATE betl_core betl_libpq)
    add_test(NAME pg_upsert_binary COMMAND test_pg_upsert_binary)
    set_tests_properties(pg_upsert_binary PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg_upsert binary roundtrip test passed"
    )

    # --- narrow-int / float32 roundtrip test --------------------------------
    add_executable(test_pg_upsert_narrow test_pg_upsert_narrow.c)
    target_link_libraries(test_pg_upsert_narrow PRIVATE betl_core betl_libpq)
    add_test(NAME pg_upsert_narrow COMMAND test_pg_upsert_narrow)
    set_tests_properties(pg_upsert_narrow PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg narrow-int roundtrip test passed"
    )

    # --- postgres.read streaming stress test (no Lua) -----------------------
    # Verifies VmHWM stays bounded across a 2MB result set with a small
    # batch_size — proves the libpq cursor path actually streams rather
    # than buffering the whole result client-side. Built as `stress_*`
    # to keep it out of the `analyze` valgrind sweep (libpq's internal
    # bookkeeping around large result sets is irrelevant to this test).
    add_executable(stress_pg_read_stream test_pg_read_stream.c)
    target_link_libraries(stress_pg_read_stream PRIVATE betl_core betl_libpq)
    add_test(NAME pg_read_stream COMMAND stress_pg_read_stream)
    set_tests_properties(pg_read_stream PROPERTIES
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "ok: pg_read_stream test passed"
    )
endif()
