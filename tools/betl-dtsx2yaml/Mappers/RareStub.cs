/* Stubs for the remaining SSIS transforms / sources / destinations
 * that we don't have a clean betl translation for. Each emits a
 * passthrough `map` (or a comment-only placeholder for sources) plus
 * a TODO header summarising what the original SSIS component did,
 * so the operator can hand-write the replacement.
 *
 * Covered here:
 *   - Fuzzy Lookup / Fuzzy Grouping
 *   - Term Lookup / Term Extraction
 *   - Cache Transform
 *   - Export Column / Import Column (BLOB ↔ file)
 *   - CDC Splitter / CDC Source
 *   - Balanced Data Distributor (round-robin fan-out)
 *   - DQS Cleansing
 *   - Data Mining Query / Data Mining Model Training */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class RareStub
{
    public static void EmitPassthrough(YamlWriter w, DtsxComponent c, string? fromId,
                                       string label, params string[] explanation)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");
        w.Comment($"TODO: SSIS {label} has no betl equivalent.");
        foreach (var line in explanation) w.Comment("      " + line);
        w.Indent(-2);
    }

    /* Per-component entry points — keep the call sites in Converter.cs
     * tidy and one-line. */

    public static void FuzzyLookup(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Fuzzy Lookup",
            "Probabilistic dimension match by token similarity.",
            "Rewrite as either a Lookup with exact keys (after cleansing) or",
            "a sql.execute that uses a SOUNDEX / Levenshtein UDF / pg_trgm.");

    public static void FuzzyGrouping(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Fuzzy Grouping",
            "Clusters near-duplicate rows by token similarity.",
            "Typically rewritten as an aggregate over a canonicalised key,",
            "or pushed to a SQL CTE that uses DIFFERENCE/SOUNDEX/pg_trgm.");

    public static void TermLookup(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Term Lookup",
            "Counts occurrences of a reference vocabulary in a text column.",
            "Rewrite as a `dotnet.script` step or a SQL-side approach using",
            "a tokenising UDF + a join against the vocabulary table.");

    public static void TermExtraction(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Term Extraction",
            "Extracts noun phrases / candidate terms from a text column.",
            "No SQL-only rewrite — needs a `dotnet.script` step with an NLP",
            "tokeniser (e.g. ML.NET, Stanford NLP, or a spaCy subprocess).");

    public static void CacheTransform(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Cache Transform",
            "Loads upstream rows into an in-memory cache for a later Lookup.",
            "betl Lookup reads its reference table directly via sql.execute;",
            "drop this step and point the downstream lookup at the source",
            "table or a temp staging table instead.");

    public static void ExportColumn(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Export Column",
            "Writes a BLOB column to a per-row file path.",
            "betl has no per-row file-write primitive; rewrite as a",
            "`dotnet.script` step that iterates rows and writes files,",
            "or stage the BLOBs to disk via a sql.execute T-SQL OUTPUT clause.");

    public static void ImportColumn(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Import Column",
            "Reads a file per row into a BLOB column.",
            "betl has no per-row file-read primitive; rewrite as a",
            "`dotnet.script` step that opens each path and emits bytes,",
            "or pre-stage all files into a single Parquet/CSV manifest.");

    public static void CdcSplitter(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "CDC Splitter",
            "Routes rows from a CDC source into insert/update/delete branches",
            "based on the __$operation column. The cleanest betl replacement",
            "is a `split` step keyed on the operation column with three",
            "outputs (where_eq 2 / 4 / 1 respectively).");

    public static void CdcSource(YamlWriter w, DtsxComponent c)
    {
        /* Emit a real `mssql.read` scaffold (the natural rewrite target)
         * so the generated YAML validates. The operator fills in the
         * connection name and the actual capture-instance fn call; we
         * leave a placeholder query that's syntactically valid. */
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: mssql.read");
        w.Comment("TODO: SSIS CDC Source has no direct betl equivalent. The natural");
        w.Comment("      rewrite is mssql.read against the CDC capture function. Fill");
        w.Comment("      in `connection:` and the capture-instance name below.");
        w.Line("connection: warehouse");
        w.Line("query: 'SELECT * FROM cdc.fn_cdc_get_all_changes_CAPTURE(@from_lsn, @to_lsn, ''all'')'");
        w.Indent(-2);
    }

    public static void BalancedDataDistributor(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Balanced Data Distributor",
            "Round-robin fan-out across N output lanes for parallel sinks.",
            "betl's `multicast` fan-outs duplicate rows (each tap gets every",
            "row), not partition them. Either use multicast + downstream",
            "filters on a hash(key) % N, or accept a single-lane sink.");

    public static void DqsCleansing(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "DQS Cleansing",
            "Runs rows against a Data Quality Services knowledge base.",
            "DQS has no OSS equivalent; rewrite as a `dotnet.script` step",
            "or a SQL-side approach using validation functions / lookups.");

    public static void DataMiningQuery(YamlWriter w, DtsxComponent c, string? fromId) =>
        EmitPassthrough(w, c, fromId, "Data Mining Query",
            "Scores rows against an SSAS mining model (DMX PREDICT).",
            "No OSS equivalent; rewrite as a `dotnet.script` step calling",
            "into an ML.NET / ONNX runtime model, or push scoring to SQL.");
}
