using Noemancer;
using Noemancer.Generated;

namespace ManagedFixture;

public sealed class PlayerController : ScriptBehaviour
{
    public int CreateCount { get; private set; }
    public int TransformEntityCount { get; private set; }

    public override void OnCreate(in ScriptContext context)
    {
        CreateCount++;
        TransformEntityCount = context.World.EntitiesWith(EngineSchema.Components.Transform).Count;
    }
}
