using Noemancer;

namespace E2GameplayProof;

public sealed class TagAuthor : ScriptBehaviour
{
    public override void OnCreate(in ScriptContext context)
    {
        var tag = context.Property<string>("tag");
        if (!string.IsNullOrWhiteSpace(tag)) context.Commands.AddTag(context.Entity, tag);
    }
}
