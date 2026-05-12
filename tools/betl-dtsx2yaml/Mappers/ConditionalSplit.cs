/* SSIS Conditional Split → betl `conditional_split`.
 *
 * The SSIS shape:
 *   <component componentClassID="Microsoft.ConditionalSplit" ...>
 *     <outputs>
 *       <output name="Hot Path" ...>
 *         <properties>
 *           <property name="FriendlyExpression">[Priority] == "hot"</property>
 *           <property name="EvaluationOrder">0</property>
 *         </properties>
 *       </output>
 *       <output name="Cold Path" ...>
 *         <properties>
 *           <property name="FriendlyExpression">[Priority] == "cold"</property>
 *           <property name="EvaluationOrder">1</property>
 *         </properties>
 *       </output>
 *       <output name="Default" isErrorOut="false">
 *         <!-- no FriendlyExpression on the default output -->
 *       </output>
 *     </outputs>
 *   </component>
 *
 * Cases get emitted in EvaluationOrder (lowest first). Outputs with
 * no FriendlyExpression become the betl `default:` bucket (SSIS calls
 * this the "Default Output").
 *
 * Expression translation is the gnarly part: SSIS expression syntax
 * is its own DSL ([Col] == "x" && [Other] > 5). We have a translator
 * (betl-ssisexpr provider) and the converter emits `lang: ssisexpr`
 * so the host wires the expression to the right engine. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class ConditionalSplit
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: conditional_split");
        if (fromId != null) w.Line($"from: {fromId}");

        /* Partition outputs into cases (have FriendlyExpression) and
         * default (no expression, not an error output). Error outputs
         * are skipped: betl currently has no "row-error redirect"
         * concept, and dropping these silently is less surprising than
         * mapping them onto the default bucket. */
        var cases = c.Outputs
            .Where(o => !o.IsErrorOut && o.Properties.ContainsKey("FriendlyExpression"))
            .OrderBy(o =>
            {
                o.Properties.TryGetValue("EvaluationOrder", out var s);
                return int.TryParse(s, out var n) ? n : int.MaxValue;
            })
            .ToList();
        var defaultOutput = c.Outputs
            .FirstOrDefault(o => !o.IsErrorOut
                              && !o.Properties.ContainsKey("FriendlyExpression"));

        if (cases.Count == 0)
        {
            w.Comment("TODO: no Conditional Split cases found — verify outputs");
            w.Line("cases: []");
            w.Indent(-2);
            return;
        }

        w.Line("cases:");
        w.Indent(2);
        foreach (var o in cases)
        {
            o.Properties.TryGetValue("FriendlyExpression", out var expr);
            w.Line($"- name: {YamlWriter.Id(o.Name)}");
            w.Indent(2);
            w.Line("lang: ssisexpr");
            w.Line("where: " + YamlWriter.Quote(expr ?? "true"));
            w.Indent(-2);
        }
        w.Indent(-2);

        if (defaultOutput != null)
            w.Line($"default: {YamlWriter.Id(defaultOutput.Name)}");

        w.Indent(-2);
    }
}
