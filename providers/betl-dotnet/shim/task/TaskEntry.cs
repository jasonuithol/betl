/* dotnet.task-only entry point. The user defines a public class
 * named `UserTask` extending Betl.BetlTask; we instantiate it and
 * call Run() in this AOT-exported function. Managed exceptions are
 * caught here so unmanaged code never sees them cross the ABI.
 *
 * Common Init (bridge registration) lives in the common shim's
 * Init.cs and is shared with dotnet.script.
 */

using System;
using System.Runtime.InteropServices;

namespace Betl;

public static unsafe class TaskEntry
{
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
            try { Log.Error(ex.ToString()); }
            catch { /* host has no log fn? swallow. */ }
            return 1;
        }
    }
}
