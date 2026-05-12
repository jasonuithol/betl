/* SSIS Sort → betl `sort`.
 *
 * SSIS Sort tags each input column with NewSortKeyPosition:
 *     0   = not part of the sort key
 *     N>0 = sort precedence N (1 = primary key)
 *
 * Direction lives in NewComparisonFlags / SortKeyPosition sign:
 * positive position = ascending, negative = descending (the absolute
 * value gives precedence). The column's `name` attribute is the betl
 * column name.
 *
 * betl sort syntax:
 *   type: sort
 *   from: ...
 *   by:
 *     - { col: name, dir: asc }
 *     - { col: id,   dir: desc }
 *
 * v0.2 walks the SSIS input columns and pulls out those with non-zero
 * SortKeyPosition. */

using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Sort
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: sort");
        if (fromId != null) w.Line($"from: {fromId}");

        /* Sort keys live on the input columns (the single <input> of
         * Microsoft.Sort). Each <inputColumn> with a non-zero
         * `<property name="NewSortKeyPosition">` participates. */
        var keys = new List<(int prec, string col, string dir)>();
        var input = c.Element?.Element("inputs")?.Element("input");
        if (input != null)
        {
            foreach (var ic in input.Element("inputColumns")?.Elements("inputColumn")
                                 ?? Enumerable.Empty<System.Xml.Linq.XElement>())
            {
                string colName = (string?)ic.Attribute("cachedName")
                              ?? (string?)ic.Attribute("name")
                              ?? "";
                string? posS = ic.Element("properties")
                                ?.Elements("property")
                                .FirstOrDefault(p => (string?)p.Attribute("name") == "NewSortKeyPosition")
                                ?.Value;
                if (!int.TryParse(posS, out int pos) || pos == 0) continue;
                string dir = pos < 0 ? "desc" : "asc";
                keys.Add((System.Math.Abs(pos), colName, dir));
            }
        }
        keys.Sort((a, b) => a.prec.CompareTo(b.prec));

        if (keys.Count == 0)
        {
            w.Comment("TODO: no sort keys found — fill `by:` with the");
            w.Comment("[{col:..., dir: asc|desc}, ...] you want");
            w.Line("by: []");
        }
        else
        {
            w.Line("by:");
            w.Indent(2);
            foreach (var (_, col, dir) in keys)
                w.Line($"- {{ col: {YamlWriter.Id(col)}, dir: {dir} }}");
            w.Indent(-2);
        }
        w.Indent(-2);
    }
}
