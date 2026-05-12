/* Structured representation of a parsed DTSX file.
 *
 * Only the bits we actually emit are modelled; everything else stays
 * in the underlying XElement so a downstream pass can grep into it
 * without re-parsing. */

using System.Collections.Generic;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml;

public sealed class DtsxPackage
{
    public string Name { get; set; } = "package";
    public List<DtsxConnection>  Connections { get; } = new();
    public List<DtsxVariable>    Variables   { get; } = new();
    public List<DtsxExecutable>  Executables { get; } = new();
}

public sealed class DtsxConnection
{
    public string Name        { get; set; } = "";
    public string CreationName{ get; set; } = "";   /* e.g. "OLEDB", "FLATFILE" */
    /* Raw connection-string blob (OLEDB) or the file path (FLATFILE),
     * stashed verbatim for the mapper to interpret. */
    public string Payload     { get; set; } = "";
    /* Original XElement for components that need to peek further. */
    public XElement? Element  { get; set; }
}

public sealed class DtsxVariable
{
    public string Namespace   { get; set; } = "User";
    public string Name        { get; set; } = "";
    /* DTSX data-type code; see DtsxParser for the SSIS → betl type
     * mapping. */
    public int    DataType    { get; set; }
    public string ValueRaw    { get; set; } = "";
}

public sealed class DtsxExecutable
{
    /* "Microsoft.Pipeline", "Microsoft.ExecuteSQLTask",
     * "Microsoft.ScriptTask", ... */
    public string Kind        { get; set; } = "";
    public string Name        { get; set; } = "";
    /* Pipeline-only: components + paths inside the dataflow. */
    public List<DtsxComponent> Components { get; } = new();
    public List<DtsxPath>      Paths      { get; } = new();
    /* Task-level executables: the raw <DTS:ObjectData> sub-tree so
     * task-specific mappers can dig in for sql / script source / etc. */
    public XElement?           ObjectData { get; set; }
}

public sealed class DtsxComponent
{
    public string RefId             { get; set; } = "";
    public string Name              { get; set; } = "";
    public string ClassId           { get; set; } = "";   /* "Microsoft.OLEDBSource" etc. */
    public string? ConnectionManagerRefId { get; set; }    /* component → connection edge */
    public Dictionary<string, string> Properties { get; } = new();
    /* Per-output port metadata. SSIS pipeline components carry a list
     * of <output> elements; each names the port and may carry its own
     * <properties> bag (e.g. conditional-split FriendlyExpression). */
    public List<DtsxOutput>           Outputs    { get; } = new();
    public XElement?                  Element    { get; set; }
}

public sealed class DtsxOutput
{
    public string  Name        { get; set; } = "";
    public bool    IsErrorOut  { get; set; }
    public Dictionary<string, string> Properties { get; } = new();
    public XElement? Element   { get; set; }
}

public sealed class DtsxPath
{
    public string  StartComponentRef { get; set; } = "";
    public string  EndComponentRef   { get; set; } = "";
    /* Bare output port name from "<comp>.Outputs[<name>]"; empty when
     * the start id had no Outputs[...] suffix. */
    public string  StartPortName     { get; set; } = "";
}
