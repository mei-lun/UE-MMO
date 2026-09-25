using UnrealBuildTool;
public class UEMMO : ModuleRules
{
    public UEMMO(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // Unity builds are disabled for this module: the test files define
        // same-named constants (e.g. TestTolerance) in their anonymous
        // namespaces, so any clean checkout fails to build once UBT merges
        // them into one translation unit. The module is small, so per-file
        // compilation cost is negligible.
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput" });
    }
}
