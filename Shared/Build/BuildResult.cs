using ZSharp.Shared.Syntax;

namespace ZSharp.Shared.Build;

public class BuildResult
{
    public string File { get; init; } = "";

    public FileHeader? Header { get; set; }

    public List<Room> Rooms { get; } = new();

    public List<Diagnostic> Diagnostics { get; } = new();

    public int ErrorCount =>
        Diagnostics.Count(d => d.Severity == DiagnosticSeverity.Error);

    public int WarningCount =>
        Diagnostics.Count(d => d.Severity == DiagnosticSeverity.Warning);

    public bool Passed => ErrorCount == 0;
}