public static class CommentParser
{
    public static bool IsSingleLineComment(string line)
    {
        return line.TrimStart().StartsWith("//");
    }

    public static bool StartsBlockComment(string line)
    {
        return line.Contains("/*");
    }

    public static bool EndsBlockComment(string line)
    {
        return line.Contains("*/");
    }
}