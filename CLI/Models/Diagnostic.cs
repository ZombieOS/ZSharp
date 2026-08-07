namespace ZSharp.CLI.Models;

public enum DiagnosticSeverity
{
    Error,
    Warning
}

public class Diagnostic
{
    public DiagnosticSeverity Severity { get; init; }

    public string Code { get; init; } = "";

    public string Message { get; init; } = "";

    public int Line { get; init; }

    public int Column { get; init; }
}