/* SSIS Audit Transform → betl `map` with `add:` for each metadata
 * column.
 *
 * SSIS Audit adds N columns whose values are drawn from the package's
 * execution context. Each output column has a `<property
 * name="AuditType">` whose integer value identifies which piece of
 * metadata to emit:
 *
 *   0  ExecutionInstanceGUID
 *   1  PackageID
 *   2  PackageName
 *   3  VersionID
 *   4  ExecutionStartTime
 *   5  MachineName
 *   6  UserName
 *   7  TaskName
 *   8  TaskID
 *
 * betl doesn't expose package-execution metadata as first-class names
 * yet (SPEC §5.2 reserves a future ${betl.run.*} namespace), so the
 * cleanest emission is a `map` step that for each audit column emits
 * the *intent* as a literal placeholder + a TODO comment naming the
 * source. The operator fills in their preferred substitution at
 * runtime — typically a parameter, a literal, or a future
 * `${betl.run.machine}` reference. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Audit
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");

        var outputCols = c.Outputs
            .SelectMany(o => (o.Element?.Element("outputColumns")
                                       ?.Elements("outputColumn")
                                       ?? Enumerable.Empty<System.Xml.Linq.XElement>()))
            .ToList();
        if (outputCols.Count == 0)
        {
            w.Comment("TODO: Audit has no <outputColumn> entries — verify");
            w.Indent(-2);
            return;
        }

        w.Line("add:");
        w.Indent(2);
        foreach (var col in outputCols)
        {
            string name = (string?)col.Attribute("name") ?? "";
            string? auditTypeRaw = col.Element("properties")
                                    ?.Elements("property")
                                    .FirstOrDefault(p => (string?)p.Attribute("name") == "AuditType")
                                    ?.Value;
            int auditType = -1;
            int.TryParse(auditTypeRaw, out auditType);

            var (label, placeholder, ssisName) = AuditFor(auditType);

            w.Line($"{YamlWriter.Id(name)}:");
            w.Indent(2);
            w.Comment($"TODO: SSIS Audit column '{name}' = {ssisName}");
            w.Comment($"      betl has no built-in {label} source yet; the");
            w.Comment("      literal below is a placeholder. Replace with a");
            w.Comment("      parameter, a literal, or a future ${betl.run.*}.");
            w.Line("lang: ssisexpr");
            w.Line($"expr: {YamlWriter.Quote(placeholder)}");
            w.Line("type: u");
            w.Indent(-2);
        }
        w.Indent(-2);
        w.Indent(-2);
    }

    /* Returns (short-label, placeholder-expression, full-SSIS-name). */
    static (string, string, string) AuditFor(int t) => t switch
    {
        0 => ("execution-instance ID", "(unset)",                  "ExecutionInstanceGUID"),
        1 => ("package ID",            "(unset)",                  "PackageID"),
        2 => ("package name",          "(unset)",                  "PackageName"),
        3 => ("version ID",            "(unset)",                  "VersionID"),
        4 => ("execution start time",  "(unset)",                  "ExecutionStartTime"),
        5 => ("machine name",          "(unset)",                  "MachineName"),
        6 => ("user name",             "(unset)",                  "UserName"),
        7 => ("task name",             "(unset)",                  "TaskName"),
        8 => ("task ID",               "(unset)",                  "TaskID"),
        _ => ("audit",                 "(unset)",                  "Unknown(" + t + ")"),
    };
}
