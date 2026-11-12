using System.Text.Json;
using FomodInstaller.Interface;
using FomodInstaller.Interface.ui;
using FomodInstaller.ModInstaller;

namespace VortexFomodHost;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        var index = Array.IndexOf(args, "--request");
        if (index < 0 || index + 1 >= args.Length)
        {
            Console.Error.WriteLine("usage: VortexFomodHost --request <json-file>");
            return 2;
        }

        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(args[index + 1]));
            var root = document.RootElement;
            if (!root.TryGetProperty("allowCSharpScripts", out var allow) || !allow.GetBoolean())
            {
                WriteError("C# FOMOD scripts require explicit consent");
                return 0;
            }

            var files = ReadStrings(root, "files");
            var stopPatterns = ReadStrings(root, "stopPatterns");
            var pluginPath = ReadString(root, "pluginPath") ?? string.Empty;
            var scriptPath = ReadString(root, "scriptPath") ?? string.Empty;
            var preset = root.TryGetProperty("preset", out var presetValue) && presetValue.ValueKind != JsonValueKind.Null
                ? Convert(presetValue)
                : null;

            var source = new HostSource();
            var installer = new Installer();
            var result = await installer.Install(
                files,
                stopPatterns,
                pluginPath,
                scriptPath,
                preset,
                false,
                false,
                _ => { },
                new DynamicCoreDelegates(source));

            var instructions = NormalizeInstructions(result, scriptPath);
            Console.WriteLine(JsonSerializer.Serialize(new { result = new { instructions } }));
            return 0;
        }
        catch (Exception error)
        {
            WriteError(error.Message);
            return 0;
        }
    }

    private static void WriteError(string message)
    {
        Console.WriteLine(JsonSerializer.Serialize(new { error = message }));
    }

    private static List<string> ReadStrings(JsonElement root, string name)
    {
        if (!root.TryGetProperty(name, out var value) || value.ValueKind != JsonValueKind.Array)
        {
            return new List<string>();
        }
        return value.EnumerateArray()
            .Where(item => item.ValueKind == JsonValueKind.String)
            .Select(item => item.GetString() ?? string.Empty)
            .ToList();
    }

    private static string? ReadString(JsonElement root, string name)
    {
        return root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;
    }

    private static object? Convert(JsonElement value)
    {
        return value.ValueKind switch
        {
            JsonValueKind.Object => value.EnumerateObject()
                .ToDictionary(property => property.Name, property => Convert(property.Value)),
            JsonValueKind.Array => value.EnumerateArray().Select(Convert).ToList(),
            JsonValueKind.String => value.GetString(),
            JsonValueKind.Number => value.TryGetInt64(out var integer) ? integer : value.GetDouble(),
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            _ => null,
        };
    }

    private static List<object> NormalizeInstructions(
        Dictionary<string, object> result,
        string scriptPath)
    {
        var instructions = new List<object>();
        var json = JsonSerializer.Serialize(result);
        using var document = JsonDocument.Parse(json);
        if (!document.RootElement.TryGetProperty("instructions", out var list) ||
            list.ValueKind != JsonValueKind.Array)
        {
            return instructions;
        }

        foreach (var instruction in list.EnumerateArray())
        {
            var type = ReadString(instruction, "type")?.ToLowerInvariant() ?? string.Empty;
            var destination = ReadString(instruction, "destination") ?? string.Empty;
            if (destination.Length == 0)
            {
                continue;
            }
            if (type is "copy" or "copyfile")
            {
                instructions.Add(new
                {
                    type = "copy",
                    source = ReadString(instruction, "source") ?? destination,
                    destination,
                });
            }
            else if (type is "generatefile" or "generate")
            {
                var data = ReadString(instruction, "data");
                if (data is null)
                {
                    continue;
                }
                var target = SafeDestination(scriptPath, destination);
                Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                File.WriteAllBytes(target, System.Convert.FromBase64String(data));
                instructions.Add(new { type = "copy", source = destination, destination });
            }
        }

        return instructions;
    }

    private static string SafeDestination(string root, string relative)
    {
        var rootPath = Path.GetFullPath(root);
        var target = Path.GetFullPath(Path.Combine(rootPath, relative));
        if (!target.StartsWith(rootPath, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"instruction escapes the extraction root: {relative}");
        }
        return target;
    }
}
