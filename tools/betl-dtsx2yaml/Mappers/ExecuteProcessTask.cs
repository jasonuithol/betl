/* SSIS Execute Process Task → betl `shell`.
 *
 * SSIS gives us an executable + a single Arguments string. betl's
 * shell task takes argv as a literal list (no shell expansion), so
 * we'd need to split Arguments — but SSIS preserves shell quoting in
 * that string and a naive whitespace split breaks for `--name="my
 * value"` and similar shapes. We emit the executable + a TODO with
 * the verbatim args string for the operator to split correctly.
 *
 * DTSX shape:
 *   <DTS:ObjectData>
 *     <ExecuteProcessData
 *         Executable="/usr/local/bin/my-tool"
 *         Arguments="--input data.csv --output out.csv"
 *         WorkingDirectory="/jobs"
 *         TimeOut="300"
 *         FailTaskIfReturnCodeIsNotSuccessValue="True"
 *         SuccessValue="0" />
 *   </DTS:ObjectData>
 */

using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class ExecuteProcessTask
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxExecutable exe,
                            FlowAttrs? flow)
    {
        w.Line($"- id: {YamlWriter.Id(exe.Name)}");
        w.Indent(2);
        FlowAttrs.Emit(w, flow);
        w.Line("type: shell");

        var data = exe.ObjectData?.Descendants()
                     .FirstOrDefault(e => e.Name.LocalName == "ExecuteProcessData");
        string ex    = (string?)data?.Attribute("Executable")       ?? "";
        string args  = (string?)data?.Attribute("Arguments")        ?? "";
        string cwd   = (string?)data?.Attribute("WorkingDirectory") ?? "";
        string tout  = (string?)data?.Attribute("TimeOut")          ?? "";
        string failOnNon0 =
            (string?)data?.Attribute("FailTaskIfReturnCodeIsNotSuccessValue")
            ?? "True";

        if (string.IsNullOrEmpty(ex))
        {
            w.Comment("TODO: SSIS Execute Process — Executable not specified");
            w.Line("argv: [\"true\"]");
        }
        else if (string.IsNullOrEmpty(args))
        {
            w.Line($"argv: [{YamlWriter.Quote(ex)}]");
        }
        else
        {
            w.Comment("TODO: SSIS Execute Process passed a single Arguments");
            w.Comment("string; split it into argv[] elements respecting any");
            w.Comment("embedded shell quoting:");
            w.Comment($"  Arguments = {args}");
            w.Line($"argv: [{YamlWriter.Quote(ex)}]");
        }

        if (!string.IsNullOrEmpty(cwd))
            w.Comment($"TODO: SSIS WorkingDirectory={cwd} — betl's shell task "
                    + "has no cwd field; wrap argv in `sh -c \"cd ...; ...\"` "
                    + "or set the cwd in the executable itself.");
        if (!string.IsNullOrEmpty(tout) && tout != "0")
            w.Line($"timeout: {tout}s");
        if (!failOnNon0.Equals("True", System.StringComparison.OrdinalIgnoreCase))
            w.Comment("note: SSIS FailTaskIfReturnCodeIsNotSuccessValue=False");
        w.Indent(-2);
    }
}
