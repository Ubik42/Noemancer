using Noemancer;

namespace ManagedFixture;

public sealed class PersistenceController : ScriptBehaviour
{
    public override void OnCreate(in ScriptContext context)
    {
        context.Commands.SaveSlot("autosave");
        context.Commands.LoadSlot("autosave");
        context.Commands.StartReplayRecording();
        context.Commands.StopReplayRecording("last-run");
        context.Commands.PlayReplay("last-run");
    }
}
