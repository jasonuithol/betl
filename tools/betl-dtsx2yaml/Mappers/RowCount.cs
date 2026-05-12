/* SSIS Row Count → betl pass-through + TODO.
 *
 * SSIS Row Count is a sink-like transform: it counts every row that
 * passes through and writes the total into a package variable
 * (`<property name="VariableName">`). The data itself is forwarded
 * unchanged.
 *
 * betl has no first-class "set variable from row count" primitive yet
 * — variables are constants set at parameter time, not mutated
 * mid-pipeline. The most faithful emission is therefore:
 *
 *   - a betl `map` step with no `add:` and no `select:` (pure pass-
 *     through — preserves wiring so downstream `from:` references
 *     keep working), and
 *   - a TODO comment naming the target SSIS variable.
 *
 * The operator typically rewrites this as either a SQL COUNT(*) in
 * the downstream destination, or removes the step entirely. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class RowCount
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");

        string? varName = c.Properties.TryGetValue("VariableName", out var v) ? v : null;
        if (!c.Properties.ContainsKey("VariableName") && c.Element != null)
        {
            varName = c.Element.Element("properties")
                                ?.Elements("property")
                                .FirstOrDefault(p => (string?)p.Attribute("name") == "VariableName")
                                ?.Value;
        }

        w.Comment("TODO: SSIS Row Count writes its tally into a package variable —");
        w.Comment("      betl has no mid-pipeline variable assignment. This step is");
        w.Comment("      emitted as a passthrough; rewrite as a SQL COUNT(*) at the");
        w.Comment("      sink, or remove if the count was only used for logging.");
        if (!string.IsNullOrEmpty(varName))
            w.Comment($"      Original SSIS target variable: {varName}");
        w.Indent(-2);
    }
}
