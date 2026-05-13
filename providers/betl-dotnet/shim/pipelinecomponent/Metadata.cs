/* SSIS metadata interfaces (the "100"-suffixed ones from
 * Microsoft.SqlServer.Dts.Pipeline.Wrapper) plus the read-only
 * implementations betl populates at startup.
 *
 * Phase 1a coverage: enough of the surface for PreExecute-style
 * code to walk InputCollection / OutputCollection, read column
 * names + types + lineage IDs, and translate via BufferManager.
 * Mutating any of these collections at runtime throws — design-
 * time mutation isn't a thing we drive. */

using System;
using System.Collections;
using System.Collections.Generic;

namespace Microsoft.SqlServer.Dts.Pipeline.Wrapper;

public interface IDTSObject100
{
    int  ID          { get; }
    string  Name     { get; set; }
    string  Description { get; set; }
}

public interface IDTSInputColumn100 : IDTSObject100
{
    int       LineageID    { get; }
    DataType  DataType     { get; set; }
    int       Length       { get; set; }
    int       Precision    { get; set; }
    int       Scale        { get; set; }
    int       CodePage     { get; set; }
}

public interface IDTSOutputColumn100 : IDTSObject100
{
    int       LineageID    { get; }
    DataType  DataType     { get; set; }
    int       Length       { get; set; }
    int       Precision    { get; set; }
    int       Scale        { get; set; }
    int       CodePage     { get; set; }
}

public interface IDTSInputColumnCollection100 : IEnumerable
{
    int                  Count { get; }
    IDTSInputColumn100   this[object index] { get; }
}

public interface IDTSOutputColumnCollection100 : IEnumerable
{
    int                  Count { get; }
    IDTSOutputColumn100  this[object index] { get; }
}

public interface IDTSInput100 : IDTSObject100
{
    int                            Buffer { get; }
    IDTSInputColumnCollection100   InputColumnCollection { get; }
}

public interface IDTSOutput100 : IDTSObject100
{
    int                             Buffer { get; }
    IDTSOutputColumnCollection100   OutputColumnCollection { get; }
    bool                            IsErrorOut { get; }
    int                             SynchronousInputID { get; set; }
}

public interface IDTSInputCollection100 : IEnumerable
{
    int             Count { get; }
    IDTSInput100    this[object index] { get; }
}

public interface IDTSOutputCollection100 : IEnumerable
{
    int             Count { get; }
    IDTSOutput100   this[object index] { get; }
}

public interface IDTSCustomProperty100 : IDTSObject100
{
    object  Value { get; set; }
}

public interface IDTSCustomPropertyCollection100 : IEnumerable
{
    int                      Count { get; }
    IDTSCustomProperty100    this[object index] { get; }
}

public interface IDTSRuntimeConnection100 : IDTSObject100
{
    string  ConnectionManagerID { get; set; }
    /* Returns whatever the host hands back for the connection — for
     * betl-dotnet that's the connection's JSON blob as a string.
     * SSIS-original code casts to OleDbConnection / SqlConnection /
     * etc.; ported code casts to string and constructs its own
     * .NET connection object from the parsed JSON. */
    object  AcquireConnection(object transaction);
    void    ReleaseConnection(object connection);
}

public interface IDTSRuntimeConnectionCollection100 : IEnumerable
{
    int                          Count { get; }
    IDTSRuntimeConnection100     this[object index] { get; }
}

public interface IDTSComponentMetaData100
{
    int                                Name_ID  { get; }
    int                                ID       { get; }
    string                             Name        { get; set; }
    string                             Description { get; set; }
    IDTSInputCollection100             InputCollection { get; }
    IDTSOutputCollection100            OutputCollection { get; }
    IDTSCustomPropertyCollection100    CustomPropertyCollection { get; }
    IDTSRuntimeConnectionCollection100 RuntimeConnectionCollection { get; }

    void FireError(int errorCode, string subComponent, string description,
                   string helpFile, int helpContext, out bool cancel);
    void FireWarning(int warningCode, string subComponent, string description,
                     string helpFile, int helpContext);
    void FireInformation(int informationCode, string subComponent, string description,
                         string helpFile, int helpContext, ref bool fireAgain);
}

/* ---- concrete impls (read-only, populated at startup) ---------------- */

internal sealed class BetlInputColumn : IDTSInputColumn100
{
    public int       ID          { get; init; }
    public string    Name        { get; set; } = "";
    public string    Description { get; set; } = "";
    public int       LineageID   { get; init; }
    public DataType  DataType    { get; set; }
    public int       Length      { get; set; }
    public int       Precision   { get; set; }
    public int       Scale       { get; set; }
    public int       CodePage    { get; set; }
}

internal sealed class BetlOutputColumn : IDTSOutputColumn100
{
    public int       ID          { get; init; }
    public string    Name        { get; set; } = "";
    public string    Description { get; set; } = "";
    public int       LineageID   { get; init; }
    public DataType  DataType    { get; set; }
    public int       Length      { get; set; }
    public int       Precision   { get; set; }
    public int       Scale       { get; set; }
    public int       CodePage    { get; set; }
}

internal sealed class BetlInputColumnCollection : IDTSInputColumnCollection100
{
    private readonly List<BetlInputColumn> _cols;
    public BetlInputColumnCollection(List<BetlInputColumn> cols) { _cols = cols; }
    public int Count => _cols.Count;
    public IDTSInputColumn100 this[object index] => Resolve(index);
    public IEnumerator GetEnumerator() => _cols.GetEnumerator();
    private IDTSInputColumn100 Resolve(object index)
    {
        if (index is int i)
        {
            if (i < 0 || i >= _cols.Count) throw new BetlPipelineException(
                $"InputColumnCollection index out of range: {i}");
            return _cols[i];
        }
        if (index is string s)
        {
            foreach (var c in _cols) if (c.Name == s) return c;
            throw new BetlPipelineException($"InputColumnCollection: no column named '{s}'");
        }
        throw new BetlPipelineException($"InputColumnCollection: bad index type {index?.GetType()}");
    }
}

internal sealed class BetlOutputColumnCollection : IDTSOutputColumnCollection100
{
    private readonly List<BetlOutputColumn> _cols;
    public BetlOutputColumnCollection(List<BetlOutputColumn> cols) { _cols = cols; }
    public int Count => _cols.Count;
    public IDTSOutputColumn100 this[object index] => Resolve(index);
    public IEnumerator GetEnumerator() => _cols.GetEnumerator();
    private IDTSOutputColumn100 Resolve(object index)
    {
        if (index is int i)
        {
            if (i < 0 || i >= _cols.Count) throw new BetlPipelineException(
                $"OutputColumnCollection index out of range: {i}");
            return _cols[i];
        }
        if (index is string s)
        {
            foreach (var c in _cols) if (c.Name == s) return c;
            throw new BetlPipelineException($"OutputColumnCollection: no column named '{s}'");
        }
        throw new BetlPipelineException($"OutputColumnCollection: bad index type {index?.GetType()}");
    }
}

internal sealed class BetlInput : IDTSInput100
{
    public int     ID          { get; init; }
    public string  Name        { get; set; } = "Input 0";
    public string  Description { get; set; } = "";
    public int     Buffer      { get; init; }
    public IDTSInputColumnCollection100 InputColumnCollection { get; init; } = null!;
}

internal sealed class BetlOutput : IDTSOutput100
{
    public int     ID          { get; init; }
    public string  Name        { get; set; } = "Output 0";
    public string  Description { get; set; } = "";
    public int     Buffer      { get; init; }
    public IDTSOutputColumnCollection100 OutputColumnCollection { get; init; } = null!;
    public bool    IsErrorOut          { get; init; }
    public int     SynchronousInputID  { get; set; }
}

internal sealed class BetlInputCollection : IDTSInputCollection100
{
    private readonly List<BetlInput> _inputs;
    public BetlInputCollection(List<BetlInput> inputs) { _inputs = inputs; }
    public int Count => _inputs.Count;
    public IDTSInput100 this[object index] => Resolve(index);
    public IEnumerator GetEnumerator() => _inputs.GetEnumerator();
    private IDTSInput100 Resolve(object index)
    {
        if (index is int i)
        {
            if (i < 0 || i >= _inputs.Count) throw new BetlPipelineException(
                $"InputCollection index out of range: {i}");
            return _inputs[i];
        }
        if (index is string s)
        {
            foreach (var x in _inputs) if (x.Name == s) return x;
            throw new BetlPipelineException($"InputCollection: no input named '{s}'");
        }
        throw new BetlPipelineException($"InputCollection: bad index type {index?.GetType()}");
    }
}

internal sealed class BetlOutputCollection : IDTSOutputCollection100
{
    private readonly List<BetlOutput> _outputs;
    public BetlOutputCollection(List<BetlOutput> outputs) { _outputs = outputs; }
    public int Count => _outputs.Count;
    public IDTSOutput100 this[object index] => Resolve(index);
    public IEnumerator GetEnumerator() => _outputs.GetEnumerator();
    private IDTSOutput100 Resolve(object index)
    {
        if (index is int i)
        {
            if (i < 0 || i >= _outputs.Count) throw new BetlPipelineException(
                $"OutputCollection index out of range: {i}");
            return _outputs[i];
        }
        if (index is string s)
        {
            foreach (var x in _outputs) if (x.Name == s) return x;
            throw new BetlPipelineException($"OutputCollection: no output named '{s}'");
        }
        throw new BetlPipelineException($"OutputCollection: bad index type {index?.GetType()}");
    }
}

internal sealed class EmptyCustomPropertyCollection : IDTSCustomPropertyCollection100
{
    public int Count => 0;
    public IDTSCustomProperty100 this[object index] =>
        throw new BetlPipelineException("CustomPropertyCollection is empty in Phase 1a");
    public IEnumerator GetEnumerator() => System.Array.Empty<IDTSCustomProperty100>().GetEnumerator();
}

/* Connection Manager wrapper. Holds a name; AcquireConnection
 * delegates to Betl.Connection.Get which talks to the host's
 * `connections:` block via the bridge function pointer. */
internal sealed class BetlRuntimeConnection : IDTSRuntimeConnection100
{
    public int     ID                  { get; init; }
    public string  Name                { get; set; } = "";
    public string  Description         { get; set; } = "";
    public string  ConnectionManagerID { get; set; } = "";

    public object AcquireConnection(object transaction)
    {
        var json = Betl.Connection.Get(Name);
        if (json == null)
            throw new BetlPipelineException(
                $"RuntimeConnection '{Name}': no connection of that name defined in "
                + "the pipeline's `connections:` block");
        return json;
    }

    public void ReleaseConnection(object connection) { /* nothing to release */ }
}

/* Lazy lookup: indexer constructs a BetlRuntimeConnection on demand
 * for any name. Iteration / Count are zero unless explicit names were
 * registered (see RegisterNames). This matches how most ported SSIS
 * components use the collection — lookup by known name from
 * PreExecute, never iterate. */
internal sealed class BetlRuntimeConnectionCollection : IDTSRuntimeConnectionCollection100
{
    private readonly List<BetlRuntimeConnection> _known = new();

    internal void RegisterNames(IEnumerable<string> names)
    {
        int id = 0;
        foreach (var n in names)
            _known.Add(new BetlRuntimeConnection { ID = id++, Name = n });
    }

    public int Count => _known.Count;
    public IDTSRuntimeConnection100 this[object index]
    {
        get
        {
            if (index is int i)
            {
                if (i < 0 || i >= _known.Count) throw new BetlPipelineException(
                    $"RuntimeConnectionCollection index out of range: {i}");
                return _known[i];
            }
            if (index is string s)
            {
                /* Lookup-by-name always succeeds: if not pre-registered,
                 * we synthesize on the fly. AcquireConnection later will
                 * fail with a clear error if the name doesn't exist in
                 * the pipeline's connections block. */
                foreach (var c in _known) if (c.Name == s) return c;
                var fresh = new BetlRuntimeConnection { ID = _known.Count, Name = s };
                _known.Add(fresh);
                return fresh;
            }
            throw new BetlPipelineException(
                $"RuntimeConnectionCollection: bad index type {index?.GetType()}");
        }
    }
    public IEnumerator GetEnumerator() => _known.GetEnumerator();
}

internal sealed class BetlComponentMetaData : IDTSComponentMetaData100
{
    public int                                Name_ID  => 0;
    public int                                ID       => 0;
    public string                             Name        { get; set; } = "UserComponent";
    public string                             Description { get; set; } = "";
    public IDTSInputCollection100             InputCollection { get; init; } = null!;
    public IDTSOutputCollection100            OutputCollection { get; init; } = null!;
    public IDTSCustomPropertyCollection100    CustomPropertyCollection { get; init; } =
        new EmptyCustomPropertyCollection();
    public IDTSRuntimeConnectionCollection100 RuntimeConnectionCollection { get; init; } =
        new BetlRuntimeConnectionCollection();

    public void FireError(int errorCode, string subComponent, string description,
                          string helpFile, int helpContext, out bool cancel)
    {
        Betl.Log.Error($"[{subComponent}] (err {errorCode}) {description}");
        cancel = true;
        throw new BetlPipelineException(description);
    }
    public void FireWarning(int warningCode, string subComponent, string description,
                            string helpFile, int helpContext)
    {
        Betl.Log.Warn($"[{subComponent}] (warn {warningCode}) {description}");
    }
    public void FireInformation(int informationCode, string subComponent, string description,
                                string helpFile, int helpContext, ref bool fireAgain)
    {
        Betl.Log.Info($"[{subComponent}] (info {informationCode}) {description}");
        fireAgain = true;
    }
}
