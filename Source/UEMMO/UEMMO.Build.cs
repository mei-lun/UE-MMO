using UnrealBuildTool;
public class UEMMO : ModuleRules
{
    public UEMMO(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // Per-task test translation units use file-local anonymous namespace
        // constants; unity chunk reshuffles when new files are added collide
        // them into one TU (e.g. PlanarMovementTests / SideCameraTests
        // TestTolerance). Per-file compilation is deterministic.
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "AnimGraphRuntime" });
        // M2-002: AMeleeEnemyController extends AAIController (AIModule type);
        // the module is otherwise unused (no behavior tree, no nav mesh).
        PublicDependencyModuleNames.AddRange(new[] { "AIModule" });
        if (Target.bBuildEditor)
        {
            // M1-031 editor-side locomotion graph builder (PrototypeAnimInstance
            // EditorBuildLocomotionGraph): anim graph nodes live in these
            // editor-only modules; the game target never compiles that path.
            PublicDependencyModuleNames.AddRange(new[] { "AnimGraph", "BlueprintGraph" });
        }
    }
}
