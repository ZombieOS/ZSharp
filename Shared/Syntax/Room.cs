using ZSharp.Shared.Build;

namespace ZSharp.Shared.Syntax;

public enum RoomVisibility
{
    Normal,
    Noticed,
    Silent
}

public class Room
{
    public string Name { get; init; } = "";

    public RoomVisibility Visibility { get; init; }

    public int StartLine { get; init; }

    public int EndLine { get; set; }
}