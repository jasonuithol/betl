/* Common bridge-pointer registration entry point. Both dotnet.task
 * and dotnet.script call this once at .so load to plumb the host
 * callbacks (log / get_param / get_connection) onto Bridges. */

using System;
using System.Runtime.InteropServices;

namespace Betl;

public static unsafe class Init
{
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_init")]
    public static int Register(
        IntPtr ctx,
        delegate* unmanaged<IntPtr, int, byte*, void> logFn,
        delegate* unmanaged<IntPtr, byte*, byte*> getParamFn,
        delegate* unmanaged<IntPtr, byte*, byte*> getConnectionFn)
    {
        Bridges.Ctx             = ctx;
        Bridges.LogFn           = logFn;
        Bridges.GetParamFn      = getParamFn;
        Bridges.GetConnectionFn = getConnectionFn;
        return 0;
    }
}
