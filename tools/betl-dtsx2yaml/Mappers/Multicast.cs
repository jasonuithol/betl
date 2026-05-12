/* SSIS Multicast → betl `multicast`.
 *
 * SSIS Multicast has one input and N outputs, each named (default
 * "Multicast Output 1", "Multicast Output 2", ...). The output names
 * carry over verbatim as betl tap ids — paths leaving the multicast
 * carry the source-port name in their startId, and `PredEdge.From`
 * adds the `:port` suffix automatically.
 *
 * Output names are sanitised through YamlWriter.Id so spaces become
 * underscores. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Multicast
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: multicast");
        if (fromId != null) w.Line($"from: {fromId}");

        var taps = c.Outputs.Where(o => !o.IsErrorOut)
                            .Select(o => YamlWriter.Id(o.Name))
                            .ToList();
        if (taps.Count == 0)
        {
            w.Comment("TODO: Multicast has no <output> elements — verify");
            w.Line("taps: []");
        }
        else
        {
            w.Line("taps: [" + string.Join(", ", taps) + "]");
        }
        w.Indent(-2);
    }
}
