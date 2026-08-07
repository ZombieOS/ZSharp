using ZSharp.Shared.Build;
using ZSharp.Shared.Workspace;

namespace ZSharp.Compiler.Build;

public static class BuildEngine
{
    public static List<BuildResult> Build(Workspace workspace)
    {
        List<BuildResult> results = new();

        foreach (string file in workspace.Files)
        {
            results.Add(HeaderValidator.Validate(file));
        }

        return results;
    }
}