/* SSIS Lookup → betl `mssql.lookup` / `postgres.lookup`.
 *
 * SSIS Lookup carries:
 *   - ConnectionManager reference (the reference DB)
 *   - `SqlCommand` (or `SqlCommandParam`) — the SELECT that builds the
 *     reference set
 *   - input/output columns marking which columns are match-keys
 *     (`<inputColumn>` with property "JoinToReferenceColumn") and
 *     which are added from the reference (`<outputColumn>` with
 *     property "CopyFromReferenceColumn")
 *
 * betl lookup syntax (mssql.lookup / postgres.lookup):
 *
 *   type: mssql.lookup
 *   from: ...
 *   connection: warehouse
 *   table: schema.tbl              # OR query: SELECT ...
 *   match:  { in_col: ref_col, ... }
 *   select: { out_col: ref_col, ... }
 *   on_miss: error | null
 *
 * v0.2 emits connection + query and skeletons for match/select. Full
 * column-mapping translation is left as a TODO since the SSIS XML
 * carries the reference-table column names only as cached values
 * inside per-column `<property>` elements. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Lookup
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        /* Connection looks up by name (refId stripped). Postgres
         * detection mirrors the Source/Destination mappers. */
        var conn = ConnectionLookup.For(pkg, c);
        bool isPg = conn != null
                 && ((conn.Payload ?? "").Contains("Postgres",
                        System.StringComparison.OrdinalIgnoreCase)
                  || (conn.Payload ?? "").Contains("Npgsql",
                        System.StringComparison.OrdinalIgnoreCase));
        string componentType = isPg ? "postgres.lookup" : "mssql.lookup";

        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line($"type: {componentType}");
        if (fromId != null) w.Line($"from: {fromId}");
        w.Line($"connection: {YamlWriter.Id(conn?.Name ?? "warehouse")}");

        c.Properties.TryGetValue("SqlCommand", out var sql);
        if (!string.IsNullOrEmpty(sql))
            w.Line("query: " + YamlWriter.Quote(sql));
        else
            w.Comment("TODO: SSIS Lookup has no SqlCommand — set `table:` or `query:`");

        /* Cache mode comment: SSIS supports full/partial/no-cache;
         * betl always loads-the-reference into memory at startup. */
        c.Properties.TryGetValue("CacheType", out var cacheType);
        if (cacheType != null && cacheType != "0" && cacheType != "1")
            w.Comment($"note: SSIS CacheType={cacheType} (partial/no-cache) — "
                    + "betl loads the full reference set into memory");

        w.Comment("TODO: SSIS join keys / output columns aren't auto-translated.");
        w.Comment("Fill `match:` with {input_col: ref_col, ...} and");
        w.Comment("`select:` with {output_col: ref_col, ...}.");
        w.Line("match:  {}");
        w.Line("select: {}");
        w.Line("on_miss: error");
        w.Indent(-2);
    }
}
