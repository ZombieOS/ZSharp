using System.Linq;
using ZSharp.Shared.Syntax;

namespace ZSharp.CLI.Models;

public class BuildResult
{
    public string File { get; init; } = "";

    public List<Room> Rooms { get; } = new();

    public FileHeader? Header { get; set; }

    public List<Diagnostic> Diagnostics { get; } = new();

    public int ErrorCount =>
        Diagnostics.Count(x => x.Severity == DiagnosticSeverity.Error);

    public int WarningCount =>
        Diagnostics.Count(x => x.Severity == DiagnosticSeverity.Warning);

    public bool Passed => ErrorCount == 0;
}