/* SSIS Union All → betl `union`.
 *
 * SSIS Union All has N input lanes, each routed to the single output.
 * betl `union` consumes a list of upstream refs in its `from:` field
 * — which is exactly what the parsed predecessors list gives us. */

using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class UnionAll
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c,
                            List<PredEdge>? preds)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: union");

        if (preds == null || preds.Count == 0)
        {
            w.Comment("TODO: Union All has no upstream paths — wire `from:` manually");
        }
        else
        {
            var refs = preds.Select(PredEdge.From).Distinct().ToList();
            w.Line("from: [" + string.Join(", ", refs) + "]");
        }
        w.Indent(-2);
    }
}
