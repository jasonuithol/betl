/* Flat File Destination → csv.write. Mirrors the source-side mapper:
 * the inner FlatFileConnectionManager's CodePage / Unicode attributes
 * become the betl `encoding:` parameter so a round-tripped recipe
 * preserves the target codepage SSIS was producing. */

namespace Betl.Dtsx2Yaml.Mappers;

public static class FlatFileDestination
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        var conn = ConnectionLookup.For(pkg, c);
        var path = conn?.Payload ?? "";
        var encoding = FlatFileSource.ResolveEncodingFor(conn);

        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: csv.write");
        if (fromId != null) w.Line($"from: {fromId}");
        if (!string.IsNullOrEmpty(path))
            w.Line("path: " + YamlWriter.Quote(path));
        else
            w.Comment("TODO: flat-file path not found — set path: manually");
        if (!string.IsNullOrEmpty(encoding))
            w.Line($"encoding: {encoding}");
        w.Indent(-2);
    }
}
