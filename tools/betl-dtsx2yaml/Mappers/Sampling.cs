/* SSIS Percentage Sampling / Row Sampling → betl TODO stub.
 *
 * Both transforms split their input into a sampled output and a
 * "rest" output. betl has no sampling primitive; the closest cousin
 * for a fixed-N sample is `limit` (after an optional `sort` by a
 * random key), but reproducing the SSIS semantics (deterministic
 * seed, equal-probability rows in either branch) needs a real
 * implementation.
 *
 * We emit a passthrough `map` so downstream wiring still validates,
 * and a TODO header summarising the rewrite hints + the original
 * percentage / row count. The "sampled" branch — usually the first
 * <output> — keeps the upstream wiring; the "rest" branch would have
 * been wired downstream of a second output, which the operator must
 * connect manually. */

using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Sampling
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId,
                            bool isPercentage)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: map");
        if (fromId != null) w.Line($"from: {fromId}");

        string? sampleSize     = ReadProperty(c, isPercentage ? "SamplingValue" : "SampleSize");
        string? samplingSeed   = ReadProperty(c, "SamplingSeed");

        string label = isPercentage ? "Percentage Sampling" : "Row Sampling";
        w.Comment($"TODO: SSIS {label} has no betl equivalent.");
        if (isPercentage)
            w.Comment("      For percentage sampling, post-process with a `map` that");
        else
            w.Comment("      For fixed-N sampling, post-process with `sort` (by a");
        if (isPercentage)
            w.Comment("      adds a random key + a `filter` keeping rows below the cut.");
        else
        {
            w.Comment("      random key) followed by `limit: N`. For deterministic");
            w.Comment("      results carry the seed through ssisexpr's RANDOM(seed).");
        }
        if (!string.IsNullOrEmpty(sampleSize))
            w.Comment($"      Original sample size: {sampleSize}");
        if (!string.IsNullOrEmpty(samplingSeed))
            w.Comment($"      Original seed: {samplingSeed}");
        w.Indent(-2);
    }

    static string? ReadProperty(DtsxComponent c, string name)
    {
        if (c.Properties.TryGetValue(name, out var v)) return v;
        return c.Element?.Element("properties")
                         ?.Elements("property")
                         .FirstOrDefault(p => (string?)p.Attribute("name") == name)
                         ?.Value;
    }
}
