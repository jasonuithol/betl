/* Flat File Source → csv.read.
 *
 * The path comes from the referenced Flat File ConnectionManager
 * (which we don't emit at the package level — see Converter
 * EmitConnections). Column definitions live in the Flat File
 * ConnectionManager too; for v0.2 we emit a TODO comment for the
 * column list rather than translating the full SSIS column metadata
 * (collation/precision-scale still need careful mapping). CodePage
 * is read from the inner ConnectionManager element and emitted as
 * the betl `encoding:` parameter so non-UTF-8 sources (the common
 * legacy SSIS case: cp1252 in European deployments) preserve their
 * original semantics. */

using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class FlatFileSource
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c)
    {
        var conn = ConnectionLookup.For(pkg, c);
        var path = conn?.Payload ?? "";
        var encoding = ResolveEncodingFor(conn);

        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: csv.read");
        if (!string.IsNullOrEmpty(path))
            w.Line("path: " + YamlWriter.Quote(path));
        else
            w.Comment("TODO: flat-file path not found — set path: manually");
        if (!string.IsNullOrEmpty(encoding))
            w.Line($"encoding: {encoding}");
        w.Comment("TODO: column schema preserved from SSIS — fill `schema:`");
        w.Comment("based on the original Flat File Connection Manager columns.");
        w.Indent(-2);
    }

    /* Inspect the FlatFileConnectionManager XML for the source codepage.
     *
     * Two SSIS shapes are common:
     *   <DTS:ConnectionManager DTS:CodePage="1252" DTS:Unicode="False" .../>
     *   <DTS:Property DTS:Name="CodePage">1252</DTS:Property> ... <DTS:Property DTS:Name="Unicode">-1</DTS:Property>
     *
     * Unicode="True" / "-1" means UTF-16-LE (SSIS calls this "Unicode"),
     * in which case CodePage is irrelevant. Otherwise we map the CodePage
     * number to a string betl's csv.read understands — typically the
     * numeric form like "1252" or "932", which the encoding helper
     * normalises to CP1252 / SHIFT_JIS for iconv.
     *
     * Returns "" if no codepage is declared (csv.read defaults to UTF-8). */
    public static string ResolveEncodingFor(DtsxConnection? conn)
    {
        var el = conn?.Element;
        if (el is null) return "";

        string? unicodeRaw = null;
        string? codePage   = null;

        foreach (var attr in el.Descendants().SelectMany(d => d.Attributes()))
        {
            if (attr.Name.LocalName == "CodePage" && codePage is null)
                codePage = attr.Value;
            else if (attr.Name.LocalName == "Unicode" && unicodeRaw is null)
                unicodeRaw = attr.Value;
        }
        foreach (var p in el.Descendants()
                            .Where(d => d.Name.LocalName == "Property"))
        {
            var nm = p.Attributes().FirstOrDefault(a => a.Name.LocalName == "Name")?.Value;
            if (nm == "CodePage" && codePage is null) codePage = p.Value;
            else if (nm == "Unicode" && unicodeRaw is null) unicodeRaw = p.Value;
        }

        if (IsTruthy(unicodeRaw)) return "utf-16";
        if (!string.IsNullOrEmpty(codePage) && codePage != "0")
        {
            /* CodePage 65001 = UTF-8 — let csv.read use its default path. */
            if (codePage == "65001") return "";
            return codePage;
        }
        return "";
    }

    static bool IsTruthy(string? v)
    {
        if (string.IsNullOrEmpty(v)) return false;
        return v == "True" || v == "true" || v == "-1" || v == "1";
    }
}
