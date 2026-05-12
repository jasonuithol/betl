/* SSIS Data Conversion → betl `map` with `add:` + cast expressions.
 *
 * Data Conversion takes a column and produces a new column with a
 * specific target type. The output column carries the target Arrow
 * type via SSIS' `dataType` attribute, and the source column is
 * referenced through the column's `<property name="SourceInputColumnLineageID">`.
 *
 * betl has no first-class "cast transform"; the SSIS-expression engine
 * exposes the SSIS `(DT_*)` cast operator, so we emit:
 *
 *   type: map
 *   from: ...
 *   add:
 *     new_col:
 *       lang: ssisexpr
 *       expr: "(DT_I4)[source_col]"
 *       type: i
 *
 * The lineage-ID → input-column-name resolution is left as a TODO
 * marker since lineage IDs are local refs threading through the
 * package — accurate resolution needs the full input-column table. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class DataConvert
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
            w.Comment("TODO: Data Conversion has no <outputColumn> entries");
            w.Indent(-2);
            return;
        }
        w.Line("add:");
        w.Indent(2);
        foreach (var col in outputCols)
        {
            string name = (string?)col.Attribute("name") ?? "";
            string? dt  = (string?)col.Attribute("dataType");
            string fmt  = DerivedColumn.DtsxDataTypeToArrowFormat(dt);
            string dtsCast = SsisCastForDataType(dt);

            w.Line($"{YamlWriter.Id(name)}:");
            w.Indent(2);
            w.Line("lang: ssisexpr");
            w.Comment("TODO: replace SOURCE with the input column name");
            w.Comment("(SSIS lineage IDs don't auto-resolve)");
            w.Line($"expr: '({dtsCast})[SOURCE]'");
            if (!string.IsNullOrEmpty(fmt)) w.Line($"type: {fmt}");
            w.Indent(-2);
        }
        w.Indent(-2);
        w.Indent(-2);
    }

    static string SsisCastForDataType(string? dt) => dt switch
    {
        "i1"   => "DT_I1",   "ui1" => "DT_UI1",
        "i2"   => "DT_I2",   "ui2" => "DT_UI2",
        "i4"   => "DT_I4",   "ui4" => "DT_UI4",
        "i8"   => "DT_I8",   "ui8" => "DT_UI8",
        "r4"   => "DT_R4",   "r8"  => "DT_R8",
        "bool" => "DT_BOOL",
        "wstr" => "DT_WSTR", "str" => "DT_STR",
        "date" => "DT_DATE", "dbDate" => "DT_DBDATE",
        "dbTime"     => "DT_DBTIME",
        "dbTimeStamp"=> "DT_DBTIMESTAMP",
        _ => "DT_WSTR",
    };
}
