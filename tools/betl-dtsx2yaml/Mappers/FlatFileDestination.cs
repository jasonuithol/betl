/* Flat File Destination → csv.write. */

namespace Betl.Dtsx2Yaml.Mappers;

public static class FlatFileDestination
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        var conn = ConnectionLookup.For(pkg, c);
        var path = conn?.Payload ?? "";

        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: csv.write");
        if (fromId != null) w.Line($"from: {fromId}");
        if (!string.IsNullOrEmpty(path))
            w.Line("path: " + YamlWriter.Quote(path));
        else
            w.Comment("TODO: flat-file path not found — set path: manually");
        w.Indent(-2);
    }
}
