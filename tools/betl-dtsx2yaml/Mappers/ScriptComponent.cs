/* SSIS Script Component (Microsoft.ScriptComponentHost) → betl
 * `dotnet.script`.
 *
 * The component carries a `ScriptComponentType` property:
 *   0 = source       — no betl analogue (sources are providers, not
 *                      script transforms); emit TODO
 *   1 = transform    — straightforward `dotnet.script`
 *   2 = destination  — emit `dotnet.script` with an empty output_schema
 *                      and a TODO suggesting the operator move the
 *                      sink logic into a downstream dotnet.task instead.
 *
 * Output schema is derived from the SSIS <outputs>/<outputColumns>
 * — each column's `dataType` attribute maps to an Arrow format char
 * via the DerivedColumn helper. Columns we can't auto-translate get
 * `type: u` (utf8) with a TODO. */

using System.Collections.Generic;
using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class ScriptComponent
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: dotnet.script");
        if (fromId != null) w.Line($"from: {fromId}");

        c.Properties.TryGetValue("ScriptComponentType", out var kindS);
        switch (kindS)
        {
            case "0":
                w.Comment("TODO: SSIS Script Source has no direct betl analogue.");
                w.Comment("Either implement as a source provider, or replace with");
                w.Comment("a dotnet.task that writes rows into a downstream lookup.");
                break;
            case "2":
                w.Comment("TODO: SSIS Script Destination has no rows-out semantic.");
                w.Comment("Either move the sink logic into a downstream dotnet.task,");
                w.Comment("or accept that dotnet.script always emits a stream.");
                break;
        }

        var scriptProject = c.Element?.Descendants()
                            .FirstOrDefault(e => e.Name.LocalName == "ScriptProject");
        string ssisLang = scriptProject?.Attribute("Language")?.Value ?? "CSharp";
        /* We need the prepared source's actual `lang` (csharp after
         * successful VB→C# translation; vbnet only on translator
         * failure). Probe up front; the same `prep` is re-used below. */
        var probeMain = ScriptCommon.PickMainScript(scriptProject, ssisLang);
        var prep = probeMain != null
            ? ScriptCommon.PrepareSource(probeMain, ssisLang)
            : new PreparedSource { Lang = ScriptCommon.MapLang(ssisLang), Source = "" };
        w.Line($"lang: {prep.Lang}");

        /* Output schema from the first non-error <output>. */
        var outputCols = c.Outputs
            .Where(o => !o.IsErrorOut)
            .SelectMany(o => (o.Element?.Element("outputColumns")
                                       ?.Elements("outputColumn")
                                       ?? Enumerable.Empty<XElement>()))
            .ToList();
        if (outputCols.Count == 0)
        {
            w.Comment("TODO: no output columns found — fill `output_schema:` to match");
            w.Comment("whatever rows your UserScript will Emit().");
            w.Line("output_schema: []");
        }
        else
        {
            w.Line("output_schema:");
            w.Indent(2);
            foreach (var col in outputCols)
            {
                string name = (string?)col.Attribute("name") ?? "";
                string fmt  = DerivedColumn.DtsxDataTypeToArrowFormat(
                                  (string?)col.Attribute("dataType"));
                if (string.IsNullOrEmpty(fmt))
                {
                    w.Comment($"TODO: dataType for '{name}' isn't auto-translatable — using utf8");
                    fmt = "u";
                }
                w.Line($"- {{ name: {YamlWriter.Id(name)}, type: {fmt} }}");
            }
            w.Indent(-2);
        }

        if (probeMain == null)
        {
            w.Comment("TODO: SSIS Script Component has no recognisable main file.");
            w.Line("source: |");
            w.Indent(2);
            w.Line("// (user source not found in DTSX)");
            w.Indent(-2);
            w.Indent(-2);
            return;
        }

        ScriptCommon.EmitTranslationHeader(w, prep,
            taskClass: "UserScript", baseClass: "Betl.BetlScript",
            entryMethod: "OnRow");
        w.Line("source: |");
        w.Indent(2);
        foreach (var line in prep.Source.Split('\n'))
            w.Line(line);
        w.Indent(-2);
        w.Indent(-2);
    }
}
