/* IDTSBufferManager100 wrapper.
 *
 * In real SSIS BufferManager translates design-time LineageIDs to
 * runtime per-buffer column indices. For Phase 1a betl uses a single
 * column space (the declared output_schema), so LineageID == column
 * index. Every output column is assigned LineageID = its position;
 * every input column that ALSO appears in output_schema (by name)
 * inherits that same LineageID. Input columns absent from output
 * aren't reachable in Phase 1a.
 *
 * Code ported from SSIS that does
 *   inputIdIdx = BufferManager.FindColumnByLineageID(input.Buffer, idColLineageID);
 * keeps compiling and gets back the index it expects, on the
 * single-buffer assumption that the source already relied on. */

namespace Microsoft.SqlServer.Dts.Pipeline.Wrapper;

public interface IDTSBufferManager100
{
    int FindColumnByLineageID(int bufferID, int lineageID);
}

internal sealed class BetlBufferManager : IDTSBufferManager100
{
    public int FindColumnByLineageID(int bufferID, int lineageID) => lineageID;
}
