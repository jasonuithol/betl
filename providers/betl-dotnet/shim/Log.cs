/* User-facing log surface, mirroring the lua provider's `log.*`
 * helpers. Levels match BetlLogLevel: trace=10, debug=20, info=30,
 * warn=40, error=50.
 *
 * Callers must own thread-safety for shared state they touch in
 * scripts; betl_log itself is safe to call from any thread. */

using System;
using System.Text;

namespace Betl;

public static unsafe class Log
{
    public static void Trace(string msg) => Write(10, msg);
    public static void Debug(string msg) => Write(20, msg);
    public static void Info (string msg) => Write(30, msg);
    public static void Warn (string msg) => Write(40, msg);
    public static void Error(string msg) => Write(50, msg);

    static void Write(int level, string msg)
    {
        if (Bridges.LogFn == null) return;
        /* UTF-8 + NUL terminator so the host can treat it as a C
         * string. Stackalloc would be nicer but msg length is
         * unbounded; rent a heap byte[] and pin it. */
        var bytes = Encoding.UTF8.GetBytes(msg ?? string.Empty);
        var buf = new byte[bytes.Length + 1];
        Array.Copy(bytes, buf, bytes.Length);
        fixed (byte* p = buf)
        {
            Bridges.LogFn(Bridges.Ctx, level, p);
        }
    }
}
