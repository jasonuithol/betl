/* Read-only access to pipeline parameters (matches the lua provider's
 * `params.<name>` surface). Returns null when the parameter is not
 * declared. */

using System;
using System.Text;

namespace Betl;

public static unsafe class Params
{
    public static string? Get(string name)
    {
        if (Bridges.GetParamFn == null) return null;
        var nb = Encoding.UTF8.GetBytes((name ?? string.Empty) + "\0");
        fixed (byte* np = nb)
        {
            byte* r = Bridges.GetParamFn(Bridges.Ctx, np);
            if (r == null) return null;
            /* C string back from the host. Find the NUL ourselves. */
            int len = 0;
            while (r[len] != 0) ++len;
            return Encoding.UTF8.GetString(r, len);
        }
    }
}
