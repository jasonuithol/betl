/* Control-flow attributes attached to a top-level betl pipeline step:
 * `after:`, `on_failure:`, `condition:`. Built by the Converter when
 * it lowers SSIS precedence constraints across container boundaries
 * and emitted by the executable-level mappers right after their
 * `- id:` line. */

using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public sealed class FlowAttrs
{
    /* Step ids that must run (and succeed by default) before this step
     * — i.e. SSIS Success precedence constraints lowered to betl edges. */
    public List<string> After       { get; } = new();
    /* "stop" (default) / "continue" / "retry". Set to "continue" when
     * SSIS Completion or Failure constraints apply, since betl's
     * default would otherwise abort the downstream on upstream failure. */
    public string?      OnFailure   { get; set; }
    /* SSIS expression that gated the constraint, passed through as a
     * betl condition with lang: ssisexpr. Single string only; we don't
     * combine multiple constraints' expressions. */
    public string?      Condition   { get; set; }
    /* Free-form one-liner emitted as a YAML comment before the attrs
     * — used for "this was a Failure constraint, semantics is best-
     * effort under betl" notes that the operator should see at the
     * step where the loss happens. */
    public List<string> Notes       { get; } = new();

    public bool IsEmpty => After.Count == 0 && OnFailure == null
                        && Condition == null && Notes.Count == 0;

    public static void Emit(YamlWriter w, FlowAttrs? a)
    {
        if (a == null || a.IsEmpty) return;
        foreach (var n in a.Notes) w.Comment(n);
        if (a.After.Count > 0)
            w.Line("after: [" + string.Join(", ", a.After) + "]");
        if (a.OnFailure != null) w.Line($"on_failure: {a.OnFailure}");
        if (a.Condition != null)
        {
            /* betl `condition:` only accepts lang: lua / python per the
             * pipeline.schema; SSIS expressions don't have a direct
             * Lua translation in the general case (DT_*, GETDATE(),
             * etc.). Pass the original through as a YAML comment so
             * the operator can hand-translate, and emit `true` so
             * the step keeps running until they do. */
            w.Comment("TODO: SSIS-expression condition needs translation to Lua:");
            w.Comment("       " + a.Condition.Replace("\n", " "));
            w.Line("condition: \"true\"");
        }
    }
}
