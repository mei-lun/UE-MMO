using UnrealBuildTool;
public class UEMMOEditorTarget : TargetRules
{
    public UEMMOEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("UEMMO");
    }
}
