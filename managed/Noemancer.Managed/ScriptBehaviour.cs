using System.Text.Json;
using System.Text.Json.Serialization;

namespace Noemancer;

public readonly record struct EntityId(string Value)
{
    public override string ToString() => Value;
}

[JsonConverter(typeof(AssetIdJsonConverter))]
public readonly record struct AssetId(string Value)
{
    public override string ToString() => Value;
}

public readonly record struct ComponentId(string Value)
{
    public override string ToString() => Value;
}

public readonly record struct PropertyId<T>(string Value)
{
    public override string ToString() => Value;
}

[JsonConverter(typeof(Float3JsonConverter))]
public readonly record struct Float3(float X, float Y, float Z);
[JsonConverter(typeof(Color3JsonConverter))]
public readonly record struct Color3(float R, float G, float B);
public readonly record struct EntityView(
    EntityId Id,
    string Name,
    Float3? Position,
    IReadOnlyList<ComponentId> Components,
    IReadOnlyList<string> Tags)
{
    public bool HasComponent(ComponentId component) => Components.Contains(component);

    public bool HasComponent(string componentId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(componentId);
        return Components.Any(component => component.Value == componentId);
    }

    public bool HasTag(string tag)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(tag);
        return Tags.Contains(tag, StringComparer.Ordinal);
    }
}
public readonly record struct CharacterMotor2DView(
    bool Grounded, float MoveInput, string Decision, string Reason,
    string GroundEntityId, string WallEntityId, ulong JumpCount, ulong LandingCount);
public readonly record struct SpritePlaybackView(
    string AssetId, string ClipId, int FrameIndex, bool Playing, ulong CompletedLoops);

public sealed class AssetIdJsonConverter : JsonConverter<AssetId>
{
    public override AssetId Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options) =>
        new(reader.GetString() ?? throw new JsonException("Asset ID must be a string."));
    public override void Write(Utf8JsonWriter writer, AssetId value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value.Value);
}

public sealed class Float3JsonConverter : JsonConverter<Float3>
{
    public override Float3 Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
    {
        using var document = JsonDocument.ParseValue(ref reader);
        var value = document.RootElement;
        if (value.ValueKind != JsonValueKind.Object) throw new JsonException("Float3 must be an x/y/z object.");
        return new(value.GetProperty("x").GetSingle(), value.GetProperty("y").GetSingle(), value.GetProperty("z").GetSingle());
    }
    public override void Write(Utf8JsonWriter writer, Float3 value, JsonSerializerOptions options)
    {
        writer.WriteStartObject(); writer.WriteNumber("x", value.X); writer.WriteNumber("y", value.Y); writer.WriteNumber("z", value.Z); writer.WriteEndObject();
    }
}

public sealed class Color3JsonConverter : JsonConverter<Color3>
{
    public override Color3 Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
    {
        using var document = JsonDocument.ParseValue(ref reader);
        var value = document.RootElement;
        if (value.ValueKind != JsonValueKind.Object) throw new JsonException("Color3 must be an x/y/z object.");
        return new(value.GetProperty("x").GetSingle(), value.GetProperty("y").GetSingle(), value.GetProperty("z").GetSingle());
    }
    public override void Write(Utf8JsonWriter writer, Color3 value, JsonSerializerOptions options)
    {
        writer.WriteStartObject(); writer.WriteNumber("x", value.R); writer.WriteNumber("y", value.G); writer.WriteNumber("z", value.B); writer.WriteEndObject();
    }
}

[AttributeUsage(AttributeTargets.Property | AttributeTargets.Field)]
public sealed class ScriptStateAttribute : Attribute;

public sealed class ScriptWorldView(ulong revision, string selfJson, string inputJson, string entitiesJson, string eventsJson)
{
    private IReadOnlyList<EntityView>? entityViews;
    private IReadOnlyDictionary<string, float>? inputValues;

    public ulong Revision { get; } = revision;
    public string SelfJson { get; } = selfJson;
    public string InputJson { get; } = inputJson;
    public string EntitiesJson { get; } = entitiesJson;
    public string EventsJson { get; } = eventsJson;

    /// <summary>
    /// Returns the bounded, typed view of the existing script entity observation.
    /// The observation remains the wire contract; this is only a convenient managed projection.
    /// </summary>
    public IReadOnlyList<EntityView> Entities => entityViews ??= ReadEntityViews();

    public IReadOnlyList<EntityView> EntityViews => Entities;

    public JsonElement Self
    {
        get
        {
            using var document = JsonDocument.Parse(SelfJson);
            return document.RootElement.Clone();
        }
    }

    public Float3? SelfVelocity
    {
        get
        {
            using var document = JsonDocument.Parse(SelfJson);
            if (!document.RootElement.TryGetProperty("velocity", out var value)) return null;
            return new(value.GetProperty("x").GetSingle(), value.GetProperty("y").GetSingle(), value.GetProperty("z").GetSingle());
        }
    }

    public CharacterMotor2DView? SelfCharacterMotor2D
    {
        get
        {
            using var document = JsonDocument.Parse(SelfJson);
            if (!document.RootElement.TryGetProperty("characterMotor2D", out var value)) return null;
            return new(value.GetProperty("grounded").GetBoolean(), value.GetProperty("moveInput").GetSingle(),
                value.GetProperty("decision").GetString()!, value.GetProperty("reason").GetString()!,
                value.GetProperty("groundEntityId").GetString()!, value.GetProperty("wallEntityId").GetString()!,
                value.GetProperty("jumpCount").GetUInt64(), value.GetProperty("landingCount").GetUInt64());
        }
    }

    public SpritePlaybackView? SelfSpritePlayback
    {
        get
        {
            using var document = JsonDocument.Parse(SelfJson);
            if (!document.RootElement.TryGetProperty("spritePlayback", out var value)) return null;
            return new(value.GetProperty("assetId").GetString()!, value.GetProperty("clipId").GetString()!,
                value.GetProperty("frameIndex").GetInt32(), value.GetProperty("playing").GetBoolean(),
                value.GetProperty("completedLoops").GetUInt64());
        }
    }

    public float InputValue(string actionId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(actionId);
        inputValues ??= ReadInputValues();
        return inputValues.TryGetValue(actionId, out var value) ? value : 0.0F;
    }

    public JsonElement? FindEntity(EntityId entity)
    {
        using var document = JsonDocument.Parse(EntitiesJson);
        foreach (var candidate in document.RootElement.GetProperty("items").EnumerateArray())
            if (candidate.GetProperty("id").GetString() == entity.Value) return candidate.Clone();
        return null;
    }

    public EntityView? FindEntityView(EntityId entity)
    {
        foreach (var candidate in Entities)
            if (candidate.Id == entity) return candidate;
        return null;
    }

    public Float3? EntityPosition(EntityId entity) => FindEntityView(entity)?.Position;

    public bool TryGetEntityPosition(EntityId entity, out Float3 position)
    {
        var value = EntityPosition(entity);
        if (value is not null)
        {
            position = value.Value;
            return true;
        }
        position = default;
        return false;
    }

    public IReadOnlyList<EntityId> EntitiesWith(string componentId)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(componentId);
        return Entities.Where(candidate => candidate.HasComponent(componentId)).Select(candidate => candidate.Id).ToArray();
    }

    public IReadOnlyList<EntityId> EntitiesWith(ComponentId component) => EntitiesWith(component.Value);

    public bool HasTag(EntityId entity, string tag)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(tag);
        return FindEntityView(entity)?.HasTag(tag) ?? false;
    }

    public IReadOnlyList<EntityId> EntitiesWithTag(string tag)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(tag);
        return Entities.Where(candidate => candidate.HasTag(tag)).Select(candidate => candidate.Id).ToArray();
    }

    public IReadOnlyList<JsonElement> Events(string? eventType = null)
    {
        using var document = JsonDocument.Parse(EventsJson);
        return document.RootElement.GetProperty("items").EnumerateArray()
            .Where(value => eventType is null || value.GetProperty("type").GetString() == eventType)
            .Select(value => value.Clone()).ToArray();
    }

    private IReadOnlyList<EntityView> ReadEntityViews()
    {
        using var document = JsonDocument.Parse(EntitiesJson);
        var items = document.RootElement.GetProperty("items");
        var result = new List<EntityView>(items.GetArrayLength());
        foreach (var candidate in items.EnumerateArray())
        {
            var id = candidate.GetProperty("id").GetString();
            var name = candidate.GetProperty("name").GetString();
            if (string.IsNullOrWhiteSpace(id) || name is null)
                throw new JsonException("Script entity observations require a stable id and name.");

            Float3? position = null;
            if (candidate.TryGetProperty("position", out var positionValue))
                position = ReadFiniteFloat3(positionValue, "position");

            var components = candidate.TryGetProperty("components", out var componentValues) &&
                             componentValues.ValueKind == JsonValueKind.Array
                ? componentValues.EnumerateArray().Select(value =>
                {
                    var component = value.GetString();
                    if (string.IsNullOrWhiteSpace(component))
                        throw new JsonException("Script entity component IDs must be non-empty strings.");
                    return new ComponentId(component);
                }).ToArray()
                : Array.Empty<ComponentId>();
            var tags = candidate.TryGetProperty("tags", out var tagValues) &&
                       tagValues.ValueKind == JsonValueKind.Array
                ? tagValues.EnumerateArray().Select(value =>
                {
                    var tag = value.GetString();
                    if (string.IsNullOrWhiteSpace(tag))
                        throw new JsonException("Script entity tags must be non-empty strings.");
                    return tag;
                }).ToArray()
                : Array.Empty<string>();
            result.Add(new EntityView(new EntityId(id), name, position, components, tags));
        }
        return result.ToArray();
    }

    private IReadOnlyDictionary<string, float> ReadInputValues()
    {
        using var input = JsonDocument.Parse(InputJson);
        var result = new Dictionary<string, float>(StringComparer.Ordinal);
        if (!input.RootElement.TryGetProperty("actions", out var actions)) return result;
        foreach (var action in actions.EnumerateArray())
        {
            var id = action.GetProperty("id").GetString();
            var value = action.GetProperty("value").GetSingle();
            if (string.IsNullOrWhiteSpace(id) || !float.IsFinite(value))
                throw new JsonException("Script input actions require a stable id and finite value.");
            result[id] = value;
        }
        return result;
    }

    private static Float3 ReadFiniteFloat3(JsonElement value, string propertyName)
    {
        if (value.ValueKind != JsonValueKind.Object ||
            !value.TryGetProperty("x", out var x) || !value.TryGetProperty("y", out var y) ||
            !value.TryGetProperty("z", out var z) || !x.TryGetSingle(out var xValue) ||
            !y.TryGetSingle(out var yValue) || !z.TryGetSingle(out var zValue) ||
            !float.IsFinite(xValue) || !float.IsFinite(yValue) || !float.IsFinite(zValue))
            throw new JsonException($"Script entity {propertyName} must be a finite x/y/z object.");
        return new Float3(xValue, yValue, zValue);
    }
}

public sealed record ScriptCommand(
    [property: JsonPropertyName("operation")] string Operation,
    [property: JsonPropertyName("entityId")] string EntityId,
    [property: JsonPropertyName("payload")] object Payload);

public sealed class ScriptCommandBuffer
{
    private readonly List<ScriptCommand> commands = [];
    private const string VelocityLinearPropertyId = "engine.entity.velocity.linear";
    private const string SpriteVisiblePropertyId = "engine.entity.sprite.visible";
    public IReadOnlyList<ScriptCommand> Commands => commands;

    public void SetPosition(EntityId entity, float x, float y, float z)
    {
        EnsureFinite(x, nameof(x));
        EnsureFinite(y, nameof(y));
        EnsureFinite(z, nameof(z));
        commands.Add(new("scene.transform.set-position", entity.Value, new { x, y, z }));
    }

    public void SetVelocityLinear(EntityId entity, Float3 linear) =>
        SetProperty(entity, VelocityLinearPropertyId, EnsureFinite(linear, nameof(linear)));

    public void SetVelocityLinear(EntityId entity, float x, float y, float z) =>
        SetVelocityLinear(entity, new Float3(x, y, z));

    public void SetSpriteVisible(EntityId entity, bool visible) =>
        SetProperty(entity, SpriteVisiblePropertyId, visible);

    public void SetProperty(EntityId entity, string propertyId, object value)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(propertyId);
        ArgumentNullException.ThrowIfNull(value);
        commands.Add(new("scene.property.set", entity.Value, new { propertyId, value }));
    }

    public void SetProperty<T>(EntityId entity, PropertyId<T> property, T value) where T : notnull =>
        SetProperty(entity, property.Value, value);

    public void SetSpritePlayback(EntityId entity, string clipId, bool flipX, float playbackSpeed = 1.0F,
                                  bool playing = true, bool restart = false)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(clipId);
        if (!float.IsFinite(playbackSpeed) || playbackSpeed < 0.0F || playbackSpeed > 16.0F)
            throw new ArgumentOutOfRangeException(nameof(playbackSpeed));
        commands.Add(new("sprite.playback.set", entity.Value,
            new { clipId, playing, playbackSpeed, flipX, restart }));
    }

    public void PlayAudio(AssetId asset, string busId = "audio.sfx", float gain = 1.0F,
                          float pitch = 1.0F, bool looping = false)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(asset.Value);
        ArgumentException.ThrowIfNullOrWhiteSpace(busId);
        if (!float.IsFinite(gain) || gain < 0.0F || gain > 4.0F)
            throw new ArgumentOutOfRangeException(nameof(gain));
        if (!float.IsFinite(pitch) || pitch <= 0.0F || pitch > 4.0F)
            throw new ArgumentOutOfRangeException(nameof(pitch));
        commands.Add(new("audio.voice.play", string.Empty,
            new { assetId = asset.Value, busId, gain, pitch, looping }));
    }

    private void RequestPersistence(string action, string slotId = "")
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(action);
        if ((action != "replay-start" && string.IsNullOrWhiteSpace(slotId)) || slotId.Length > 64 ||
            slotId.Any(character => !char.IsAsciiLetterOrDigit(character) &&
            character is not '.' and not '_' and not '-'))
            throw new ArgumentException("Persistence slot IDs use only ASCII letters, digits, dot, underscore or dash.", nameof(slotId));
        commands.Add(new("gameplay.persistence.request", string.Empty, new { action, slotId }));
    }

    public void SaveSlot(string slotId) => RequestPersistence("save", slotId);
    public void LoadSlot(string slotId) => RequestPersistence("load", slotId);
    public void StartReplayRecording() => RequestPersistence("replay-start");
    public void StopReplayRecording(string slotId) => RequestPersistence("replay-stop", slotId);
    public void PlayReplay(string slotId) => RequestPersistence("replay-play", slotId);

    public void SpawnPrefab(EntityId source, EntityId newEntity, string displayName, float x, float y, float z)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(source.Value);
        ArgumentException.ThrowIfNullOrWhiteSpace(newEntity.Value);
        commands.Add(new("gameplay.prefab.spawn", source.Value, new { newEntityId = newEntity.Value, displayName, x, y, z }));
    }

    public void Despawn(EntityId entity) => commands.Add(new("gameplay.entity.despawn", entity.Value, new { }));

    public void EmitEvent(EntityId target, string eventType, object? payload = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(eventType);
        commands.Add(new("gameplay.event.emit", target.Value, new { eventType, payload = payload ?? new { } }));
    }

    public void AddTag(EntityId entity, string tag)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(tag);
        commands.Add(new("gameplay.tag.set", entity.Value, new { tag, present = true }));
    }

    public void RemoveTag(EntityId entity, string tag)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(tag);
        commands.Add(new("gameplay.tag.set", entity.Value, new { tag, present = false }));
    }

    private static Float3 EnsureFinite(Float3 value, string parameterName)
    {
        EnsureFinite(value.X, parameterName);
        EnsureFinite(value.Y, parameterName);
        EnsureFinite(value.Z, parameterName);
        return value;
    }

    private static void EnsureFinite(float value, string parameterName)
    {
        if (!float.IsFinite(value)) throw new ArgumentOutOfRangeException(parameterName, "Values must be finite.");
    }
}

public sealed class ScriptContext(EntityId entity, string argumentsJson, ScriptWorldView world)
{
    public EntityId Entity { get; } = entity;
    public string ArgumentsJson { get; } = argumentsJson;
    public ScriptWorldView World { get; } = world;
    public ScriptCommandBuffer Commands { get; } = new();

    public float DeltaSeconds
    {
        get
        {
            using var arguments = JsonDocument.Parse(ArgumentsJson);
            return arguments.RootElement.TryGetProperty("deltaSeconds", out var delta) ? delta.GetSingle() : 0.0F;
        }
    }

    public ScriptContact? Contact
    {
        get
        {
            using var arguments = JsonDocument.Parse(ArgumentsJson);
            if (!arguments.RootElement.TryGetProperty("contact", out var contact)) return null;
            var normal = contact.GetProperty("normal");
            return new(new EntityId(contact.GetProperty("selfId").GetString()!),
                new EntityId(contact.GetProperty("otherId").GetString()!),
                new Float3(normal.GetProperty("x").GetSingle(), normal.GetProperty("y").GetSingle(), normal.GetProperty("z").GetSingle()),
                contact.GetProperty("penetration").GetSingle(), contact.GetProperty("isTrigger").GetBoolean());
        }
    }

    public T? Property<T>(string name)
    {
        using var arguments = JsonDocument.Parse(ArgumentsJson);
        if (!arguments.RootElement.TryGetProperty("properties", out var properties) ||
            !properties.TryGetProperty(name, out var value)) return default;
        return value.Deserialize<T>();
    }
}

public readonly record struct ScriptContact(EntityId Self, EntityId Other, Float3 Normal, float Penetration, bool IsTrigger);

public sealed class ScriptTimer(float durationSeconds, bool repeating = false)
{
    public float DurationSeconds { get; } = durationSeconds > 0.0F ? durationSeconds :
        throw new ArgumentOutOfRangeException(nameof(durationSeconds));
    public bool Repeating { get; } = repeating;
    public float ElapsedSeconds { get; private set; }
    public bool IsElapsed { get; private set; }

    public bool Advance(float deltaSeconds)
    {
        if (deltaSeconds < 0.0F || !float.IsFinite(deltaSeconds))
            throw new ArgumentOutOfRangeException(nameof(deltaSeconds));
        if (IsElapsed && !Repeating) return false;
        ElapsedSeconds += deltaSeconds;
        if (ElapsedSeconds < DurationSeconds) return false;
        IsElapsed = true;
        if (Repeating)
        {
            ElapsedSeconds %= DurationSeconds;
            IsElapsed = false;
        }
        return true;
    }

    public void Reset() { ElapsedSeconds = 0.0F; IsElapsed = false; }
}

public abstract class ScriptBehaviour
{
    public virtual void OnCreate(in ScriptContext context) { }
    public virtual void OnFixedUpdate(in ScriptContext context) { }
    public virtual void OnUpdate(in ScriptContext context) { }
    public virtual void OnContactEnter(in ScriptContext context) { }
    public virtual void OnContactStay(in ScriptContext context) { }
    public virtual void OnContactExit(in ScriptContext context) { }
    public virtual void OnTriggerEnter(in ScriptContext context) { }
    public virtual void OnTriggerStay(in ScriptContext context) { }
    public virtual void OnTriggerExit(in ScriptContext context) { }
    public virtual void OnDestroy(in ScriptContext context) { }
}
