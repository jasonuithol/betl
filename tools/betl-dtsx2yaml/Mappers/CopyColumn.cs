/* SSIS Copy Column → betl `map` with `add:` that copies a source
 * column to a new column.
 *
 * SSIS Copy Column carries a list of output columns whose source is
 * a referenced input lineage; we emit `add: { new: { expr: [src] } }`
 * for each. The source column name comes from the lineage-ID property
 * which often resolves only via the lineage table — we fall back to
 * the output column's `cachedName` (the input column's friendly name
 * recorded at design time) where available. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class CopyColumn
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");

        var outputCols = c.Outputs
            .SelectMany(o => (o.Element?.Element("outputColumns")
                                       ?.Elements("outputColumn")
                                       ?? Enumerable.Empty<System.Xml.Linq.XElement>()))
            .ToList();
        if (outputCols.Count == 0)
        {
            w.Comment("TODO: Copy Column has no <outputColumn> entries — verify");
            w.Indent(-2);
            return;
        }

        w.Line("add:");
        w.Indent(2);
        foreach (var col in outputCols)
        {
            string name = (string?)col.Attribute("name") ?? "";
            string? cached = (string?)col.Attribute("cachedName");
            string? lineage = col.Element("properties")
                                ?.Elements("property")
                                .FirstOrDefault(p => (string?)p.Attribute("name") == "CopyColumnInputColumnLineageID")
                                ?.Value;
            string srcLabel = !string.IsNullOrEmpty(cached) ? cached!
                            : (!string.IsNullOrEmpty(lineage) && !lineage.StartsWith("#")) ? lineage!
                            : "SOURCE";

            string? dt  = (string?)col.Attribute("dataType");
            string fmt  = DerivedColumn.DtsxDataTypeToArrowFormat(dt);

            w.Line($"{YamlWriter.Id(name)}:");
            w.Indent(2);
            w.Line("lang: ssisexpr");
            w.Line($"expr: {YamlWriter.Quote("[" + srcLabel + "]")}");
            if (!string.IsNullOrEmpty(fmt)) w.Line($"type: {fmt}");
            if (srcLabel == "SOURCE")
                w.Comment("TODO: replace SOURCE with the actual input column name");
            w.Indent(-2);
        }
        w.Indent(-2);
        w.Indent(-2);
    }
}
