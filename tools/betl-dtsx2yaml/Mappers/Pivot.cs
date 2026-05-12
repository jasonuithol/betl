/* SSIS Pivot → betl `pivot`.
 *
 * SSIS Pivot is fiddly: each output column has a `PivotKeyValue`
 * property pinning it to one value of the pivot column, plus a
 * `PivotUsage`-flagged input column (0=passthrough, 1=set-key,
 * 2=pivot-key, 3=value). v0.2 captures the structural shape but
 * defers the column-name resolution to the operator.
 *
 *   type: pivot
 *   from: ...
 *   id_cols: [...]          PivotUsage=1 (set-key)
 *   name_col: ...           PivotUsage=2 (pivot-key)
 *   value_col: ...          PivotUsage=3 (value)
 *   pivot_keys: [...]       distinct PivotKeyValue values across outputs */

using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Pivot
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: pivot");
        if (fromId != null) w.Line($"from: {fromId}");

        var idCols    = new List<string>();
        string? nameCol = null, valueCol = null;

        var input = c.Element?.Element("inputs")?.Element("input");
        if (input != null)
        {
            foreach (var ic in input.Element("inputColumns")?.Elements("inputColumn")
                                 ?? Enumerable.Empty<System.Xml.Linq.XElement>())
            {
                string colName = (string?)ic.Attribute("cachedName")
                              ?? (string?)ic.Attribute("name") ?? "";
                string? usage = ic.Element("properties")
                                ?.Elements("property")
                                .FirstOrDefault(p => (string?)p.Attribute("name") == "PivotUsage")
                                ?.Value;
                switch (usage)
                {
                    case "1": idCols.Add(colName); break;
                    case "2": nameCol  = colName; break;
                    case "3": valueCol = colName; break;
                }
            }
        }

        var pivotKeys = c.Outputs
            .SelectMany(o => (o.Element?.Element("outputColumns")
                                       ?.Elements("outputColumn")
                                       ?? Enumerable.Empty<System.Xml.Linq.XElement>()))
            .Select(col => col.Element("properties")
                            ?.Elements("property")
                            .FirstOrDefault(p => (string?)p.Attribute("name") == "PivotKeyValue")
                            ?.Value ?? "")
            .Where(s => !string.IsNullOrEmpty(s))
            .Distinct()
            .ToList();

        if (idCols.Count > 0)
            w.Line("id_cols: [" + string.Join(", ", idCols.Select(YamlWriter.Id)) + "]");
        else
            w.Comment("TODO: no id_cols (PivotUsage=1) found — fill manually");

        if (nameCol != null)  w.Line($"name_col: {YamlWriter.Id(nameCol)}");
        else                  w.Comment("TODO: no name_col (PivotUsage=2) found");
        if (valueCol != null) w.Line($"value_col: {YamlWriter.Id(valueCol)}");
        else                  w.Comment("TODO: no value_col (PivotUsage=3) found");

        if (pivotKeys.Count > 0)
            w.Line("pivot_keys: [" + string.Join(", ", pivotKeys.Select(YamlWriter.Id)) + "]");
        else
            w.Comment("TODO: no pivot_keys found — list the distinct name_col values");
        w.Indent(-2);
    }
}
