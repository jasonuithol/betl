/* SSIS Derived Column → betl `map` with `add:`.
 *
 * SSIS Derived Column adds (or replaces) output columns whose values
 * come from SSIS expressions. Each output column has:
 *   - `name` attribute (the new/replaced column name)
 *   - `<property name="Expression">[Old] + " suffix"</property>`
 *   - `<property name="FriendlyExpression">...same, more readable...</property>`
 *
 * betl map syntax:
 *
 *   type: map
 *   from: ...
 *   add:
 *     full_name:
 *       lang: ssisexpr
 *       expr: "[FirstName] + ' ' + [LastName]"
 *       type: u
 *
 * SSIS expression translation runs at evaluate time through the
 * betl-ssisexpr engine, so we emit `lang: ssisexpr` and pass the
 * expression text through verbatim. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class DerivedColumn
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
            w.Comment("TODO: Derived Column has no <outputColumn> entries — verify");
            w.Indent(-2);
            return;
        }
        w.Line("add:");
        w.Indent(2);
        foreach (var col in outputCols)
        {
            string name = (string?)col.Attribute("name") ?? "";
            string? friendly = col.Element("properties")
                                ?.Elements("property")
                                .FirstOrDefault(p => (string?)p.Attribute("name") == "FriendlyExpression")
                                ?.Value;
            string? raw = col.Element("properties")
                            ?.Elements("property")
                            .FirstOrDefault(p => (string?)p.Attribute("name") == "Expression")
                            ?.Value;
            string expr = friendly ?? raw ?? "";

            /* Arrow format hint from the column's `dataType` attribute. */
            string? dt = (string?)col.Attribute("dataType");
            string fmt = DtsxDataTypeToArrowFormat(dt);

            w.Line($"{YamlWriter.Id(name)}:");
            w.Indent(2);
            w.Line("lang: ssisexpr");
            w.Line("expr: " + YamlWriter.Quote(expr));
            if (!string.IsNullOrEmpty(fmt)) w.Line($"type: {fmt}");
            w.Indent(-2);
        }
        w.Indent(-2);
        w.Indent(-2);
    }

    /* SSIS DTSDataType → Arrow C format character (where the mapping is
     * unambiguous). Returns "" for types we can't safely auto-translate
     * — the operator can add `type:` manually. */
    public static string DtsxDataTypeToArrowFormat(string? dt) => dt switch
    {
        "i1" => "c",   /* DT_I1 → int8  */
        "ui1" => "C",  /* DT_UI1 → uint8 */
        "i2" => "s",   /* DT_I2 → int16 */
        "ui2" => "S",  /* DT_UI2 → uint16 */
        "i4" => "i",   /* DT_I4 → int32 */
        "ui4" => "I",  /* DT_UI4 → uint32 */
        "i8" => "l",   /* DT_I8 → int64 */
        "ui8" => "L",  /* DT_UI8 → uint64 */
        "r4" => "f",   /* DT_R4 → float32 */
        "r8" => "g",   /* DT_R8 → float64 */
        "bool" => "b", /* DT_BOOL → bool */
        "wstr" => "u",
        "str"  => "u", /* both map to utf8 — encoding handled at read-time */
        "text" => "u",
        "ntext" => "u",
        _ => "",
    };
}
