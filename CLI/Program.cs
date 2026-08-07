using ZSharp.CLI.Commands;

namespace ZSharp.CLI;

internal class Program
{
    static void Main(string[] args)
    {
        CommandManager.Execute(args);
    }
}