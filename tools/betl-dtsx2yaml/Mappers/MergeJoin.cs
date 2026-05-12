/* SSIS Merge Join → betl `join`.
 *
 * Merge Join has two input lanes ("Merge Join Left Input" and "Merge
 * Join Right Input") plus a single output. JoinType:
 *     0 = full outer
 *     1 = left outer
 *     2 = inner
 *
 * betl join syntax:
 *   - id: jn
 *     type: join
 *     from: [left, right]      # ordered pair, port 0 = left
 *     kind: inner|left|outer   # SSIS "full" → betl "outer"
 *     on:  { left_col: right_col }
 *
 * Join keys come from the *input columns'* `JoinKey` + `SortKeyPosition`
 * attributes in the SSIS XML — a deeper translation than v0.2 attempts.
 * We emit a `on: {}` skeleton with TODO so the operator fills in the
 * key pairs. The first predecessor is taken as the left lane and the
 * second as the right; SSIS's path-name convention almost always
 * produces that order at the path level. */

using System.Collections.Generic;

namespace Betl.Dtsx2Yaml.Mappers;

public static class MergeJoin
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c,
                            List<PredEdge>? preds)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: join");

        string? leftFrom  = (preds != null && preds.Count >= 1)
                                ? PredEdge.From(preds[0]) : null;
        string? rightFrom = (preds != null && preds.Count >= 2)
                                ? PredEdge.From(preds[1]) : null;
        if (preds != null && preds.Count > 2)
            w.Comment($"TODO: Merge Join has {preds.Count} upstream paths "
                    + "— betl join takes exactly two (left, right)");
        if (leftFrom == null || rightFrom == null)
            w.Comment("TODO: Merge Join missing one or both upstream paths");
        w.Line($"from: [{leftFrom ?? "TODO_LEFT"}, {rightFrom ?? "TODO_RIGHT"}]");

        c.Properties.TryGetValue("JoinType", out var jt);
        string kind = jt switch
        {
            "0" => "outer",       /* SSIS "Full outer" → betl "outer" */
            "1" => "left",
            "2" => "inner",
            _   => "inner",
        };
        w.Line($"kind: {kind}");

        w.Comment("TODO: SSIS Merge Join carries join keys on the *input*");
        w.Comment("columns via JoinKey + SortKeyPosition. Fill in the map:");
        w.Comment("  on: { left_col_name: right_col_name, ... }");
        w.Line("on: {}");
        w.Indent(-2);
    }
}
