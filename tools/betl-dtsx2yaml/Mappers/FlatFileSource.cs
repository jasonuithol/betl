/* Flat File Source → csv.read.
 *
 * The path comes from the referenced Flat File ConnectionManager
 * (which we don't emit at the package level — see Converter
 * EmitConnections). Column definitions live in the Flat File
 * ConnectionManager too; for v0.2 we emit a TODO comment for the
 * column list rather than translating the full SSIS column metadata
 * (collation/codepage/precision-scale all need careful mapping). */

namespace Betl.Dtsx2Yaml.Mappers;

public static class FlatFileSource
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c)
    {
        var conn = ConnectionLookup.For(pkg, c);
        var path = conn?.Payload ?? "";

        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: csv.read");
        if (!string.IsNullOrEmpty(path))
            w.Line("path: " + YamlWriter.Quote(path));
        else
            w.Comment("TODO: flat-file path not found — set path: manually");
        w.Comment("TODO: column schema preserved from SSIS — fill `schema:`");
        w.Comment("based on the original Flat File Connection Manager columns.");
        w.Indent(-2);
    }
}
