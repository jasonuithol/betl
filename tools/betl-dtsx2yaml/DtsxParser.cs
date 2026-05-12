/* DTSX XML → DtsxPackage. Uses LINQ to XML; only the bits we care
 * about are pulled into the strongly-typed model. */

using System;
using System.IO;
using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml;

public static class DtsxParser
{
    public static readonly XNamespace DtsNs =
        "www.microsoft.com/SqlServer/Dts";

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

        var execs = root.Element(DtsNs + "Executables");
        if (execs != null)
        {
            foreach (var e in execs.Elements(DtsNs + "Executable"))
                pkg.Executables.Add(ParseExecutable(e));
        }
        return pkg;
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

    static DtsxExecutable ParseExecutable(XElement e)
    {
        var exe = new DtsxExecutable
        {
            Kind = (string?)e.Attribute(DtsNs + "ExecutableType") ?? "",
            Name = (string?)e.Attribute(DtsNs + "ObjectName") ?? "",
            ObjectData = e.Element(DtsNs + "ObjectData"),
        };
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
