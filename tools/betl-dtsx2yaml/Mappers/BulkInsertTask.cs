/* SSIS Bulk Insert Task → betl `sql.execute` running a T-SQL BULK INSERT.
 *
 * BIT is a SQL-Server-only thin wrapper over T-SQL's `BULK INSERT`
 * statement. The natural betl mapping is a sql.execute that issues
 * the same statement against the referenced mssql connection — same
 * behaviour, slightly more obvious in the YAML.
 *
 * DTSX shape (attributes live in a separate SQLTask namespace):
 *   <DTS:ObjectData>
 *     <BulkInsertTaskData
 *         SQL:Connection="{guid}"
 *         SQL:DestinationTableName="dbo.stage"
 *         SQL:FieldTerminator=","
 *         SQL:RowTerminator="\r\n"
 *         SQL:FirstRow="2"
 *         SQL:CodePage="ACP"
 *         SQL:DataFileType="char"
 *         File="C:\inbox\file.csv" />
 *   </DTS:ObjectData>
 */

using System.Linq;
using System.Text;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class BulkInsertTask
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxExecutable exe,
                            FlowAttrs? flow)
    {
        w.Line($"- id: {YamlWriter.Id(exe.Name)}");
        w.Indent(2);
        FlowAttrs.Emit(w, flow);
        w.Line("type: sql.execute");

        var data = exe.ObjectData?.Descendants()
                     .FirstOrDefault(e => e.Name.LocalName == "BulkInsertTaskData");

        string? connRef = AttrByLocal(data, "Connection");
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
        w.Line($"connection: {YamlWriter.Id(conn?.Name ?? "warehouse")}");

        string table   = AttrByLocal(data, "DestinationTableName") ?? "TODO_TABLE";
        string file    = AttrByLocal(data, "File") ?? "";
        string fieldT  = AttrByLocal(data, "FieldTerminator") ?? ",";
        string rowT    = AttrByLocal(data, "RowTerminator")   ?? "\\n";
        string firstRow= AttrByLocal(data, "FirstRow")        ?? "";
        string codePg  = AttrByLocal(data, "CodePage")        ?? "";

        var sb = new StringBuilder();
        sb.Append("BULK INSERT ").Append(table)
          .Append(" FROM '").Append(file).Append("'\n")
          .Append("WITH (\n")
          .Append("  FIELDTERMINATOR = '").Append(fieldT).Append("',\n")
          .Append("  ROWTERMINATOR = '")  .Append(rowT)  .Append("'");
        if (!string.IsNullOrEmpty(firstRow))
            sb.Append(",\n  FIRSTROW = ").Append(firstRow);
        if (!string.IsNullOrEmpty(codePg) && codePg != "RAW")
            sb.Append(",\n  CODEPAGE = '").Append(codePg).Append("'");
        sb.Append("\n)");

        w.Line("sql: |");
        w.Indent(2);
        foreach (var line in sb.ToString().Split('\n')) w.Line(line);
        w.Indent(-2);

        w.Comment("note: SSIS Bulk Insert ran on the SQL Server host's");
        w.Comment("filesystem — the FROM path must be visible to the");
        w.Comment("server, not the betl runner.");
        w.Indent(-2);
    }

    static string? AttrByLocal(XElement? e, string localName)
        => e?.Attributes()
             .FirstOrDefault(a => a.Name.LocalName == localName)?.Value;
}
