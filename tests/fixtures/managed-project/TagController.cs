using Noemancer;

namespace ManagedFixture;

public sealed class TagController : ScriptBehaviour
{
    public bool ObservedTagged { get; private set; }
    public int ContactEnterCount { get; private set; }

    public override void OnCreate(in ScriptContext context) => context.Commands.AddTag(context.Entity, "state.script-tested");

    public override void OnUpdate(in ScriptContext context)
    {
        ObservedTagged = context.World.HasTag(context.Entity, "state.script-tested");
        context.Commands.RemoveTag(context.Entity, "state.script-tested");
    }

    public override void OnContactEnter(in ScriptContext context)
    {
        ContactEnterCount++;
    }
}
