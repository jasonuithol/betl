/* Shared bits between ScriptTask and ScriptComponent mappers.
 *
 *   - Language detection (CSharp / VisualBasic → betl lang ids)
 *   - Main-source-file lookup (the SSIS-default <ProjectItem
 *     Name="ScriptMain.cs">, falling back to the first .cs / .vb item)
 *   - VB.NET → C# translation via ICSharpCode.CodeConverter
 *     (conversion-time only; betl's NativeAOT shim can only compile
 *      C#, so VB sources are translated up front)
 *   - The translation-notes block-comment header — a punch list of
 *     edits the operator must perform after import, since SSIS' VSTA
 *     base classes have no automatic equivalent in betl. */

using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public sealed class PreparedSource
{
    public string Lang        { get; init; } = "csharp";
    public string Source      { get; init; } = "";
    /* True when we ran ICSharpCode.CodeConverter on a VB original. */
    public bool   FromVbAuto  { get; init; }
    /* Set when the original was VB but CodeConverter failed — we then
     * fall back to emitting the raw VB and the operator must translate
     * by hand. */
    public string? FailureNote { get; init; }
}

public static class ScriptCommon
{
    public static string MapLang(string ssisLang) => ssisLang switch
    {
        "CSharp"      => "csharp",
        "C#"          => "csharp",
        "VisualBasic" => "vbnet",
        "VB"          => "vbnet",
        _             => "csharp",         /* best-effort fallback */
    };

    /* Find the user's main source file inside a <ScriptProject>. SSIS
     * conventionally names it ScriptMain.cs / ScriptMain.vb, but we
     * fall back to the first .cs/.vb ProjectItem if the convention
     * isn't followed. Returns the <ProjectItem> XElement, or null if
     * nothing usable was found. */
    public static XElement? PickMainScript(XElement? scriptProject, string ssisLang)
    {
        if (scriptProject == null) return null;
        var items = scriptProject.Descendants()
                        .Where(e => e.Name.LocalName == "ProjectItem")
                        .ToList();
        if (items.Count == 0) return null;

        string ext = ssisLang.Contains("VisualBasic",
                        System.StringComparison.OrdinalIgnoreCase) ? ".vb" : ".cs";

        /* Prefer the conventional ScriptMain file. */
        var main = items.FirstOrDefault(i =>
            ((string?)i.Attribute("Name") ?? "")
                .EndsWith("Main" + ext, System.StringComparison.OrdinalIgnoreCase));
        if (main != null) return main;

        /* Anything ending with .cs / .vb that isn't an obvious SSIS
         * scaffold file (Component.* , Properties/AssemblyInfo.*) */
        return items.FirstOrDefault(i =>
        {
            var n = ((string?)i.Attribute("Name") ?? "");
            return n.EndsWith(ext, System.StringComparison.OrdinalIgnoreCase)
                && !n.StartsWith("Properties/", System.StringComparison.OrdinalIgnoreCase)
                && !n.Equals("Component" + ext, System.StringComparison.OrdinalIgnoreCase);
        });
    }

    /* Resolve a ProjectItem's raw text + the SSIS language into the
     * final (lang, source) pair to emit. C# passes through; VB is
     * run through CodeConverter and downgrades to raw-VB only when
     * the converter throws. */
    public static PreparedSource PrepareSource(XElement mainItem, string ssisLang)
    {
        string raw = (mainItem.Value ?? "").Replace("\r\n", "\n");
        string lang = MapLang(ssisLang);
        if (lang != "vbnet")
            return new PreparedSource { Lang = "csharp", Source = raw };

        var conv = VbToCs.Translate(raw);
        if (conv.Success)
            return new PreparedSource
            {
                Lang       = "csharp",
                Source     = conv.CSharpSource!.Replace("\r\n", "\n"),
                FromVbAuto = true,
            };

        /* Translation failed — surface the original VB plus a
         * one-line failure note so the operator knows what to do. */
        return new PreparedSource
        {
            Lang        = "vbnet",
            Source      = raw,
            FailureNote = conv.Diagnostics.Length > 0
                            ? conv.Diagnostics.Split('\n')[0]
                            : "CodeConverter returned no diagnostics",
        };
    }

    /* Emit the rename-this / change-base / replace-Dts-with-Betl
     * checklist as YAML comments. Operator-facing — the goal is to
     * make the import a 5-minute exercise rather than a forensic
     * project. */
    public static void EmitTranslationHeader(YamlWriter w, PreparedSource prep,
                                             string taskClass, string baseClass,
                                             string entryMethod)
    {
        w.Comment("─── translation checklist ─────────────────────────────────");
        if (prep.FromVbAuto)
        {
            w.Comment("Original SSIS source was VB.NET; auto-translated to C# by");
            w.Comment("ICSharpCode.CodeConverter at conversion time. Verify the");
            w.Comment("converted body before relying on it — Roslyn's converter");
            w.Comment("is syntactic, not semantic, and SSIS-specific identifiers");
            w.Comment("(Dts.*, VSTART*Base, Input0Buffer, ...) come through verbatim.");
        }
        else if (prep.Lang == "vbnet")
        {
            w.Comment("This is VB.NET source.  betl's runtime is C#-only");
            w.Comment("(Roslyn rejects [UnmanagedCallersOnly] on VB members,");
            w.Comment("which the AOT shim needs).");
            w.Comment($"VB→C# auto-translation FAILED: {prep.FailureNote}");
            w.Comment("Translate manually before this step can run.");
        }
        w.Comment($"1. Rename `class ScriptMain` to `class {taskClass}`.");
        w.Comment($"2. Change the base class to `{baseClass}`.");
        if (entryMethod == "Run")
        {
            w.Comment("3. Rename `public void Main()` to `public override void Run()`.");
        }
        else
        {
            w.Comment("3. Replace SSIS' Input0_ProcessInputRow(Input0Buffer Row)");
            w.Comment("   with `public override void OnRow(Betl.InputRow row)`.");
            w.Comment("   Use `Emit(new Betl.OutputRow { ... })` to push rows out.");
            w.Comment("   Override `OnEof()` for fan-in (end-of-stream aggregation).");
        }
        w.Comment("4. Replace `Dts.Variables[\"x\"].Value` with `Betl.Params.Get(\"x\")`.");
        w.Comment("5. Replace `Dts.Connections[\"x\"].AcquireConnection(null)`");
        w.Comment("   with `Betl.Connection.Get(\"x\")` (returns the raw DSN).");
        w.Comment("6. Replace `Dts.Events.FireInformation(...)` with `Betl.Log.Info(...)`.");
        w.Comment("───────────────────────────────────────────────────────────");
    }
}
