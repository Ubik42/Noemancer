using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text.Json;
using System.Text.Json.Serialization;
using Noemancer;

namespace Noemancer.ManagedHost;

public static class EntryPoint
{
    private static readonly Lock Gate = new();
    private static readonly Dictionary<string, SessionState> Sessions = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe int Invoke(byte* requestUtf8, int requestLength, byte* responseUtf8, int responseCapacity)
    {
        byte[] response;
        try
        {
            using var request = JsonDocument.Parse(new ReadOnlySpan<byte>(requestUtf8, requestLength).ToArray());
            response = Dispatch(request.RootElement);
        }
        catch (Exception exception)
        {
            response = ErrorResponse(exception, 0);
        }
        if (response.Length > responseCapacity) return -response.Length;
        response.CopyTo(new Span<byte>(responseUtf8, responseCapacity));
        return response.Length;
    }

    private static byte[] Dispatch(JsonElement root)
    {
        var operation = RequiredString(root, "operation");
        var sessionId = RequiredString(root, "sessionId");
        lock (Gate)
        {
            if (operation == "session.release") return ReleaseSession(sessionId);
            if (operation == "instance.release") return ReleaseInstance(root, sessionId);
            if (operation == "state.capture") return CaptureInstanceState(root, sessionId);
            if (operation == "state.restore") return RestoreInstanceState(root, sessionId);
            if (operation == "project.inspect")
            {
                if (!Sessions.TryGetValue(sessionId, out var projectSession)) Sessions.Add(sessionId, projectSession = new());
                try { return InspectProject(root, sessionId, projectSession); }
                catch (Exception exception) { return ErrorResponse(exception, projectSession.LoadGeneration, sessionId); }
            }
            if (operation != "lifecycle.invoke") throw new InvalidOperationException($"Unsupported host operation '{operation}'.");
            if (!Sessions.TryGetValue(sessionId, out var session)) Sessions.Add(sessionId, session = new());
            try { return InvokeLifecycle(root, sessionId, session); }
            catch (Exception exception) { return ErrorResponse(exception, session.LoadGeneration, sessionId); }
        }
    }

    private static byte[] InspectProject(JsonElement root, string sessionId, SessionState session)
    {
        var assemblyPath = RequiredString(root, "projectAssembly");
        var fingerprint = RequiredString(root, "projectFingerprint");
        EnsureProjectLoaded(session, assemblyPath, fingerprint);
        var types = session.ProjectAssembly!.GetTypes()
            .Where(type => typeof(ScriptBehaviour).IsAssignableFrom(type) && !type.IsAbstract && type.IsClass)
            .OrderBy(type => type.FullName, StringComparer.Ordinal)
            .Select(type => new
            {
                fullName = type.FullName!,
                displayName = type.Name,
                callbacks = LifecycleCallbacks
                    .Where(callback => type.GetMethod(callback, BindingFlags.Instance | BindingFlags.Public)?.DeclaringType !=
                        typeof(ScriptBehaviour))
                    .ToArray()
            })
            .ToArray();
        return JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = "noemancer.managed-type-catalog/0.1", success = true, sessionId,
            projectAssembly = assemblyPath, projectFingerprint = fingerprint,
            loadGeneration = session.LoadGeneration, typeCount = types.Length, types
        });
    }

    private static readonly string[] LifecycleCallbacks =
    [
        "OnCreate", "OnFixedUpdate", "OnUpdate", "OnContactEnter", "OnContactStay", "OnContactExit",
        "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit", "OnDestroy"
    ];

    private static byte[] InvokeLifecycle(JsonElement root, string sessionId, SessionState session)
    {
        CollectRetiredContexts(session);
        var instanceId = RequiredString(root, "instanceId");
        var entityId = RequiredString(root, "entityId");
        var assemblyAsset = RequiredString(root, "assemblyAsset");
        var typeName = RequiredString(root, "typeName");
        var callback = RequiredString(root, "callback");
        var arguments = root.GetProperty("arguments").Clone();
        var worldContext = root.GetProperty("context");
        var assemblyPath = OptionalString(root, "projectAssembly");
        var fingerprint = OptionalString(root, "projectFingerprint");
        var projectCodeExecuted = false;
        IReadOnlyDictionary<string, object?> state = new Dictionary<string, object?>();
        IReadOnlyList<ScriptCommand> commands = [];
        MigrationReport? migration = null;
        if (!string.IsNullOrWhiteSpace(assemblyPath))
        {
            EnsureProjectLoaded(session, assemblyPath, fingerprint);
            if (!session.Instances.TryGetValue(instanceId, out var instance))
            {
                var type = session.ProjectAssembly!.GetType(typeName, throwOnError: true, ignoreCase: false)!;
                if (!typeof(ScriptBehaviour).IsAssignableFrom(type) || type.IsAbstract)
                    throw new InvalidOperationException($"'{typeName}' must be a concrete ScriptBehaviour.");
                instance = (ScriptBehaviour?)Activator.CreateInstance(type)
                    ?? throw new InvalidOperationException($"Unable to construct '{typeName}'.");
                if (session.PendingStates.TryGetValue(instanceId, out var pendingState))
                {
                    migration = RestoreScriptState(instance, pendingState);
                    session.PendingStates.Remove(instanceId);
                }
                session.Instances.Add(instanceId, instance);
            }
            var context = new ScriptContext(new EntityId(entityId), arguments.GetRawText(), new ScriptWorldView(
                worldContext.GetProperty("revision").GetUInt64(), worldContext.GetProperty("self").GetRawText(),
                worldContext.GetProperty("input").GetRawText(), worldContext.GetProperty("entities").GetRawText(),
                worldContext.GetProperty("events").GetRawText()));
            switch (callback)
            {
                case "OnCreate": instance.OnCreate(in context); break;
                case "OnFixedUpdate": instance.OnFixedUpdate(in context); break;
                case "OnUpdate": instance.OnUpdate(in context); break;
                case "OnContactEnter": instance.OnContactEnter(in context); break;
                case "OnContactStay": instance.OnContactStay(in context); break;
                case "OnContactExit": instance.OnContactExit(in context); break;
                case "OnTriggerEnter": instance.OnTriggerEnter(in context); break;
                case "OnTriggerStay": instance.OnTriggerStay(in context); break;
                case "OnTriggerExit": instance.OnTriggerExit(in context); break;
                case "OnDestroy": instance.OnDestroy(in context); session.Instances.Remove(instanceId); break;
                default: throw new InvalidOperationException($"Unsupported lifecycle callback '{callback}'.");
            }
            projectCodeExecuted = true;
            state = ReadPublicState(instance);
            commands = context.Commands.Commands;
        }
        return JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = "noemancer.managed-callback-result/0.3", success = true,
            runtime = RuntimeInformation.FrameworkDescription, sessionId, instanceId, entityId, assemblyAsset,
            typeName, callback, arguments, projectCodeExecuted, projectAssembly = assemblyPath,
            loadGeneration = session.LoadGeneration, state, commands,
            migration,
            retiredLoadContexts = session.RetiredContexts.Select(value => new { value.Generation, value.Collected })
        });
    }

    private static byte[] ReleaseSession(string sessionId)
    {
        var released = Sessions.Remove(sessionId, out var session);
        if (session is not null)
        {
            session.Instances.Clear();
            session.PendingStates.Clear();
            session.RetiredContexts.Clear();
            session.ProjectAssembly = null;
            session.ProjectContext?.Unload();
            session.ProjectContext = null;
        }
        return JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = "noemancer.managed-session-release/0.1", success = true, sessionId, released,
            activeSessionCount = Sessions.Count
        });
    }

    private static byte[] ReleaseInstance(JsonElement root, string sessionId)
    {
        var instanceId = RequiredString(root, "instanceId");
        var released = Sessions.TryGetValue(sessionId, out var session) &&
            (session.Instances.Remove(instanceId) | session.PendingStates.Remove(instanceId));
        return JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = "noemancer.managed-instance-release/0.1", success = true, sessionId, instanceId, released
        });
    }

    private static byte[] CaptureInstanceState(JsonElement root, string sessionId)
    {
        var instanceId = RequiredString(root, "instanceId");
        if (!Sessions.TryGetValue(sessionId, out var session) || !session.Instances.TryGetValue(instanceId, out var instance))
            return JsonSerializer.SerializeToUtf8Bytes(new { schemaVersion = "noemancer.managed-state/0.1", success = false,
                code = "scripting.instance-not-created", sessionId, instanceId, state = new Dictionary<string, JsonElement>() });
        var state = CaptureScriptState(instance).ToDictionary(value => value.Key,
            value => JsonDocument.Parse(value.Value).RootElement.Clone(), StringComparer.Ordinal);
        return JsonSerializer.SerializeToUtf8Bytes(new { schemaVersion = "noemancer.managed-state/0.1", success = true,
            code = "ok", sessionId, instanceId, state });
    }

    private static byte[] RestoreInstanceState(JsonElement root, string sessionId)
    {
        var instanceId = RequiredString(root, "instanceId");
        if (!root.TryGetProperty("state", out var stateElement) || stateElement.ValueKind != JsonValueKind.Object)
            throw new InvalidOperationException("Managed state must be an object.");
        if (!Sessions.TryGetValue(sessionId, out var session)) Sessions.Add(sessionId, session = new());
        var state = stateElement.EnumerateObject().ToDictionary(value => value.Name, value => value.Value.GetRawText(), StringComparer.Ordinal);
        MigrationReport? migration = null;
        if (session.Instances.TryGetValue(instanceId, out var instance)) migration = RestoreScriptState(instance, state);
        else session.PendingStates[instanceId] = state;
        return JsonSerializer.SerializeToUtf8Bytes(new { schemaVersion = "noemancer.managed-state-restore/0.1", success = true,
            code = "ok", sessionId, instanceId, pending = migration is null, migration,
            publicState = instance is null ? null : ReadPublicState(instance) });
    }

    private static void EnsureProjectLoaded(SessionState session, string assemblyPath, string fingerprint)
    {
        var canonicalPath = Path.GetFullPath(assemblyPath);
        if (session.ProjectAssembly is not null && string.Equals(session.ProjectFingerprint, fingerprint, StringComparison.Ordinal)) return;
        session.PendingStates.Clear();
        foreach (var (instanceId, instance) in session.Instances)
            session.PendingStates[instanceId] = CaptureScriptState(instance);
        session.Instances.Clear();
        RetiredContext? retired = null;
        if (session.ProjectContext is not null)
            retired = DetachAndUnloadProjectContext(session);
        session.ProjectContext = new ProjectLoadContext(canonicalPath);
        session.ProjectAssembly = session.ProjectContext.LoadMainAssembly(canonicalPath);
        session.ProjectFingerprint = fingerprint;
        session.LoadGeneration++;
        if (retired is not null) session.RetiredContexts.Add(retired);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static RetiredContext DetachAndUnloadProjectContext(SessionState session)
    {
        var context = session.ProjectContext!;
        var retired = new RetiredContext(session.LoadGeneration, new WeakReference(context));
        session.ProjectAssembly = null;
        session.ProjectContext = null;
        context.Unload();
        return retired;
    }

    private static void CollectRetiredContexts(SessionState session)
    {
        if (!session.RetiredContexts.Any(value => !value.Collected)) return;
        GC.Collect(GC.MaxGeneration, GCCollectionMode.Forced, blocking: true, compacting: false);
        foreach (var retired in session.RetiredContexts)
            if (!retired.Reference.IsAlive) retired.Collected = true;
    }

    private static IReadOnlyDictionary<string, object?> ReadPublicState(ScriptBehaviour instance)
    {
        var state = new SortedDictionary<string, object?>(StringComparer.Ordinal);
        foreach (var property in instance.GetType().GetProperties(BindingFlags.Instance | BindingFlags.Public))
        {
            if (!property.CanRead || property.GetIndexParameters().Length != 0) continue;
            var value = property.GetValue(instance);
            if (value is null || value is string || value.GetType().IsPrimitive || value.GetType().IsEnum || value is decimal)
                state[property.Name] = value;
        }
        return state;
    }

    private static Dictionary<string, string> CaptureScriptState(ScriptBehaviour instance)
    {
        var state = new Dictionary<string, string>(StringComparer.Ordinal);
        var type = instance.GetType();
        foreach (var property in type.GetProperties(BindingFlags.Instance | BindingFlags.Public))
            if (property.GetCustomAttribute<ScriptStateAttribute>() is not null && property.CanRead &&
                property.SetMethod?.IsPublic == true && property.GetIndexParameters().Length == 0)
                state[$"property:{property.Name}"] = JsonSerializer.Serialize(property.GetValue(instance), property.PropertyType);
        foreach (var field in type.GetFields(BindingFlags.Instance | BindingFlags.Public))
            if (field.GetCustomAttribute<ScriptStateAttribute>() is not null && !field.IsInitOnly)
                state[$"field:{field.Name}"] = JsonSerializer.Serialize(field.GetValue(instance), field.FieldType);
        return state;
    }

    private static MigrationReport RestoreScriptState(ScriptBehaviour instance, IReadOnlyDictionary<string, string> state)
    {
        var type = instance.GetType();
        var members = new List<MigrationMember>();
        foreach (var (key, json) in state)
        {
            try
            {
                if (key.StartsWith("property:", StringComparison.Ordinal))
                {
                    var property = type.GetProperty(key[9..], BindingFlags.Instance | BindingFlags.Public);
                    if (property is null) { members.Add(new(key, "missing-member", "Public property no longer exists.")); continue; }
                    if (property.GetCustomAttribute<ScriptStateAttribute>() is null) { members.Add(new(key, "not-persistent", "Property no longer has ScriptState.")); continue; }
                    if (property.SetMethod?.IsPublic != true) { members.Add(new(key, "not-writable", "Property has no public setter.")); continue; }
                    property.SetValue(instance, JsonSerializer.Deserialize(json, property.PropertyType));
                    members.Add(new(key, "restored", ""));
                }
                else if (key.StartsWith("field:", StringComparison.Ordinal))
                {
                    var field = type.GetField(key[6..], BindingFlags.Instance | BindingFlags.Public);
                    if (field is null) { members.Add(new(key, "missing-member", "Public field no longer exists.")); continue; }
                    if (field.GetCustomAttribute<ScriptStateAttribute>() is null) { members.Add(new(key, "not-persistent", "Field no longer has ScriptState.")); continue; }
                    if (field.IsInitOnly) { members.Add(new(key, "not-writable", "Field is readonly.")); continue; }
                    field.SetValue(instance, JsonSerializer.Deserialize(json, field.FieldType));
                    members.Add(new(key, "restored", ""));
                }
                else members.Add(new(key, "unsupported-key", "State key has an unknown member prefix."));
            }
            catch (Exception exception) { members.Add(new(key, "deserialize-failed", $"{exception.GetType().Name}: {exception.Message}")); }
        }
        return new(type.FullName ?? type.Name, members.Count(value => value.Status == "restored"),
            members.Count(value => value.Status != "restored"), members);
    }

    private sealed record MigrationMember(
        [property: JsonPropertyName("key")] string Key,
        [property: JsonPropertyName("status")] string Status,
        [property: JsonPropertyName("detail")] string Detail);
    private sealed record MigrationReport(
        [property: JsonPropertyName("typeName")] string TypeName,
        [property: JsonPropertyName("restoredCount")] int RestoredCount,
        [property: JsonPropertyName("failedCount")] int FailedCount,
        [property: JsonPropertyName("members")] IReadOnlyList<MigrationMember> Members);

    private static byte[] ErrorResponse(Exception exception, int generation, string sessionId = "") =>
        JsonSerializer.SerializeToUtf8Bytes(new
        {
            schemaVersion = "noemancer.managed-callback-result/0.3", success = false, sessionId,
            error = exception.GetType().Name, message = exception.Message,
            stackTrace = exception.StackTrace is { Length: > 8192 } trace ? trace[..8192] : exception.StackTrace,
            loadGeneration = generation
        });

    private static string RequiredString(JsonElement root, string name)
    {
        var value = root.GetProperty(name).GetString();
        return string.IsNullOrWhiteSpace(value) ? throw new JsonException($"'{name}' must be a non-empty string.") : value;
    }

    private static string OptionalString(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() ?? "" : "";

    private sealed class SessionState
    {
        public Dictionary<string, ScriptBehaviour> Instances { get; } = [];
        public Dictionary<string, Dictionary<string, string>> PendingStates { get; } = [];
        public ProjectLoadContext? ProjectContext { get; set; }
        public Assembly? ProjectAssembly { get; set; }
        public string ProjectFingerprint { get; set; } = "";
        public int LoadGeneration { get; set; }
        public List<RetiredContext> RetiredContexts { get; } = [];
    }

    private sealed class RetiredContext(int generation, WeakReference reference)
    {
        public int Generation { get; } = generation;
        public WeakReference Reference { get; } = reference;
        public bool Collected { get; set; }
    }

    private sealed class ProjectLoadContext(string mainAssemblyPath) : AssemblyLoadContext(isCollectible: true)
    {
        private readonly AssemblyDependencyResolver resolver = new(mainAssemblyPath);
        public Assembly LoadMainAssembly(string assemblyPath) => LoadManagedWithoutLock(assemblyPath);

        private Assembly LoadManagedWithoutLock(string assemblyPath)
        {
            using var assembly = new MemoryStream(File.ReadAllBytes(assemblyPath), writable: false);
            var symbolsPath = Path.ChangeExtension(assemblyPath, ".pdb");
            if (!File.Exists(symbolsPath)) return LoadFromStream(assembly);
            using var symbols = new MemoryStream(File.ReadAllBytes(symbolsPath), writable: false);
            return LoadFromStream(assembly, symbols);
        }

        protected override Assembly? Load(AssemblyName assemblyName)
        {
            if (string.Equals(assemblyName.Name, typeof(ScriptBehaviour).Assembly.GetName().Name, StringComparison.Ordinal))
                return typeof(ScriptBehaviour).Assembly;
            var shared = Default.Assemblies.FirstOrDefault(value => string.Equals(value.GetName().Name, assemblyName.Name, StringComparison.Ordinal));
            if (shared is not null) return shared;
            var path = resolver.ResolveAssemblyToPath(assemblyName);
            return path is null ? null : LoadManagedWithoutLock(path);
        }
        protected override nint LoadUnmanagedDll(string unmanagedDllName)
        {
            var path = resolver.ResolveUnmanagedDllToPath(unmanagedDllName);
            return path is null ? 0 : LoadUnmanagedDllFromPath(path);
        }
    }
}
