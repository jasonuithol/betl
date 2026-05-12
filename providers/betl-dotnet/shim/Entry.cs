/* [UnmanagedCallersOnly] entry points exported as plain C symbols
 * from the compiled .so. The host C plugin (dotnet_provider.c) calls:
 *
 *   betl_dotnet_init(ctx, log_fnptr, get_param_fnptr)
 *     — called once at .so load. Returns 0 on success.
 *
 *   betl_dotnet_task_run()
 *     — runs the user's task once. Returns 0 on success, non-zero on
 *       exception (caught here so unmanaged code never sees managed
 *       exceptions crossing the ABI).
 *
 * The user's `UserTask : BetlTask` class is part of this same
 * compilation unit (the C plugin concatenates user source with the
 * shim before publish).
 */

using System;
using System.Runtime.InteropServices;

namespace Betl;

public static unsafe class Entry
{
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_init")]
    public static int Init(
        IntPtr ctx,
        delegate* unmanaged<IntPtr, int, byte*, void> logFn,
        delegate* unmanaged<IntPtr, byte*, byte*> getParamFn)
    {
        Bridges.Ctx        = ctx;
        Bridges.LogFn      = logFn;
        Bridges.GetParamFn = getParamFn;
        return 0;
    }

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_task_run")]
    public static int TaskRun()
    {
        try
        {
            var task = new UserTask();
            task.Run();
            return 0;
        }
        catch (Exception ex)
        {
            /* Surface the exception as an error log line, then return
             * non-zero so the host marks the task as failed. */
            try { Log.Error(ex.ToString()); }
            catch { /* nothing to do — host has no log fn? swallow. */ }
            return 1;
        }
    }
}
