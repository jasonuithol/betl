/* SSIS Slowly Changing Dimension → betl SCD type-2 recipe (commented
 * scaffold).
 *
 * The SCD component in SSIS is a thick, opinionated wizard-generated
 * orchestrator: under the hood it's a lookup + classify-split-route
 * across several named outputs. betl doesn't ship a one-shot SCD
 * component; the *pattern* lives in examples/05-scd-type2/ as a
 * composition of standard transforms (join + map + conditional_split
 * + union + postgres.exec), which keeps the moving parts visible.
 *
 * On translation:
 *   - Emit a passthrough `map` (so the resulting YAML stays valid and
 *     the downstream `from:` references continue to point at this id).
 *   - Emit a long comment block containing the recipe scaffold, with
 *     the connection / table / SCD-property values pulled from the
 *     SSIS component baked in. The operator removes the passthrough
 *     and the comments after porting.
 *
 * What we extract from the DTSX SCD component:
 *   SqlCommand                       — the SELECT against the dim
 *   TableOrViewName                  — the target dim table
 *   UpdateChangingAttributeHistory   — whether to generate the
 *                                      close-old-version branch
 *   Input column metadata (ColumnType):
 *     0 = business key, 1 = type-1 "changing", 2 = type-2 "historical",
 *     3 = "fixed". v1 honours 0 + 2; emits a TODO for 1 / 3. */

using System.Collections.Generic;
using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Scd
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");

        string? sqlCommand     = ReadProperty(c, "SqlCommand");
        string? tableName      = ReadProperty(c, "TableOrViewName");
        string? updateChanging = ReadProperty(c, "UpdateChangingAttributeHistory");
        bool updateHistory     = string.Equals(updateChanging, "True",
                                     System.StringComparison.OrdinalIgnoreCase);

        var keys       = ColumnsByType(c, 0);
        var historical = ColumnsByType(c, 2);
        var changing   = ColumnsByType(c, 1);
        var fixedCols  = ColumnsByType(c, 3);

        /* Resolve the connection reference, if present, to a betl
         * connection name. Falls back to "warehouse" so the comment is
         * still legible. */
        string connName = ResolveConnection(pkg, c) ?? "warehouse";

        w.Comment("──────────────────────────────────────────────────────────────────");
        w.Comment("TODO: SSIS Slowly Changing Dimension — replace this `map` with");
        w.Comment("      the SCD type-2 recipe below. See examples/05-scd-type2/");
        w.Comment("      for the full pattern + README.");
        if (!string.IsNullOrEmpty(tableName))
            w.Comment($"      Target dim table:   {tableName}");
        if (keys.Count > 0)
            w.Comment($"      Business key(s):    {string.Join(", ", keys)}");
        if (historical.Count > 0)
            w.Comment($"      Type-2 (tracked):   {string.Join(", ", historical)}");
        if (changing.Count > 0)
            w.Comment($"      Type-1 (overwrite): {string.Join(", ", changing)} "
                    + "[not modelled by the example recipe — add a separate "
                    + "UPDATE branch]");
        if (fixedCols.Count > 0)
            w.Comment($"      Fixed (no-change):  {string.Join(", ", fixedCols)} "
                    + "[validate upstream — recipe ignores]");
        w.Comment($"      Update history?     {(updateHistory ? "yes" : "no — type-1 mode only")}");

        w.Comment("");
        w.Comment("      ── recipe scaffold (uncomment + adapt) ──");
        w.Comment("      # --- LEFT-JOIN staging against the current dim image ---");
        if (!string.IsNullOrEmpty(sqlCommand))
        {
            w.Comment("      # SSIS dim-lookup query (was on the SCD component):");
            foreach (var line in sqlCommand.Replace("\r\n", "\n").Split('\n'))
                w.Comment("      #   " + line);
        }
        w.Comment($"      # - id: current_dim");
        w.Comment($"      #   type: postgres.read");
        w.Comment($"      #   connection: {connName}");
        w.Comment($"      #   query: |");
        w.Comment($"      #     SELECT <bk>      AS dim_<bk>,");
        w.Comment($"      #            <sk>      AS dim_sk,");
        if (historical.Count > 0)
            foreach (var col in historical)
                w.Comment($"      #            {col,-10} AS dim_{col},");
        w.Comment($"      #       FROM {tableName ?? "<dim_view>"}_current   -- WHERE is_current");
        w.Comment($"      #");
        w.Comment($"      # - id: joined");
        w.Comment($"      #   type: join");
        w.Comment($"      #   kind: left");
        w.Comment($"      #   from: [<upstream>, current_dim]");
        string keyJoin = keys.Count > 0
            ? string.Join(", ", keys.Select(k => $"{k}: dim_{k}"))
            : "<bk>: dim_<bk>";
        w.Comment($"      #   on: {{ {keyJoin} }}");
        w.Comment($"      #");
        w.Comment($"      # - id: classify        # NEW / CHANGED / UNCHANGED");
        w.Comment($"      #   type: map");
        w.Comment($"      #   from: joined");
        w.Comment($"      #   add:");
        w.Comment($"      #     scd_status:");
        w.Comment($"      #       lang: lua");
        w.Comment($"      #       expr: |");
        w.Comment($"      #         if row.dim_sk == nil then return \"NEW\" end");
        if (historical.Count > 0)
        {
            var cmp = string.Join("\n      #            or ",
                historical.Select(col => $"row.{col} ~= row.dim_{col}"));
            w.Comment($"      #         if {cmp}");
            w.Comment($"      #         then return \"CHANGED\" else return \"UNCHANGED\" end");
        }
        else
        {
            w.Comment($"      #         -- compare each tracked column");
            w.Comment($"      #         return \"UNCHANGED\"");
        }
        w.Comment($"      #");
        w.Comment($"      # - id: route");
        w.Comment($"      #   type: conditional_split");
        w.Comment($"      #   from: classify");
        w.Comment($"      #   cases:");
        w.Comment($"      #     - {{ name: new,     where: 'row.scd_status == \"NEW\"' }}");
        if (updateHistory)
            w.Comment($"      #     - {{ name: changed, where: 'row.scd_status == \"CHANGED\"' }}");
        w.Comment($"      #   default: unchanged");
        w.Comment($"      #");
        w.Comment($"      # - id: to_insert       # both NEW + CHANGED produce a new dim row");
        w.Comment($"      #   type: union");
        if (updateHistory)
            w.Comment($"      #   from: [route:new, route:changed]");
        else
            w.Comment($"      #   from: [route:new]");
        w.Comment($"      #");
        w.Comment($"      # - id: insert_new_version");
        w.Comment($"      #   type: postgres.exec");
        w.Comment($"      #   from: to_insert");
        w.Comment($"      #   connection: {connName}");
        var nvCols = new List<string> { };
        nvCols.AddRange(keys);
        nvCols.AddRange(historical);
        var col_list = string.Join(", ", nvCols);
        var ph_list  = string.Join(", ", nvCols.Select((_, i) => $"${i + 1}"));
        w.Comment($"      #   sql: |");
        w.Comment($"      #     INSERT INTO {tableName ?? "<dim>"} ({col_list}, valid_from, is_current)");
        w.Comment($"      #     VALUES ({ph_list}, '${{params.batch_ts}}', TRUE)");
        w.Comment($"      #   parameters: [{col_list}]");
        if (updateHistory)
        {
            w.Comment($"      #");
            w.Comment($"      # - id: close_old_version");
            w.Comment($"      #   type: postgres.exec");
            w.Comment($"      #   from: route:changed");
            w.Comment($"      #   connection: {connName}");
            w.Comment($"      #   sql: |");
            w.Comment($"      #     UPDATE {tableName ?? "<dim>"}");
            w.Comment($"      #        SET is_current = FALSE,");
            w.Comment($"      #            valid_to   = '${{params.batch_ts}}'");
            w.Comment($"      #      WHERE <sk> = $1");
            w.Comment($"      #   parameters: [dim_sk]");
        }
        w.Comment("──────────────────────────────────────────────────────────────────");
        w.Indent(-2);
    }

    static string? ReadProperty(DtsxComponent c, string name)
    {
        if (c.Properties.TryGetValue(name, out var v)) return v;
        return c.Element?.Element("properties")
                         ?.Elements("property")
                         .FirstOrDefault(p => (string?)p.Attribute("name") == name)
                         ?.Value;
    }

    /* Collect input column names whose ColumnType custom property
     * matches the requested integer. SSIS encodes:
     *   0 = key, 1 = type-1 changing, 2 = type-2 historical, 3 = fixed. */
    static List<string> ColumnsByType(DtsxComponent c, int type)
    {
        var result = new List<string>();
        var inputs = c.Element?.Element("inputs");
        if (inputs == null) return result;
        foreach (var inp in inputs.Elements("input"))
        foreach (var col in inp.Element("inputColumns")?.Elements("inputColumn")
                            ?? Enumerable.Empty<XElement>())
        {
            var props = col.Element("properties")?.Elements("property")
                          ?? Enumerable.Empty<XElement>();
            string? colType = props
                .FirstOrDefault(p => (string?)p.Attribute("name") == "ColumnType")
                ?.Value;
            if (colType == type.ToString())
            {
                /* The input column's "cachedName" is the staged column;
                 * fall back to the lineage-named property. */
                string name = (string?)col.Attribute("cachedName")
                           ?? (string?)col.Attribute("name")
                           ?? "?";
                result.Add(name);
            }
        }
        return result;
    }

    /* Resolve the SCD component's connection reference (a DTSID GUID,
     * already extracted by the parser into ConnectionManagerRefId) to
     * a betl connection name via the package's connections list.
     * Returns null when the link can't be resolved. */
    static string? ResolveConnection(DtsxPackage pkg, DtsxComponent c)
    {
        if (string.IsNullOrEmpty(c.ConnectionManagerRefId)) return null;
        foreach (var cm in pkg.Connections)
        {
            if (cm.Element?.Attribute(DtsxParser.DtsNs + "DTSID")?.Value
                == c.ConnectionManagerRefId)
                return YamlWriter.Id(cm.Name);
        }
        return null;
    }
}
