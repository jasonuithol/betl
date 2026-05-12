/* Thin wrapper around ICSharpCode.CodeConverter for snippet
 * conversion (VB.NET → C#).
 *
 * SSIS Script Tasks / Script Components written in VisualBasic
 * cannot be compiled by betl's NativeAOT shim — Roslyn rejects
 * [UnmanagedCallersOnly] on VB members (BC37316), and that attribute
 * is how the shim wires its entry points to the C side. The fix is
 * to convert the source at DTSX → YAML time so the generated YAML
 * always ships C#.
 *
 * ICSharpCode.CodeConverter does the Roslyn-driven syntactic
 * conversion. It wraps the snippet in a partial class internally
 * (the result is line-equivalent C# minus VB-specific constructs
 * that have no C# analogue — those are left as TODO comments in
 * the output by the library itself).
 *
 * The conversion is best-effort: SSIS-specific identifiers
 * (Dts.Variables, Dts.Connections, VSTARTScriptObjectModelBase,
 * Input0Buffer, etc.) carry over verbatim — they're still SSIS-API
 * references that the operator must hand-translate to Betl.Params /
 * Betl.Connection / Betl.BetlScript. ScriptCommon's translation
 * checklist still applies. */

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;
using System.Threading;
using ICSharpCode.CodeConverter;
using Microsoft.CodeAnalysis;

namespace Betl.Dtsx2Yaml;

public static class VbToCs
{
    public sealed class Result
    {
        public string? CSharpSource { get; init; }
        public bool    Success      => CSharpSource != null;
        public string  Diagnostics  { get; init; } = "";
    }

    /* Match `Imports X.Y.Z` and `Imports Aliased = X.Y` at top of file
     * (allowing leading whitespace). VB's `Imports` is the rough
     * equivalent of C#'s `using` — same statement, different keyword. */
    static readonly Regex ImportsLine =
        new(@"^\s*Imports\s+([\w\.]+)(?:\s*=\s*([\w\.]+))?\s*$",
            RegexOptions.IgnoreCase | RegexOptions.Compiled);

    /* CodeConverter signals a partial-conversion failure by wrapping
     * the whole snippet in `#error ...` + `/* Cannot convert ... *​/`.
     * The result has Success=true but is unusable. We detect this and
     * downgrade to a real failure so the mapper falls back to raw VB. */
    static readonly Regex CannotConvert =
        new(@"#error\s+Cannot convert\s+\w+",
            RegexOptions.IgnoreCase | RegexOptions.Compiled);

    /* Synchronous wrapper — the converter API is async but we only
     * use it in a single-pass CLI tool, so blocking is fine. */
    public static Result Translate(string vbSource)
    {
        if (string.IsNullOrWhiteSpace(vbSource))
            return new Result { CSharpSource = "", Diagnostics = "" };

        /* Strip top-level `Imports` lines. Snippet-mode CodeConverter
         * tries to classify them and needs IEmbeddedLanguageClassificationService
         * — not registered in our headless workspace. Pull them out
         * up front; emit the equivalent `using` directives ourselves
         * in front of the converted body. */
        var usings = new List<string>();
        var bodyLines = new List<string>();
        foreach (var line in vbSource.Replace("\r\n", "\n").Split('\n'))
        {
            var m = ImportsLine.Match(line);
            if (m.Success)
            {
                /* Translate `Imports Alias = Ns.X` → `using Alias = Ns.X;`,
                 * `Imports Ns.X` → `using Ns.X;`. Skip the "Global"
                 * pseudo-namespace, which is a VB-specific affordance. */
                string target = m.Groups[2].Success
                                ? $"{m.Groups[1].Value} = {m.Groups[2].Value}"
                                : m.Groups[1].Value;
                if (target.StartsWith("Global.", StringComparison.OrdinalIgnoreCase))
                    target = target.Substring("Global.".Length);
                if (!string.Equals(target, "Global", StringComparison.OrdinalIgnoreCase))
                    usings.Add($"using {target};");
                continue;
            }
            bodyLines.Add(line);
        }
        string vbBody = string.Join("\n", bodyLines).Trim();

        var input = new CodeWithOptions(vbBody)
            .SetFromLanguage(LanguageNames.VisualBasic)
            .SetToLanguage(LanguageNames.CSharp)
            .WithTypeReferences(null);   /* null = default netstandard refs */

        string converted;
        try
        {
            var task = CodeConverter.ConvertAsync(input, CancellationToken.None);
            /* Blocking on the converter task is fine in a single-threaded
             * CLI; there's no UI thread to deadlock against. VSTHRD002
             * is a Visual-Studio-extension lint that doesn't apply here. */
#pragma warning disable VSTHRD002
            var conv = task.GetAwaiter().GetResult();
#pragma warning restore VSTHRD002
            if (!conv.Success)
                return new Result { Diagnostics = conv.GetExceptionsAsString() };
            converted = conv.ConvertedCode ?? "";
        }
        catch (Exception ex)
        {
            return new Result { Diagnostics = ex.ToString() };
        }

        if (CannotConvert.IsMatch(converted))
        {
            /* Partial-conversion wrapper — first line gives the gist. */
            var first = converted.Split('\n', 2)[0].Trim();
            return new Result { Diagnostics = first };
        }

        if (usings.Count > 0)
            converted = string.Join("\n", usings) + "\n\n" + converted;
        return new Result { CSharpSource = converted };
    }
}
