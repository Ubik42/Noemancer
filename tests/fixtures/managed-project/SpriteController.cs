using Noemancer;

namespace ManagedFixture;

public sealed class SpriteController : ScriptBehaviour
{
    public bool SawCharacterMotor { get; private set; }
    public string InitialClip { get; private set; } = "";

    public override void OnCreate(in ScriptContext context)
    {
        SawCharacterMotor = context.World.SelfCharacterMotor2D is not null;
        InitialClip = context.World.SelfSpritePlayback?.ClipId ?? "";
        context.Commands.SetSpritePlayback(context.Entity, "run", flipX: true, playbackSpeed: 1.25F);
        context.Commands.PlayAudio(new AssetId("asset.audio.script-fixture"), gain: 0.75F, pitch: 1.1F);
    }
}
