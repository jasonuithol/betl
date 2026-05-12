/* Connection lookup. Returns the connection's raw JSON blob exactly
 * as the host has it — the script is responsible for parsing fields
 * out (e.g. dsn, type, schema). Matches the lua provider's
 * `connection(name)` helper. */

using System;
using System.Text;

namespace Betl;

public static unsafe class Connection
{
    public static string? Get(string name)
    {
        if (Bridges.GetConnectionFn == null) return null;
        var nb = Encoding.UTF8.GetBytes((name ?? string.Empty) + "\0");
        fixed (byte* np = nb)
        {
            byte* r = Bridges.GetConnectionFn(Bridges.Ctx, np);
            if (r == null) return null;
            int len = 0;
            while (r[len] != 0) ++len;
            return Encoding.UTF8.GetString(r, len);
        }
    }
}
