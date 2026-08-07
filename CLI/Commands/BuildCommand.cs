using ZSharp.CLI.Utils;
using ZSharp.Compiler.Build;
using ZSharp.Shared.Build;
using ZSharp.Shared.Workspace;

namespace ZSharp.CLI.Commands;

public static class BuildCommand
{
    public static void Execute()
    {
        Console.WriteLine("========================================");
        Console.WriteLine("Z# Build");
        Console.WriteLine("========================================");
        Console.WriteLine();

        Workspace workspace = WorkspaceScanner.Scan();

        Console.WriteLine($"Workspace: {workspace.RootDirectory}");
        Console.WriteLine($"Found {workspace.FileCount} Z# file(s).");

        if (workspace.FileCount == 0)
        {
            Console.WriteLine();
            Console.ForegroundColor = ConsoleColor.Yellow;
            Console.WriteLine("[Z#] No .zsharp files were found.");
            Console.ResetColor();
            return;
        }

        List<BuildResult> results = BuildEngine.Build(workspace);

        ConsoleRenderer.Print(results);
    }
}