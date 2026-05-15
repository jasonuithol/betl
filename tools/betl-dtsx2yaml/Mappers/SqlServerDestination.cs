/* SQL Server Destination → mssql.upsert.
 *
 * Microsoft.SQLServerDestination is SSIS' SQL-Server-specific bulk
 * insert sink. It only runs against a co-located SQL Server (uses
 * BULK INSERT under the hood) and is heavily used in older packages
 * that prefer it over OLEDB Destination for throughput. From betl's
 * perspective there's no need for a distinct component: the user
 * said "just the ability to write to SQL Server", so we route it
 * through the same mssql.upsert that OLEDB Destination uses.
 *
 * Property differences vs OLEDB Destination:
 *   - Table name is in `BulkInsertTableName` (not `OpenRowset`).
 *   - No AccessMode — it's always bulk insert.
 *   - Bunch of bulk-insert knobs (KeepIdentity, KeepNulls, FireTriggers,
 *     CheckConstraints, FirstRow, LastRow, MaxErrors, Order, Tablock,
 *     Timeout, MaxInsertCommitSize) that betl's mssql.upsert doesn't
 *     have parity with — surfaced as TODO comments so the operator
 *     can decide whether to keep equivalent behaviour. */

namespace Betl.Dtsx2Yaml.Mappers;

public static class SqlServerDestination
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        var conn = ConnectionLookup.For(pkg, c);
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: mssql.upsert");
        if (fromId != null) w.Line($"from: {fromId}");
        w.Line($"connection: {YamlWriter.Id(conn?.Name ?? "warehouse")}");

        c.Properties.TryGetValue("BulkInsertTableName", out var table);
        if (!string.IsNullOrEmpty(table))
            w.Line($"table: {YamlWriter.Quote(table)}");
        else
            w.Comment("TODO: SQL Server Destination has no BulkInsertTableName — table unknown");

        /* Surface the non-trivial bulk-insert knobs so the operator can
         * decide. We don't translate them — betl's mssql.upsert is
         * higher level than BULK INSERT options. */
        FlagOptional(w, c, "BulkInsertKeepIdentity",   "true",
            "KEEPIDENTITY (preserve source identity values)");
        FlagOptional(w, c, "BulkInsertFireTriggers",   "true",
            "FIRE_TRIGGERS (run insert triggers)");
        FlagOptional(w, c, "BulkInsertCheckConstraints", "false",
            "CHECK_CONSTRAINTS=false (skip constraint validation)");

        w.Comment("TODO: SSIS SQL Server Destination is insert-only; betl upsert needs");
        w.Comment("a key column list. Replace [] below with the actual primary key.");
        w.Line("key: []");
        w.Indent(-2);
    }

    /* Emit a TODO comment when a property has a value that diverges
     * from betl's default behaviour and is worth flagging. */
    static void FlagOptional(YamlWriter w, DtsxComponent c, string prop,
                             string nonDefault, string explanation)
    {
        if (c.Properties.TryGetValue(prop, out var v)
            && v.Equals(nonDefault, System.StringComparison.OrdinalIgnoreCase))
        {
            w.Comment($"TODO: SSIS {prop}={nonDefault} — {explanation}; "
                    + "betl mssql.upsert has no equivalent knob.");
        }
    }
}
