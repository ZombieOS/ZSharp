using System;
using System.IO;
using System.Linq;

namespace ZSharp.CLI
{
    internal class Program
    {
        static void Main(string[] args)
        {
            if (args.Length == 0)
            {
                PrintHelp();
                return;
            }

            string command = args[0].ToLower();

            switch (command)
            {
                case "build":
                    Build(args);
                    break;

                default:
                    Console.WriteLine($"[Z#] Unknown command '{command}'.");
                    Console.WriteLine();
                    PrintHelp();
                    break;
            }
        }

        static void Build(string[] args)
        {
            bool strict = args.Contains("--strict");

            Console.WriteLine("[Z#] Starting Build...");
            Console.WriteLine();

            string workspace = Directory.GetCurrentDirectory();

            Console.WriteLine($"[Z#] Workspace: {workspace}");
            Console.WriteLine("[Z#] Searching for Z# files...");

            string[] zsharpFiles = Directory.GetFiles(
                workspace,
                "*.zsharp",
                SearchOption.AllDirectories
            );

            Console.WriteLine($"[Z#] Found {zsharpFiles.Length} Z# file(s).");

            Console.WriteLine();

            if (strict)
            {
                Console.WriteLine("[Z#] Strict Mode Enabled");
                Console.WriteLine();
            }

            Console.WriteLine("[Z#] Build Complete.");
        }

        static void PrintHelp()
        {
            Console.WriteLine("Z# CLI");
            Console.WriteLine();
            Console.WriteLine("Commands:");
            Console.WriteLine("  build");
            Console.WriteLine("  build --strict");
        }
    }
}