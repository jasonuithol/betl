/* SSIS Character Map → betl `map` with `add:` per output column.
 *
 * SSIS Character Map applies one or more character-class operations to
 * a column. The operations are a bitfield in `<property
 * name="MapFlags">`:
 *
 *   1   Uppercase
 *   2   Lowercase
 *   4   ByteReversal
 *   8   Hiragana
 *   16  Katakana
 *   32  HalfWidth
 *   64  FullWidth
 *   128 SimplifiedChinese
 *   256 TraditionalChinese
 *   512 LinguisticCasing
 *
 * The source column comes through SSIS' lineage ID
 * (`<property name="InputColumnLineageID">`), which we can't resolve
 * without the full lineage table. SSIS' Character Map also records
 * the source column's friendly name in `cachedName` on the matching
 * `<inputColumn>` — we prefer that when present.
 *
 * The output flavour can be `InPlaceChange` (replace existing) or
 * `NewColumn` (add a new column). We emit `add:` either way; replacing
 * is a manual delete of the input from `select:` downstream.
 *
 * Mappings to ssisexpr:
 *
 *   Uppercase  → UPPER([col])
 *   Lowercase  → LOWER([col])
 *
 * Everything else (locale-aware width / kana / Chinese conversions)
 * has no SSIS-expression equivalent — we emit a TODO listing the
 * operation flags so the operator can plug in their own translation
 * (typically via a Script Component). */

using System.Linq;
using System.Text;

namespace Betl.Dtsx2Yaml.Mappers;

public static class CharacterMap
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
            w.Comment("TODO: Character Map has no <outputColumn> entries — verify");
            w.Indent(-2);
            return;
        }

        w.Line("add:");
        w.Indent(2);
        foreach (var col in outputCols)
        {
            string name = (string?)col.Attribute("name") ?? "";
            int flags = ReadIntProperty(col, "MapFlags");
            string srcCachedName = ReadStringProperty(col, "InputColumnLineageID");

            w.Line($"{YamlWriter.Id(name)}:");
            w.Indent(2);
            EmitColumnExpression(w, name, srcCachedName, flags);
            w.Line("type: u");
            w.Indent(-2);
        }
        w.Indent(-2);
        w.Indent(-2);
    }

    static void EmitColumnExpression(YamlWriter w, string outName,
                                     string srcCachedName, int flags)
    {
        /* When the source lineage points at #N, we don't have a name —
         * fall back to the output name and flag the substitution. */
        string srcLabel = !string.IsNullOrEmpty(srcCachedName) && !srcCachedName.StartsWith("#")
                              ? srcCachedName
                              : outName;

        var ops = DescribeFlags(flags);
        if (ops.Count == 0)
        {
            w.Comment("TODO: Character Map has no MapFlags — verify");
            w.Line("lang: ssisexpr");
            w.Line($"expr: {YamlWriter.Quote("[" + srcLabel + "]")}");
            return;
        }

        bool upper = (flags & 1) != 0;
        bool lower = (flags & 2) != 0;
        bool onlyCase = (flags & ~3) == 0;

        if (onlyCase && (upper ^ lower))
        {
            string fn = upper ? "UPPER" : "LOWER";
            w.Line("lang: ssisexpr");
            w.Line($"expr: {YamlWriter.Quote(fn + "([" + srcLabel + "])")}");
            if (srcCachedName.StartsWith("#"))
                w.Comment($"TODO: replace [{srcLabel}] with the actual input column name");
            return;
        }

        /* Anything beyond plain UPPER/LOWER — we can't translate the
         * locale-sensitive operations into ssisexpr. */
        w.Comment("TODO: SSIS Character Map operations not expressible in ssisexpr:");
        w.Comment("        " + string.Join(", ", ops));
        w.Comment("      Likely needs a betl `dotnet.script` step or a SQL function.");
        w.Line("lang: ssisexpr");
        w.Line($"expr: {YamlWriter.Quote("[" + srcLabel + "]")}");
    }

    static System.Collections.Generic.List<string> DescribeFlags(int f)
    {
        var ops = new System.Collections.Generic.List<string>();
        if ((f & 1) != 0)   ops.Add("Uppercase");
        if ((f & 2) != 0)   ops.Add("Lowercase");
        if ((f & 4) != 0)   ops.Add("ByteReversal");
        if ((f & 8) != 0)   ops.Add("Hiragana");
        if ((f & 16) != 0)  ops.Add("Katakana");
        if ((f & 32) != 0)  ops.Add("HalfWidth");
        if ((f & 64) != 0)  ops.Add("FullWidth");
        if ((f & 128) != 0) ops.Add("SimplifiedChinese");
        if ((f & 256) != 0) ops.Add("TraditionalChinese");
        if ((f & 512) != 0) ops.Add("LinguisticCasing");
        return ops;
    }

    static int ReadIntProperty(System.Xml.Linq.XElement col, string name)
    {
        string? v = col.Element("properties")?.Elements("property")
                       .FirstOrDefault(p => (string?)p.Attribute("name") == name)?.Value;
        return int.TryParse(v, out var i) ? i : 0;
    }

    static string ReadStringProperty(System.Xml.Linq.XElement col, string name)
    {
        return col.Element("properties")?.Elements("property")
                  .FirstOrDefault(p => (string?)p.Attribute("name") == name)?.Value ?? "";
    }
}
