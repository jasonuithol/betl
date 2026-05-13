/* Abstract base class for user-defined PipelineComponents.
 *
 * The user-facing convention: declare a public class named
 * `UserComponent` extending PipelineComponent and override
 * `ProcessInput`. Other lifecycle hooks (PreExecute, PostExecute,
 * Cleanup) are optional virtuals. ProvideComponentProperties /
 * Validate / ReinitializeMetaData are design-time methods betl
 * does not drive — overrides compile and are reachable from
 * other user code but won't be called by the runtime.
 *
 *   public class UserComponent
 *       : Microsoft.SqlServer.Dts.Pipeline.PipelineComponent
 *   {
 *       int idIdx = -1, doubledIdx = -1;
 *
 *       public override void PreExecute() {
 *           base.PreExecute();
 *           var input = ComponentMetaData.InputCollection[0];
 *           foreach (Microsoft.SqlServer.Dts.Pipeline.Wrapper.IDTSInputColumn100 c
 *                    in input.InputColumnCollection) {
 *               if (c.Name == "id")
 *                   idIdx = BufferManager.FindColumnByLineageID(input.Buffer, c.LineageID);
 *           }
 *           var output = ComponentMetaData.OutputCollection[0];
 *           foreach (Microsoft.SqlServer.Dts.Pipeline.Wrapper.IDTSOutputColumn100 c
 *                    in output.OutputColumnCollection) {
 *               if (c.Name == "doubled")
 *                   doubledIdx = BufferManager.FindColumnByLineageID(input.Buffer, c.LineageID);
 *           }
 *       }
 *
 *       public override void ProcessInput(int inputID, PipelineBuffer buffer) {
 *           while (buffer.NextRow()) {
 *               buffer.SetInt64(doubledIdx, buffer.GetInt64(idIdx) * 2);
 *           }
 *       }
 *   } */

using Microsoft.SqlServer.Dts.Pipeline.Wrapper;

namespace Microsoft.SqlServer.Dts.Pipeline;

public abstract class PipelineComponent
{
    /* Populated by the runtime before any lifecycle method is
     * called. Reading these in the constructor is undefined. */
    public IDTSComponentMetaData100 ComponentMetaData { get; internal set; } = null!;
    public IDTSBufferManager100     BufferManager     { get; internal set; } = null!;

    /* Design-time virtuals. Not driven by the betl runtime; left
     * here so user code that overrides them still compiles. */
    public virtual void ProvideComponentProperties() { }
    public virtual DTSValidationStatus Validate() => DTSValidationStatus.VS_ISVALID;
    public virtual void ReinitializeMetaData() { }
    public virtual void PerformUpgrade(int pipelineVersion) { }
    public virtual void SetComponentProperty(string propertyName, object value) { }

    /* Runtime lifecycle — these ARE driven by betl. */
    public virtual void PreExecute() { }
    public virtual void PrimeOutput(int outputs, int[] outputIDs, PipelineBuffer[] buffers) { }
    public abstract void ProcessInput(int inputID, PipelineBuffer buffer);
    public virtual void PostExecute() { }
    public virtual void Cleanup() { }
}

public enum DTSValidationStatus
{
    VS_ISVALID                        = 0,
    VS_ISCORRUPT                      = 1,
    VS_NEEDSNEWMETADATA               = 2,
    VS_ISBROKEN                       = 3,
}
