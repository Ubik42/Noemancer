using Noemancer;

namespace E2GameplayProof;

public sealed class PlayerGameplay : ScriptBehaviour
{
    public int CollectedCount { get; private set; }
    public int RespawnCount { get; private set; }
    public bool Won { get; private set; }
    public string LastOtherId { get; private set; } = "";

    public override void OnTriggerEnter(in ScriptContext context)
    {
        var contact = context.Contact;
        if (contact is null) return;
        var other = contact.Value.Other;
        LastOtherId = other.Value;
        if (context.World.HasTag(other, "gameplay.collectible"))
        {
            CollectedCount++;
            context.Commands.Despawn(other);
            context.Commands.AddTag(context.Entity, "inventory.key");
            context.Commands.EmitEvent(context.Entity, "gameplay.item.collected", new { item = other.Value });
        }
        else if (context.World.HasTag(other, "gameplay.hazard"))
        {
            RespawnCount++;
            context.Commands.SetPosition(context.Entity, context.Property<float>("spawnX"),
                context.Property<float>("spawnY"), context.Property<float>("spawnZ"));
        }
        else if (context.World.HasTag(other, "gameplay.goal") && context.World.HasTag(context.Entity, "inventory.key"))
        {
            Won = true;
            context.Commands.EmitEvent(context.Entity, "gameplay.level.completed", new { goal = other.Value });
        }
    }
}
