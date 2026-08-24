using Noemancer;
using Noemancer.Generated;

namespace ManagedFixture;

public sealed class MoverController : ScriptBehaviour
{
    public float ObservedInput { get; private set; }
    public ulong ObservedRevision { get; private set; }
    public int TriggerEnterCount { get; private set; }
    public string TriggerOtherId { get; private set; } = "";
    public bool TriggerWasSensor { get; private set; }

    public override void OnTriggerEnter(in ScriptContext context)
    {
        TriggerEnterCount++;
        TriggerOtherId = context.Contact?.Other.Value ?? "";
        TriggerWasSensor = context.Contact?.IsTrigger ?? false;
    }

    public override void OnUpdate(in ScriptContext context)
    {
        ObservedInput = context.World.InputValue("gameplay.move.x");
        ObservedRevision = context.World.Revision;
        context.Commands.SetProperty(context.Entity, EngineSchema.Properties.Transform.Position, new Float3(4.0F, 5.0F, 6.0F));
        context.Commands.EmitEvent(context.Entity, "script.fixture.moved", new { input = ObservedInput });
    }
}
