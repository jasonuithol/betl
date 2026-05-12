/* SSIS Merge → betl `union` (with a sort-order caveat).
 *
 * SSIS Merge takes exactly two sorted inputs and interleaves them
 * preserving the sort key. betl's `union` is unordered (it appends
 * batches as they arrive), so this conversion drops the ordering
 * guarantee.
 *
 * The body shape mirrors UnionAll — multiple `from:` refs, no extra
 * properties — but we add a TODO header so the operator can re-sort
 * downstream if order matters. */

using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Merge
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c,
                            List<PredEdge>? preds)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: union");
        w.Comment("TODO: SSIS Merge preserves sort order across inputs; betl's");
        w.Comment("      union does not. If downstream needs sorted rows, add a");
        w.Comment("      `sort` step after this one with the original merge keys.");

        if (preds == null || preds.Count == 0)
        {
            w.Comment("TODO: Merge has no upstream paths — wire `from:` manually");
        }
        else
        {
            var refs = preds.Select(PredEdge.From).Distinct().ToList();
            w.Line("from: [" + string.Join(", ", refs) + "]");
        }
        w.Indent(-2);
    }
}
