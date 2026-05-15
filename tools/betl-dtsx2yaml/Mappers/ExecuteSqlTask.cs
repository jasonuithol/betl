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

        /* If a <DTS:PropertyExpression DTS:Name="SqlStatementSource">
         * is present, it's the runtime override — prefer it. Translate
         * its @[$Project::X] refs to betl ${params.x}. The static
         * SqlStatementSource above is the design-time fallback baked in
         * for SSIS validation; in practice it's a stale snapshot. */
        if (exe.PropertyExpressions.TryGetValue("SqlStatementSource", out var dyn)
            && !string.IsNullOrEmpty(dyn))
        {
            sql = Converter.TranslateSsisExpression(StripExpressionQuotes(dyn));
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

    /* SSIS string-expression syntax embeds SQL literals between
     * "..." with `+` concatenation:
     *     "UPDATE x SET y='" + @[$Project::A] + "' WHERE z=1"
     * For SQL injection into a betl `sql:` block, we want the inner
     * SQL with the @[$Project::A] refs already substituted out. After
     * Converter.TranslateSsisExpression rewrites the references, we
     * still have the surrounding "..." + "..." structure. Stripping the
     * concatenation/quotes is fiddly; just leave the raw form and let
     * the operator finish. We do strip one outer pair of straight
     * double-quotes if the WHOLE expression is one quoted literal,
     * which is the common case for simple parameterless statements. */
    static string StripExpressionQuotes(string s)
    {
        s = s.Trim();
        if (s.Length >= 2 && s[0] == '"' && s[^1] == '"' &&
            s.IndexOf('"', 1) == s.Length - 1)
        {
            return s.Substring(1, s.Length - 2);
        }
        return s;
    }
}
