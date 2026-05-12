/* Predecessor edge — the data needed by a mapper to format its
 * `from:` reference. Carries the upstream component's betl id, the
 * raw SSIS port name on the upstream side (e.g. "Multicast Output
 * 1"), and the upstream's component class so the from-ref formatter
 * can decide whether a `parent:port` suffix is required.
 *
 * Only multi-output upstream classes need the port suffix; for
 * everything else `from: parent` is the correct betl form. */

namespace Betl.Dtsx2Yaml.Mappers;

public sealed class PredEdge
{
    public string ParentId      { get; set; } = "";
    public string ParentPort    { get; set; } = "";
    public string ParentClassId { get; set; } = "";

    /* Format the from-ref. For Microsoft.ConditionalSplit and
     * Microsoft.Multicast the upstream port name is significant — the
     * downstream attaches to a named output port, so emit
     * `parent:port`. For everything else the upstream's port is
     * "<Component> Output", which collapses to the default output. */
    public static string From(PredEdge e)
    {
        bool multiPort = e.ParentClassId == "Microsoft.ConditionalSplit"
                      || e.ParentClassId == "Microsoft.Multicast";
        if (multiPort && !string.IsNullOrEmpty(e.ParentPort))
            return e.ParentId + ":" + YamlWriter.Id(e.ParentPort);
        return e.ParentId;
    }
}
