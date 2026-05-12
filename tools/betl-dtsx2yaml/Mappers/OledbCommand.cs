/* SSIS OLE DB Command → betl TODO stub.
 *
 * OLE DB Command runs a parameterised SQL statement once per row, with
 * the row's columns bound to "?" parameters. betl has no per-row SQL
 * primitive — the closest fit is to refactor the upstream pipeline so
 * that the per-row writes become a batch MERGE / UPDATE driven by a
 * sql.execute step, but that rewrite is package-specific.
 *
 * We emit a passthrough `map` so downstream wiring keeps working, plus
 * a TODO that preserves the original SqlCommand for the operator. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class OledbCommand
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");

        string? sql = c.Properties.TryGetValue("SqlCommand", out var s) ? s : null;
        if (sql == null && c.Element != null)
        {
            sql = c.Element.Element("properties")
                          ?.Elements("property")
                          .FirstOrDefault(p => (string?)p.Attribute("name") == "SqlCommand")
                          ?.Value;
        }

        w.Comment("TODO: SSIS OLE DB Command runs SQL once per row — betl has no");
        w.Comment("      per-row SQL primitive. Typical rewrites:");
        w.Comment("        - aggregate upstream into a staging table, then run a");
        w.Comment("          single batch MERGE / UPDATE via a sql.execute step;");
        w.Comment("        - if it was logging-only, drop the step.");
        if (!string.IsNullOrEmpty(sql))
        {
            w.Comment("      Original SqlCommand:");
            foreach (var line in sql.Replace("\r\n", "\n").Split('\n'))
                w.Comment("        " + line);
        }
        w.Indent(-2);
    }
}
