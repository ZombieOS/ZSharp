using ZSharp;

[ZSharpExport]
public class Sample
{
    public int value;
    public readonly string readOnly = "fixed";
    public string publicGetPrivateSet { get; private set; } = "readable";
    public string initOnly { get; init; } = "initial";
    private readonly string secret = "must not escape";
    private static int calls;

    public Sample(int initial) { value = initial; }
    public int increase(int amount) { value += amount; return value; }
    public Sample self() { return this; }
    private string hidden() => secret;
    public static int add(int left, int right) => left + right;
    public static int counted() => ++calls;
    public static ExactNumber exact(ExactNumber value) => value;
    public static int[] array(int[] value) => value;
    public static Dictionary<string, object> record() => new() { ["name"] = "bridge", ["value"] = 42 };
    public static string printed() { Console.WriteLine("user-output-stays-on-stderr"); return "printed"; }
    public static object? callback(int value) => Runtime.Call("Tests:Bridge:Double", value);
    public static object? proxy(ZSharpObject value)
    {
        object? old = value.Get("value");
        value.Set("value", 21);
        return value.Invoke("increase", old);
    }
    public static string ambiguous(string value) => "text";
    public static string ambiguous(Sample value) => "object";
    public static string overload(int value) => "int";
    public static string overload(string value) => "text";
}

internal class NotPublic
{
    [ZSharpExport] public static string leak() => "no";
}
