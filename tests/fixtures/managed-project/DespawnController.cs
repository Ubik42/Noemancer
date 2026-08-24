using Noemancer;

namespace ManagedFixture;

public sealed class DespawnController : ScriptBehaviour
{
    public override void OnUpdate(in ScriptContext context)
    {
        var target = context.Property<string>("target");
        if (!string.IsNullOrWhiteSpace(target) && context.World.FindEntity(new EntityId(target)) is not null)
            context.Commands.Despawn(new EntityId(target));
    }
}
