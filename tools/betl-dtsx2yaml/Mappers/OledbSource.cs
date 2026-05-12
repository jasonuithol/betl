/* OLEDB Source → mssql.read or postgres.read (based on the
 * referenced connection's CreationName).
 *
 * SSIS' AccessMode tells us how the source picks data:
 *    0 = OpenRowset (table name)
 *    1 = OpenRowset from variable
 *    2 = SQL command
 *    3 = SQL command from variable
 *  We map 0 → "SELECT * FROM <table>" and 2 → the raw SqlCommand.
 *  Variables (1, 3) emit a TODO since variable-driven SQL needs a
 *  betl ${params.X} substitution which the operator should write. */

namespace Betl.Dtsx2Yaml.Mappers;

public static class OledbSource
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c)
    {
        var conn = ConnectionLookup.For(pkg, c);
        string componentType = conn?.CreationName switch
        {
            "OLEDB" => "mssql.read",
            "ADO.NET" => "mssql.read",
            _ => "mssql.read",
        };
        /* Postgres detection mirrors what OledbConnection does. */
        if (conn != null && IsPostgres(conn))
            componentType = "postgres.read";

        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line($"type: {componentType}");
        w.Line($"connection: {YamlWriter.Id(conn?.Name ?? "warehouse")}");

        c.Properties.TryGetValue("AccessMode", out var modeS);
        c.Properties.TryGetValue("SqlCommand", out var sql);
        c.Properties.TryGetValue("OpenRowset", out var rowset);
        int mode = 0; int.TryParse(modeS, out mode);
        switch (mode)
        {
            case 0:
            case 1:
                if (string.IsNullOrEmpty(rowset))
                {
                    w.Comment("TODO: AccessMode=table but OpenRowset missing");
                    w.Line("query: " + YamlWriter.Quote("SELECT 1"));
                }
                else
                {
                    w.Line("query: " + YamlWriter.Quote("SELECT * FROM " + rowset));
                }
                if (mode == 1)
                    w.Comment("note: original SSIS source read the table name from "
                            + "a variable; substitute via ${params.X} if needed");
                break;
            case 2:
            case 3:
                w.Line("query: " + YamlWriter.Quote(sql ?? "SELECT 1"));
                if (mode == 3)
                    w.Comment("note: original SSIS source read the SQL from a "
                            + "variable; substitute via ${params.X} if needed");
                break;
            default:
                w.Comment($"TODO: unknown AccessMode {mode} — manual edit needed");
                w.Line("query: " + YamlWriter.Quote("SELECT 1"));
                break;
        }
        w.Indent(-2);
    }

    static bool IsPostgres(DtsxConnection c)
    {
        var s = c.Payload ?? "";
        return s.Contains("Postgres", System.StringComparison.OrdinalIgnoreCase)
            || s.Contains("Npgsql", System.StringComparison.OrdinalIgnoreCase);
    }
}

internal static class ConnectionLookup
{
    public static DtsxConnection? For(DtsxPackage pkg, DtsxComponent c)
    {
        if (c.ConnectionManagerRefId == null) return null;
        /* refIds look like "Package.ConnectionManagers[Warehouse]" —
         * extract the bracketed name. */
        var rid = c.ConnectionManagerRefId;
        int lb = rid.IndexOf('[');
        int rb = rid.IndexOf(']', lb + 1);
        if (lb < 0 || rb < 0) return null;
        string name = rid[(lb + 1)..rb];
        foreach (var cm in pkg.Connections)
            if (cm.Name == name) return cm;
        return null;
    }
}
