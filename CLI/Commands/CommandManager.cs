namespace ZSharp.CLI.Commands;

public static class CommandManager
{
    public static void Execute(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("Z# CLI");
            Console.WriteLine("Usage: zsharp <command>");
            Console.WriteLine();
            Console.WriteLine("Commands:");
            Console.WriteLine("  build");
            return;
        }

        switch (args[0].ToLower())
        {
            case "build":
                BuildCommand.Execute();
                break;

            default:
                Console.WriteLine($"Unknown command: {args[0]}");
                break;
        }
    }
}