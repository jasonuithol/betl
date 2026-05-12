/* Bridges between AOT'd user code and the betl host.
 *
 * The host calls betl_dotnet_init() once at .so load, passing function
 * pointers for the host-side primitives (logging, parameter lookup,
 * connection lookup). We stash them as `delegate*<...>` fields here;
 * the higher-level wrappers in Log/Params/Connection use them.
 *
 * Why function pointers rather than DllImport: NativeAOT can use
 * DllImport with the special "self" / "__Internal" name for callbacks
 * into the host process, but the conventions differ subtly between
 * platforms. Passing function pointers at init time is a zero-magic
 * approach that works identically on every platform NativeAOT supports.
 */

using System;

namespace Betl;

internal static unsafe class Bridges
{
    /* Opaque BetlContext* — opaque to the AOT'd code, we just pass it
     * back to the host on every callback. */
    internal static IntPtr Ctx;

    /* void betl_dotnet_log(BetlContext*, int level, const char *msg). */
    internal static delegate* unmanaged<IntPtr, int, byte*, void> LogFn;

    /* const char *betl_get_param(BetlContext*, const char *name).
     * Returned pointer is host-owned (no free on our side). */
    internal static delegate* unmanaged<IntPtr, byte*, byte*> GetParamFn;

    /* const char *betl_get_connection(BetlContext*, const char *name).
     * Returned pointer is host-owned. The value is the connection's
     * JSON blob (whatever the YAML declared under connections.<name>);
     * parsing is the script's responsibility. */
    internal static delegate* unmanaged<IntPtr, byte*, byte*> GetConnectionFn;
}
