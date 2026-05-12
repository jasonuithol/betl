/* Internal dispatch hub for dotnet.script.
 *
 * The plugin emits a Generated.cs alongside the static shim files at
 * compile-cache time. Generated.cs is responsible for:
 *
 *   1. Declaring `Betl.InputRow` and `Betl.OutputRow` partial classes
 *      with the schema-determined typed properties.
 *   2. Filling in the partial method `Dispatch.ExtractInputRow` —
 *      pulls each cell out of a row index in the upstream batch.
 *   3. Filling in the partial method `Dispatch.WriteOutputRow` —
 *      calls the host's per-cell emit setters for each declared
 *      output column.
 *
 * The non-generated bits live here:
 *   - The [UnmanagedCallersOnly] entry points the plugin C code
 *     calls into.
 *   - The static script instance and lifecycle.
 *   - The Emit batching glue (thread-local emit-context pointer).
 */

using System;
using System.Runtime.InteropServices;

namespace Betl;

internal static unsafe partial class Dispatch
{
    /* Singleton user-script instance, created at script init. */
    internal static BetlScript? Script;

    /* Set by the host before each batch / EOF call. Used by the
     * generated WriteOutputRow to address per-cell setter callbacks
     * against the right output staging context. */
    [ThreadStatic] private static IntPtr _emitCtx;

    /* Host-supplied setters. Filled in by ScriptInit. */
    internal static delegate* unmanaged<IntPtr, int, long,            void> SetInt64Fn;
    internal static delegate* unmanaged<IntPtr, int, double,          void> SetFloat64Fn;
    internal static delegate* unmanaged<IntPtr, int, byte,            void> SetBoolFn;
    internal static delegate* unmanaged<IntPtr, int, byte*, int,      void> SetUtf8Fn;
    internal static delegate* unmanaged<IntPtr, int,                  void> SetNullFn;
    internal static delegate* unmanaged<IntPtr,                       void> CommitRowFn;

    /* Partial methods filled by Generated.cs. */
    internal static partial InputRow ExtractInputRow(ArrowArray* batch, long row);
    internal static partial void WriteOutputRow(IntPtr emitCtx, OutputRow row);

    /* User code calls BetlScript.Emit(...) which routes here. */
    internal static void EmitRow(OutputRow row)
    {
        if (_emitCtx == IntPtr.Zero)
            throw new InvalidOperationException(
                "Emit called outside OnRow / OnEof");
        WriteOutputRow(_emitCtx, row);
    }

    /* ---- [UnmanagedCallersOnly] entry points -------------------- */

    /* Called once after Init() to register the per-cell output
     * setters. Kept separate from the task-style Init so dotnet.task
     * (which doesn't emit Arrow rows) doesn't have to know about
     * these symbols. */
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_script_register_emit")]
    public static int RegisterEmit(
        delegate* unmanaged<IntPtr, int, long,        void> setInt64,
        delegate* unmanaged<IntPtr, int, double,      void> setFloat64,
        delegate* unmanaged<IntPtr, int, byte,        void> setBool,
        delegate* unmanaged<IntPtr, int, byte*, int,  void> setUtf8,
        delegate* unmanaged<IntPtr, int,              void> setNull,
        delegate* unmanaged<IntPtr,                   void> commitRow)
    {
        SetInt64Fn   = setInt64;
        SetFloat64Fn = setFloat64;
        SetBoolFn    = setBool;
        SetUtf8Fn    = setUtf8;
        SetNullFn    = setNull;
        CommitRowFn  = commitRow;
        return 0;
    }

    /* Construct the singleton user-script instance. Returns 0 on
     * success; a non-zero managed exception path is caught here so
     * unmanaged code never sees managed exceptions cross the ABI. */
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_script_init")]
    public static int ScriptInit()
    {
        try
        {
            Script = new UserScript();
            return 0;
        }
        catch (Exception ex)
        {
            try { Log.Error("UserScript construction failed: " + ex); } catch { }
            return 1;
        }
    }

    /* Process one input batch. `batch` points at the struct ArrowArray
     * the upstream produced (top-level struct with N child columns).
     * `emitCtx` is opaque to us; we pass it back to the per-cell
     * setters. Returns 0 on success. */
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_script_process_batch")]
    public static int ProcessBatch(ArrowArray* batch, IntPtr emitCtx)
    {
        if (Script == null) return 1;
        _emitCtx = emitCtx;
        try
        {
            long len = batch->Length;
            for (long i = 0; i < len; ++i)
            {
                var row = ExtractInputRow(batch, i);
                Script.OnRow(row);
            }
            return 0;
        }
        catch (Exception ex)
        {
            try { Log.Error("OnRow threw: " + ex); } catch { }
            return 1;
        }
        finally { _emitCtx = IntPtr.Zero; }
    }

    /* Called when the upstream signals end-of-stream. The script gets
     * one final chance to flush state. */
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_script_on_eof")]
    public static int OnEof(IntPtr emitCtx)
    {
        if (Script == null) return 1;
        _emitCtx = emitCtx;
        try
        {
            Script.OnEof();
            return 0;
        }
        catch (Exception ex)
        {
            try { Log.Error("OnEof threw: " + ex); } catch { }
            return 1;
        }
        finally { _emitCtx = IntPtr.Zero; }
    }
}
