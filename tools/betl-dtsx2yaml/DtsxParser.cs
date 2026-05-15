/* DTSX XML → DtsxPackage. Uses LINQ to XML; only the bits we care
 * about are pulled into the strongly-typed model. */

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml;

public static class DtsxParser
{
    public static readonly XNamespace DtsNs =
        "www.microsoft.com/SqlServer/Dts";
    public static readonly XNamespace SsisNs =
        "www.microsoft.com/SqlServer/SSIS";

    public static DtsxPackage LoadFile(string path)
    {
        var doc = XDocument.Load(path);
        if (doc.Root == null)
            throw new InvalidDataException("dtsx file has no root element");
        return ParseRoot(doc.Root);
    }

    public static DtsxPackage Parse(string xml)
    {
        var doc = XDocument.Parse(xml);
        if (doc.Root == null)
            throw new InvalidDataException("empty document");
        return ParseRoot(doc.Root);
    }

    /* Standalone .conmgr file: a top-level <DTS:ConnectionManager> with
     * the same shape as an embedded package <ConnectionManager>. Real
     * SSIS Project Deployment Model packages reference connection
     * managers stored in separate .conmgr files at the project root.
     * Components reference them by name via
     *   connectionManagerRefId="Project.ConnectionManagers[<name>]"
     * which already matches what ConnectionLookup parses. */
    public static DtsxConnection LoadConmgrFile(string path)
    {
        var doc = XDocument.Load(path);
        if (doc.Root == null)
            throw new InvalidDataException("conmgr file has no root element");
        return ParseConnection(doc.Root);
    }

    /* Project.params (or any <SSIS:Parameters> document). */
    public static List<DtsxParameter> LoadParamsFile(string path)
    {
        var doc = XDocument.Load(path);
        if (doc.Root == null) return new List<DtsxParameter>();
        return ParseSsisParameters(doc.Root, scopeLabel: "Project");
    }

    static DtsxPackage ParseRoot(XElement root)
    {
        var pkg = new DtsxPackage
        {
            Name = (string?)root.Attribute(DtsNs + "ObjectName") ?? "package",
        };

        var conns = root.Element(DtsNs + "ConnectionManagers");
        if (conns != null)
        {
            foreach (var cm in conns.Elements(DtsNs + "ConnectionManager"))
                pkg.Connections.Add(ParseConnection(cm));
        }

        var vars = root.Element(DtsNs + "Variables");
        if (vars != null)
        {
            foreach (var v in vars.Elements(DtsNs + "Variable"))
                pkg.Variables.Add(ParseVariable(v));
        }

        /* Package-scope parameters: <SSIS:Parameter> can live as a
         * descendant of the package root in Project Deployment Model
         * packages. The element uses the SSIS:* namespace, not DTS:*. */
        pkg.Parameters.AddRange(ParseSsisParameters(root, scopeLabel: "Package"));

        string pkgRefId = (string?)root.Attribute(DtsNs + "refId") ?? "Package";
        var execs = root.Element(DtsNs + "Executables");
        if (execs != null)
        {
            foreach (var e in execs.Elements(DtsNs + "Executable"))
                pkg.Executables.Add(ParseExecutable(e, pkgRefId));
        }
        /* Top-level precedence constraints live as a sibling of
         * <DTS:Executables> on the package root, the same way they
         * appear inside containers. */
        pkg.RootPrecedence.AddRange(ParsePrecedences(root));
        return pkg;
    }

    /* <SSIS:Parameters> / <SSIS:Parameter> blocks. Used in both
     * Project.params (root is <SSIS:Parameters>) and embedded in a
     * .dtsx (root is the package; we walk descendants). Each parameter
     * has a flat <SSIS:Properties> bag — we pull the four fields that
     * matter for conversion. DataType here is System.TypeCode
     * (18=String, 11=Int64, etc.) — distinct from the VARENUM used on
     * <DTS:Variable> values. */
    static List<DtsxParameter> ParseSsisParameters(XElement scope, string scopeLabel)
    {
        var list = new List<DtsxParameter>();
        var paramEls = scope.Name.LocalName == "Parameters"
            ? scope.Elements(SsisNs + "Parameter")
            : scope.Descendants(SsisNs + "Parameter");
        foreach (var p in paramEls)
        {
            var name = (string?)p.Attribute(SsisNs + "Name") ?? "";
            if (string.IsNullOrEmpty(name)) continue;
            string desc = "", value = "";
            int dt = 0;
            bool sensitive = false;
            var props = p.Element(SsisNs + "Properties");
            if (props != null)
            {
                foreach (var prop in props.Elements(SsisNs + "Property"))
                {
                    var pn = (string?)prop.Attribute(SsisNs + "Name") ?? "";
                    switch (pn)
                    {
                        case "Description": desc  = prop.Value; break;
                        case "Value":       value = prop.Value; break;
                        case "DataType":    int.TryParse(prop.Value, out dt); break;
                        case "Sensitive":   sensitive = prop.Value == "1"
                                                || prop.Value.Equals("True",
                                                    System.StringComparison.OrdinalIgnoreCase);
                                            break;
                    }
                }
            }
            list.Add(new DtsxParameter
            {
                Scope = scopeLabel,
                Name = name,
                Description = desc,
                DataType = dt,
                ValueRaw = value,
                Sensitive = sensitive,
            });
        }
        return list;
    }

    static List<DtsxPrecedence> ParsePrecedences(XElement container)
    {
        var list = new List<DtsxPrecedence>();
        var pcs = container.Element(DtsNs + "PrecedenceConstraints");
        if (pcs == null) return list;
        foreach (var pc in pcs.Elements(DtsNs + "PrecedenceConstraint"))
        {
            int.TryParse((string?)pc.Attribute(DtsNs + "Value")  ?? "0", out int val);
            int.TryParse((string?)pc.Attribute(DtsNs + "EvalOp") ?? "1", out int evalOp);
            string logAnd = (string?)pc.Attribute(DtsNs + "LogicalAnd") ?? "True";
            list.Add(new DtsxPrecedence
            {
                FromRefId  = (string?)pc.Attribute(DtsNs + "From")       ?? "",
                ToRefId    = (string?)pc.Attribute(DtsNs + "To")         ?? "",
                Value      = val,
                EvalOp     = evalOp,
                Expression = (string?)pc.Attribute(DtsNs + "Expression") ?? "",
                LogicalAnd = !logAnd.Equals("False",
                                System.StringComparison.OrdinalIgnoreCase),
            });
        }
        return list;
    }

    static DtsxConnection ParseConnection(XElement cm)
    {
        var c = new DtsxConnection
        {
            Name         = (string?)cm.Attribute(DtsNs + "ObjectName") ?? "",
            CreationName = (string?)cm.Attribute(DtsNs + "CreationName") ?? "",
            Element      = cm,
        };
        /* Two payload shapes:
         *   OLEDB / ADO.NET: <ObjectData><ConnectionManager ConnectionString="..."/></ObjectData>
         *   FLATFILE:        <ObjectData><ConnectionManager ConnectionString="<file path>" ... />
         * For now we grab whichever ConnectionString attribute we find. */
        var inner = cm.Element(DtsNs + "ObjectData")
                     ?.Descendants(DtsNs + "ConnectionManager").FirstOrDefault();
        if (inner != null)
        {
            c.Payload = (string?)inner.Attribute(DtsNs + "ConnectionString") ?? "";
        }
        return c;
    }

    static DtsxVariable ParseVariable(XElement v)
    {
        var name = (string?)v.Attribute(DtsNs + "ObjectName") ?? "";
        var ns   = (string?)v.Attribute(DtsNs + "Namespace") ?? "User";
        int type = 0;
        string raw = "";
        var vv = v.Element(DtsNs + "VariableValue");
        if (vv != null)
        {
            int.TryParse((string?)vv.Attribute(DtsNs + "DataType") ?? "0", out type);
            raw = vv.Value;
        }
        return new DtsxVariable {
            Namespace = ns, Name = name, DataType = type, ValueRaw = raw,
        };
    }

    static DtsxExecutable ParseExecutable(XElement e, string parentRefId)
    {
        string name = (string?)e.Attribute(DtsNs + "ObjectName") ?? "";
        /* DTS:refId is the SSIS-assigned unique path, e.g.
         *   "Package\Loop Files\Truncate"
         * Synthesise from parent\name when the source omits it
         * (older exports occasionally do). */
        string refId = (string?)e.Attribute(DtsNs + "refId")
                       ?? (parentRefId + "\\" + name);
        var exe = new DtsxExecutable
        {
            Kind       = (string?)e.Attribute(DtsNs + "ExecutableType") ?? "",
            Name       = name,
            RefId      = refId,
            ObjectData = e.Element(DtsNs + "ObjectData"),
        };

        /* <DTS:PropertyExpression DTS:Name="X">expr</...> — runtime
         * overrides for individual task properties. They sit at the same
         * level as <DTS:ObjectData>. Mappers consult this dictionary
         * before falling back to the static property value. */
        foreach (var px in e.Elements(DtsNs + "PropertyExpression"))
        {
            var pname = (string?)px.Attribute(DtsNs + "Name");
            if (!string.IsNullOrEmpty(pname))
                exe.PropertyExpressions[pname!] = px.Value;
        }
        if (exe.Kind == "Microsoft.Pipeline")
        {
            /* <ObjectData><pipeline xmlns=""><components/>...<paths/></pipeline></ObjectData>
             * Note the inner pipeline is in the default (empty) namespace
             * — unlike everything else in DTSX which is in DtsNs. */
            var pipeline = exe.ObjectData?.Element("pipeline");
            if (pipeline != null)
            {
                var comps = pipeline.Element("components");
                if (comps != null)
                {
                    foreach (var c in comps.Elements("component"))
                        exe.Components.Add(ParseComponent(c));
                }
                var paths = pipeline.Element("paths");
                if (paths != null)
                {
                    foreach (var p in paths.Elements("path"))
                        exe.Paths.Add(ParsePath(p));
                }
            }
        }

        /* Containers (Sequence / ForEachLoop / ForLoop) carry nested
         * <DTS:Executables> + their own <DTS:PrecedenceConstraints>.
         * Recurse so the model captures the full tree. */
        if (exe.IsContainer)
        {
            var childExecs = e.Element(DtsNs + "Executables");
            if (childExecs != null)
            {
                foreach (var ce in childExecs.Elements(DtsNs + "Executable"))
                    exe.Children.Add(ParseExecutable(ce, refId));
            }
            exe.Precedence.AddRange(ParsePrecedences(e));
        }

        /* Foreach Loop has <DTS:ForEachEnumerator> and
         * <DTS:ForEachVariableMappings> at the executable level. */
        if (exe.IsForEachLoop)
        {
            var fe = e.Element(DtsNs + "ForEachEnumerator");
            var feObj = fe?.Element(DtsNs + "ObjectData");
            var feInner = feObj?.Elements().FirstOrDefault();
            if (feInner != null)
            {
                exe.ForeachEnumeratorType = feInner.Name.LocalName;
                foreach (var a in feInner.Attributes())
                    exe.ForeachEnumProps[a.Name.LocalName] = a.Value;
            }
            var maps = e.Element(DtsNs + "ForEachVariableMappings");
            if (maps != null)
            {
                foreach (var m in maps.Elements(DtsNs + "ForEachVariableMapping"))
                {
                    string vn = (string?)m.Attribute(DtsNs + "VariableName") ?? "";
                    if (!string.IsNullOrEmpty(vn))
                        exe.ForeachVarMappings.Add(vn);
                }
            }
        }

        /* For Loop carries init / eval / assign as attributes in a
         * sibling namespace (DTS:ForLoop). We localName-match. */
        if (exe.IsForLoop)
        {
            foreach (var a in e.Attributes())
            {
                switch (a.Name.LocalName)
                {
                    case "InitExpression":   exe.ForLoopInit   = a.Value; break;
                    case "EvalExpression":   exe.ForLoopEval   = a.Value; break;
                    case "AssignExpression": exe.ForLoopAssign = a.Value; break;
                }
            }
        }
        return exe;
    }

    static DtsxComponent ParseComponent(XElement c)
    {
        var co = new DtsxComponent
        {
            RefId   = (string?)c.Attribute("refId")           ?? "",
            Name    = (string?)c.Attribute("name")            ?? "",
            ClassId = (string?)c.Attribute("componentClassID")?? "",
            Element = c,
        };
        var conn = c.Element("connections")?.Element("connection");
        if (conn != null)
            co.ConnectionManagerRefId =
                (string?)conn.Attribute("connectionManagerRefId");
        var props = c.Element("properties");
        if (props != null)
        {
            foreach (var p in props.Elements("property"))
            {
                var n = (string?)p.Attribute("name");
                if (n != null) co.Properties[n] = p.Value;
            }
        }
        var outs = c.Element("outputs");
        if (outs != null)
        {
            foreach (var o in outs.Elements("output"))
            {
                var op = new DtsxOutput
                {
                    Name       = (string?)o.Attribute("name") ?? "",
                    IsErrorOut = ((string?)o.Attribute("isErrorOut") ?? "false")
                                    .Equals("true", System.StringComparison.OrdinalIgnoreCase),
                    Element    = o,
                };
                var pp = o.Element("properties");
                if (pp != null)
                {
                    foreach (var p in pp.Elements("property"))
                    {
                        var n = (string?)p.Attribute("name");
                        if (n != null) op.Properties[n] = p.Value;
                    }
                }
                co.Outputs.Add(op);
            }
        }
        return co;
    }

    static DtsxPath ParsePath(XElement p)
    {
        /* startId / endId in DTSX point at *port* refIds (Outputs[...]
         * / Inputs[...]). We strip the trailing .Outputs[...] /
         * .Inputs[...] segment to get the bare component refId, but
         * retain the start-port name — multi-output components
         * (conditional_split, multicast) need it to wire downstream
         * `from: parent:port` references. */
        string start = (string?)p.Attribute("startId") ?? "";
        string end   = (string?)p.Attribute("endId")   ?? "";
        return new DtsxPath
        {
            StartComponentRef = StripPort(start),
            EndComponentRef   = StripPort(end),
            StartPortName     = ExtractPort(start),
        };
    }

    static string StripPort(string refId)
    {
        int dot = refId.LastIndexOf('.');
        if (dot < 0) return refId;
        var tail = refId.AsSpan(dot + 1);
        if (tail.StartsWith("Inputs[") || tail.StartsWith("Outputs[")
            || tail.StartsWith("ErrorOutputs["))
            return refId[..dot];
        return refId;
    }

    /* Pull "Hot Path" out of "...Outputs[Hot Path]". Returns "" when
     * the refId has no Outputs[...] suffix (e.g. it's already bare or
     * it points at an Inputs[...] segment). */
    static string ExtractPort(string refId)
    {
        int dot = refId.LastIndexOf('.');
        if (dot < 0) return "";
        var tail = refId.Substring(dot + 1);
        if (!tail.StartsWith("Outputs[")) return "";
        int rb = tail.LastIndexOf(']');
        if (rb < "Outputs[".Length) return "";
        return tail.Substring("Outputs[".Length, rb - "Outputs[".Length);
    }
}
