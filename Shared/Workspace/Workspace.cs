namespace ZSharp.Shared.Workspace;

public class Workspace
{
    public string RootDirectory { get; }

    public List<string> Files { get; }

    public int FileCount => Files.Count;

    public Workspace(string rootDirectory, List<string> files)
    {
        RootDirectory = rootDirectory;
        Files = files;
    }
}