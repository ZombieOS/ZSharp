using ZSharp.Shared.Build;
using ZSharp.Compiler.Headers;

namespace ZSharp.Compiler.Build;

public static class HeaderValidator
{
    public static BuildResult Validate(string file)
    {
        BuildResult result = new()
        {
            File = file
        };

        string[] lines = File.ReadAllLines(file);

        string? header = lines
            .FirstOrDefault(x => !string.IsNullOrWhiteSpace(x));

        if (header == null)
        {
            result.Diagnostics.Add(new Diagnostic
            {
                Severity = DiagnosticSeverity.Error,
                Code = "ZS1001",
                Line = 1,
                Column = 1,
                Message = "Missing file header."
            });

            return result;
        }

        header = header.Trim();

        if (!header.StartsWith("zsharp = "))
        {
            result.Diagnostics.Add(new Diagnostic
            {
                Severity = DiagnosticSeverity.Error,
                Code = "ZS1002",
                Line = 1,
                Column = 1,
                Message = "Expected 'zsharp = <project type>'."
            });

            return result;
        }

        string projectType =
            header.Substring("zsharp = ".Length);

        if (!ProjectTypes.All.Contains(projectType))
        {
            result.Diagnostics.Add(new Diagnostic
            {
                Severity = DiagnosticSeverity.Error,
                Code = "ZS1003",
                Line = 1,
                Column = 1,
                Message = $"Unknown project type '{projectType}'."
            });

            return result;
        }

        result.Header = new FileHeader
        {
            ProjectType = projectType
        };

        return result;
    }
}