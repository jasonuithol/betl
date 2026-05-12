/* Execute SQL Task → betl `sql.execute`.
 *
 * sql.execute is provider-agnostic — it dispatches to the actual SQL
 * engine based on the referenced connection's `type:` (mssql /
 * postgres / mysql / ...). So we just emit `type: sql.execute` plus
 * `connection:` + `sql:`; the connection block (emitted earlier by
 * OledbConnection / etc.) carries the engine choice.
 *
 * The DTSX shape (simplified):
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
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxExecutable exe,
                            FlowAttrs? flow)
    {
        w.Line($"- id: {YamlWriter.Id(exe.Name)}");
        w.Indent(2);
        FlowAttrs.Emit(w, flow);

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

        w.Line("type: sql.execute");
        w.Line($"connection: {YamlWriter.Id(conn?.Name ?? "warehouse")}");
        if (!string.IsNullOrEmpty(sql))
        {
            /* Use a YAML block scalar to preserve multi-line SQL. */
            w.Line("sql: |");
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
