/* SSIS Script Task (Microsoft.ScriptTask) → betl `dotnet.task`.
 *
 * SSIS Script Task XML carries the user's source inside
 * <ScriptProject> elements (in a sibling namespace), with a
 * Language="CSharp" / "VisualBasic" attribute and one or more
 * <ProjectItem Name="ScriptMain.cs">CDATA</ProjectItem> entries.
 * We pluck the user's main file (ends in "Main.cs" / "Main.vb"),
 * inline it under `source: |`, and add a block-comment header that
 * lists the rename / signature edits the operator needs to apply
 * before the task is runnable under betl.
 *
 * Language=CSharp emits `lang: csharp`. Language=VisualBasic emits
 * `lang: vbnet` plus a stronger TODO pointing at the VB→C# converter
 * task (v0.2+1). Other languages emit a hard TODO. */

using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class ScriptTask
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxExecutable exe,
                            FlowAttrs? flow)
    {
        w.Line($"- id: {YamlWriter.Id(exe.Name)}");
        w.Indent(2);
        FlowAttrs.Emit(w, flow);
        w.Line("type: dotnet.task");

        var scriptProject = exe.ObjectData?.Descendants()
                            .FirstOrDefault(e => e.Name.LocalName == "ScriptProject");
        string ssisLang = scriptProject?.Attribute("Language")?.Value ?? "CSharp";

        var mainItem = ScriptCommon.PickMainScript(scriptProject, ssisLang);
        if (mainItem == null)
        {
            w.Line($"lang: {ScriptCommon.MapLang(ssisLang)}");
            w.Comment("TODO: SSIS Script Task has no recognisable main file");
            w.Comment("(expected <ProjectItem Name=\"ScriptMain.cs\"> or similar).");
            w.Line("source: |");
            w.Indent(2);
            w.Line("// (user source not found in DTSX)");
            w.Indent(-2);
            w.Indent(-2);
            return;
        }

        var prep = ScriptCommon.PrepareSource(mainItem, ssisLang);
        w.Line($"lang: {prep.Lang}");

        ScriptCommon.EmitTranslationHeader(w, prep,
            taskClass: "UserTask", baseClass: "Betl.BetlTask",
            entryMethod: "Run");
        w.Line("source: |");
        w.Indent(2);
        foreach (var line in prep.Source.Split('\n'))
            w.Line(line);
        w.Indent(-2);

        w.Indent(-2);
    }
}
