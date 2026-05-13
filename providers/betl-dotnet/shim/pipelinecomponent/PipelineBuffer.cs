/* PipelineBuffer — the row-batch façade SSIS PipelineComponents
 * use to read/write rows during ProcessInput.
 *
 * SSIS model: one buffer per data-flow buffer-id, shared between
 * input and output in synchronous transforms. The component walks
 * rows with NextRow() and uses typed Get*(columnIndex) / Set*
 * (columnIndex, value) accessors per cell.
 *
 * Phase 1a betl model: a single column space defined by the
 * declared output_schema. Initial values for each cell come from
 * the same-named input column if present, else NULL. Get* reads
 * the staged value; Set* writes the staged value. NextRow advances
 * and commits the previously-iterated row.
 *
 * Supported types in Phase 1a: DT_I8 (long), DT_R8 (double),
 * DT_BOOL, DT_WSTR (string). Other DataType values throw. Extra
 * SSIS-named convenience accessors (GetInt32 / GetSingle / etc.)
 * are translated through the wider type.
 *
 * Direction of indexing: indices are POSITIONS in output_schema.
 * BufferManager.FindColumnByLineageID returns the same number. */

using System;
using System.Runtime.CompilerServices;
using Betl;
using Microsoft.SqlServer.Dts.Pipeline.Wrapper;

namespace Microsoft.SqlServer.Dts.Pipeline;

public abstract class PipelineBuffer
{
    public abstract long  RowCount    { get; }
    public abstract bool  EndOfRowset { get; }
    public abstract int   ColumnCount { get; }
    public abstract bool  NextRow();

    public abstract bool  IsNull   (int columnIndex);
    public abstract void  SetNull  (int columnIndex);

    public abstract long      GetInt64 (int columnIndex);
    public abstract void      SetInt64 (int columnIndex, long value);
    public abstract double    GetDouble(int columnIndex);
    public abstract void      SetDouble(int columnIndex, double value);
    public abstract bool      GetBoolean(int columnIndex);
    public abstract void      SetBoolean(int columnIndex, bool value);
    public abstract string    GetString(int columnIndex);
    public abstract void      SetString(int columnIndex, string value);

    /* SSIS API surface — alternative typed accessors that
     * narrow/widen through the supported set. Implemented in terms
     * of the abstract methods so subclasses only fill those in. */
    public int    GetInt32 (int c) => (int)   GetInt64(c);
    public short  GetInt16 (int c) => (short) GetInt64(c);
    public sbyte  GetSByte (int c) => (sbyte) GetInt64(c);
    public uint   GetUInt32(int c) => (uint)  GetInt64(c);
    public ushort GetUInt16(int c) => (ushort)GetInt64(c);
    public byte   GetByte  (int c) => (byte)  GetInt64(c);
    public float  GetSingle(int c) => (float) GetDouble(c);

    public void   SetInt32 (int c, int v)    => SetInt64(c, v);
    public void   SetInt16 (int c, short v)  => SetInt64(c, v);
    public void   SetSByte (int c, sbyte v)  => SetInt64(c, v);
    public void   SetUInt32(int c, uint v)   => SetInt64(c, v);
    public void   SetUInt16(int c, ushort v) => SetInt64(c, v);
    public void   SetByte  (int c, byte v)   => SetInt64(c, v);
    public void   SetSingle(int c, float v)  => SetDouble(c, v);

    /* SetEndOfRowset is meaningful on output buffers in async
     * transforms; in Phase 1a sync model it's a no-op the
     * ProcessInput flush handles automatically. */
    public virtual void SetEndOfRowset() { }
}

/* ---- runtime impl ----------------------------------------------------- */

internal enum CellType : byte
{
    Int64   = 1,
    Float64 = 2,
    Bool    = 3,
    Utf8    = 4,
}

internal sealed class BufferColumnSpec
{
    public string   Name = "";
    public CellType Type;
    public int      InputIndex = -1; /* -1 = no same-named input column */
}

/* Internal staging row representation. Built once per batch from
 * the input ArrowArray; the user's Get / Set methods read and
 * write these fields. Each row is one StagingRow; the buffer
 * holds an array of them sized to the input batch's row count.
 *
 * Why a per-row object rather than per-column SoA: simplicity.
 * The Phase 1a hot-path cost is the user's per-cell access, not
 * the staging layout. SoA can come in Phase 2 if profiling justifies. */
internal sealed class StagingRow
{
    public long[]    I64;
    public double[]  F64;
    public bool[]    Bool;
    public string?[] Utf8;
    public bool[]    IsNull;

    public StagingRow(int n)
    {
        I64    = new long  [n];
        F64    = new double[n];
        Bool   = new bool  [n];
        Utf8   = new string?[n];
        IsNull = new bool  [n];
        for (int i = 0; i < n; ++i) IsNull[i] = true;
    }
}

internal sealed unsafe class BetlPipelineBuffer : PipelineBuffer
{
    private readonly BufferColumnSpec[] _cols;
    private readonly StagingRow[]       _rows;
    private long _currentRow = -1;

    public override long RowCount    => _rows.LongLength;
    public override bool EndOfRowset => _currentRow >= _rows.LongLength;
    public override int  ColumnCount => _cols.Length;

    internal BetlPipelineBuffer(BufferColumnSpec[] cols, ArrowArray* batch)
    {
        _cols = cols;
        long n = batch->Length;
        _rows = new StagingRow[n];
        for (long r = 0; r < n; ++r) _rows[r] = new StagingRow(cols.Length);

        /* Pre-populate each cell from its same-named input column if any. */
        for (int c = 0; c < cols.Length; ++c)
        {
            int ic = cols[c].InputIndex;
            if (ic < 0) continue;
            ArrowArray* child = batch->Children[ic];
            PopulateFromArrowChild(c, cols[c].Type, child);
        }
    }

    private void PopulateFromArrowChild(int colIdx, CellType type, ArrowArray* child)
    {
        long off  = child->Offset;
        long n    = child->Length;
        byte* validity = child->NullCount > 0 && child->Buffers[0] != null
            ? (byte*)child->Buffers[0] : null;

        switch (type)
        {
            case CellType.Int64:
            {
                long* vals = (long*)child->Buffers[1];
                for (long r = 0; r < n; ++r)
                {
                    long ix = off + r;
                    bool isNull = validity != null
                        && ((validity[ix / 8] >> (int)(ix & 7)) & 1u) == 0;
                    if (!isNull)
                    {
                        _rows[r].I64[colIdx] = vals[ix];
                        _rows[r].IsNull[colIdx] = false;
                    }
                }
                break;
            }
            case CellType.Float64:
            {
                double* vals = (double*)child->Buffers[1];
                for (long r = 0; r < n; ++r)
                {
                    long ix = off + r;
                    bool isNull = validity != null
                        && ((validity[ix / 8] >> (int)(ix & 7)) & 1u) == 0;
                    if (!isNull)
                    {
                        _rows[r].F64[colIdx] = vals[ix];
                        _rows[r].IsNull[colIdx] = false;
                    }
                }
                break;
            }
            case CellType.Bool:
            {
                byte* bits = (byte*)child->Buffers[1];
                for (long r = 0; r < n; ++r)
                {
                    long ix = off + r;
                    bool isNull = validity != null
                        && ((validity[ix / 8] >> (int)(ix & 7)) & 1u) == 0;
                    if (!isNull)
                    {
                        byte b = bits[ix / 8];
                        _rows[r].Bool[colIdx] = ((b >> (int)(ix & 7)) & 1) != 0;
                        _rows[r].IsNull[colIdx] = false;
                    }
                }
                break;
            }
            case CellType.Utf8:
            {
                int*  offs = (int*) child->Buffers[1];
                byte* data = (byte*)child->Buffers[2];
                for (long r = 0; r < n; ++r)
                {
                    long ix = off + r;
                    bool isNull = validity != null
                        && ((validity[ix / 8] >> (int)(ix & 7)) & 1u) == 0;
                    if (!isNull)
                    {
                        int s = offs[ix], e = offs[ix + 1];
                        _rows[r].Utf8[colIdx] =
                            System.Text.Encoding.UTF8.GetString(data + s, e - s);
                        _rows[r].IsNull[colIdx] = false;
                    }
                }
                break;
            }
        }
    }

    public override bool NextRow()
    {
        _currentRow++;
        return _currentRow < _rows.LongLength;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private StagingRow Cur()
    {
        if (_currentRow < 0 || _currentRow >= _rows.LongLength)
            throw new BetlPipelineException(
                "PipelineBuffer: row cursor not positioned. Call NextRow() first.");
        return _rows[_currentRow];
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void RequireType(int columnIndex, CellType expected)
    {
        if (columnIndex < 0 || columnIndex >= _cols.Length)
            throw new BetlPipelineException(
                $"PipelineBuffer: column index out of range: {columnIndex}");
        if (_cols[columnIndex].Type != expected)
            throw new BetlPipelineException(
                $"PipelineBuffer: column {columnIndex} ({_cols[columnIndex].Name}) "
                + $"is {_cols[columnIndex].Type}, not {expected}");
    }

    public override bool IsNull(int columnIndex) => Cur().IsNull[columnIndex];

    public override void SetNull(int columnIndex)
    {
        var r = Cur();
        r.IsNull[columnIndex] = true;
    }

    public override long GetInt64(int columnIndex)
    {
        RequireType(columnIndex, CellType.Int64);
        return Cur().I64[columnIndex];
    }
    public override void SetInt64(int columnIndex, long value)
    {
        RequireType(columnIndex, CellType.Int64);
        var r = Cur();
        r.I64[columnIndex] = value;
        r.IsNull[columnIndex] = false;
    }
    public override double GetDouble(int columnIndex)
    {
        RequireType(columnIndex, CellType.Float64);
        return Cur().F64[columnIndex];
    }
    public override void SetDouble(int columnIndex, double value)
    {
        RequireType(columnIndex, CellType.Float64);
        var r = Cur();
        r.F64[columnIndex] = value;
        r.IsNull[columnIndex] = false;
    }
    public override bool GetBoolean(int columnIndex)
    {
        RequireType(columnIndex, CellType.Bool);
        return Cur().Bool[columnIndex];
    }
    public override void SetBoolean(int columnIndex, bool value)
    {
        RequireType(columnIndex, CellType.Bool);
        var r = Cur();
        r.Bool[columnIndex] = value;
        r.IsNull[columnIndex] = false;
    }
    public override string GetString(int columnIndex)
    {
        RequireType(columnIndex, CellType.Utf8);
        return Cur().Utf8[columnIndex] ?? "";
    }
    public override void SetString(int columnIndex, string value)
    {
        RequireType(columnIndex, CellType.Utf8);
        var r = Cur();
        r.Utf8[columnIndex] = value ?? "";
        r.IsNull[columnIndex] = false;
    }

    /* Internal: flush all rows to the host emit context using the
     * dispatch setters. Called once per batch at end of ProcessInput. */
    internal void FlushTo(IntPtr emitCtx)
    {
        for (long r = 0; r < _rows.LongLength; ++r)
        {
            var row = _rows[r];
            for (int c = 0; c < _cols.Length; ++c)
            {
                if (row.IsNull[c])
                {
                    Betl.Pipeline.PcDispatch.SetNullFn(emitCtx, c);
                    continue;
                }
                switch (_cols[c].Type)
                {
                    case CellType.Int64:
                        Betl.Pipeline.PcDispatch.SetInt64Fn(emitCtx, c, row.I64[c]); break;
                    case CellType.Float64:
                        Betl.Pipeline.PcDispatch.SetFloat64Fn(emitCtx, c, row.F64[c]); break;
                    case CellType.Bool:
                        Betl.Pipeline.PcDispatch.SetBoolFn(emitCtx, c, (byte)(row.Bool[c] ? 1 : 0)); break;
                    case CellType.Utf8:
                    {
                        var bytes = System.Text.Encoding.UTF8.GetBytes(row.Utf8[c] ?? "");
                        fixed (byte* p = bytes)
                            Betl.Pipeline.PcDispatch.SetUtf8Fn(emitCtx, c, p, bytes.Length);
                        break;
                    }
                }
            }
            Betl.Pipeline.PcDispatch.CommitRowFn(emitCtx);
        }
    }
}
