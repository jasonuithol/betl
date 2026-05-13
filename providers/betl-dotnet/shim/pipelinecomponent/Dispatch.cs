/* Internal dispatch hub for dotnet.pipelinecomponent.
 *
 * Parallel to Betl.Dispatch (script). Kept in a separate
 * Betl.Pipeline namespace + PcDispatch class to avoid colliding
 * with the existing script dispatch.
 *
 * The C provider:
 *   1. Calls betl_dotnet_init (registers Log/Param/Connection
 *      bridges into Betl.Bridges) — same as for dotnet.script.
 *   2. Calls betl_dotnet_pc_register_emit to install the per-cell
 *      output setters + commit_row callback.
 *   3. Calls betl_dotnet_pc_register_schema once with the input
 *      and output schema descriptors. Builds metadata + column
 *      specs.
 *   4. Calls betl_dotnet_pc_init to construct the UserComponent.
 *   5. Calls betl_dotnet_pc_pre_execute.
 *   6. For each batch: betl_dotnet_pc_process_batch(arrowArray*,
 *      emit_ctx).
 *   7. betl_dotnet_pc_post_execute + betl_dotnet_pc_cleanup at
 *      end-of-stream. */

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

    /* Schema state (filled at register_schema, used to construct
     * the PipelineBuffer for each batch). */
    internal static BufferColumnSpec[] OutputCols = Array.Empty<BufferColumnSpec>();
    internal static BetlComponentMetaData Metadata = new();
    internal static BetlBufferManager     BufManager = new();

    /* Host-supplied output setters (same shape as script's). */
    internal static delegate* unmanaged<IntPtr, int, long,            void> SetInt64Fn;
    internal static delegate* unmanaged<IntPtr, int, double,          void> SetFloat64Fn;
    internal static delegate* unmanaged<IntPtr, int, byte,            void> SetBoolFn;
    internal static delegate* unmanaged<IntPtr, int, byte*, int,      void> SetUtf8Fn;
    internal static delegate* unmanaged<IntPtr, int,                  void> SetNullFn;
    internal static delegate* unmanaged<IntPtr,                       void> CommitRowFn;

    /* ----- entry points ------------------------------------------------ */

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_register_emit")]
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

    /* Schema descriptors are passed as parallel arrays of
     * NUL-terminated C strings: names and one-char Arrow format
     * codes ('l', 'g', 'b', 'u'). The host owns the buffers; we
     * copy into managed strings immediately. */
    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_register_schema")]
    public static int RegisterSchema(
        byte** inputNames,  byte* inputFmts,  int nInput,
        byte** outputNames, byte* outputFmts, int nOutput)
    {
        try
        {
            var inputs  = new List<BetlInputColumn>(nInput);
            for (int i = 0; i < nInput; ++i)
            {
                var name = CStr(inputNames[i]);
                inputs.Add(new BetlInputColumn {
                    ID = i, Name = name, LineageID = -1,    /* unset until paired below */
                    DataType = ArrowFmtToDataType((char)inputFmts[i]),
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
                };
            }
            /* Pair input columns to outputs by name; assign matching
             * lineage IDs so user lookups by input lineageID hit
             * the same buffer index as output lookups. */
            for (int i = 0; i < nInput; ++i)
            {
                for (int j = 0; j < nOutput; ++j)
                {
                    if (inputs[i].Name == outputs[j].Name)
                    {
                        /* Mutate via init-set proxy: re-create with paired LineageID. */
                        inputs[i] = new BetlInputColumn {
                            ID = inputs[i].ID, Name = inputs[i].Name,
                            LineageID = j, DataType = inputs[i].DataType,
                            Length = inputs[i].Length, Precision = inputs[i].Precision,
                            Scale = inputs[i].Scale, CodePage = inputs[i].CodePage,
                        };
                        specs[j].InputIndex = i;
                        break;
                    }
                }
            }

            var inputColl  = new BetlInputColumnCollection(inputs);
            var outputColl = new BetlOutputColumnCollection(outputs);
            var inputObj   = new BetlInput  { ID = 0, Buffer = 0,
                                              InputColumnCollection  = inputColl };
            var outputObj  = new BetlOutput { ID = 0, Buffer = 0,
                                              OutputColumnCollection = outputColl,
                                              SynchronousInputID = 0 };
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

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_process_batch")]
    public static int ProcessBatch(ArrowArray* batch, IntPtr emitCtx)
    {
        if (Component == null) return 1;
        try
        {
            var buffer = new BetlPipelineBuffer(OutputCols, batch);
            Component.ProcessInput(0, buffer);
            buffer.FlushTo(emitCtx);
            return 0;
        }
        catch (Exception ex)
        {
            try { Betl.Log.Error("ProcessInput threw: " + ex); } catch { }
            return 1;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "betl_dotnet_pc_post_execute")]
    public static int PostExecute()
    {
        if (Component == null) return 1;
        try { Component.PostExecute(); return 0; }
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
        _   => throw new BetlPipelineException(
                   $"dotnet.pipelinecomponent: unsupported Arrow format '{fmt}' "
                   + "(Phase 1a: l, g, b, u)"),
    };

    private static CellType ArrowFmtToCellType(char fmt) => fmt switch
    {
        'l' => CellType.Int64,
        'g' => CellType.Float64,
        'b' => CellType.Bool,
        'u' => CellType.Utf8,
        _   => throw new BetlPipelineException(
                   $"dotnet.pipelinecomponent: unsupported Arrow format '{fmt}'"),
    };
}
