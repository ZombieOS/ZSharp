using ZSharp.Shared.Build;

namespace ZSharp.CLI.Utils;

public static class ConsoleRenderer
{
    public static void Print(List<BuildResult> results)
    {
        Console.WriteLine();

        bool failed = results.Any(x => !x.Passed);

        Console.ForegroundColor =
            failed ? ConsoleColor.Red : ConsoleColor.Green;

        Console.WriteLine(
            failed
                ? "[Z#] BUILD FAILED"
                : "[Z#] BUILD SUCCESSFUL");

        Console.ResetColor();

        Console.WriteLine();

        foreach (BuildResult result in results)
        {
            Console.Write(result.File);

            if (result.Passed)
            {
                Console.ForegroundColor = ConsoleColor.Green;

                Console.WriteLine(
                    $" passed with {result.ErrorCount} errors and {result.WarningCount} warnings");

                Console.ResetColor();
            }
            else
            {
                Console.ForegroundColor = ConsoleColor.Red;

                Console.WriteLine(
                    $" failed with {result.ErrorCount} errors and {result.WarningCount} warnings");

                Console.ResetColor();
            }

            foreach (Diagnostic diagnostic in result.Diagnostics)
            {
                Console.ForegroundColor =
                    diagnostic.Severity == DiagnosticSeverity.Error
                        ? ConsoleColor.Red
                        : ConsoleColor.Yellow;

                Console.WriteLine(
                    $"  {diagnostic.Code}: {diagnostic.Message}");

                Console.ResetColor();
            }

            Console.WriteLine();
        }
    }
}