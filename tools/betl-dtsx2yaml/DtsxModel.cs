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
    public List<DtsxConnection>  Connections    { get; } = new();
    public List<DtsxVariable>    Variables      { get; } = new();
    /* Project- or package-scope parameters. SSIS Project Deployment
     * Model packages reference these as @[$Project::X] / @[$Package::X],
     * with definitions either in Project.params (project-level) or in
     * the .dtsx itself (package-level). DataType here is System.TypeCode
     * (NOT VARENUM — that's only DtsxVariable.DataType). */
    public List<DtsxParameter>   Parameters     { get; } = new();
    public List<DtsxExecutable>  Executables    { get; } = new();
    /* Top-level precedence constraints — edges between direct
     * children of the package. Constraints scoped to a Sequence /
     * ForeachLoop / ForLoop sit on the container instead. */
    public List<DtsxPrecedence>  RootPrecedence { get; } = new();
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
    /* VARENUM (VT_*) code from a <DTS:VariableValue DataType="...">.
     * See Converter.MapVariableType. */
    public int    DataType    { get; set; }
    public string ValueRaw    { get; set; } = "";
}

/* SSIS Project/Package parameter as found in Project.params or in an
 * <SSIS:Parameter> block inside a .dtsx. Distinct from DtsxVariable:
 *  - parameters are project-deployment-model artefacts referenced as
 *    @[$Project::X] or @[$Package::X];
 *  - their DataType uses System.TypeCode (18=String), NOT VARENUM. */
public sealed class DtsxParameter
{
    public string Scope       { get; set; } = "Project"; /* "Project" or "Package" */
    public string Name        { get; set; } = "";
    public string Description { get; set; } = "";
    public int    DataType    { get; set; }              /* System.TypeCode */
    public string ValueRaw    { get; set; } = "";
    public bool   Sensitive   { get; set; }
}

public sealed class DtsxExecutable
{
    /* "Microsoft.Pipeline", "Microsoft.ExecuteSQLTask",
     * "Microsoft.ScriptTask", "Microsoft.Sequence",
     * "Microsoft.ForEachLoop", "Microsoft.ForLoop", ... */
    public string Kind        { get; set; } = "";
    public string Name        { get; set; } = "";
    /* Fully-qualified executable path, e.g. "Package\Stage 1\Truncate".
     * Used by precedence constraints to identify their endpoints.
     * Synthesised from the parent chain if the source XML omits the
     * DTS:refId attribute. */
    public string RefId       { get; set; } = "";
    /* Pipeline-only: components + paths inside the dataflow. */
    public List<DtsxComponent> Components { get; } = new();
    public List<DtsxPath>      Paths      { get; } = new();
    /* Task-level executables: the raw <DTS:ObjectData> sub-tree so
     * task-specific mappers can dig in for sql / script source / etc. */
    public XElement?           ObjectData { get; set; }

    /* SSIS <DTS:PropertyExpression DTS:Name="X">expr</...>: a sibling
     * of <DTS:ObjectData> that overrides task property X with an SSIS
     * expression evaluated at runtime. Common pattern: parameterise
     * SqlStatementSource with @[$Project::Server] etc. The static
     * value stored elsewhere is the design-time fallback; the dynamic
     * expression wins at runtime. Mappers should prefer the entry in
     * this dictionary when present. */
    public Dictionary<string,string> PropertyExpressions { get; } = new();

    /* Container support: a Sequence / ForEachLoop / ForLoop nests
     * child executables under <DTS:Executables>. Empty for leaf tasks. */
    public List<DtsxExecutable> Children    { get; } = new();
    /* Precedence constraints scoped to this container — edges between
     * direct children. The root package itself holds top-level edges. */
    public List<DtsxPrecedence> Precedence  { get; } = new();

    /* Foreach Loop specifics. */
    public string?                    ForeachEnumeratorType { get; set; }
    public Dictionary<string,string>  ForeachEnumProps      { get; } = new();
    public List<string>               ForeachVarMappings    { get; } = new();

    /* For Loop specifics (init / eval / assign are SSIS expressions). */
    public string? ForLoopInit   { get; set; }
    public string? ForLoopEval   { get; set; }
    public string? ForLoopAssign { get; set; }

    /* SSIS containers come in two naming flavours depending on the
     * SSIS version that wrote the package — newer SSDT emits the
     * "Microsoft.<Type>" names while older / direct-typed exports use
     * "STOCK:<TYPE>". We accept both. */
    public bool IsContainer =>
        Kind == "Microsoft.Sequence"   || Kind == "STOCK:SEQUENCE" ||
        Kind == "Microsoft.ForEachLoop"|| Kind == "STOCK:FOREACHLOOP" ||
        Kind == "Microsoft.ForLoop"    || Kind == "STOCK:FORLOOP";

    public bool IsSequence    =>
        Kind == "Microsoft.Sequence" || Kind == "STOCK:SEQUENCE";
    public bool IsForEachLoop =>
        Kind == "Microsoft.ForEachLoop" || Kind == "STOCK:FOREACHLOOP";
    public bool IsForLoop     =>
        Kind == "Microsoft.ForLoop" || Kind == "STOCK:FORLOOP";
}

/* An SSIS precedence constraint — directed edge between two
 * sibling executables inside a container. Models Success/Failure/
 * Completion semantics + optional expression gating + AND/OR
 * convergence. */
public sealed class DtsxPrecedence
{
    public string FromRefId  { get; set; } = "";
    public string ToRefId    { get; set; } = "";
    /* Value (DTS:Value): 0=Success (default), 1=Failure, 2=Completion. */
    public int    Value      { get; set; }
    /* EvalOp (DTS:EvalOp): 1=Constraint only (default),
     *                     2=Expression only,
     *                     3=Expression AND Constraint,
     *                     4=Expression OR Constraint. */
    public int    EvalOp     { get; set; } = 1;
    public string Expression { get; set; } = "";
    /* When multiple constraints target the same downstream:
     * LogicalAnd=True (default) requires all to satisfy;
     * LogicalAnd=False requires any one. */
    public bool   LogicalAnd { get; set; } = true;
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
