using ZSharp.Shared.Workspace;

namespace ZSharp.CLI.Utils;

public static class WorkspaceScanner
{
    public static Workspace Scan()
    {
        string root = FindWorkspaceRoot();

        List<string> files = Directory
            .GetFiles(root, "*.zsharp", SearchOption.AllDirectories)
            .ToList();

        return new Workspace(root, files);
    }

    private static string FindWorkspaceRoot()
    {
        DirectoryInfo? directory =
            new(Directory.GetCurrentDirectory());

        while (directory != null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "ZSharp.sln")))
            {
                return directory.FullName;
            }

            directory = directory.Parent;
        }

        return Directory.GetCurrentDirectory();
    }
}