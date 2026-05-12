/* OLEDB Destination → mssql.upsert / postgres.upsert.
 *
 * SSIS' OLEDB Destination is insert-only by default (with a few
 * fast-load knobs). betl's `*.upsert` requires a key column list.
 * We emit `key: []` with a TODO so the user fills in the merge key
 * — or replaces with a different sink if they truly want
 * insert-only behaviour (not currently in betl). */

namespace Betl.Dtsx2Yaml.Mappers;

public static class OledbDestination
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        var conn = ConnectionLookup.For(pkg, c);
        string componentType = "mssql.upsert";
        if (conn != null && IsPostgres(conn)) componentType = "postgres.upsert";

        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line($"type: {componentType}");
        if (fromId != null) w.Line($"from: {fromId}");
        w.Line($"connection: {YamlWriter.Id(conn?.Name ?? "warehouse")}");

        c.Properties.TryGetValue("OpenRowset", out var rowset);
        if (!string.IsNullOrEmpty(rowset))
            w.Line($"table: {YamlWriter.Quote(rowset)}");
        else
            w.Comment("TODO: OLE DB Destination has no OpenRowset — table unknown");

        w.Comment("TODO: SSIS OLEDB Destination is insert-only; betl upsert needs");
        w.Comment("a key column list. Replace [] below with the actual primary key.");
        w.Line("key: []");
        w.Indent(-2);
    }

    static bool IsPostgres(DtsxConnection c)
    {
        var s = c.Payload ?? "";
        return s.Contains("Postgres", System.StringComparison.OrdinalIgnoreCase)
            || s.Contains("Npgsql", System.StringComparison.OrdinalIgnoreCase);
    }
}
