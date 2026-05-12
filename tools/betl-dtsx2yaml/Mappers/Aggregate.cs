/* SSIS Aggregate → betl `aggregate`.
 *
 * SSIS Aggregate output columns carry an `AggregationType` property:
 *     0 = Group By
 *     1 = Count
 *     2 = Count Distinct
 *     3 = Sum
 *     4 = Average
 *     5 = Min
 *     6 = Max
 *
 * betl aggregate syntax:
 *
 *   type: aggregate
 *   from: ...
 *   group_by: [col, col]
 *   compute:
 *     n: { agg: count }
 *     s: { agg: sum, over: id }
 *
 * The source column for each aggregation lives in the column's
 * `AggregationColumnId` (a refId pointing at an input column).
 * Translating that refId → input column name needs an `<inputColumn>`
 * lookup, so for v0.2 we emit the skeleton with `over: TODO` markers
 * for any non-count aggregation. */

using System.Collections.Generic;
using System.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class Aggregate
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxComponent c, string? fromId)
    {
        w.Line($"- id: {YamlWriter.Id(c.Name)}");
        w.Indent(2);
        w.Line("type: aggregate");
        if (fromId != null) w.Line($"from: {fromId}");

        var outputCols = c.Outputs
            .SelectMany(o => (o.Element?.Element("outputColumns")
                                       ?.Elements("outputColumn")
                                       ?? Enumerable.Empty<System.Xml.Linq.XElement>()))
            .ToList();

        var groupBy = new List<string>();
        var aggs    = new List<(string name, string func, bool needsOver)>();
        foreach (var col in outputCols)
        {
            string name = (string?)col.Attribute("name") ?? "";
            string? aggType = col.Element("properties")
                                ?.Elements("property")
                                .FirstOrDefault(p => (string?)p.Attribute("name") == "AggregationType")
                                ?.Value;
            if (aggType == "0")
            {
                groupBy.Add(name);
                continue;
            }
            string func = aggType switch
            {
                "1" => "count",
                "2" => "count",        /* count_distinct → fold to count + TODO */
                "3" => "sum",
                "4" => "avg",
                "5" => "min",
                "6" => "max",
                _   => "count",
            };
            bool isCountDistinct = aggType == "2";
            bool needsOver       = func != "count";
            if (isCountDistinct)
                w.Comment($"TODO: column '{name}' is SSIS COUNT DISTINCT — "
                        + "betl has no distinct-count yet; using `count` placeholder");
            aggs.Add((name, func, needsOver));
        }

        if (groupBy.Count > 0)
            w.Line("group_by: [" + string.Join(", ", groupBy.Select(YamlWriter.Id)) + "]");
        else
            w.Comment("TODO: no group-by columns found — empty group_by aggregates "
                    + "over the whole stream");

        if (aggs.Count == 0)
        {
            w.Comment("TODO: no aggregations found — verify SSIS output columns");
            w.Line("compute: {}");
        }
        else
        {
            w.Line("compute:");
            w.Indent(2);
            foreach (var (name, func, needsOver) in aggs)
            {
                if (needsOver)
                {
                    w.Comment("TODO: SSIS AggregationColumnId refId not resolved — "
                            + "fill `over:` with the input column name");
                    w.Line($"{YamlWriter.Id(name)}: {{ agg: {func}, over: TODO }}");
                }
                else
                {
                    w.Line($"{YamlWriter.Id(name)}: {{ agg: {func} }}");
                }
            }
            w.Indent(-2);
        }
        w.Indent(-2);
    }
}
