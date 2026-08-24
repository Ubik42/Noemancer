using Noemancer;

namespace ManagedFixture;

public sealed class AtomicFailureController : ScriptBehaviour
{
    public override void OnUpdate(in ScriptContext context)
    {
        context.Commands.SetPosition(context.Entity, 99.0F, 98.0F, 97.0F);
        context.Commands.Despawn(new EntityId("entity.bootstrap-root"));
    }
}
