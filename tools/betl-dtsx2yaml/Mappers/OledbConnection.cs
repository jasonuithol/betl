/* OLEDB / ADO.NET connection-string → betl `connections.<name>`.
 *
 * SSIS connection strings are provider-keyed (Provider=SQLNCLI11;...,
 * or Provider=MSOLEDBSQL;..., or Provider=Microsoft.ACE.OLEDB.16.0;...
 * for Access/Excel). betl currently understands MSSQL (via unixODBC)
 * and Postgres (via libpq DSNs). For anything else we emit a TODO.
 *
 * Translation strategy is best-effort: parse the SSIS key/value
 * format, extract Data Source / Initial Catalog / Auth, build a
 * plausible ODBC DSN or libpq DSN. Operators are expected to verify
 * the result — provider-specific options (MultiSubnetFailover etc.)
 * don't survive the round-trip. */

using System;
using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class OledbConnection
{
    public static void Emit(YamlWriter w, DtsxConnection c)
    {
        var parts = ParseConnectionString(c.Payload);
        string? provider = LookupCaseInsensitive(parts, "Provider");
        bool isSql = provider != null
            && (provider.StartsWith("SQLNCLI", StringComparison.OrdinalIgnoreCase)
             || provider.StartsWith("MSOLEDBSQL", StringComparison.OrdinalIgnoreCase)
             || provider.StartsWith("SQLOLEDB",  StringComparison.OrdinalIgnoreCase));
        /* Postgres in SSIS usually goes through Npgsql via ADO.NET
         * rather than OLEDB, but be permissive in detection. */
        bool isPg  = provider != null
            && (provider.StartsWith("Npgsql",     StringComparison.OrdinalIgnoreCase)
             || provider.Contains("Postgres",     StringComparison.OrdinalIgnoreCase)
             || (LookupCaseInsensitive(parts, "Driver") ?? "")
                  .Contains("PostgreSQL", StringComparison.OrdinalIgnoreCase));

        w.Line(YamlWriter.Id(c.Name) + ":");
        w.Indent(2);
        if (isSql)
        {
            w.Line("type: mssql");
            w.Line("dsn: " + YamlWriter.Quote(BuildMssqlDsn(parts)));
        }
        else if (isPg)
        {
            w.Line("type: postgres");
            w.Line("dsn: " + YamlWriter.Quote(BuildPgDsn(parts)));
        }
        else
        {
            w.Line("type: mssql");
            w.Comment($"TODO: SSIS provider '{provider ?? "?"}' is not directly "
                    + "supported — verify the DSN below");
            /* Best effort: just hand the raw string back to the user. */
            w.Line("dsn: " + YamlWriter.Quote(c.Payload));
        }
        w.Indent(-2);
    }

    static string BuildMssqlDsn(Dictionary<string, string> parts)
    {
        var server = LookupCaseInsensitive(parts, "Data Source")
                  ?? LookupCaseInsensitive(parts, "Server")
                  ?? "(local)";
        var db = LookupCaseInsensitive(parts, "Initial Catalog")
              ?? LookupCaseInsensitive(parts, "Database")
              ?? "";
        var integrated =
            (LookupCaseInsensitive(parts, "Integrated Security") ?? "")
                .Equals("SSPI", StringComparison.OrdinalIgnoreCase)
         || (LookupCaseInsensitive(parts, "Integrated Security") ?? "")
                .Equals("True", StringComparison.OrdinalIgnoreCase);
        var user = LookupCaseInsensitive(parts, "User ID")
                ?? LookupCaseInsensitive(parts, "UID");
        var pw   = LookupCaseInsensitive(parts, "Password")
                ?? LookupCaseInsensitive(parts, "PWD");

        var sb = new System.Text.StringBuilder();
        sb.Append("Driver={ODBC Driver 18 for SQL Server};Server=").Append(server);
        if (!string.IsNullOrEmpty(db)) sb.Append(";Database=").Append(db);
        if (integrated)
        {
            sb.Append(";Trusted_Connection=yes");
        }
        else if (user != null)
        {
            sb.Append(";UID=").Append(user);
            if (pw != null) sb.Append(";PWD=").Append(pw);
        }
        sb.Append(";TrustServerCertificate=yes");
        return sb.ToString();
    }

    static string BuildPgDsn(Dictionary<string, string> parts)
    {
        /* Map common SSIS/Npgsql keys → libpq DSN keys. */
        var host = LookupCaseInsensitive(parts, "Host")
                ?? LookupCaseInsensitive(parts, "Server")
                ?? LookupCaseInsensitive(parts, "Data Source")
                ?? "localhost";
        var port = LookupCaseInsensitive(parts, "Port") ?? "5432";
        var db   = LookupCaseInsensitive(parts, "Database")
                ?? LookupCaseInsensitive(parts, "Initial Catalog")
                ?? "";
        var user = LookupCaseInsensitive(parts, "Username")
                ?? LookupCaseInsensitive(parts, "User ID")
                ?? LookupCaseInsensitive(parts, "UID");
        var pw   = LookupCaseInsensitive(parts, "Password")
                ?? LookupCaseInsensitive(parts, "PWD");
        var sb = new System.Text.StringBuilder();
        sb.Append("postgresql://");
        if (user != null) { sb.Append(user); if (pw != null) sb.Append(':').Append(pw); sb.Append('@'); }
        sb.Append(host).Append(':').Append(port);
        if (!string.IsNullOrEmpty(db)) sb.Append('/').Append(db);
        return sb.ToString();
    }

    /* SSIS connection strings are roughly key=value;key=value with
     * optional whitespace and braces around quoted values. */
    static Dictionary<string, string> ParseConnectionString(string s)
    {
        var d = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        int i = 0;
        while (i < s.Length)
        {
            while (i < s.Length && (s[i] == ' ' || s[i] == ';')) ++i;
            int eq = s.IndexOf('=', i);
            if (eq < 0) break;
            string key = s[i..eq].Trim();
            i = eq + 1;
            while (i < s.Length && s[i] == ' ') ++i;
            string val;
            if (i < s.Length && s[i] == '{')
            {
                int end = s.IndexOf('}', i + 1);
                if (end < 0) { val = s[(i + 1)..]; i = s.Length; }
                else { val = s[(i + 1)..end]; i = end + 1; }
            }
            else
            {
                int semi = s.IndexOf(';', i);
                if (semi < 0) { val = s[i..]; i = s.Length; }
                else { val = s[i..semi]; i = semi; }
            }
            d[key] = val.Trim();
        }
        return d;
    }

    static string? LookupCaseInsensitive(Dictionary<string, string> d, string key)
        => d.TryGetValue(key, out var v) ? v : null;
}
