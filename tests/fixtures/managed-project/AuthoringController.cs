using Noemancer;

namespace ManagedFixture;

public sealed class AuthoringController : ScriptBehaviour
{
    private readonly ScriptTimer timer = new(0.01F);
    public bool TimerFired { get; private set; }
    public float ConfiguredRate { get; private set; }
    public bool FoundCube { get; private set; }
    public int RenderableCount { get; private set; }

    public override void OnUpdate(in ScriptContext context)
    {
        ConfiguredRate = context.Property<float>("rate");
        TimerFired = timer.Advance(context.DeltaSeconds);
        FoundCube = context.World.FindEntity(new EntityId("entity.demo-cube")) is not null;
        RenderableCount = context.World.EntitiesWith("engine.component.MeshRenderer").Count;
        context.Commands.SetProperty(context.Entity, "engine.entity.material.roughness", 0.25F);
        context.Commands.SetVelocityLinear(new EntityId("entity.demo-cube"), 2.0F, -1.0F, 0.0F);
        context.Commands.SpawnPrefab(context.Entity, new EntityId("entity.script-spawned"),
            "Script Spawned", 8.0F, 2.0F, 1.0F);
    }
}
