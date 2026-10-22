using System.Collections;
using System.Globalization;
using System.Numerics;
using System.Reflection;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace ZSharp;

internal sealed class Worker
{
    private const int MaxFrameBytes = 1024 * 1024;
    private readonly TextReader input;
    private readonly TextWriter protocol;
    private readonly Dictionary<string, Assembly> modules = new(OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
    private readonly Dictionary<string, object> objects = new();
    private readonly Dictionary<object, string> identities = new(ReferenceEqualityComparer.Instance);
    private readonly int thread = Environment.CurrentManagedThreadId;
    private long nextObject = 1;
    private long nextCallback = 1;

    private Worker(TextReader input, TextWriter protocol)
    {
        this.input = input;
        this.protocol = protocol;
        Runtime.Callback = Callback;
    }

    public static void Main()
    {
        Console.InputEncoding = new UTF8Encoding(false);
        Console.OutputEncoding = new UTF8Encoding(false);
        TextWriter protocol = Console.Out;
        Console.SetOut(Console.Error);
        var worker = new Worker(Console.In, protocol);
        try
        {
            string? line;
            while ((line = worker.ReadFrame()) is not null) worker.Accept(line);
        }
        finally
        {
            Runtime.Callback = null;
            worker.objects.Clear();
            worker.identities.Clear();
        }
    }

    private static string Text(JsonElement request, string field)
        => request.TryGetProperty(field, out JsonElement value) && value.ValueKind != JsonValueKind.Null
            ? value.GetString() ?? "" : "";

    private void Send(object? id, bool ok, object? value)
    {
        var response = new Dictionary<string, object?> { ["id"] = id, ["op"] = "result", ["ok"] = ok };
        response[ok ? "value" : "error"] = value;
        Write(response);
    }

    private void Write(object value)
    {
        string json = JsonSerializer.Serialize(value);
        if (Encoding.UTF8.GetByteCount(json) > MaxFrameBytes)
            throw new ArgumentException("C# bridge frame exceeds the 1 MiB limit");
        protocol.WriteLine(json);
        protocol.Flush();
    }

    private string? ReadFrame()
    {
        var line = new StringBuilder();
        int character;
        while ((character = input.Read()) != -1 && character != '\n')
        {
            if (line.Length == MaxFrameBytes) throw new ArgumentException("C# bridge frame exceeds the 1 MiB limit");
            line.Append((char)character);
        }
        if (character == -1 && line.Length == 0) return null;
        string result = line.ToString();
        if (Encoding.UTF8.GetByteCount(result) > MaxFrameBytes)
            throw new ArgumentException("C# bridge frame exceeds the 1 MiB limit");
        return result;
    }

    private void Accept(string line)
    {
        object? id = null;
        try
        {
            using JsonDocument document = JsonDocument.Parse(line, new JsonDocumentOptions { MaxDepth = 128 });
            JsonElement request = document.RootElement;
            id = request.TryGetProperty("id", out JsonElement value) ? value.Clone() : null;
            Send(id, true, Encode(Dispatch(request), 0));
        }
        catch (Exception error)
        {
            while (error is TargetInvocationException && error.InnerException is not null) error = error.InnerException;
            Send(id, false, $"{error.GetType().Name}: {error.Message}");
        }
    }

    private object? Callback(string operation, string? target, string? reference, string? member, object?[] arguments)
    {
        if (Environment.CurrentManagedThreadId != thread)
            throw new InvalidOperationException("Z# callbacks must run on the thread handling the current foreign call");
        long id = -nextCallback++;
        var request = new Dictionary<string, object?> {
            ["id"] = id, ["op"] = operation, ["args"] = arguments.Select(argument => Encode(argument, 0)).ToArray()
        };
        if (target is not null) request["target"] = target;
        if (reference is not null) { request["ref"] = reference; request["member"] = member; }
        Write(request);
        string? line;
        while ((line = ReadFrame()) is not null)
        {
            using JsonDocument document = JsonDocument.Parse(line);
            JsonElement response = document.RootElement;
            if (Text(response, "op") == "result" && response.TryGetProperty("id", out JsonElement receivedId)
                && receivedId.ValueKind == JsonValueKind.Number && receivedId.GetInt64() == id)
            {
                if (!response.GetProperty("ok").GetBoolean())
                    throw new InvalidOperationException("Z# callback failed: " + Text(response, "error"));
                return Decode(response.GetProperty("value"), 0);
            }
            if (Text(response, "op") != "result")
                Send(response.TryGetProperty("id", out var nestedId) ? nestedId.Clone() : null, false,
                    "Reentrant call to the active C# worker is not supported");
            else throw new InvalidOperationException("Unexpected ZVM bridge callback response");
        }
        throw new InvalidOperationException("ZVM bridge connection closed during callback");
    }

    private sealed class ModuleContext(string filename) : AssemblyLoadContext(isCollectible: false)
    {
        private readonly string directory = Path.GetDirectoryName(filename)!;
        protected override Assembly? Load(AssemblyName name)
        {
            if (name.Name == typeof(ZSharpExportAttribute).Assembly.GetName().Name)
                return typeof(ZSharpExportAttribute).Assembly;
            string candidate = Path.Combine(directory, name.Name + ".dll");
            return File.Exists(candidate) ? LoadFromAssemblyPath(candidate) : null;
        }
    }

    private Assembly Module(string filename)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(filename);
        string path = Path.GetFullPath(filename);
        if (modules.TryGetValue(path, out Assembly? assembly)) return assembly;
        assembly = new ModuleContext(path).LoadFromAssemblyPath(path);
        modules.Add(path, assembly);
        return assembly;
    }

    private static bool Exported(MemberInfo member)
    {
        if (member.DeclaringType is not Type type || !type.IsVisible) return false;
        bool visible = member switch {
            MethodBase method => method.IsPublic,
            FieldInfo field => field.IsPublic,
            PropertyInfo property => property.GetAccessors().Any(accessor => accessor.IsPublic),
            _ => false
        };
        return visible && (member.IsDefined(typeof(ZSharpExportAttribute), false)
            || type.IsDefined(typeof(ZSharpExportAttribute), false));
    }

    private static Type TargetType(Assembly module, string target)
    {
        Type[] types = module.GetExportedTypes();
        if (target.Length != 0)
        {
            Type? qualified = types.FirstOrDefault(type => type.FullName == target);
            if (qualified is not null) return qualified;
            Type[] candidates = types.Where(type => type.Name == target).ToArray();
            return candidates.Length == 1 ? candidates[0]
                : throw new ArgumentException(candidates.Length == 0
                    ? $"Public C# class '{target}' is not part of the imported assembly"
                    : $"Multiple exported C# classes are named '{target}'; use a qualified target");
        }
        string name = Path.GetFileNameWithoutExtension(module.Location);
        if (name.EndsWith(".zcs", StringComparison.OrdinalIgnoreCase)) name = name[..^4];
        Type[] named = types.Where(type => type.Name == name).ToArray();
        if (named.Length == 1) return named[0];
        if (named.Length > 1) throw new ArgumentException("C# module requires a qualified class target");
        Type[] exported = types.Where(type => type.IsDefined(typeof(ZSharpExportAttribute), false)).ToArray();
        return exported.Length == 1 ? exported[0]
            : throw new ArgumentException("C# target requires an explicit exported class name");
    }

    private object? Dispatch(JsonElement request)
    {
        string operation = Text(request, "op");
        string reference = Text(request, "ref");
        object? instance = null;
        if (reference.Length != 0 && !objects.TryGetValue(reference, out instance))
            throw new ArgumentException("C# object reference is invalid or has been released: " + reference);
        if (operation == "release")
        {
            if (instance is null) throw new ArgumentException("C# release requires a live object reference");
            objects.Remove(reference);
            identities.Remove(instance);
            return null;
        }
        string target = Text(request, "target"), member = Text(request, "member");
        Type type;
        if (instance is not null) type = instance.GetType();
        else
        {
            if (operation != "new" && member.Length == 0)
            {
                int separator = target.LastIndexOf('.');
                member = separator < 0 ? target : target[(separator + 1)..];
                target = separator < 0 ? "" : target[..separator];
            }
            type = TargetType(Module(Text(request, "module")), target);
        }
        object?[] arguments = request.TryGetProperty("args", out JsonElement raw)
            ? raw.EnumerateArray().Select(argument => Decode(argument, 0)).ToArray() : [];
        if (operation == "new")
        {
            var match = Match(type.GetConstructors().Where(Exported), arguments, type.FullName + " constructor");
            return ((ConstructorInfo)match.Method).Invoke(match.Arguments);
        }
        const BindingFlags flags = BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static;
        if (operation is "get" or "set")
        {
            if (operation == "get" && arguments.Length != 0 || operation == "set" && arguments.Length != 1)
                throw new ArgumentException("C# get takes no arguments; set takes exactly one value");
            PropertyInfo? property = type.GetProperty(member, flags);
            if (property is not null && Exported(property) && property.GetIndexParameters().Length == 0)
            {
                MethodInfo? accessor = operation == "get" ? property.GetMethod : property.SetMethod;
                if (accessor is null || !accessor.IsPublic || instance is null && !accessor.IsStatic)
                    throw new ArgumentException("C# property does not have an exported public " + operation + " accessor");
                if (operation == "set" && accessor.ReturnParameter.GetRequiredCustomModifiers()
                    .Contains(typeof(System.Runtime.CompilerServices.IsExternalInit)))
                    throw new ArgumentException("C# init-only property cannot be changed after construction: " + member);
                if (operation == "get") return property.GetValue(instance);
                property.SetValue(instance, ConvertValue(arguments[0], property.PropertyType).Value);
                return null;
            }
            FieldInfo? field = type.GetField(member, flags);
            if (field is null || !Exported(field) || instance is null && !field.IsStatic)
                throw new ArgumentException("C# field/property is not exported: " + member);
            if (operation == "get") return field.GetValue(instance);
            if (field.IsInitOnly || field.IsLiteral) throw new ArgumentException("C# field is read-only: " + member);
            field.SetValue(instance, ConvertValue(arguments[0], field.FieldType).Value);
            return null;
        }
        if (operation is not "call" and not "invoke") throw new ArgumentException("Unknown C# bridge operation: " + operation);
        var methods = type.GetMethods(flags).Where(method => method.Name == member && !method.IsSpecialName
            && !method.ContainsGenericParameters && Exported(method) && (instance is not null || method.IsStatic));
        var selected = Match(methods, arguments, type.FullName + "." + member);
        return ((MethodInfo)selected.Method).Invoke(instance, selected.Arguments);
    }

    private static (MethodBase Method, object?[] Arguments) Match(IEnumerable<MethodBase> methods, object?[] arguments, string target)
    {
        MethodBase? best = null;
        object?[]? bestArguments = null;
        int bestScore = int.MaxValue;
        bool ambiguous = false;
        foreach (MethodBase method in methods)
        {
            ParameterInfo[] parameters = method.GetParameters();
            if (parameters.Length != arguments.Length || parameters.Any(parameter => parameter.ParameterType.IsByRef
                || parameter.IsDefined(typeof(ParamArrayAttribute)))) continue;
            try
            {
                object?[] converted = new object?[arguments.Length];
                int score = 0;
                for (int i = 0; i < arguments.Length; i++)
                {
                    var conversion = ConvertValue(arguments[i], parameters[i].ParameterType);
                    converted[i] = conversion.Value;
                    score += conversion.Score;
                }
                if (score < bestScore) { best = method; bestArguments = converted; bestScore = score; ambiguous = false; }
                else if (score == bestScore) ambiguous = true;
            }
            catch (Exception invalid) when (invalid is ArgumentException or OverflowException or FormatException) { }
        }
        if (best is null) throw new ArgumentException("No exported C# overload accepts these arguments: " + target);
        if (ambiguous) throw new ArgumentException("Ambiguous exported C# overload: " + target);
        return (best, bestArguments!);
    }

    private static (object? Value, int Score) ConvertValue(object? value, Type type)
    {
        Type underlying = Nullable.GetUnderlyingType(type) ?? type;
        if (value is null)
        {
            if (type.IsValueType && Nullable.GetUnderlyingType(type) is null)
                throw new ArgumentException("Null cannot be passed to a non-nullable C# parameter");
            return (null, 1);
        }
        if (underlying == typeof(object)) return (value, 20);
        if (value is ExactNumber number)
        {
            if (underlying == typeof(ExactNumber)) return (number, 0);
            if (underlying == typeof(BigInteger)) return (number.Integer(), 1);
            if (underlying == typeof(int)) return (checked((int)number.Integer()), 1);
            if (underlying == typeof(long)) return (checked((long)number.Integer()), 2);
            if (underlying == typeof(uint)) return (checked((uint)number.Integer()), 2);
            if (underlying == typeof(ulong)) return (checked((ulong)number.Integer()), 3);
            if (underlying == typeof(short)) return (checked((short)number.Integer()), 3);
            if (underlying == typeof(ushort)) return (checked((ushort)number.Integer()), 4);
            if (underlying == typeof(byte)) return (checked((byte)number.Integer()), 4);
            if (underlying == typeof(sbyte)) return (checked((sbyte)number.Integer()), 5);
            if (underlying == typeof(decimal))
            {
                decimal converted = decimal.Parse(number.Text, NumberStyles.Float, CultureInfo.InvariantCulture);
                if (number.Canonical() != new ExactNumber(converted.ToString(CultureInfo.InvariantCulture)).Canonical())
                    throw new ArgumentException("Number cannot be represented exactly by a C# decimal parameter");
                return (converted, 4);
            }
            if (underlying == typeof(double))
            {
                double result = double.Parse(number.Text, CultureInfo.InvariantCulture);
                if (double.IsFinite(result)) return (result, 5);
            }
            if (underlying == typeof(float))
            {
                float result = float.Parse(number.Text, CultureInfo.InvariantCulture);
                if (float.IsFinite(result)) return (result, 6);
            }
        }
        if (underlying == typeof(char) && value is string text && text.Length == 1) return (text[0], 1);
        if (underlying.IsInstanceOfType(value)) return (value, 0);
        if (underlying.IsArray && value is object?[] array)
        {
            Type elementType = underlying.GetElementType()!;
            Array result = Array.CreateInstance(elementType, array.Length);
            int score = 1;
            for (int i = 0; i < array.Length; i++)
            {
                var element = ConvertValue(array[i], elementType);
                result.SetValue(element.Value, i);
                score += element.Score;
            }
            return (result, score);
        }
        throw new ArgumentException("Bridge value cannot be converted to " + type.FullName);
    }

    private object? Decode(JsonElement value, int depth)
    {
        if (depth > 128) throw new ArgumentException("Bridge value nesting limit exceeded");
        switch (value.ValueKind)
        {
            case JsonValueKind.Null: return null;
            case JsonValueKind.True: return true;
            case JsonValueKind.False: return false;
            case JsonValueKind.String: return value.GetString();
            case JsonValueKind.Number: return new ExactNumber(value.GetRawText());
            case JsonValueKind.Array: return value.EnumerateArray().Select(element => Decode(element, depth + 1)).ToArray();
            case JsonValueKind.Object:
                if (value.TryGetProperty("$number", out JsonElement number))
                {
                    var exact = new ExactNumber(number.GetString() ?? "");
                    exact.Validate();
                    return exact;
                }
                if (value.TryGetProperty("$ref", out JsonElement reference))
                    return objects.TryGetValue(reference.GetString() ?? "", out object? instance) ? instance
                        : throw new ArgumentException("C# object reference is invalid or has been released");
                if (value.TryGetProperty("$zref", out JsonElement zreference))
                    return new ZSharpObject(zreference.GetString()!, Text(value, "$type"));
                if (value.TryGetProperty("$record", out JsonElement record))
                    return record.EnumerateObject().ToDictionary(entry => entry.Name, entry => Decode(entry.Value, depth + 1));
                throw new ArgumentException("Unknown C# bridge value tag");
            default: throw new ArgumentException("Invalid C# bridge value");
        }
    }

    private object? Encode(object? value, int depth)
    {
        if (depth > 128) throw new ArgumentException("Bridge value nesting limit exceeded");
        if (value is null or string or bool) return value;
        if (value is char character) return character.ToString();
        if (value is ZSharpObject proxy) return new Dictionary<string, object?> { ["$zref"] = proxy.Reference, ["$type"] = proxy.Type };
        if (value is ExactNumber exact)
        {
            exact.Validate();
            return new Dictionary<string, object?> { ["$number"] = exact.Text };
        }
        if (value is sbyte or byte or short or ushort or int or uint or long or ulong or BigInteger or decimal or double or float)
        {
            if (value is double number && !double.IsFinite(number) || value is float single && !float.IsFinite(single))
                throw new ArgumentException("Non-finite C# numbers cannot cross the bridge");
            return new Dictionary<string, object?> { ["$number"] = ((IFormattable)value).ToString(null, CultureInfo.InvariantCulture) };
        }
        if (value is IDictionary dictionary)
        {
            var record = new Dictionary<string, object?>();
            foreach (DictionaryEntry entry in dictionary)
            {
                if (entry.Key is not string key) throw new ArgumentException("C# records require text keys");
                record.Add(key, Encode(entry.Value, depth + 1));
            }
            return new Dictionary<string, object?> { ["$record"] = record };
        }
        if (value is Array or IList)
            return ((IEnumerable)value).Cast<object?>().Select(element => Encode(element, depth + 1)).ToArray();
        if (!identities.TryGetValue(value, out string? reference))
        {
            reference = (nextObject++).ToString(CultureInfo.InvariantCulture);
            identities.Add(value, reference);
            objects.Add(reference, value);
        }
        return new Dictionary<string, object?> { ["$ref"] = reference, ["$type"] = value.GetType().FullName };
    }
}
