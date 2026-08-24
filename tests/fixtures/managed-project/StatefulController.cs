using Noemancer;

namespace ManagedFixture;

public sealed class StatefulController : ScriptBehaviour
{
    [ScriptState]
    public int Counter { get; set; }

    public override void OnUpdate(in ScriptContext context) => Counter++;
}
