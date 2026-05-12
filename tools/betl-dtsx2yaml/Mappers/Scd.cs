/* SSIS Slowly Changing Dimension → betl TODO stub.
 *
 * SSIS' SCD wizard generates a Lookup + Conditional Split + multiple
 * Derived Column / OLE DB Command branches inline; the SCD component
 * itself orchestrates this. By the time the package is saved, the
 * downstream nodes carry the actual logic, but the SCD component
 * still appears in the pipeline as the entry point.
 *
 * There's no clean betl primitive for "type-2 dimension merge" — the
 * typical translation is a single sql.execute that does a MERGE
 * against the dimension table (when the underlying engine supports
 * MERGE / UPSERT), or a sequence of lookup + filter + insert/update
 * dataflow steps.
 *
 * We emit a passthrough `map` + a TODO describing the typical
 * rewrite, preserving downstream wiring and the SCD's reference to
 * its dimension connection / table for the operator. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Scd
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");

        /* SqlCommand here is the SELECT against the dim table.
         * TableOrViewName is the target dim table name. */
        string? sqlCommand    = ReadProperty(c, "SqlCommand");
        string? tableName     = ReadProperty(c, "TableOrViewName");
        string? updateChanging= ReadProperty(c, "UpdateChangingAttributeHistory");

        w.Comment("TODO: SSIS Slowly Changing Dimension has no betl equivalent.");
        w.Comment("      The standard rewrite is a single sql.execute that");
        w.Comment("      MERGEs the staged rows into the dimension table");
        w.Comment("      (mssql/pg/mysql MERGE/UPSERT syntax), or a dataflow");
        w.Comment("      with lookup + conditional_split + two destinations");
        w.Comment("      (insert new vs. update existing).");
        if (!string.IsNullOrEmpty(tableName))
            w.Comment($"      Dimension table: {tableName}");
        if (!string.IsNullOrEmpty(updateChanging))
            w.Comment($"      UpdateChangingAttributeHistory = {updateChanging}");
        if (!string.IsNullOrEmpty(sqlCommand))
        {
            w.Comment("      SSIS dim-lookup query:");
            foreach (var line in sqlCommand.Replace("\r\n", "\n").Split('\n'))
                w.Comment("        " + line);
        }
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
}
