/* Internal dispatch hub for dotnet.pipelinecomponent.
 *
 * Parallel to Betl.Dispatch (script). Kept in a separate
 * Betl.Pipeline namespace + PcDispatch class to avoid colliding
 * with the existing script dispatch.
 *
 * The C provider:
 *   1. betl_dotnet_init — register Log/Param/Connection bridges.
 *   2. betl_dotnet_pc_register_emit — host setters.
 *   3. betl_dotnet_pc_register_schema(..., async) — schema + mode.
 *   4. betl_dotnet_pc_init — construct UserComponent.
 *   5. betl_dotnet_pc_pre_execute.
 *   6. betl_dotnet_pc_prime_output — async mode only; passes the
 *      long-lived output buffer to user.PrimeOutput so the user
 *      can stash it for AddRow / Set* calls during ProcessInput.
 *   7. Per batch: betl_dotnet_pc_process_batch(arrowArray*,emit_ctx).
 *   8. betl_dotnet_pc_post_execute + cleanup at end-of-stream.
 *
 * Sync vs async branch:
 *   - Sync: ProcessBatch constructs a BetlSyncPipelineBuffer over
 *     the input batch (output_schema column space, pre-populated
 *     from same-named input cols), passes to ProcessInput, flushes.
 *     PrimeOutput is not driven.
 *   - Async: PrimeOutput delivers a long-lived BetlOutputPipeline-
 *     Buffer. ProcessBatch constructs a fresh BetlInputPipeline-
 *     Buffer over each input batch and passes that to ProcessInput.
 *     After the call, the OutputBuffer's staged rows are flushed.
 *     PostExecute can emit final rows; those are flushed too. */

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Betl;

using Microsoft.SqlServer.Dts.Pipeline;
using Microsoft.SqlServer.Dts.Pipeline.Wrapper;

namespace Betl.Pipeline;

internal static unsafe class PcDispatch
{
    /* Singleton user-component instance. */
    internal static PipelineComponent? Component;

    /* Schema state (filled at register_schema). */
    internal static BufferColumnSpec[] OutputCols = Array.Empty<BufferColumnSpec>();
    internal static char[]             InputFmts  = Array.Empty<char>();
    internal static bool               Async      = false;
    internal static BetlComponentMetaData Metadata = new();
    internal static BetlBufferManager     BufManager = new();

    /* Long-lived output buffer for async mode. Null in sync. */
    internal static BetlOutputPipelineBuffer? OutputBuffer;

    /* Host-supplied output setters (same shape as script's). */
    internal static delegate* unmanaged<IntPtr, int, long,            void> SetInt64Fn;
    internal static delegate* unmanaged<IntPtr, int, double,          void> SetFloat64Fn;
    internal static delegate* unmanaged<IntPtr, int, byte,            void> SetBoolFn;
    internal static delegate* unmanaged<IntPtr, int, byte*, int,      void> SetUtf8Fn;
    internal static delegate* unmanaged<IntPtr, int,                  void> SetNullFn;
    internal static delegate* unmanaged<IntPtr,                       void> CommitRowFn;
    /* Phase 2 — flags the current in-flight row for the error stream. */
    internal static delegate* unmanaged<IntPtr, int, int,             void> SetErrorFn;

    /* ----- entry points ------------------------------------------------ */

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_register_emit")]
    public static int RegisterEmit(
        delegate* unmanaged<IntPtr, int, long,        void> setInt64,
        delegate* unmanaged<IntPtr, int, double,      void> setFloat64,
        delegate* unmanaged<IntPtr, int, byte,        void> setBool,
        delegate* unmanaged<IntPtr, int, byte*, int,  void> setUtf8,
        delegate* unmanaged<IntPtr, int,              void> setNull,
        delegate* unmanaged<IntPtr,                   void> commitRow,
        delegate* unmanaged<IntPtr, int, int,         void> setError)
    {
        SetInt64Fn   = setInt64;
        SetFloat64Fn = setFloat64;
        SetBoolFn    = setBool;
        SetUtf8Fn    = setUtf8;
        SetNullFn    = setNull;
        CommitRowFn  = commitRow;
        SetErrorFn   = setError;
        return 0;
    }

    /* Schema descriptors are passed as parallel arrays of
     * NUL-terminated C strings: names and one-char Arrow format
     * codes. `async` (0/1) controls sync vs async wiring:
     *   - sync (0): input cols pair to outputs by name with matching
     *     lineage IDs; input.Buffer = output.Buffer = 0 so BufferManager
     *     lookups translate identically in either column space.
     *   - async (1): input and output have independent lineage IDs
     *     (0..n_in-1 vs 0..n_out-1) and different buffer IDs (0 vs 1). */
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_register_schema")]
    public static int RegisterSchema(
        byte** inputNames,  byte* inputFmts,  int nInput,
        byte** outputNames, byte* outputFmts, int nOutput,
        int async)
    {
        try
        {
            bool isAsync = async != 0;
            Async = isAsync;

            var inFmts = new char[nInput];
            for (int i = 0; i < nInput; ++i) inFmts[i] = (char)inputFmts[i];
            InputFmts = inFmts;

            var inputs  = new List<BetlInputColumn>(nInput);
            for (int i = 0; i < nInput; ++i)
            {
                var name = CStr(inputNames[i]);
                inputs.Add(new BetlInputColumn {
                    ID = i, Name = name,
                    LineageID = isAsync ? i : -1, /* async: identity; sync: paired below */
                    DataType  = ArrowFmtToDataType(inFmts[i]),
                });
            }
            var outputs = new List<BetlOutputColumn>(nOutput);
            var specs   = new BufferColumnSpec[nOutput];
            for (int i = 0; i < nOutput; ++i)
            {
                var name = CStr(outputNames[i]);
                char fmt = (char)outputFmts[i];
                outputs.Add(new BetlOutputColumn {
                    ID = i, Name = name, LineageID = i,
                    DataType = ArrowFmtToDataType(fmt),
                });
                specs[i] = new BufferColumnSpec {
                    Name = name,
                    Type = ArrowFmtToCellType(fmt),
                    InputIndex = -1,
                    InputFmt   = '?',
                    OutputFmt  = fmt,
                };
            }

            if (!isAsync)
            {
                /* Sync: pair input cols to same-named outputs so
                 * the unified column space (output_schema) gets
                 * pre-populated, and input lineage IDs align with
                 * output positions. */
                for (int i = 0; i < nInput; ++i)
                {
                    for (int j = 0; j < nOutput; ++j)
                    {
                        if (inputs[i].Name == outputs[j].Name)
                        {
                            inputs[i] = new BetlInputColumn {
                                ID = inputs[i].ID, Name = inputs[i].Name,
                                LineageID = j, DataType = inputs[i].DataType,
                                Length = inputs[i].Length, Precision = inputs[i].Precision,
                                Scale = inputs[i].Scale, CodePage = inputs[i].CodePage,
                            };
                            specs[j].InputIndex = i;
                            specs[j].InputFmt   = inFmts[i];
                            break;
                        }
                    }
                }
            }

            int inputBufferID  = 0;
            int outputBufferID = isAsync ? 1 : 0;

            var inputColl  = new BetlInputColumnCollection(inputs);
            var outputColl = new BetlOutputColumnCollection(outputs);
            var inputObj   = new BetlInput  { ID = 0, Buffer = inputBufferID,
                                              InputColumnCollection  = inputColl };
            var outputObj  = new BetlOutput { ID = 0, Buffer = outputBufferID,
                                              OutputColumnCollection = outputColl,
                                              SynchronousInputID = isAsync ? 0 : 1 };
            Metadata = new BetlComponentMetaData {
                InputCollection  = new BetlInputCollection (new List<BetlInput> { inputObj }),
                OutputCollection = new BetlOutputCollection(new List<BetlOutput> { outputObj }),
            };
            OutputCols = specs;
            return 0;
        }
        catch (Exception ex)
        {
            try { Betl.Log.Error("pc_register_schema threw: " + ex); } catch { }
            return 1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_init")]
    public static int Init()
    {
        try
        {
            Component = new UserComponent();
            Component.ComponentMetaData = Metadata;
            Component.BufferManager     = BufManager;
            if (Async)
                OutputBuffer = new BetlOutputPipelineBuffer(OutputCols);
            return 0;
        }
        catch (Exception ex)
        {
            try { Betl.Log.Error("UserComponent construction failed: " + ex); } catch { }
            return 1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_pre_execute")]
    public static int PreExecute()
    {
        if (Component == null) return 1;
        try { Component.PreExecute(); return 0; }
        catch (Exception ex)
        {
            try { Betl.Log.Error("PreExecute threw: " + ex); } catch { }
            return 1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_prime_output")]
    public static int PrimeOutput()
    {
        if (Component == null) return 1;
        if (!Async || OutputBuffer == null) return 0;     /* sync: no-op */
        try
        {
            /* SSIS convention: outputIDs[] is the output ID list,
             * buffers[] is the matching PipelineBuffer list. We
             * pass [outputID=0] and [OutputBuffer]. */
            Component.PrimeOutput(1, new int[] { 0 }, new PipelineBuffer[] { OutputBuffer });
            return 0;
        }
        catch (Exception ex)
        {
            try { Betl.Log.Error("PrimeOutput threw: " + ex); } catch { }
            return 1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_process_batch")]
    public static int ProcessBatch(ArrowArray* batch, IntPtr emitCtx)
    {
        if (Component == null) return 1;
        try
        {
            if (Async)
            {
                var inputBuf = new BetlInputPipelineBuffer(InputFmts, batch);
                Component.ProcessInput(0, inputBuf);
                OutputBuffer?.FlushTo(emitCtx);
            }
            else
            {
                var buffer = new BetlSyncPipelineBuffer(OutputCols, batch);
                Component.ProcessInput(0, buffer);
                buffer.FlushTo(emitCtx);
            }
            return 0;
        }
        catch (Exception ex)
        {
            try { Betl.Log.Error("ProcessInput threw: " + ex); } catch { }
            return 1;
        }
    }

    /* In async mode, PostExecute may add final rows to the output
     * buffer; those need to be flushed via the emit_ctx the host
     * supplies on this call. The C side passes the same DsEmitter
     * pointer it would for process_batch. */
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_post_execute")]
    public static int PostExecute(IntPtr emitCtx)
    {
        if (Component == null) return 1;
        try
        {
            Component.PostExecute();
            if (Async && OutputBuffer != null && emitCtx != IntPtr.Zero)
                OutputBuffer.FlushTo(emitCtx);
            return 0;
        }
        catch (Exception ex)
        {
            try { Betl.Log.Error("PostExecute threw: " + ex); } catch { }
            return 1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_cleanup")]
    public static int Cleanup()
    {
        if (Component == null) return 0;
        try { Component.Cleanup(); return 0; }
        catch (Exception ex)
        {
            try { Betl.Log.Error("Cleanup threw: " + ex); } catch { }
            return 1;
        }
    }

    /* ----- helpers ----------------------------------------------------- */

    private static string CStr(byte* p)
    {
        if (p == null) return "";
        int len = 0;
        while (p[len] != 0) ++len;
        return Encoding.UTF8.GetString(p, len);
    }

    private static DataType ArrowFmtToDataType(char fmt) => fmt switch
    {
        'l' => DataType.DT_I8,
        'g' => DataType.DT_R8,
        'b' => DataType.DT_BOOL,
        'u' => DataType.DT_WSTR,
        'c' => DataType.DT_I1,
        's' => DataType.DT_I2,
        'i' => DataType.DT_I4,
        'C' => DataType.DT_UI1,
        'S' => DataType.DT_UI2,
        'I' => DataType.DT_UI4,
        'L' => DataType.DT_UI8,
        'f' => DataType.DT_R4,
        'D' => DataType.DT_DBDATE,
        'T' => DataType.DT_DBTIMESTAMP2,
        'M' => DataType.DT_DBTIME2,
        'z' => DataType.DT_BYTES,
        _   => throw new BetlPipelineException(
                   $"dotnet.pipelinecomponent: unsupported format '{fmt}'"),
    };

    /* Format chars fold to a small storage tag. Date/timestamp/time
     * all share Int64 storage; the OutputFmt char on the column spec
     * preserves the semantic type for GetDate/SetDate dispatching. */
    private static CellType ArrowFmtToCellType(char fmt) => fmt switch
    {
        'l' or 'L' or 'i' or 'I' or 's' or 'S' or 'c' or 'C'
        or 'D' or 'T' or 'M'                                 => CellType.Int64,
        'g' or 'f'                                           => CellType.Float64,
        'b'                                                  => CellType.Bool,
        'u'                                                  => CellType.Utf8,
        'z'                                                  => CellType.Binary,
        _ => throw new BetlPipelineException(
                $"dotnet.pipelinecomponent: unsupported format '{fmt}'"),
    };
}
