using UnrealBuildTool;
public class UEMMOTarget : TargetRules
{
    public UEMMOTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("UEMMO");
    }
}
