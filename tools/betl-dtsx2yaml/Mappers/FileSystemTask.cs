/* SSIS File System Task → betl `file.copy` / `file.move` / `file.delete`.
 *
 * The DTSX shape:
 *   <DTS:ObjectData>
 *     <FileSystemData
 *         Operation="CopyFile|MoveFile|DeleteFile|RenameFile|
 *                    CreateDirectory|DeleteDirectory|DeleteDirectoryContent|
 *                    SetAttributes"
 *         Source="..."
 *         Destination="..."
 *         OverwriteDestinationFile="True|False"
 *         IsSourcePathVariable="True|False"     -- source is a variable name
 *         IsDestinationPathVariable="True|False"
 *         />
 *   </DTS:ObjectData>
 *
 * Maps cleanly to betl's three file tasks for the four common ops.
 * Directory / attribute ops have no betl equivalent and fall back to
 * a `shell` task with an OS-portable `mkdir`/`rm`/`chmod` shape +
 * TODO comment. */

using System.Linq;
using System.Xml.Linq;

namespace Betl.Dtsx2Yaml.Mappers;

public static class FileSystemTask
{
    public static void Emit(YamlWriter w, DtsxPackage pkg, DtsxExecutable exe,
                            FlowAttrs? flow)
    {
        w.Line($"- id: {YamlWriter.Id(exe.Name)}");
        w.Indent(2);
        FlowAttrs.Emit(w, flow);

        var data = exe.ObjectData?.Descendants()
                     .FirstOrDefault(e => e.Name.LocalName == "FileSystemData");
        string op   = (string?)data?.Attribute("Operation")   ?? "";
        string src  = (string?)data?.Attribute("Source")      ?? "";
        string dst  = (string?)data?.Attribute("Destination") ?? "";
        string ow   = (string?)data?.Attribute("OverwriteDestinationFile") ?? "False";
        bool overwrite = ow.Equals("True", System.StringComparison.OrdinalIgnoreCase);

        bool srcIsVar = ((string?)data?.Attribute("IsSourcePathVariable") ?? "False")
                          .Equals("True", System.StringComparison.OrdinalIgnoreCase);
        bool dstIsVar = ((string?)data?.Attribute("IsDestinationPathVariable") ?? "False")
                          .Equals("True", System.StringComparison.OrdinalIgnoreCase);

        string srcRef = srcIsVar ? VarRef(src) : src;
        string dstRef = dstIsVar ? VarRef(dst) : dst;

        switch (op)
        {
            case "CopyFile":
                w.Line("type: file.copy");
                EmitFromTo(w, srcRef, dstRef, overwrite);
                break;
            case "MoveFile":
            case "RenameFile":
                w.Line("type: file.move");
                EmitFromTo(w, srcRef, dstRef, overwrite);
                break;
            case "DeleteFile":
                w.Line("type: file.delete");
                w.Line("path: " + YamlWriter.Quote(srcRef));
                break;
            case "CreateDirectory":
                FallbackShell(w, op, "mkdir", new[] { "-p", srcRef });
                break;
            case "DeleteDirectory":
                FallbackShell(w, op, "rm", new[] { "-rf", srcRef });
                break;
            case "DeleteDirectoryContent":
                /* SSIS deletes children but leaves the directory.
                 * `rm -rf <dir>/*` requires shell expansion which betl's
                 * shell task doesn't do (argv is literal). Use find. */
                FallbackShell(w, op, "find",
                              new[] { srcRef, "-mindepth", "1", "-delete" });
                break;
            default:
                w.Comment($"TODO: SSIS File System operation '{op}' has no direct");
                w.Comment("betl equivalent; falling back to a no-op shell task.");
                w.Line("type: shell");
                w.Line("argv: [\"true\"]");
                break;
        }
        w.Indent(-2);
    }

    static void EmitFromTo(YamlWriter w, string src, string dst, bool overwrite)
    {
        w.Line("src: " + YamlWriter.Quote(src));
        w.Line("dst: " + YamlWriter.Quote(dst));
        if (!overwrite)
            w.Comment("note: SSIS OverwriteDestinationFile=False — betl file "
                    + "tasks overwrite unconditionally; rewire if you need "
                    + "the if-not-exists guard.");
    }

    static void FallbackShell(YamlWriter w, string ssisOp, string cmd, string[] args)
    {
        w.Comment($"TODO: SSIS File System '{ssisOp}' has no direct betl");
        w.Comment("task; using a shell task that approximates the operation");
        w.Comment("(non-Linux hosts will need a different argv).");
        w.Line("type: shell");
        var quoted = new System.Collections.Generic.List<string> { YamlWriter.Quote(cmd) };
        foreach (var a in args) quoted.Add(YamlWriter.Quote(a));
        w.Line("argv: [" + string.Join(", ", quoted) + "]");
    }

    /* SSIS variable references in path fields look like "User::FilePath"
     * (without the @[]). The DTSX expression engine substitutes the
     * value at runtime. betl's ${params.X} substitution does the same
     * for User-scope vars; emit the param ref so the operator only has
     * to map the var to a parameter. */
    static string VarRef(string ssisVarName)
    {
        /* Strip leading "User::" prefix if present. */
        const string user = "User::";
        string bare = ssisVarName.StartsWith(user,
                          System.StringComparison.OrdinalIgnoreCase)
                          ? ssisVarName.Substring(user.Length)
                          : ssisVarName;
        return "${params." + YamlWriter.Id(bare) + "}";
    }
}
