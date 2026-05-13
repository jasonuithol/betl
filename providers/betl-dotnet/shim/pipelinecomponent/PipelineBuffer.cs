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

/* The base type has every accessor declared so SSIS-derived source
 * compiles against it. Sync transforms use one concrete buffer with
 * all of them; async transforms get two — an input view that only
 * implements Get* / NextRow and an output view that only implements
 * Set* / AddRow. Unsupported calls throw with a clear message. */
public abstract class PipelineBuffer
{
    public virtual long  RowCount    => throw NotSupported("RowCount");
    public virtual bool  EndOfRowset => throw NotSupported("EndOfRowset");
    public virtual int   ColumnCount => throw NotSupported("ColumnCount");
    public virtual bool  NextRow()                  => throw NotSupported("NextRow");
    public virtual void  AddRow()                   => throw NotSupported("AddRow");
    public virtual void  SetEndOfRowset()           { }    /* tolerated as a hint */

    public virtual bool  IsNull   (int c)           => throw NotSupported("IsNull");
    public virtual void  SetNull  (int c)           => throw NotSupported("SetNull");

    public virtual long      GetInt64 (int c)               => throw NotSupported("GetInt64");
    public virtual void      SetInt64 (int c, long v)       => throw NotSupported("SetInt64");
    public virtual double    GetDouble(int c)               => throw NotSupported("GetDouble");
    public virtual void      SetDouble(int c, double v)     => throw NotSupported("SetDouble");
    public virtual bool      GetBoolean(int c)              => throw NotSupported("GetBoolean");
    public virtual void      SetBoolean(int c, bool v)      => throw NotSupported("SetBoolean");
    public virtual string    GetString(int c)               => throw NotSupported("GetString");
    public virtual void      SetString(int c, string v)     => throw NotSupported("SetString");

    /* Narrow accessors route through the widened ones. */
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

    private static System.Exception NotSupported(string op) =>
        new BetlPipelineException(
            "PipelineBuffer: " + op + " is not supported in this buffer's mode "
            + "(input vs output vs sync).");
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
    public char     InputFmt   = '?'; /* Arrow format of the paired input column */
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

/* Sync transform: a single buffer used for both reads and writes,
 * indexed by output_schema. Same-named input columns pre-populate
 * cells; user reads/writes via Get / Set; NextRow advances. */
internal sealed unsafe class BetlSyncPipelineBuffer : PipelineBuffer
{
    private readonly BufferColumnSpec[] _cols;
    private readonly StagingRow[]       _rows;
    private long _currentRow = -1;

    public override long RowCount    => _rows.LongLength;
    public override bool EndOfRowset => _currentRow >= _rows.LongLength;
    public override int  ColumnCount => _cols.Length;

    internal BetlSyncPipelineBuffer(BufferColumnSpec[] cols, ArrowArray* batch)
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
            PopulateFromArrowChild(c, cols[c].InputFmt, child);
        }
    }

    private void PopulateFromArrowChild(int colIdx, char inputFmt, ArrowArray* child)
    {
        long off  = child->Offset;
        long n    = child->Length;
        byte* validity = child->NullCount > 0 && child->Buffers[0] != null
            ? (byte*)child->Buffers[0] : null;

        switch (inputFmt)
        {
            case 'l':
                ReadWideInt(colIdx, (long*)child->Buffers[1], off, n, validity);
                break;
            case 'L':
                /* uint64 storage is bit-identical to int64 here */
                ReadWideInt(colIdx, (long*)child->Buffers[1], off, n, validity);
                break;
            case 'i':
                ReadNarrowInt32(colIdx, (int*)child->Buffers[1], off, n, validity);
                break;
            case 'I':
                ReadNarrowUInt32(colIdx, (uint*)child->Buffers[1], off, n, validity);
                break;
            case 's':
                ReadNarrowInt16(colIdx, (short*)child->Buffers[1], off, n, validity);
                break;
            case 'S':
                ReadNarrowUInt16(colIdx, (ushort*)child->Buffers[1], off, n, validity);
                break;
            case 'c':
                ReadNarrowInt8(colIdx, (sbyte*)child->Buffers[1], off, n, validity);
                break;
            case 'C':
                ReadNarrowUInt8(colIdx, (byte*)child->Buffers[1], off, n, validity);
                break;
            case 'g':
                ReadWideFloat(colIdx, (double*)child->Buffers[1], off, n, validity);
                break;
            case 'f':
                ReadNarrowFloat32(colIdx, (float*)child->Buffers[1], off, n, validity);
                break;
            case 'b':
                ReadBoolBits(colIdx, (byte*)child->Buffers[1], off, n, validity);
                break;
            case 'u':
                ReadUtf8(colIdx, (int*)child->Buffers[1], (byte*)child->Buffers[2],
                         off, n, validity);
                break;
            default:
                throw new BetlPipelineException(
                    $"PipelineBuffer: unsupported input Arrow format '{inputFmt}' "
                    + $"at column index {colIdx}");
        }
    }

    private static bool IsNull(byte* validity, long ix) =>
        validity != null && ((validity[ix / 8] >> (int)(ix & 7)) & 1u) == 0;

    private void ReadWideInt(int colIdx, long* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].I64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadNarrowInt32(int colIdx, int* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].I64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadNarrowUInt32(int colIdx, uint* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].I64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadNarrowInt16(int colIdx, short* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].I64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadNarrowUInt16(int colIdx, ushort* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].I64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadNarrowInt8(int colIdx, sbyte* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].I64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadNarrowUInt8(int colIdx, byte* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].I64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadWideFloat(int colIdx, double* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].F64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadNarrowFloat32(int colIdx, float* vals, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                _rows[r].F64[colIdx] = vals[ix];
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadBoolBits(int colIdx, byte* bits, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                byte b = bits[ix / 8];
                _rows[r].Bool[colIdx] = ((b >> (int)(ix & 7)) & 1) != 0;
                _rows[r].IsNull[colIdx] = false;
            }
        }
    }
    private void ReadUtf8(int colIdx, int* offs, byte* data, long off, long n, byte* validity)
    {
        for (long r = 0; r < n; ++r) {
            long ix = off + r;
            if (!IsNull(validity, ix)) {
                int s = offs[ix], e = offs[ix + 1];
                _rows[r].Utf8[colIdx] = System.Text.Encoding.UTF8.GetString(data + s, e - s);
                _rows[r].IsNull[colIdx] = false;
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

/* Async input view: read-only over an upstream Arrow batch.
 * NextRow advances; Get / IsNull work; Set / AddRow throw. Indexing
 * is by INPUT column position (the upstream Arrow batch's children),
 * NOT by output_schema. */
internal sealed unsafe class BetlInputPipelineBuffer : PipelineBuffer
{
    private readonly char[]   _fmts;
    private readonly ArrowArray* _batch;
    private long _currentRow = -1;
    private readonly long _rowCount;

    public override long RowCount    => _rowCount;
    public override bool EndOfRowset => _currentRow >= _rowCount;
    public override int  ColumnCount => _fmts.Length;

    internal BetlInputPipelineBuffer(char[] fmts, ArrowArray* batch)
    {
        _fmts = fmts;
        _batch = batch;
        _rowCount = batch->Length;
    }

    public override bool NextRow()
    {
        _currentRow++;
        return _currentRow < _rowCount;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private long CurIx(int colIdx)
    {
        if (_currentRow < 0 || _currentRow >= _rowCount)
            throw new BetlPipelineException(
                "PipelineBuffer: row cursor not positioned. Call NextRow() first.");
        if (colIdx < 0 || colIdx >= _fmts.Length)
            throw new BetlPipelineException(
                $"PipelineBuffer: input column index out of range: {colIdx}");
        ArrowArray* child = _batch->Children[colIdx];
        return child->Offset + _currentRow;
    }

    private static bool IsBitZero(byte* validity, long ix) =>
        validity != null && ((validity[ix / 8] >> (int)(ix & 7)) & 1u) == 0;

    public override bool IsNull(int colIdx)
    {
        long ix = CurIx(colIdx);
        ArrowArray* child = _batch->Children[colIdx];
        byte* validity = child->NullCount > 0 && child->Buffers[0] != null
            ? (byte*)child->Buffers[0] : null;
        return IsBitZero(validity, ix);
    }

    private void RequireFmt(int colIdx, params char[] accepted)
    {
        char f = _fmts[colIdx];
        foreach (char a in accepted) if (f == a) return;
        throw new BetlPipelineException(
            $"PipelineBuffer: input column {colIdx} is '{f}', "
            + $"not one of [{string.Join(",", accepted)}]");
    }

    public override long GetInt64(int colIdx)
    {
        RequireFmt(colIdx, 'l', 'L', 'i', 'I', 's', 'S', 'c', 'C');
        long ix = CurIx(colIdx);
        ArrowArray* child = _batch->Children[colIdx];
        void* buf = child->Buffers[1];
        return _fmts[colIdx] switch
        {
            'l' => ((long*)buf)[ix],
            'L' => ((long*)buf)[ix],
            'i' => ((int*)buf)[ix],
            'I' => ((uint*)buf)[ix],
            's' => ((short*)buf)[ix],
            'S' => ((ushort*)buf)[ix],
            'c' => ((sbyte*)buf)[ix],
            'C' => ((byte*)buf)[ix],
            _   => throw new BetlPipelineException("unreachable"),
        };
    }

    public override double GetDouble(int colIdx)
    {
        RequireFmt(colIdx, 'g', 'f');
        long ix = CurIx(colIdx);
        ArrowArray* child = _batch->Children[colIdx];
        void* buf = child->Buffers[1];
        return _fmts[colIdx] == 'g' ? ((double*)buf)[ix] : ((float*)buf)[ix];
    }

    public override bool GetBoolean(int colIdx)
    {
        RequireFmt(colIdx, 'b');
        long ix = CurIx(colIdx);
        ArrowArray* child = _batch->Children[colIdx];
        byte* bits = (byte*)child->Buffers[1];
        return ((bits[ix / 8] >> (int)(ix & 7)) & 1) != 0;
    }

    public override string GetString(int colIdx)
    {
        RequireFmt(colIdx, 'u');
        long ix = CurIx(colIdx);
        ArrowArray* child = _batch->Children[colIdx];
        int*  offs = (int*) child->Buffers[1];
        byte* data = (byte*)child->Buffers[2];
        int s = offs[ix], e = offs[ix + 1];
        return System.Text.Encoding.UTF8.GetString(data + s, e - s);
    }
}

/* Async output view: write-only sink. AddRow appends a new staging
 * row (initially all-null); Set* writes to that row; FlushTo emits
 * all staged rows through the host setters and clears for the next
 * batch. Indexing is by OUTPUT column position. */
internal sealed unsafe class BetlOutputPipelineBuffer : PipelineBuffer
{
    private readonly BufferColumnSpec[] _cols;
    private readonly System.Collections.Generic.List<StagingRow> _rows = new();

    public override int  ColumnCount => _cols.Length;
    public override long RowCount    => _rows.Count;
    public override bool EndOfRowset => false; /* output never EOFs from the user side */

    internal BetlOutputPipelineBuffer(BufferColumnSpec[] cols) { _cols = cols; }

    public override void AddRow() { _rows.Add(new StagingRow(_cols.Length)); }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private StagingRow Cur()
    {
        if (_rows.Count == 0)
            throw new BetlPipelineException(
                "PipelineBuffer: AddRow() must be called before Set*().");
        return _rows[_rows.Count - 1];
    }

    private void RequireType(int colIdx, CellType expected)
    {
        if (colIdx < 0 || colIdx >= _cols.Length)
            throw new BetlPipelineException(
                $"PipelineBuffer: output column index out of range: {colIdx}");
        if (_cols[colIdx].Type != expected)
            throw new BetlPipelineException(
                $"PipelineBuffer: output column {colIdx} ({_cols[colIdx].Name}) "
                + $"is {_cols[colIdx].Type}, not {expected}");
    }

    public override void SetNull(int colIdx)
    {
        if (colIdx < 0 || colIdx >= _cols.Length)
            throw new BetlPipelineException(
                $"PipelineBuffer: output column index out of range: {colIdx}");
        var r = Cur();
        r.IsNull[colIdx] = true;
    }
    public override void SetInt64(int colIdx, long value)
    {
        RequireType(colIdx, CellType.Int64);
        var r = Cur();
        r.I64[colIdx] = value;
        r.IsNull[colIdx] = false;
    }
    public override void SetDouble(int colIdx, double value)
    {
        RequireType(colIdx, CellType.Float64);
        var r = Cur();
        r.F64[colIdx] = value;
        r.IsNull[colIdx] = false;
    }
    public override void SetBoolean(int colIdx, bool value)
    {
        RequireType(colIdx, CellType.Bool);
        var r = Cur();
        r.Bool[colIdx] = value;
        r.IsNull[colIdx] = false;
    }
    public override void SetString(int colIdx, string value)
    {
        RequireType(colIdx, CellType.Utf8);
        var r = Cur();
        r.Utf8[colIdx] = value ?? "";
        r.IsNull[colIdx] = false;
    }

    /* Flush staged rows to the host via the same setter ABI sync
     * mode uses; clear the staging for the next batch. */
    internal void FlushTo(IntPtr emitCtx)
    {
        for (int r = 0; r < _rows.Count; ++r)
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
        _rows.Clear();
    }
}
