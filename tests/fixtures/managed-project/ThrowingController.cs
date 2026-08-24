using Noemancer;

namespace ManagedFixture;

public sealed class ThrowingController : ScriptBehaviour
{
    public override void OnUpdate(in ScriptContext context)
    {
        throw new InvalidOperationException($"fixture failure for {context.Entity}");
    }
}
