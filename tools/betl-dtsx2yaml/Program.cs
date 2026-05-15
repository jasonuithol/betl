/* betl-dtsx2yaml — convert SSIS DTSX packages to betl YAML.
 *
 * Usage:
 *   betl-dtsx2yaml <input.dtsx> [-o <output.yml>]
 *
 * Default output is stdout. Unsupported DTSX components emit a
 * "TODO: manual migration required" comment in-place so the operator
 * can spot them without parsing the whole stderr.
 *
 * Project Deployment Model: SSDT lays out a project as a directory
 * containing the .dtsx alongside sibling .conmgr files (one per
 * project-level connection) and a Project.params file. The .dtsx
 * references these by name. When given a .dtsx, we auto-scan its
 * parent directory for these siblings and merge them into the package
 * model so the operator doesn't need a separate flag. */

using System;
using System.IO;

namespace Betl.Dtsx2Yaml;

public static class Program
{
    public static int Main(string[] args)
    {
        string? input = null;
        string? output = null;
        bool verbose = false;
        for (int i = 0; i < args.Length; ++i)
        {
            switch (args[i])
            {
                case "-o":
                case "--output":
                    if (i + 1 >= args.Length) { Usage(); return 2; }
                    output = args[++i];
                    break;
                case "-v":
                case "--verbose":
                    verbose = true;
                    break;
                case "-h":
                case "--help":
                    Usage();
                    return 0;
                default:
                    if (input != null) { Usage(); return 2; }
                    input = args[i];
                    break;
            }
        }
        if (input == null) { Usage(); return 2; }

        if (!File.Exists(input))
        {
            Console.Error.WriteLine($"betl-dtsx2yaml: input not found: {input}");
            return 1;
        }

        try
        {
            var pkg = DtsxParser.LoadFile(input);
            MergeProjectSiblings(pkg, input);
            var yaml = Converter.PackageToYaml(pkg, verbose);
            if (output != null) File.WriteAllText(output, yaml);
            else Console.Out.Write(yaml);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("betl-dtsx2yaml: " + ex.Message);
            if (verbose) Console.Error.WriteLine(ex.StackTrace);
            return 1;
        }
    }

    static void Usage()
    {
        Console.Error.WriteLine(
            "Usage: betl-dtsx2yaml <input.dtsx> [-o <output.yml>] [-v]\n" +
            "\n" +
            "Converts an SSIS DTSX package to betl YAML. Unsupported\n" +
            "components emit a 'TODO: manual migration' comment in-place.");
    }

    /* Auto-discover project-level siblings sitting next to the .dtsx:
     *   *.conmgr      → DtsxConnection appended to pkg.Connections
     *   Project.params → DtsxParameter list appended to pkg.Parameters
     * No-op when the input is a standalone .dtsx with no siblings —
     * existing single-file conversions are unaffected. */
    static void MergeProjectSiblings(DtsxPackage pkg, string inputPath)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(inputPath));
        if (dir == null || !Directory.Exists(dir)) return;
        foreach (var conmgr in Directory.EnumerateFiles(dir, "*.conmgr"))
        {
            try
            {
                var conn = DtsxParser.LoadConmgrFile(conmgr);
                if (!string.IsNullOrEmpty(conn.Name))
                    pkg.Connections.Add(conn);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"betl-dtsx2yaml: ignoring {Path.GetFileName(conmgr)}: {ex.Message}");
            }
        }
        foreach (var paramsFile in Directory.EnumerateFiles(dir, "Project.params"))
        {
            try
            {
                pkg.Parameters.AddRange(DtsxParser.LoadParamsFile(paramsFile));
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"betl-dtsx2yaml: ignoring {Path.GetFileName(paramsFile)}: {ex.Message}");
            }
        }
    }
}
