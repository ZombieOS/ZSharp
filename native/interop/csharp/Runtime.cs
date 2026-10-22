using System.Globalization;
using System.Numerics;
using System.Text.RegularExpressions;

[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("ZSharp.Interop.Worker")]

namespace ZSharp;

/// <summary>Exports a public type or individual public member to Z#.</summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Method |
    AttributeTargets.Constructor | AttributeTargets.Property | AttributeTargets.Field, Inherited = false)]
public sealed class ZSharpExportAttribute : Attribute { }

/// <summary>An exact VM number, before conversion to a native C# numeric parameter.</summary>
public readonly record struct ExactNumber(string Text)
{
    public override string ToString() => Text;

    internal BigInteger Integer()
    {
        string[] exponent = Text.Split('e', 'E');
        string[] fraction = exponent[0].Split('.');
        int scale = (exponent.Length == 2 ? int.Parse(exponent[1], CultureInfo.InvariantCulture) : 0)
            - (fraction.Length == 2 ? fraction[1].Length : 0);
        if (Math.Abs((long)scale) > 65536)
            throw new ArgumentException("Number is outside the C# integer parameter's range");
        BigInteger coefficient = BigInteger.Parse(string.Concat(fraction), CultureInfo.InvariantCulture);
        if (scale >= 0) return coefficient * BigInteger.Pow(10, scale);
        BigInteger quotient = BigInteger.DivRem(coefficient, BigInteger.Pow(10, -scale), out BigInteger remainder);
        if (!remainder.IsZero) throw new ArgumentException("Fractional number cannot be passed to an integer parameter");
        return quotient;
    }

    internal void Validate()
    {
        if (Text is null || !Regex.IsMatch(Text, @"\A-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?\z"))
            throw new ArgumentException("$number must contain valid decimal text");
    }

    internal string Canonical()
    {
        Validate();
        string[] exponent = Text.Split('e', 'E');
        string[] fraction = exponent[0].Split('.');
        BigInteger scale = (exponent.Length == 2 ? BigInteger.Parse(exponent[1], CultureInfo.InvariantCulture) : BigInteger.Zero)
            - (fraction.Length == 2 ? fraction[1].Length : 0);
        string digits = string.Concat(fraction).TrimStart('-').TrimStart('0');
        if (digits.Length == 0) return "0";
        string trimmed = digits.TrimEnd('0');
        scale += digits.Length - trimmed.Length;
        return (Text.StartsWith('-') ? "-" : "") + trimmed + "e" + scale.ToString(CultureInfo.InvariantCulture);
    }
}

/// <summary>Calls back into an imported brain in the active Z# VM.</summary>
public static class Runtime
{
    internal static Func<string, string?, string?, string?, object?[], object?>? Callback;

    public static object? Call(string target, params object?[] arguments)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(target);
        return Dispatch("host.call", target, null, null, arguments);
    }

    internal static object? Dispatch(string operation, string? target, string? reference, string? member, object?[] arguments)
        => (Callback ?? throw new InvalidOperationException("Z# callbacks are only available inside the ZVM worker"))
            (operation, target, reference, member, arguments);
}

/// <summary>A live Z# object; Get/Set/Invoke act on its original VM state.</summary>
public sealed class ZSharpObject
{
    internal string Reference { get; }
    internal string Type { get; }

    internal ZSharpObject(string reference, string type)
    {
        Reference = reference;
        Type = type;
    }

    public object? Get(string member) => Runtime.Dispatch("host.get", null, Reference, member, []);
    public void Set(string member, object? value) => Runtime.Dispatch("host.set", null, Reference, member, [value]);
    public object? Invoke(string member, params object?[] arguments)
        => Runtime.Dispatch("host.invoke", null, Reference, member, arguments);
    public override string ToString() => $"{Type} (Z# reference {Reference})";
}
