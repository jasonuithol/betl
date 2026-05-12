/* Execute SQL Task → mssql.sql / pg.sql.
 *
 * The DTSX shape (simplified) is:
 *   <DTS:ObjectData>
 *     <SQLTask:SqlTaskData
 *         SQLTask:Connection="{...connection-id...}"
 *         SQLTask:SqlStatementSource="SELECT ..." ... />
 *   </DTS:ObjectData>
 *
 * SQLTask: is a separate namespace. We pull the SqlStatementSource
 * and Connection attributes off whichever element carries them
 * (the local-name match avoids us needing to bind the namespace). */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class ExecuteSqlTask
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxExecutable exe)
    {
        w.Line($"- id: {YamlWriter.Id(exe.Name)}");
        w.Indent(2);

        var sqlTask = exe.ObjectData?.Elements()
                        .FirstOrDefault(e => e.Name.LocalName == "SqlTaskData");
        string? connRef = null, sql = null;
        if (sqlTask != null)
        {
            foreach (var a in sqlTask.Attributes())
            {
                if (a.Name.LocalName == "Connection")          connRef = a.Value;
                else if (a.Name.LocalName == "SqlStatementSource") sql   = a.Value;
            }
        }

        /* Connection here is by *DTSID GUID* not by name. Match it
         * against pkg.Connections by their DTSID attribute. */
        DtsxConnection? conn = null;
        if (connRef != null)
        {
            foreach (var cm in pkg.Connections)
            {
                if (cm.Element?.Attribute(DtsxParser.DtsNs + "DTSID")?.Value == connRef)
                {
                    conn = cm; break;
                }
            }
        }

        bool isPg = conn?.Payload?.Contains("Postgres",
                        System.StringComparison.OrdinalIgnoreCase) == true
                 || conn?.Payload?.Contains("Npgsql",
                        System.StringComparison.OrdinalIgnoreCase) == true;
        w.Line(isPg ? "type: postgres.sql" : "type: mssql.sql");
        w.Line($"connection: {YamlWriter.Id(conn?.Name ?? "warehouse")}");
        if (!string.IsNullOrEmpty(sql))
        {
            /* Use a YAML block scalar to preserve multi-line SQL. */
            w.Line("query: |");
            w.Indent(2);
            foreach (var line in sql.Replace("\r\n", "\n").Split('\n'))
                w.Line(line);
            w.Indent(-2);
        }
        else
        {
            w.Comment("TODO: SQL statement source missing");
        }
        w.Indent(-2);
    }
}
