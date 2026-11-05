// M6-021: the DNFM character picture layer's automated suite. Covers the
// task card's acceptance list:
//   1. the frozen M6-020 manifest contract deserializes into the visual set
//      (and contract violations refuse the WHOLE build),
//   2. the frame sampling pure math (duration accumulation, loop wrap,
//      once-clip end hold, event-crossing walk),
//   3. the section 5.1 placement math (canvas -> world with the frozen
//      formula, the ONE facing flip, the once-calibrated cm/pixel scale,
//      the page UV rect),
//   4. the component playback (SetAction/TickVisual/ResetVisual, the
//      as-is event feed), and
//   5. the pawn gate wiring: UEMMO.PlayerDNFMVisual default 0 keeps the 3D
//      path untouched (the M6-016 baseline), gate 1 hides the mesh, places
//      the plane on the true capsule-bottom anchor (ground AND jump Z),
//      flips the plane exactly once with the actor yaw and restores
//      everything on gate 0 (runtime teardown).
// No real atlas assets are needed: the visual sets are built from mock
// manifest JSON strings in-memory (the M6-022 import owns the real data).
#include "Misc/AutomationTest.h"

#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Character/DNFMCharacterVisualSet.h"
#include "../Character/SideViewCameraComponent.h"
#include "../PrototypeCharacter.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Math/Axis.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M6_021_Tests
{
    // A two-page, two-action mock manifest honoring the frozen M6-020 schema
    // (Docs/Tasks/M6-020.md): pages with sha256+size; actions with loop,
    // source_ani, source_sha256 and frames carrying duration/page/uv/canvas/
    // pivot/events. Frame 0 of "rest" carries one audio event.
    static const TCHAR* const MockManifest = TEXT(R"raw({
  "generator": "Scripts/DNFM/prepare_fighter_visual_set.py",
  "character": "fighter",
  "outfit": "0000a",
  "source_root": "D:\\Github-Poj\\DNFM",
  "pages": [
    {"file": "fighter_page_0.png", "sha256": "aaa0", "size": [2048, 2048]},
    {"file": "fighter_page_1.png", "sha256": "bbb1", "size": [1024, 1024]}
  ],
  "actions": [
    {
      "action_id": "rest",
      "source_ani": "script/character/fighter/animation/rest.ani",
      "source_sha256": "hash-rest",
      "loop": true,
      "frames": [
        {"index": 0, "duration_ms": 100, "page": 0, "uv_rect_px": [16, 32, 128, 256],
         "canvas_size_px": [512, 512], "pivot_px": [64, 240],
         "events": [{"type": "audio", "name": "snd_step"}]},
        {"index": 1, "duration_ms": 200, "page": 0, "uv_rect_px": [160, 32, 128, 256],
         "canvas_size_px": [512, 512], "pivot_px": [64, 240]},
        {"index": 2, "duration_ms": 300, "page": 1, "uv_rect_px": [0, 0, 64, 96],
         "canvas_size_px": [512, 512], "pivot_px": [32, 90]}
      ]
    },
    {
      "action_id": "attack1",
      "source_ani": "script/character/fighter/animation/attack1.ani",
      "source_sha256": "hash-attack1",
      "loop": false,
      "frames": [
        {"index": 0, "duration_ms": 80, "page": 0, "uv_rect_px": [300, 300, 100, 100],
         "canvas_size_px": [512, 512], "pivot_px": [50, 95],
         "events": [{"type": "audio", "name": "snd_swing"}]},
        {"index": 1, "duration_ms": 120, "page": 0, "uv_rect_px": [400, 300, 100, 100],
         "canvas_size_px": [512, 512], "pivot_px": [50, 95]}
      ]
    }
  ]
})raw");

    /** Builds the in-memory set from the mock manifest (no textures bound). */
    static UDNFMCharacterVisualSet* BuildMockSet(FAutomationTestBase& Test, FString& OutError)
    {
        UDNFMCharacterVisualSet* Set = NewObject<UDNFMCharacterVisualSet>(GetTransientPackage());
        if (!Test.TestTrue(TEXT("the mock manifest builds"),
            UDNFMCharacterVisualSet::BuildFromManifestJson(MockManifest, {}, *Set, OutError)))
        {
            Test.AddError(FString::Printf(TEXT("mock manifest build failed: %s"), *OutError));
            return nullptr;
        }
        return Set;
    }

    /**
     * RAII guard for the UEMMO.PlayerDNFMVisual gate (the CVar decides the
     * pawn BeginPlay path, so the value must be set before SpawnActor and
     * restored even when the test fails early).
     */
    struct FDNFMVisualGateGuard
    {
        IConsoleVariable* CVar = nullptr;
        int32 Original = 0;

        FDNFMVisualGateGuard()
            : CVar(IConsoleManager::Get().FindConsoleVariable(TEXT("UEMMO.PlayerDNFMVisual")))
        {
            if (CVar != nullptr)
            {
                Original = CVar->GetInt();
            }
        }
        ~FDNFMVisualGateGuard()
        {
            if (CVar != nullptr)
            {
                CVar->Set(Original, ECVF_SetByCode);
            }
        }
        void Set(int32 Value)
        {
            if (CVar != nullptr)
            {
                CVar->Set(Value, ECVF_SetByCode);
            }
        }
    };

    // ---- world-scene helpers (the M3_027F temp-world precedent) -----------

    constexpr float TestFrameSeconds = 1.0f / 60.0f;

    static APrototypeCharacter* SpawnTestPlayer(UWorld& World, const FVector& Base)
    {
        FActorSpawnParameters Params;
        APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
            APrototypeCharacter::StaticClass(), Base, FRotator::ZeroRotator, Params);
        if (Player != nullptr && Player->GetCharacterMovement() != nullptr)
        {
            Player->GetCharacterMovement()->bRunPhysicsWithNoController = true;
        }
        return Player;
    }
}

using namespace UE::UEMMO::Tasks::M6_021;
using namespace UE::UEMMO::Tasks::M6_021_Tests;

// 1) The frozen M6-020 manifest contract deserializes field by field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021ManifestParses,
    "UEMMO.Tasks.M6_021.ManifestParses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021ManifestParses::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    TestEqual(TEXT("identity rides through"), Set->CharacterId, TEXT("fighter"));
    TestEqual(TEXT("outfit rides through"), Set->OutfitId, TEXT("0000a"));
    TestTrue(TEXT("generator rides through"), Set->Generator.Contains(TEXT("prepare_fighter_visual_set")));
    TestEqual(TEXT("page count matches"), Set->PageSizesPx.Num(), 2);
    TestTrue(TEXT("page 0 size"), Set->PageSizesPx[0] == FIntPoint(2048, 2048));
    TestTrue(TEXT("page 1 size"), Set->PageSizesPx[1] == FIntPoint(1024, 1024));
    TestEqual(TEXT("page hash traceability"), Set->PageSha256s[1], TEXT("bbb1"));

    TestEqual(TEXT("action count matches"), Set->Actions.Num(), 2);
    const FDNFMActionClip* Rest = Set->FindAction(FName(TEXT("rest")));
    if (!TestNotNull(TEXT("the rest clip exists"), Rest))
    {
        return false;
    }
    TestTrue(TEXT("rest loops"), Rest->bLoop);
    TestEqual(TEXT("rest source hash"), Rest->SourceSha256, TEXT("hash-rest"));
    TestTrue(TEXT("rest source ani traceability"), Rest->SourceAniPath.Contains(TEXT("rest.ani")));
    TestEqual(TEXT("rest frame count"), Rest->Frames.Num(), 3);
    TestEqual(TEXT("frame 0 duration"), Rest->Frames[0].DurationMs, 100);
    TestEqual(TEXT("frame 1 duration"), Rest->Frames[1].DurationMs, 200);
    TestEqual(TEXT("frame 2 page"), Rest->Frames[2].PageIndex, 1);
    TestTrue(TEXT("frame 0 uv pos"), Rest->Frames[0].UvRectPosPx == FIntPoint(16, 32));
    TestTrue(TEXT("frame 0 uv size"), Rest->Frames[0].UvRectSizePx == FIntPoint(128, 256));
    TestTrue(TEXT("frame 0 canvas"), Rest->Frames[0].CanvasSizePx == FIntPoint(512, 512));
    TestTrue(TEXT("frame 0 pivot (rect-relative, y down)"), Rest->Frames[0].PivotPx == FIntPoint(64, 240));
    TestEqual(TEXT("frame 0 event count"), Rest->Frames[0].Events.Num(), 1);
    if (Rest->Frames[0].Events.Num() == 1)
    {
        TestEqual(TEXT("event type"), Rest->Frames[0].Events[0].Type, FName(TEXT("audio")));
        TestEqual(TEXT("event name"), Rest->Frames[0].Events[0].Name, FName(TEXT("snd_step")));
    }
    const FDNFMActionClip* Attack = Set->FindAction(FName(TEXT("attack1")));
    if (TestNotNull(TEXT("the attack1 clip exists"), Attack))
    {
        TestFalse(TEXT("attack1 does not loop"), Attack->bLoop);
    }
    TestTrue(TEXT("unset page texture is the honest null"),
        Set->LoadPageTexture(0) == nullptr);
    TestEqual(TEXT("unknown action index"), Set->GetActionIndex(FName(TEXT("missing"))), INDEX_NONE);
    return true;
}

// 2) Contract violations refuse the whole build (validation-first, plan 10).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021ManifestRefuses,
    "UEMMO.Tasks.M6_021.ManifestRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021ManifestRefuses::RunTest(const FString& Parameters)
{
    const TCHAR* const BrokenManifests[] = {
        // not JSON at all
        TEXT("this is not json"),
        // missing pages
        TEXT(R"raw({"pages": [], "actions": [{"action_id": "a", "loop": false,
            "frames": [{"index": 0, "duration_ms": 10, "page": 0,
            "uv_rect_px": [0,0,10,10], "canvas_size_px": [10,10], "pivot_px": [5,5]}]}]})raw"),
        // uv rect exceeding the page bounds (the atlas overrun the tools must catch)
        TEXT(R"raw({"pages": [{"file": "p.png", "sha256": "s", "size": [64, 64]}],
          "actions": [{"action_id": "a", "loop": false,
            "frames": [{"index": 0, "duration_ms": 10, "page": 0,
            "uv_rect_px": [50, 50, 30, 30], "canvas_size_px": [30,30], "pivot_px": [5,5]}]}]})raw"),
        // duplicate action id
        TEXT(R"raw({"pages": [{"file": "p.png", "sha256": "s", "size": [64, 64]}],
          "actions": [
            {"action_id": "a", "loop": false, "frames": [{"index": 0, "duration_ms": 10, "page": 0,
             "uv_rect_px": [0,0,10,10], "canvas_size_px": [10,10], "pivot_px": [5,5]}]},
            {"action_id": "a", "loop": false, "frames": [{"index": 0, "duration_ms": 10, "page": 0,
             "uv_rect_px": [0,0,10,10], "canvas_size_px": [10,10], "pivot_px": [5,5]}]}]})raw"),
        // non-contiguous frame index
        TEXT(R"raw({"pages": [{"file": "p.png", "sha256": "s", "size": [64, 64]}],
          "actions": [{"action_id": "a", "loop": false,
            "frames": [
              {"index": 0, "duration_ms": 10, "page": 0, "uv_rect_px": [0,0,10,10],
               "canvas_size_px": [10,10], "pivot_px": [5,5]},
              {"index": 2, "duration_ms": 10, "page": 0, "uv_rect_px": [0,0,10,10],
               "canvas_size_px": [10,10], "pivot_px": [5,5]}]}]})raw"),
        // zero duration frame
        TEXT(R"raw({"pages": [{"file": "p.png", "sha256": "s", "size": [64, 64]}],
          "actions": [{"action_id": "a", "loop": false,
            "frames": [{"index": 0, "duration_ms": 0, "page": 0, "uv_rect_px": [0,0,10,10],
            "canvas_size_px": [10,10], "pivot_px": [5,5]}]}]})raw"),
        // missing loop flag
        TEXT(R"raw({"pages": [{"file": "p.png", "sha256": "s", "size": [64, 64]}],
          "actions": [{"action_id": "a",
            "frames": [{"index": 0, "duration_ms": 10, "page": 0, "uv_rect_px": [0,0,10,10],
            "canvas_size_px": [10,10], "pivot_px": [5,5]}]}]})raw"),
    };
    for (const TCHAR* Manifest : BrokenManifests)
    {
        UDNFMCharacterVisualSet* Set = NewObject<UDNFMCharacterVisualSet>(GetTransientPackage());
        FString Error;
        TestTrue(TEXT("a broken manifest refuses the build"),
            !UDNFMCharacterVisualSet::BuildFromManifestJson(Manifest, {}, *Set, Error));
        TestFalse(TEXT("the refusal names its concrete reason"), Error.IsEmpty());
        TestEqual(TEXT("the refused build leaves no actions"), Set->Actions.Num(), 0);
        AddInfo(FString::Printf(TEXT("refusal: %s"), *Error));
    }
    // A texture-count mismatch refuses too (the M6-022 helper contract).
    UDNFMCharacterVisualSet* Set = NewObject<UDNFMCharacterVisualSet>(GetTransientPackage());
    FString Error;
    UTexture2D* Dummy = nullptr;
    const TArray<UTexture2D*> OneTexture = {Dummy};
    TestTrue(TEXT("a texture-count mismatch refuses the build"),
        !UDNFMCharacterVisualSet::BuildFromManifestJson(MockManifest, OneTexture, *Set, Error));
    TestTrue(TEXT("the mismatch error names the counts"),
        Error.Contains(TEXT("1")) && Error.Contains(TEXT("2")));
    return true;
}

// 3) Frame sampling: duration accumulation, loop wrap, once-clip end hold.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021FrameSampling,
    "UEMMO.Tasks.M6_021.FrameSampling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021FrameSampling::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    const FDNFMActionClip* Rest = Set->FindAction(FName(TEXT("rest")));
    const FDNFMActionClip* Attack = Set->FindAction(FName(TEXT("attack1")));
    if (!TestNotNull(TEXT("the rest clip exists"), Rest) || !TestNotNull(TEXT("the attack1 clip exists"), Attack))
    {
        return false;
    }
    TestEqual(TEXT("loop total is the duration sum"), ComputeClipTotalDurationMs(Rest->Frames), 600);
    TestEqual(TEXT("once total is the duration sum"), ComputeClipTotalDurationMs(Attack->Frames), 200);

    // Loop: boundaries belong to the frame that starts there.
    TestEqual(TEXT("loop t=0 is frame 0"), SampleFrameIndexByElapsedMs(Rest->Frames, 0.0, true), 0);
    TestEqual(TEXT("loop t=99.9 is frame 0"), SampleFrameIndexByElapsedMs(Rest->Frames, 99.9, true), 0);
    TestEqual(TEXT("loop t=100 is frame 1"), SampleFrameIndexByElapsedMs(Rest->Frames, 100.0, true), 1);
    TestEqual(TEXT("loop t=299.9 is frame 1"), SampleFrameIndexByElapsedMs(Rest->Frames, 299.9, true), 1);
    TestEqual(TEXT("loop t=300 is frame 2"), SampleFrameIndexByElapsedMs(Rest->Frames, 300.0, true), 2);
    TestEqual(TEXT("loop t=599.9 is frame 2"), SampleFrameIndexByElapsedMs(Rest->Frames, 599.9, true), 2);
    TestEqual(TEXT("loop t=600 wraps to frame 0"), SampleFrameIndexByElapsedMs(Rest->Frames, 600.0, true), 0);
    // 1000ms wraps to local 400ms, which sits inside frame 2 ([300..600)).
    TestEqual(TEXT("loop t=1000 wraps into frame 2"), SampleFrameIndexByElapsedMs(Rest->Frames, 1000.0, true), 2);
    TestEqual(TEXT("loop wraps many cycles deterministically"),
        SampleFrameIndexByElapsedMs(Rest->Frames, 300.0 + 600.0 * 7, true), 2);

    // Once: the end frame holds (plan section 6.2).
    TestEqual(TEXT("once t=0 is frame 0"), SampleFrameIndexByElapsedMs(Attack->Frames, 0.0, false), 0);
    TestEqual(TEXT("once t=80 is frame 1"), SampleFrameIndexByElapsedMs(Attack->Frames, 80.0, false), 1);
    TestEqual(TEXT("once t=199.9 is frame 1"), SampleFrameIndexByElapsedMs(Attack->Frames, 199.9, false), 1);
    TestEqual(TEXT("once past the end holds frame 1"), SampleFrameIndexByElapsedMs(Attack->Frames, 5000.0, false), 1);
    TestEqual(TEXT("once exactly at the end holds frame 1"),
        SampleFrameIndexByElapsedMs(Attack->Frames, 200.0, false), 1);

    // Degenerate inputs stay deterministic.
    TArray<FDNFMVisualFrame> Empty;
    TestEqual(TEXT("empty clip samples to none"), SampleFrameIndexByElapsedMs(Empty, 10.0, true), INDEX_NONE);
    TArray<FDNFMVisualFrame> ZeroDurations;
    ZeroDurations.AddDefaulted(2);
    TestEqual(TEXT("all-zero durations hold the last frame"),
        SampleFrameIndexByElapsedMs(ZeroDurations, 10.0, true), 1);
    // A loop clip wraps negative time the modular way (-50ms -> 550ms, frame
    // 2); a non-loop clip clamps it to its first frame.
    TestEqual(TEXT("negative elapsed wraps modulo the loop"),
        SampleFrameIndexByElapsedMs(Rest->Frames, -50.0, true), 2);
    TestEqual(TEXT("negative elapsed clamps for a once-clip"),
        SampleFrameIndexByElapsedMs(Attack->Frames, -50.0, false), 0);
    return true;
}

// 4) The event crossing walk feeds the honest interval for the M6-023 dedup.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021EventCrossingWalk,
    "UEMMO.Tasks.M6_021.EventCrossingWalk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021EventCrossingWalk::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    const FDNFMActionClip* Rest = Set->FindAction(FName(TEXT("rest")));
    const FDNFMActionClip* Attack = Set->FindAction(FName(TEXT("attack1")));
    if (!TestNotNull(TEXT("the rest clip exists"), Rest) || !TestNotNull(TEXT("the attack1 clip exists"), Attack))
    {
        return false;
    }
    // Start-of-playback walk (previous = invalid): 0 through the new index.
    TArray<int32> Start = ComputeCrossedFrameIndices(Rest->Frames, INDEX_NONE, 2, true);
    TestTrue(TEXT("start walk reaches 0..2"),
        Start.Num() == 3 && Start[0] == 0 && Start[1] == 1 && Start[2] == 2);
    // A plain forward step reports only the new frame.
    TArray<int32> Single = ComputeCrossedFrameIndices(Rest->Frames, 0, 1, true);
    TestTrue(TEXT("a single step reports one frame"), Single.Num() == 1 && Single[0] == 1);
    // The loop wrap walks to the table end and back through 0.
    TArray<int32> Wrapped = ComputeCrossedFrameIndices(Rest->Frames, 1, 0, true);
    TestTrue(TEXT("the loop wrap walks 2 then 0"),
        Wrapped.Num() == 2 && Wrapped[0] == 2 && Wrapped[1] == 0);
    // A same-frame advance reports nothing (no duplicate events).
    TestEqual(TEXT("an unchanged frame reports nothing"),
        ComputeCrossedFrameIndices(Rest->Frames, 1, 1, true).Num(), 0);
    // Non-loop never wraps backward.
    TestEqual(TEXT("a non-loop backward request reports nothing"),
        ComputeCrossedFrameIndices(Attack->Frames, 1, 0, false).Num(), 0);
    return true;
}

// 5) The section 5.1 placement math: the frozen formula, one facing flip,
// the once-calibrated scale and the page UV rect.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021CanvasPlacement,
    "UEMMO.Tasks.M6_021.CanvasPlacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021CanvasPlacement::RunTest(const FString& Parameters)
{
    // A simple axis-aligned camera basis keeps the hand-computed expectations
    // readable: R = +X, U = +Z (the real rig's tilted U is exercised by the
    // component-level tests).
    const FVector R(1, 0, 0);
    const FVector U(0, 0, 1);
    const FVector Foot(1000.0, 2000.0, 300.0);
    const float Scale = 0.5f; // cm per pixel
    const FIntPoint Pivot(40, 100); // canvas pivot (y down)

    // The pivot itself lands exactly on P for both facings (the foot anchor).
    TestTrue(TEXT("facing +1 keeps the pivot at P"),
        ComputeCanvasPointWorld(Foot, R, U, 1.0f, Pivot, Pivot, Scale).Equals(Foot, 0.01));
    TestTrue(TEXT("facing -1 keeps the pivot at P"),
        ComputeCanvasPointWorld(Foot, R, U, -1.0f, Pivot, Pivot, Scale).Equals(Foot, 0.01));
    // Canvas +x maps to R*Facing: +80 px right of pivot lands +40 cm / -40 cm.
    TestTrue(TEXT("facing +1 maps canvas +x onto +R"),
        ComputeCanvasPointWorld(Foot, R, U, 1.0f, Pivot + FIntPoint(80, 0), Pivot, Scale)
            .Equals(Foot + FVector(40, 0, 0), 0.01));
    TestTrue(TEXT("facing -1 maps canvas +x onto -R (the ONE flip)"),
        ComputeCanvasPointWorld(Foot, R, U, -1.0f, Pivot + FIntPoint(80, 0), Pivot, Scale)
            .Equals(Foot - FVector(40, 0, 0), 0.01));
    // Canvas y is down: a point 60 px ABOVE the pivot lifts 30 cm along U.
    TestTrue(TEXT("canvas up maps onto +U"),
        ComputeCanvasPointWorld(Foot, R, U, 1.0f, Pivot - FIntPoint(0, 60), Pivot, Scale)
            .Equals(Foot + FVector(0, 0, 30), 0.01));
    // Both axes at once: the frozen formula's two terms add.
    TestTrue(TEXT("the formula's two terms add independently"),
        ComputeCanvasPointWorld(Foot, R, U, -1.0f, Pivot + FIntPoint(20, -50), Pivot, Scale)
            .Equals(Foot + FVector(-10, 0, 25), 0.01));

    // The one-time calibration: s = height cm / head-top px; invalid input
    // refuses (0 stays uncalibrated - the plane must not draw a guess).
    TestTrue(TEXT("calibration divides height by pixels"),
        FMath::IsNearlyEqual(ComputeCmPerPixel(320.0f, 180.0f), 0.5625f, 1e-5f));
    TestEqual(TEXT("zero pixels refuse"), ComputeCmPerPixel(0.0f, 180.0f), 0.0f);
    TestEqual(TEXT("zero height refuses"), ComputeCmPerPixel(320.0f, 0.0f), 0.0f);
    TestEqual(TEXT("negative input refuses"), ComputeCmPerPixel(-1.0f, 180.0f), 0.0f);

    // The quad transform: basis X = R*Facing, Y = U, center from the formula.
    FDNFMVisualFrame Frame;
    Frame.UvRectPosPx = FIntPoint(10, 20);
    Frame.UvRectSizePx = FIntPoint(128, 256);
    Frame.PivotPx = FIntPoint(64, 256); // pivot at the rect bottom center
    const FTransform FacingRight = ComputeFrameQuadTransform(Foot, R, U, 1.0f, Frame, Scale);
    const FTransform FacingLeft = ComputeFrameQuadTransform(Foot, R, U, -1.0f, Frame, Scale);
    // Pivot canvas = (10+64, 20+256) = (74, 276); center canvas = (74, 148).
    // Vertical: (276-148) = 128 px above the pivot -> 64 cm up.
    // Horizontal: center x == pivot x -> no offset.
    TestTrue(TEXT("the quad center sits on the anchor column, 64 cm up"),
        FacingRight.GetTranslation().Equals(Foot + FVector(0, 0, 64), 0.01));
    TestTrue(TEXT("facing +1 quad X axis equals +R"),
        FacingRight.GetUnitAxis(EAxis::X).Equals(R, 1e-4));
    TestTrue(TEXT("facing -1 quad X axis equals -R (single flip, Y unchanged)"),
        FacingLeft.GetUnitAxis(EAxis::X).Equals(-R, 1e-4));
    TestTrue(TEXT("the quad Y axis stays the camera Up for both facings"),
        FacingRight.GetUnitAxis(EAxis::Y).Equals(U, 1e-4)
        && FacingLeft.GetUnitAxis(EAxis::Y).Equals(U, 1e-4));
    // Quad scale: the engine plane is 100 local units; 128 px * 0.5 = 64 cm.
    TestTrue(TEXT("quad width scale maps 128 px to 64 cm"),
        FMath::IsNearlyEqual(FacingRight.GetScale3D().X, 0.64f, 1e-5f));
    TestTrue(TEXT("quad height scale maps 256 px to 128 cm"),
        FMath::IsNearlyEqual(FacingRight.GetScale3D().Y, 1.28f, 1e-5f));

    // The page UV rect (the material parameter values): UE texture UVs run
    // y-down from the image top, so the manifest rect normalizes directly.
    FVector2D UvMin;
    FVector2D UvSize;
    ComputePageUVRect(Frame, FIntPoint(2048, 1024), UvMin, UvSize);
    TestTrue(TEXT("uv min normalizes"), UvMin.Equals(FVector2D(10.0 / 2048.0, 20.0 / 1024.0), 1e-6));
    TestTrue(TEXT("uv size normalizes"), UvSize.Equals(FVector2D(128.0 / 2048.0, 256.0 / 1024.0), 1e-6));
    ComputePageUVRect(Frame, FIntPoint(0, 0), UvMin, UvSize);
    TestTrue(TEXT("a degenerate page falls back to the unit rect"),
        UvMin.IsNearlyZero() && UvSize.Equals(FVector2D(1.0f, 1.0f)));
    return true;
}

// 6) The component playback: SetAction / TickVisual / ResetVisual and the
// as-is event feed (no world needed - the plane path stays inert).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021ComponentPlayback,
    "UEMMO.Tasks.M6_021.ComponentPlayback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021ComponentPlayback::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewObject<UDNFMCharacterVisualComponent>(GetTransientPackage());
    if (!TestNotNull(TEXT("the component constructs without a world"), Visual))
    {
        return false;
    }
    TestFalse(TEXT("idle before any set"), Visual->IsVisualActive());
    TestEqual(TEXT("idle action is none"), Visual->GetCurrentActionId(), FName(NAME_None));
    TestFalse(TEXT("an unknown action refuses"), Visual->SetAction(FName(TEXT("missing"))));
    Visual->SetVisualSet(Set);

    // Event feed recorder (the native delegate the M6-023 presenter binds).
    struct FEventLog
    {
        TArray<FString> Rows;
        void Record(uint64 ActorId, FName ActionName, int32 FrameIndex, FName EventTag, FName EventName)
        {
            Rows.Add(FString::Printf(TEXT("%llu|%s|%d|%s|%s"), ActorId,
                *ActionName.ToString(), FrameIndex, *EventTag.ToString(), *EventName.ToString()));
        }
    } Log;
    Visual->OnVisualFrameEvent.AddLambda([&Log](uint64 ActorId, FName ActionName, int32 FrameIndex,
        FName EventTag, FName EventName)
    {
        Log.Record(ActorId, ActionName, FrameIndex, EventTag, EventName);
    });

    TestTrue(TEXT("a known action starts"), Visual->SetAction(FName(TEXT("rest"))));
    TestTrue(TEXT("playback active after SetAction"), Visual->IsVisualActive());
    TestEqual(TEXT("SetAction starts at frame 0"), Visual->GetCurrentFrameIndex(), 0);
    // Frame 0 is current immediately; its event fired exactly once (this
    // ownerless test component reports actor id 0 - the documented fallback).
    TestEqual(TEXT("frame-0 event fired once on SetAction"), Log.Rows.Num(), 1);
    if (Log.Rows.Num() == 1)
    {
        TestTrue(TEXT("the event row carries action, frame, tag and name in order"),
            Log.Rows[0].StartsWith(TEXT("0|rest|0|audio|snd_step")));
    }

    // Loop playback over frames [0..100) [100..300) [300..600): the walk
    // crosses frame boundaries and finally wraps to frame 0, whose event
    // fires again on the wrap (the honest replay; dedup is M6-023's job).
    Log.Rows.Reset();
    Visual->TickVisual(0.05); // elapsed 50ms: still frame 0
    TestEqual(TEXT("50ms keeps frame 0"), Visual->GetCurrentFrameIndex(), 0);
    TestEqual(TEXT("no event without a frame change"), Log.Rows.Num(), 0);
    Visual->TickVisual(0.05); // elapsed 100ms: frame 1
    TestEqual(TEXT("100ms reaches frame 1"), Visual->GetCurrentFrameIndex(), 1);
    TestEqual(TEXT("frame 1 carries no event"), Log.Rows.Num(), 0);
    Visual->TickVisual(0.1); // elapsed 200ms: frame 1 continues
    TestEqual(TEXT("200ms stays frame 1"), Visual->GetCurrentFrameIndex(), 1);
    Visual->TickVisual(0.1); // elapsed 300ms: frame 2
    TestEqual(TEXT("300ms reaches frame 2"), Visual->GetCurrentFrameIndex(), 2);
    TestEqual(TEXT("frame 2 carries no event"), Log.Rows.Num(), 0);
    Visual->TickVisual(0.3); // elapsed 600ms: wrapped to frame 0
    TestEqual(TEXT("600ms wraps to frame 0"), Visual->GetCurrentFrameIndex(), 0);
    TestEqual(TEXT("the wrap re-fires frame 0's event exactly once"), Log.Rows.Num(), 1);
    Visual->TickVisual(0.1); // elapsed 700ms: frame 1
    TestEqual(TEXT("700ms reaches frame 1 after the wrap"), Visual->GetCurrentFrameIndex(), 1);
    TestEqual(TEXT("the post-wrap step adds no events"), Log.Rows.Num(), 1);

    // A once-clip holds its end frame and never re-fires it.
    TestTrue(TEXT("switching actions works"), Visual->SetAction(FName(TEXT("attack1"))));
    Log.Rows.Reset();
    Visual->TickVisual(0.5); // way past the 200ms once-total
    TestEqual(TEXT("a once-clip holds its end frame"), Visual->GetCurrentFrameIndex(), 1);
    Visual->TickVisual(0.5);
    TestEqual(TEXT("the end hold stays on frame 1"), Visual->GetCurrentFrameIndex(), 1);
    TestEqual(TEXT("no repeated events while holding"), Log.Rows.Num(), 0);

    // ResetVisual: the clean stop (playback state and plane visibility).
    Visual->SetMaterialInterface(nullptr); // logic-only run: the plane path stays inert
    Visual->ResetVisual();
    TestFalse(TEXT("reset stops playback"), Visual->IsVisualActive());
    TestEqual(TEXT("reset clears the frame"), Visual->GetCurrentFrameIndex(), INDEX_NONE);
    TestEqual(TEXT("reset clears the action"), Visual->GetCurrentActionId(), FName(NAME_None));
    TestFalse(TEXT("reset hides the plane"), Visual->IsPlaneVisible());
    Log.Rows.Reset();
    Visual->TickVisual(0.1);
    TestEqual(TEXT("reset stops the event feed"), Log.Rows.Num(), 0);
    // Re-entry after reset starts fresh (the presenter restart pattern).
    TestTrue(TEXT("re-entry after reset works"), Visual->SetAction(FName(TEXT("rest"))));
    TestEqual(TEXT("re-entry restarts at frame 0"), Visual->GetCurrentFrameIndex(), 0);

    // SetVisualSet(null) detaches everything.
    Visual->SetVisualSet(nullptr);
    TestFalse(TEXT("a null set detaches playback"), Visual->IsVisualActive());
    TestFalse(TEXT("a null set refuses SetAction"), Visual->SetAction(FName(TEXT("rest"))));
    return true;
}

// 7) Gate 0 (the default) keeps the 3D baseline: no DNFM activation, no
// plane, capsule and movement untouched (the M6-016 baseline stays).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021Gate0Keeps3D,
    "UEMMO.Tasks.M6_021.Gate0Keeps3D",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021Gate0Keeps3D::RunTest(const FString& Parameters)
{
    FDNFMVisualGateGuard Gate;
    Gate.Set(0);
    FTestWorldWrapper Wrapper;
    if (!TestTrue(TEXT("the manually ticked test world is created"),
        Wrapper.CreateTestWorld(EWorldType::Game)))
    {
        return true;
    }
    UWorld* World = Wrapper.GetTestWorld();
    if (!TestNotNull(TEXT("the test world is available"), World)
        || !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
    {
        return true;
    }
    APrototypeCharacter* Player = SpawnTestPlayer(*World, FVector(88000.0, 71000.0, 600.0));
    if (!TestNotNull(TEXT("the player exists"), Player))
    {
        return true;
    }
    TestFalse(TEXT("gate 0 keeps the DNFM mode off"), Player->IsDNFMVisualActive());
    TestNotNull(TEXT("the DNFM component exists (created always, inert by default)"),
        Player->GetDNFMVisual());
    TestTrue(TEXT("the 3D mesh stays visible"), Player->GetMesh()->IsVisible());
    TestNotNull(TEXT("the capsule remains the gameplay body"), Player->GetCapsuleComponent());
    TestNotNull(TEXT("movement remains available"), Player->GetCharacterMovement());
    TestFalse(TEXT("no plane was created for playback"), Player->GetDNFMVisual()->IsPlaneVisible());
    // A world tick with the gate off changes nothing (the M6-015 preview
    // path stays behind its own gate; the DNFM path stays inert).
    Wrapper.TickTestWorld(TestFrameSeconds);
    TestTrue(TEXT("the 3D mesh stays visible after a tick"), Player->GetMesh()->IsVisible());
    TestFalse(TEXT("still no plane after a tick"), Player->GetDNFMVisual()->IsPlaneVisible());
    TestFalse(TEXT("playback stays idle"), Player->GetDNFMVisual()->IsVisualActive());
    return true;
}

// 8) Gate 1: mesh hidden (capsule/movement kept), the plane anchored on the
// true capsule bottom (ground and jump Z), exactly one facing flip, clean
// runtime teardown back to the 3D hero.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_021Gate1Placement,
    "UEMMO.Tasks.M6_021.Gate1Placement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_021Gate1Placement::RunTest(const FString& Parameters)
{
    FDNFMVisualGateGuard Gate;
    Gate.Set(1); // BEFORE spawn: the pawn BeginPlay activates the picture mode
    FTestWorldWrapper Wrapper;
    if (!TestTrue(TEXT("the manually ticked test world is created"),
        Wrapper.CreateTestWorld(EWorldType::Game)))
    {
        return true;
    }
    UWorld* World = Wrapper.GetTestWorld();
    if (!TestNotNull(TEXT("the test world is available"), World)
        || !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
    {
        return true;
    }
    APrototypeCharacter* Player = SpawnTestPlayer(*World, FVector(92000.0, 73000.0, 600.0));
    if (!TestNotNull(TEXT("the player exists"), Player))
    {
        return true;
    }
    TestTrue(TEXT("gate 1 activates the DNFM mode"), Player->IsDNFMVisualActive());
    TestFalse(TEXT("the 3D mesh hides (animation keeps running)"), Player->GetMesh()->IsVisible());
    TestNotNull(TEXT("the capsule stays the gameplay body"), Player->GetCapsuleComponent());
    TestNotNull(TEXT("movement stays available"), Player->GetCharacterMovement());

    UDNFMCharacterVisualComponent* Visual = Player->GetDNFMVisual();
    if (!TestNotNull(TEXT("the DNFM component exists"), Visual))
    {
        return true;
    }
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    Visual->SetVisualSet(Set);
    Visual->SetWorldScaleFromStandingHeight(320.0f, 180.0f);
    TestTrue(TEXT("the calibration derives the shared cm/pixel"),
        FMath::IsNearlyEqual(Visual->GetCmPerPixel(), 0.5625f, 1e-5f));
    if (!TestTrue(TEXT("the rest action starts"), Visual->SetAction(FName(TEXT("rest")))))
    {
        return false;
    }
    UStaticMeshComponent* Plane = Visual->GetPlaneComponent();
    if (!TestNotNull(TEXT("the picture plane exists"), Plane))
    {
        return true;
    }
    TestTrue(TEXT("the calibrated playback shows the plane"), Visual->IsPlaneVisible());
    TestTrue(TEXT("the plane has no collision (the capsule owns the body)"),
        Plane->GetCollisionEnabled() == ECollisionEnabled::NoCollision);

    // Ground placement: the plane pose equals the section 5.1 expectation for
    // the real rig basis and the actor's current facing (+1, yaw 0).
    const USideViewCameraComponent* Rig = Player->FindComponentByClass<USideViewCameraComponent>();
    if (!TestNotNull(TEXT("the side-view rig exists"), Rig))
    {
        return true;
    }
    const FVector RigRight = Rig->GetComponentTransform().GetUnitAxis(EAxis::Y);
    const FVector RigUp = Rig->GetComponentTransform().GetUnitAxis(EAxis::Z);
    const UCapsuleComponent* Capsule = Player->GetCapsuleComponent();
    const auto FootOf = [&Capsule]()
    {
        return Capsule->GetComponentLocation() - FVector(0.0f, 0.0f, Capsule->GetScaledCapsuleHalfHeight());
    };
    const FDNFMActionClip* Rest = Set->FindAction(FName(TEXT("rest")));
    const FTransform GroundExpected = ComputeFrameQuadTransform(
        FootOf(), RigRight, RigUp, 1.0f, Rest->Frames[0], Visual->GetCmPerPixel());
    const FTransform& GroundActual = Plane->GetComponentTransform();
    TestTrue(TEXT("the plane center sits on the true capsule bottom anchor"),
        GroundActual.GetTranslation().Equals(GroundExpected.GetTranslation(), 0.01f));
    TestTrue(TEXT("the plane X basis matches the facing (+1) expectation"),
        GroundActual.GetUnitAxis(EAxis::X).Equals(GroundExpected.GetUnitAxis(EAxis::X), 1e-4f));
    TestTrue(TEXT("the plane Y basis matches the camera Up expectation"),
        GroundActual.GetUnitAxis(EAxis::Y).Equals(GroundExpected.GetUnitAxis(EAxis::Y), 1e-4f));
    TestTrue(TEXT("the plane scale matches the frame size times s"),
        GroundActual.GetScale3D().Equals(GroundExpected.GetScale3D(), 1e-4f));
    TestTrue(TEXT("the plane lies in the camera picture plane (no extra yaw)"),
        FMath::Abs(FVector::DotProduct(GroundActual.GetUnitAxis(EAxis::Z),
            (RigRight ^ RigUp).GetSafeNormal())) > 0.999f);

    // Jump Z: raising the capsule raises the plane exactly as much, keeping
    // the offset relative to the foot anchor constant (the anchor follows the
    // real Z, the plan's section 5.1 requirement).
    const FVector GroundPlaneLocation = Plane->GetComponentLocation();
    const FVector GroundFoot = FootOf();
    Player->SetActorLocation(Player->GetActorLocation() + FVector(0.0f, 0.0f, 150.0f),
        /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
    Visual->TickVisual(TestFrameSeconds);
    TestTrue(TEXT("a jump lifts the plane with the real Z"),
        FMath::IsNearlyEqual(Plane->GetComponentLocation().Z - GroundPlaneLocation.Z, 150.0f, 0.1f));
    TestTrue(TEXT("the plane-to-foot offset stays constant through the jump"),
        (Plane->GetComponentLocation() - FootOf())
            .Equals(GroundPlaneLocation - GroundFoot, 0.01f));

    // The ONE facing flip: the actor yaw feeds Facing only; the plane never
    // rotates a second time and its up axis never moves.
    Player->SetActorRotation(FRotator(0.0f, 180.0f, 0.0f));
    Visual->TickVisual(TestFrameSeconds);
    TestTrue(TEXT("facing left mirrors the quad X axis once"),
        Plane->GetComponentTransform().GetUnitAxis(EAxis::X).Equals(-RigRight, 1e-4f));
    TestTrue(TEXT("facing left keeps the quad Y axis on the camera Up"),
        Plane->GetComponentTransform().GetUnitAxis(EAxis::Y).Equals(RigUp, 1e-4f));
    TestTrue(TEXT("facing left keeps the plane in the camera picture plane"),
        FMath::Abs(FVector::DotProduct(Plane->GetComponentTransform().GetUnitAxis(EAxis::Z),
            (RigRight ^ RigUp).GetSafeNormal())) > 0.999f);
    Player->SetActorRotation(FRotator::ZeroRotator);
    Visual->TickVisual(TestFrameSeconds);
    TestTrue(TEXT("facing right restores the quad X axis"),
        Plane->GetComponentTransform().GetUnitAxis(EAxis::X).Equals(RigRight, 1e-4f));

    // ResetVisual cleanup in-world.
    Visual->ResetVisual();
    TestFalse(TEXT("reset hides the plane"), Visual->IsPlaneVisible());
    TestFalse(TEXT("reset stops playback"), Visual->IsVisualActive());

    // Runtime gate teardown: the CVar flip back to 0 plus one apply pass
    // restores the 3D hero (the clean rollback path).
    Gate.Set(0);
    Player->ApplyDNFMVisualIfEnabled();
    TestFalse(TEXT("the runtime gate-off tears the mode down"), Player->IsDNFMVisualActive());
    TestTrue(TEXT("the 3D mesh returns"), Player->GetMesh()->IsVisible());
    TestFalse(TEXT("the plane stays hidden after teardown"), Visual->IsPlaneVisible());
    return true;
}

#endif
