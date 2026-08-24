using Noemancer;

namespace ManagedFixture;

public sealed class UiController : ScriptBehaviour
{
    public int ActionCount { get; private set; }
    public string LastActionId { get; private set; } = "";
    public string LastNodeId { get; private set; } = "";

    public override void OnUiAction(in ScriptContext context)
    {
        var action = context.UiAction;
        if (action is null) return;
        ActionCount++;
        LastActionId = action.Value.ActionId;
        LastNodeId = action.Value.NodeId;
    }
}
