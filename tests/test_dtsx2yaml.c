/* betl-dtsx2yaml end-to-end test.
 *
 * Runs the .NET converter against a hand-crafted DTSX fixture
 * (tools/betl-dtsx2yaml/fixtures/simple-oledb.dtsx) and asserts that
 * the resulting YAML contains the expected pieces:
 *
 *   - betl discriminator and package name
 *   - mssql connection with a translated ODBC DSN
 *   - parameter for the User-scope variable
 *   - Execute SQL Task → mssql.sql
 *   - dataflow with OLEDB Source → Flat File Destination
 *   - csv.write `from:` wired to the OLEDB Source's id
 *
 * Skips with rc=77 when the .NET SDK isn't installed (the
 * cmake-time gate already skips the converter build, but we
 * defend against running with a stale build dir).
 *
 * Paths to the dotnet binary, the publish dll, and the fixture
 * are baked in by CMake via -D defines. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK_CONTAINS(haystack, needle) do {                       \
    if (strstr((haystack), (needle)) == NULL) {                     \
        fprintf(stderr, "FAIL %s:%d: expected substring not found:" \
                " %s\n", __FILE__, __LINE__, (needle));             \
        failures++;                                                 \
    }                                                               \
} while (0)

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Run the converter against `fixture` writing to `out_path`. Returns
 * the converter binary's exit status (0 on success). */
static int run_convert(const char *fixture, const char *out_path) {
    char cmd[8192];
    int n = snprintf(cmd, sizeof cmd,
                     "'%s' '%s' '%s' -o '%s'",
                     BETL_DTSX2YAML_DOTNET,
                     BETL_DTSX2YAML_DLL,
                     fixture, out_path);
    if (n < 0 || (size_t)n >= sizeof cmd) return -1;
    return system(cmd);
}

int main(void) {
    /* SDK presence guard — match the test_dotnet_* convention. */
    struct stat st;
    if (stat(BETL_DTSX2YAML_DOTNET, &st) != 0) {
        fprintf(stderr, "SKIP: dotnet not at %s\n", BETL_DTSX2YAML_DOTNET);
        return 77;
    }
    if (stat(BETL_DTSX2YAML_DLL, &st) != 0) {
        fprintf(stderr, "SKIP: dtsx2yaml dll not at %s\n", BETL_DTSX2YAML_DLL);
        return 77;
    }

    /* --- simple-oledb.dtsx: end-to-end source/dest + ExecSQL ----------- */
    const char *out_path = "/tmp/betl_dtsx2yaml_test.yml";
    int rc = run_convert(BETL_DTSX2YAML_FIXTURE, out_path);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for simple-oledb fixture\n", rc);
        return 1;
    }

    char *yaml = slurp_file(out_path);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read converter output %s\n", out_path);
        return 1;
    }

    /* Top-level shape. */
    CHECK_CONTAINS(yaml, "betl: 1");
    CHECK_CONTAINS(yaml, "name: simpleoledb");

    /* Connections — OLEDB → mssql DSN, flat-file inlined as comment. */
    CHECK_CONTAINS(yaml, "connections:");
    CHECK_CONTAINS(yaml, "warehouse:");
    CHECK_CONTAINS(yaml, "type: mssql");
    CHECK_CONTAINS(yaml, "Driver={ODBC Driver 18 for SQL Server}");
    CHECK_CONTAINS(yaml, "Server=db.example.com");
    CHECK_CONTAINS(yaml, "Database=Sales");
    CHECK_CONTAINS(yaml, "Trusted_Connection=yes");
    /* Flat-file should NOT emit its own connections entry. */
    CHECK_CONTAINS(yaml, "flat-file connection 'OrdersCsv'");

    /* Parameters — User::BatchTag → string default 'nightly'. */
    CHECK_CONTAINS(yaml, "parameters:");
    CHECK_CONTAINS(yaml, "batchtag:");
    CHECK_CONTAINS(yaml, "type: string");
    CHECK_CONTAINS(yaml, "default: 'nightly'");

    /* Pipeline shape. */
    CHECK_CONTAINS(yaml, "pipeline:");
    CHECK_CONTAINS(yaml, "id: truncate_stage");
    CHECK_CONTAINS(yaml, "type: sql.execute");
    CHECK_CONTAINS(yaml, "TRUNCATE TABLE stage.Orders");

    CHECK_CONTAINS(yaml, "id: extract_orders");
    CHECK_CONTAINS(yaml, "type: dataflow");

    /* Source: mssql.read with the AccessMode=2 SqlCommand passed through. */
    CHECK_CONTAINS(yaml, "id: oledb_source");
    CHECK_CONTAINS(yaml, "type: mssql.read");
    CHECK_CONTAINS(yaml, "connection: warehouse");
    CHECK_CONTAINS(yaml, "SELECT * FROM dbo.Orders");

    /* Destination: csv.write wired to the source by id. */
    CHECK_CONTAINS(yaml, "id: flat_file_destination");
    CHECK_CONTAINS(yaml, "type: csv.write");
    CHECK_CONTAINS(yaml, "from: oledb_source");
    CHECK_CONTAINS(yaml, "path: '/tmp/orders.csv'");

    free(yaml);

    /* --- transforms.dtsx: every transform mapper ----------------------- */
    const char *tf_out = "/tmp/betl_dtsx2yaml_transforms.yml";
    rc = run_convert(BETL_DTSX2YAML_TRANSFORMS_FIXTURE, tf_out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for transforms fixture\n", rc);
        return 1;
    }
    yaml = slurp_file(tf_out);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read transforms YAML\n");
        return 1;
    }

    /* Derived Column → map+add with ssisexpr. */
    CHECK_CONTAINS(yaml, "id: derive");
    CHECK_CONTAINS(yaml, "type: map");
    CHECK_CONTAINS(yaml, "lang: ssisexpr");
    CHECK_CONTAINS(yaml, "[id] + \"-derived\"");

    /* Data Conversion → map+add with (DT_*) cast. */
    CHECK_CONTAINS(yaml, "id: convert");
    CHECK_CONTAINS(yaml, "'(DT_I4)[SOURCE]'");

    /* Conditional Split — cases in eval-order, default labeled. */
    CHECK_CONTAINS(yaml, "id: split");
    CHECK_CONTAINS(yaml, "type: conditional_split");
    CHECK_CONTAINS(yaml, "name: hot_path");
    CHECK_CONTAINS(yaml, "name: cold_path");
    CHECK_CONTAINS(yaml, "default: default");

    /* Sort — by precedence + direction sign. */
    CHECK_CONTAINS(yaml, "id: sortstep");
    CHECK_CONTAINS(yaml, "type: sort");
    CHECK_CONTAINS(yaml, "{ col: id, dir: asc }");
    CHECK_CONTAINS(yaml, "{ col: price_i4, dir: desc }");
    /* sortstep's input came from Split's hot_path output. */
    CHECK_CONTAINS(yaml, "from: split:hot_path");

    /* Aggregate — group_by + compute map. */
    CHECK_CONTAINS(yaml, "id: agg");
    CHECK_CONTAINS(yaml, "type: aggregate");
    CHECK_CONTAINS(yaml, "group_by: [priority]");
    CHECK_CONTAINS(yaml, "cnt: { agg: count }");
    CHECK_CONTAINS(yaml, "total: { agg: sum, over: TODO }");

    /* Multicast — taps list + downstream port-aware from-ref. */
    CHECK_CONTAINS(yaml, "id: fanout");
    CHECK_CONTAINS(yaml, "type: multicast");
    CHECK_CONTAINS(yaml, "taps: [tapa, tapb]");
    CHECK_CONTAINS(yaml, "from: fanout:tapa");
    CHECK_CONTAINS(yaml, "from: fanout:tapb");

    /* Lookup — mssql.lookup with query. */
    CHECK_CONTAINS(yaml, "id: lk");
    CHECK_CONTAINS(yaml, "type: mssql.lookup");
    CHECK_CONTAINS(yaml, "SELECT id, color FROM dim_color");

    /* Union All — from: [<cold>, <default>] in path order. */
    CHECK_CONTAINS(yaml, "id: unite");
    CHECK_CONTAINS(yaml, "type: union");
    CHECK_CONTAINS(yaml, "from: [split:cold_path, split:default]");

    /* Merge Join — kind: left (JoinType=1), from: [left, right]. */
    CHECK_CONTAINS(yaml, "id: mj");
    CHECK_CONTAINS(yaml, "type: join");
    CHECK_CONTAINS(yaml, "kind: left");
    CHECK_CONTAINS(yaml, "from: [sortstep, convert]");

    /* Pivot — id_cols / name_col / value_col / pivot_keys. */
    CHECK_CONTAINS(yaml, "id: pvt");
    CHECK_CONTAINS(yaml, "type: pivot");
    CHECK_CONTAINS(yaml, "id_cols: [region]");
    CHECK_CONTAINS(yaml, "name_col: month");
    CHECK_CONTAINS(yaml, "value_col: amount");
    CHECK_CONTAINS(yaml, "pivot_keys: [jan, feb]");

    free(yaml);

    /* --- scripts.dtsx: ScriptTask + ScriptComponent + VB.NET -------- */
    const char *sc_out = "/tmp/betl_dtsx2yaml_scripts.yml";
    rc = run_convert(BETL_DTSX2YAML_SCRIPTS_FIXTURE, sc_out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for scripts fixture\n", rc);
        return 1;
    }
    yaml = slurp_file(sc_out);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read scripts YAML\n");
        return 1;
    }

    /* C# Script Task: dotnet.task + lang csharp + translation header. */
    CHECK_CONTAINS(yaml, "id: logstart");
    CHECK_CONTAINS(yaml, "type: dotnet.task");
    CHECK_CONTAINS(yaml, "lang: csharp");
    CHECK_CONTAINS(yaml, "translation checklist");
    CHECK_CONTAINS(yaml, "Rename `class ScriptMain` to `class UserTask`");
    CHECK_CONTAINS(yaml, "Change the base class to `Betl.BetlTask`");
    CHECK_CONTAINS(yaml, "Rename `public void Main()` to `public override void Run()`");
    /* Original C# source is inlined verbatim. */
    CHECK_CONTAINS(yaml, "public class ScriptMain : VSTARTScriptObjectModelBase");
    CHECK_CONTAINS(yaml, "Dts.Variables[\"User::BatchTag\"].Value");

    /* VB.NET Script Task: auto-translated to C# by CodeConverter.
     * The post-conversion source must be C# (no `Inherits`, no
     * `End Sub`) and lang must be `csharp`. */
    CHECK_CONTAINS(yaml, "id: logendvb");
    CHECK_CONTAINS(yaml, "Original SSIS source was VB.NET; auto-translated to C#");
    /* `Imports System` survives as `using System;` (we strip the VB
     * Imports clause and re-prepend the C# equivalent). */
    CHECK_CONTAINS(yaml, "using System;");
    /* The body comes through as C# — `Public Sub Main()` becomes
     * `public void Main()`, `CType(x, Integer)` becomes `(int)x`. */
    CHECK_CONTAINS(yaml, "public void Main()");
    CHECK_CONTAINS(yaml, "(int)ScriptResults.Success");

    /* C# Script Component: dotnet.script + output_schema + checklist. */
    CHECK_CONTAINS(yaml, "id: enrich");
    CHECK_CONTAINS(yaml, "type: dotnet.script");
    CHECK_CONTAINS(yaml, "from: src");
    CHECK_CONTAINS(yaml, "- { name: id, type: l }");
    CHECK_CONTAINS(yaml, "- { name: cleaned, type: u }");
    CHECK_CONTAINS(yaml, "Rename `class ScriptMain` to `class UserScript`");
    CHECK_CONTAINS(yaml, "Change the base class to `Betl.BetlScript`");
    CHECK_CONTAINS(yaml, "Replace SSIS' Input0_ProcessInputRow");
    CHECK_CONTAINS(yaml, "public class ScriptMain : UserComponent");

    free(yaml);

    /* --- controlflow.dtsx: containers + precedence constraints ----- */
    const char *cf_out = "/tmp/betl_dtsx2yaml_controlflow.yml";
    rc = run_convert(BETL_DTSX2YAML_CONTROLFLOW_FIXTURE, cf_out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for controlflow fixture\n", rc);
        return 1;
    }
    yaml = slurp_file(cf_out);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read controlflow YAML\n");
        return 1;
    }

    /* Sequence flattens — no `id: stage_1` step, only its children. */
    CHECK_CONTAINS(yaml, "id: init");
    CHECK_CONTAINS(yaml, "id: truncate");
    CHECK_CONTAINS(yaml, "id: load");
    CHECK_CONTAINS(yaml, "id: cleanup");
    if (strstr(yaml, "id: stage_1") != NULL) {
        fprintf(stderr, "FAIL: stage_1 emitted as a step "
                "(Sequence should have flattened)\n");
        failures++;
    }
    /* Sequence header comment. */
    CHECK_CONTAINS(yaml, "sequence: Stage 1");

    /* Init → Stage 1 (Sequence) lowered to Init → Truncate (entry leaf). */
    CHECK_CONTAINS(yaml, "after: [init]");
    /* Internal Sequence constraint Truncate → Load preserved. */
    CHECK_CONTAINS(yaml, "after: [truncate]");
    /* Stage 1 → Cleanup lowered to Load → Cleanup (exit leaf). */
    CHECK_CONTAINS(yaml, "after: [load]");

    /* Cleanup → Notify is a Failure constraint: on_failure: continue +
     * a TODO calling out the gap. */
    CHECK_CONTAINS(yaml, "after: [cleanup]");
    CHECK_CONTAINS(yaml, "on_failure: continue");
    CHECK_CONTAINS(yaml, "TODO: SSIS Failure precedence");

    /* Notify → Done is a Completion constraint: on_failure: continue +
     * a softer note. */
    CHECK_CONTAINS(yaml, "after: [notify]");
    CHECK_CONTAINS(yaml, "SSIS Completion precedence");

    /* Done → ProcessFile carried an SSIS expression — emitted as TODO
     * comment + condition: "true" so the YAML still validates. */
    CHECK_CONTAINS(yaml, "after: [done]");
    CHECK_CONTAINS(yaml, "TODO: SSIS-expression condition");
    CHECK_CONTAINS(yaml, "@[User::SkipLoop] == false");
    CHECK_CONTAINS(yaml, "condition: \"true\"");

    /* ForEach Loop header — preserves enumerator type + props + var. */
    CHECK_CONTAINS(yaml, "ForEach Loop: Per File");
    CHECK_CONTAINS(yaml, "Enumerator: ForEachFileEnumerator");
    CHECK_CONTAINS(yaml, "Folder = /tmp/inbox");
    CHECK_CONTAINS(yaml, "FileSpec = *.csv");
    CHECK_CONTAINS(yaml, "User::CurrentFile");
    CHECK_CONTAINS(yaml, "run ONCE under betl");

    /* ProcessFile body still emitted (the loss is iteration, not the body). */
    CHECK_CONTAINS(yaml, "id: processfile");

    free(yaml);

    /* --- tasks.dtsx: FileSystem / BulkInsert / ExecuteProcess ----- */
    const char *tk_out = "/tmp/betl_dtsx2yaml_tasks.yml";
    rc = run_convert(BETL_DTSX2YAML_TASKS_FIXTURE, tk_out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for tasks fixture\n", rc);
        return 1;
    }
    yaml = slurp_file(tk_out);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read tasks YAML\n");
        return 1;
    }

    /* CopyExtract → file.copy with src/dst. */
    CHECK_CONTAINS(yaml, "id: copyextract");
    CHECK_CONTAINS(yaml, "type: file.copy");
    CHECK_CONTAINS(yaml, "src: '/tmp/inbox/data.csv'");
    CHECK_CONTAINS(yaml, "dst: '/tmp/archive/data.csv'");

    /* DeleteSrc → file.delete with path. */
    CHECK_CONTAINS(yaml, "id: deletesrc");
    CHECK_CONTAINS(yaml, "type: file.delete");
    CHECK_CONTAINS(yaml, "path: '/tmp/inbox/data.csv'");

    /* BulkLoad → sql.execute with BULK INSERT statement. */
    CHECK_CONTAINS(yaml, "id: bulkload");
    CHECK_CONTAINS(yaml, "type: sql.execute");
    CHECK_CONTAINS(yaml, "BULK INSERT dbo.staging_orders");
    CHECK_CONTAINS(yaml, "FIELDTERMINATOR = ','");
    CHECK_CONTAINS(yaml, "FIRSTROW = 2");

    /* RunTool → shell with argv + timeout. */
    CHECK_CONTAINS(yaml, "id: runtool");
    CHECK_CONTAINS(yaml, "type: shell");
    CHECK_CONTAINS(yaml, "argv: ['/usr/local/bin/my-tool']");
    CHECK_CONTAINS(yaml, "timeout: 300s");
    /* SSIS Arguments string preserved as TODO comment. */
    CHECK_CONTAINS(yaml, "--input data.csv --output report.csv");
    /* WorkingDirectory captured as a TODO. */
    CHECK_CONTAINS(yaml, "TODO: SSIS WorkingDirectory=/var/jobs");

    free(yaml);

    /* --- transforms2.dtsx: the rest of the SSIS default transform set ---- */
    const char *t2_out = "/tmp/betl_dtsx2yaml_transforms2.yml";
    rc = run_convert(BETL_DTSX2YAML_TRANSFORMS2_FIXTURE, t2_out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for transforms2 fixture\n", rc);
        return 1;
    }
    yaml = slurp_file(t2_out);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read transforms2 YAML\n");
        return 1;
    }

    /* Row Count → passthrough map + TODO naming the variable target. */
    CHECK_CONTAINS(yaml, "id: counter");
    CHECK_CONTAINS(yaml, "type: map");
    CHECK_CONTAINS(yaml, "from: src");
    CHECK_CONTAINS(yaml, "no mid-pipeline variable assignment");
    CHECK_CONTAINS(yaml, "Original SSIS target variable: User::RowsProcessed");

    /* Audit → map with 4 add: entries, one per audit metadata column. */
    CHECK_CONTAINS(yaml, "id: tag");
    CHECK_CONTAINS(yaml, "pkg_name:");
    CHECK_CONTAINS(yaml, "= PackageName");
    CHECK_CONTAINS(yaml, "machine:");
    CHECK_CONTAINS(yaml, "= MachineName");
    CHECK_CONTAINS(yaml, "start_at:");
    CHECK_CONTAINS(yaml, "= ExecutionStartTime");
    CHECK_CONTAINS(yaml, "who:");
    CHECK_CONTAINS(yaml, "= UserName");

    /* CharacterMap → UPPER([label]) / LOWER([label]) for the case ops,
     * TODO header for Katakana. */
    CHECK_CONTAINS(yaml, "id: charmap");
    CHECK_CONTAINS(yaml, "label_up:");
    CHECK_CONTAINS(yaml, "expr: 'UPPER([label])'");
    CHECK_CONTAINS(yaml, "label_dn:");
    CHECK_CONTAINS(yaml, "expr: 'LOWER([label])'");
    CHECK_CONTAINS(yaml, "label_kana:");
    CHECK_CONTAINS(yaml, "not expressible in ssisexpr");
    CHECK_CONTAINS(yaml, "Katakana");

    /* Copy Column → map with `add: id_copy: expr [id]`. */
    CHECK_CONTAINS(yaml, "id: dup");
    CHECK_CONTAINS(yaml, "id_copy:");
    CHECK_CONTAINS(yaml, "expr: '[id]'");

    /* OLE DB Command → passthrough map + TODO preserving SqlCommand. */
    CHECK_CONTAINS(yaml, "id: touch");
    CHECK_CONTAINS(yaml, "per-row SQL primitive");
    CHECK_CONTAINS(yaml, "UPDATE dbo.Things SET seen_at = GETUTCDATE() WHERE id = ?");

    /* Merge → union with two from: refs + sort-order caveat. */
    CHECK_CONTAINS(yaml, "id: mrg");
    CHECK_CONTAINS(yaml, "type: union");
    CHECK_CONTAINS(yaml, "from: [src, src2]");
    CHECK_CONTAINS(yaml, "SSIS Merge preserves sort order");

    /* SCD → passthrough + recipe scaffold referencing example 05.
     * The fixture's <inputColumns> declare ColumnType per column:
     *   dim_id = 0 (key), label = 2 (type-2), email = 1 (type-1),
     *   region = 3 (fixed). The scaffold extracts and lists each. */
    CHECK_CONTAINS(yaml, "id: dimupdate");
    CHECK_CONTAINS(yaml, "examples/05-scd-type2");
    CHECK_CONTAINS(yaml, "Target dim table:   [dbo].[DimThing]");
    CHECK_CONTAINS(yaml, "SELECT dim_id, label FROM dbo.DimThing");
    CHECK_CONTAINS(yaml, "Update history?     yes");
    CHECK_CONTAINS(yaml, "Business key(s):    dim_id");
    CHECK_CONTAINS(yaml, "Type-2 (tracked):   label");
    CHECK_CONTAINS(yaml, "Type-1 (overwrite): email");
    CHECK_CONTAINS(yaml, "Fixed (no-change):  region");

    /* Percentage / Row Sampling — both passthrough + TODO + preserved values. */
    CHECK_CONTAINS(yaml, "id: pctsamp");
    CHECK_CONTAINS(yaml, "Percentage Sampling");
    CHECK_CONTAINS(yaml, "Original sample size: 10");
    CHECK_CONTAINS(yaml, "Original seed: 42");
    CHECK_CONTAINS(yaml, "id: rowsamp");
    CHECK_CONTAINS(yaml, "Row Sampling");
    CHECK_CONTAINS(yaml, "Original sample size: 1000");

    /* Fuzzy Lookup / Grouping — passthrough + TODO. */
    CHECK_CONTAINS(yaml, "id: fzlk");
    CHECK_CONTAINS(yaml, "Fuzzy Lookup has no betl equivalent");
    CHECK_CONTAINS(yaml, "id: fzgp");
    CHECK_CONTAINS(yaml, "Fuzzy Grouping has no betl equivalent");

    /* Term Lookup / Extraction. */
    CHECK_CONTAINS(yaml, "id: tmlk");
    CHECK_CONTAINS(yaml, "Term Lookup has no betl equivalent");
    CHECK_CONTAINS(yaml, "id: tmex");
    CHECK_CONTAINS(yaml, "Term Extraction has no betl equivalent");

    /* Cache Transform — passthrough + TODO. */
    CHECK_CONTAINS(yaml, "id: cachet");
    CHECK_CONTAINS(yaml, "Cache Transform has no betl equivalent");

    /* Export / Import Column. */
    CHECK_CONTAINS(yaml, "id: expcol");
    CHECK_CONTAINS(yaml, "Export Column has no betl equivalent");
    CHECK_CONTAINS(yaml, "id: impcol");
    CHECK_CONTAINS(yaml, "Import Column has no betl equivalent");

    /* CDC Splitter (passthrough) + CDC Source (mssql.read scaffold). */
    CHECK_CONTAINS(yaml, "id: cdcsp");
    CHECK_CONTAINS(yaml, "CDC Splitter has no betl equivalent");
    CHECK_CONTAINS(yaml, "id: cdcsrc");
    CHECK_CONTAINS(yaml, "type: mssql.read");
    CHECK_CONTAINS(yaml, "CDC Source has no direct betl equivalent");
    CHECK_CONTAINS(yaml, "cdc.fn_cdc_get_all_changes_CAPTURE");

    /* Balanced Data Distributor. */
    CHECK_CONTAINS(yaml, "id: bdd");
    CHECK_CONTAINS(yaml, "Balanced Data Distributor has no betl equivalent");

    /* DQS Cleansing. */
    CHECK_CONTAINS(yaml, "id: dqs");
    CHECK_CONTAINS(yaml, "DQS Cleansing has no betl equivalent");

    /* Data Mining Query. */
    CHECK_CONTAINS(yaml, "id: dmq");
    CHECK_CONTAINS(yaml, "Data Mining Query has no betl equivalent");

    free(yaml);

    /* --- kitchen-sink.dtsx: real-format fixture (dtexec /Validate ✓) ----
     * The other fixtures above are deliberately minimal — they strip
     * DTSX to the attribute subset the converter actually reads.
     * kitchen-sink.dtsx is the opposite: it's a full-fidelity package
     * that validates clean against the SQL Server 2022 dtexec on Linux
     * (via mcp-ssis). Its job is to catch regressions where the
     * converter quietly stops handling the shape of XML that real SSDT
     * actually emits. Notably it pins the VARENUM (VT_*) DataType
     * encoding for Variable values — see Converter.cs MapVariableType. */
    const char *ks_out = "/tmp/betl_dtsx2yaml_kitchen_sink.yml";
    rc = run_convert(BETL_DTSX2YAML_KITCHENSINK_FIXTURE, ks_out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for kitchen-sink fixture\n", rc);
        return 1;
    }
    yaml = slurp_file(ks_out);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read kitchen-sink YAML\n");
        return 1;
    }

    /* Top shape + connection. */
    CHECK_CONTAINS(yaml, "name: kitchensink");
    CHECK_CONTAINS(yaml, "warehouse:");
    CHECK_CONTAINS(yaml, "type: mssql");

    /* VARENUM mapping: DataType=8 (VT_BSTR) → string, 3 (VT_I4) → int,
     * 11 (VT_BOOL) → bool. This is the regression test for the
     * System.TypeCode-vs-VARENUM bug. */
    CHECK_CONTAINS(yaml, "batchtag:");
    CHECK_CONTAINS(yaml, "type: string");
    CHECK_CONTAINS(yaml, "default: 'nightly'");
    CHECK_CONTAINS(yaml, "minamount:");
    CHECK_CONTAINS(yaml, "type: int");
    CHECK_CONTAINS(yaml, "default: '100'");
    CHECK_CONTAINS(yaml, "skipexecsql:");
    CHECK_CONTAINS(yaml, "type: bool");

    /* Pipeline: ExecuteSQL → sql.execute, Pipeline → dataflow with
     * mssql.read + map (derive) + map (rowcount passthrough), and
     * the precedence constraint becomes after: [truncate_stage]. */
    CHECK_CONTAINS(yaml, "id: truncate_stage");
    CHECK_CONTAINS(yaml, "type: sql.execute");
    CHECK_CONTAINS(yaml, "TRUNCATE TABLE stage.Orders");
    CHECK_CONTAINS(yaml, "id: extract_orders");
    CHECK_CONTAINS(yaml, "after: [truncate_stage]");
    CHECK_CONTAINS(yaml, "type: dataflow");
    CHECK_CONTAINS(yaml, "id: olesource");
    CHECK_CONTAINS(yaml, "type: mssql.read");
    CHECK_CONTAINS(yaml, "SELECT id, amount FROM dbo.Orders");
    CHECK_CONTAINS(yaml, "id: derive");
    CHECK_CONTAINS(yaml, "[amount] + (DT_I8)1");
    CHECK_CONTAINS(yaml, "id: rowcount");
    CHECK_CONTAINS(yaml, "Original SSIS target variable: User::MinAmount");

    free(yaml);

    /* --- projectsample/ProjectSample.dtsx: project-deployment-model ----
     * The fixture lives in a directory alongside Warehouse.conmgr and
     * Project.params. The converter is expected to auto-discover those
     * siblings and pull them into the package model. This is the
     * regression test for:
     *   - external .conmgr loading (no <ConnectionManagers> in the .dtsx)
     *   - Project.params loading with System.TypeCode-encoded DataType
     *     (DT=18 → string, distinct from VARENUM)
     *   - <DTS:PropertyExpression> overriding SqlStatementSource with
     *     a runtime SSIS expression splicing @[$Project::X]
     *   - STOCK:SEQUENCE container (older SSDT spelling)
     *   - Microsoft.SQLServerDestination → mssql.bulkinsert (since the
     *     bulkinsert sink landed, this is the semantic match — insert-only,
     *     bulk-array binding, no MERGE machinery). */
    const char *ps_out = "/tmp/betl_dtsx2yaml_projectsample.yml";
    rc = run_convert(BETL_DTSX2YAML_PROJECT_FIXTURE, ps_out);
    if (rc != 0) {
        fprintf(stderr, "FAIL: converter returned %d for projectsample fixture\n", rc);
        return 1;
    }
    yaml = slurp_file(ps_out);
    if (!yaml) {
        fprintf(stderr, "FAIL: cannot read projectsample YAML\n");
        return 1;
    }

    /* External .conmgr → connection emitted under the project-level
     * name "Warehouse" (lower-snake-cased). */
    CHECK_CONTAINS(yaml, "warehouse:");
    CHECK_CONTAINS(yaml, "type: mssql");
    CHECK_CONTAINS(yaml, "Server=db.example.com");
    CHECK_CONTAINS(yaml, "Database=Sales");

    /* Project.params (TypeCode-encoded — distinct from VARENUM). */
    CHECK_CONTAINS(yaml, "batchtag:");
    CHECK_CONTAINS(yaml, "type: string");      /* TypeCode 18 = String */
    CHECK_CONTAINS(yaml, "default: 'nightly'");
    CHECK_CONTAINS(yaml, "rowlimit:");
    CHECK_CONTAINS(yaml, "type: int");          /* TypeCode 9 = Int32 */
    CHECK_CONTAINS(yaml, "default: '10000'");
    /* Description carried through as a comment. */
    CHECK_CONTAINS(yaml, "Run tag for the nightly batch");

    /* STOCK:SEQUENCE container picked up under the same code path as
     * Microsoft.Sequence — children flatten with a comment header. */
    CHECK_CONTAINS(yaml, "sequence: Migrate");

    /* ExecuteSQLTask with a PropertyExpression: the dynamic expression
     * wins and project-param refs resolve to ${params.X}. The static
     * 'placeholder' value baked into SqlStatementSource must NOT appear. */
    CHECK_CONTAINS(yaml, "id: tag_batch");
    CHECK_CONTAINS(yaml, "type: sql.execute");
    CHECK_CONTAINS(yaml, "${params.batchtag}");
    CHECK_CONTAINS(yaml, "${params.rowlimit}");
    if (strstr(yaml, "'placeholder'") != NULL) {
        fprintf(stderr, "FAIL: static SqlStatementSource leaked through "
                "(PropertyExpression should override it)\n");
        failures++;
    }

    /* Microsoft.SQLServerDestination → mssql.bulkinsert with the
     * BulkInsertTableName picked up as `table:`. */
    CHECK_CONTAINS(yaml, "id: sqldest");
    CHECK_CONTAINS(yaml, "type: mssql.bulkinsert");
    CHECK_CONTAINS(yaml, "table: '[dbo].[Orders_Staged]'");
    /* BulkInsertKeepIdentity=true surfaces as a TODO. */
    CHECK_CONTAINS(yaml, "BulkInsertKeepIdentity");

    /* Precedence inside the container lowers to a flat after:. */
    CHECK_CONTAINS(yaml, "after: [tag_batch]");

    free(yaml);

    if (failures > 0) {
        fprintf(stderr, "FAIL: %d substring check(s) missed\n", failures);
        return 1;
    }
    printf("ok: dtsx2yaml end-to-end test passed\n");
    return 0;
}
