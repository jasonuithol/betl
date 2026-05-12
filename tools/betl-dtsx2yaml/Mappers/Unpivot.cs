/* SSIS Unpivot → betl `unpivot`.
 *
 *   type: unpivot
 *   from: ...
 *   id_cols: [...]
 *   value_cols: [...]    SSIS pivots one set of input columns per pivot key
 *   name_col: ...
 *   value_col: ...
 *
 * SSIS Unpivot input columns:
 *   - PivotKeyValue empty → passthrough (id_col)
 *   - PivotKeyValue set   → value_col, with the value being the
 *                           name-col literal for that input column
 *
 * Output columns identify the name_col (one carrying PivotKeyValueName)
 * and the value_col (one carrying DestinationColumn). */

using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Unpivot
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: unpivot");
        if (fromId != null) w.Line($"from: {fromId}");

        var idCols    = new List<string>();
        var valueCols = new List<string>();
        var input = c.Element?.Element("inputs")?.Element("input");
        if (input != null)
        {
            foreach (var ic in input.Element("inputColumns")?.Elements("inputColumn")
                                 ?? Enumerable.Empty<System.Xml.Linq.XElement>())
            {
                string colName = (string?)ic.Attribute("cachedName")
                              ?? (string?)ic.Attribute("name") ?? "";
                string? pkv = ic.Element("properties")
                                ?.Elements("property")
                                .FirstOrDefault(p => (string?)p.Attribute("name") == "PivotKeyValue")
                                ?.Value;
                if (string.IsNullOrEmpty(pkv)) idCols.Add(colName);
                else                            valueCols.Add(colName);
            }
        }

        string? nameCol = null, valueCol = null;
        foreach (var oc in c.Outputs
                            .SelectMany(o => (o.Element?.Element("outputColumns")
                                                       ?.Elements("outputColumn")
                                                       ?? Enumerable.Empty<System.Xml.Linq.XElement>())))
        {
            string ocName = (string?)oc.Attribute("name") ?? "";
            var props = oc.Element("properties")?.Elements("property").ToList()
                          ?? new List<System.Xml.Linq.XElement>();
            bool isNameCarrier = props.Any(p =>
                (string?)p.Attribute("name") == "PivotKeyValueName");
            if (isNameCarrier) nameCol = ocName;
            else if (valueCol == null) valueCol = ocName;
        }

        if (idCols.Count > 0)
            w.Line("id_cols: [" + string.Join(", ", idCols.Select(YamlWriter.Id)) + "]");
        else
            w.Comment("TODO: no id_cols (passthrough inputs) found");
        if (valueCols.Count > 0)
            w.Line("value_cols: [" + string.Join(", ", valueCols.Select(YamlWriter.Id)) + "]");
        else
            w.Comment("TODO: no value_cols (PivotKeyValue inputs) found");
        if (nameCol != null)  w.Line($"name_col: {YamlWriter.Id(nameCol)}");
        else                  w.Comment("TODO: name_col not identified");
        if (valueCol != null) w.Line($"value_col: {YamlWriter.Id(valueCol)}");
        else                  w.Comment("TODO: value_col not identified");
        w.Indent(-2);
    }
}
