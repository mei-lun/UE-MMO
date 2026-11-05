// M6-023: the DNFM display-target suite. Covers the task card's acceptance
// list without any imported asset (the visual sets are built from a mock
// manifest in memory; the M6-022 import owns the real data):
//   1. the attack segment table vs the frozen combat-attacks.json facts
//      (every hit-pose segment sits INSIDE the existing active window),
//   2. the display priority (death -> down/getup/hit -> attack -> jump ->
//      ground move/idle) with death preempting a running attack,
//   3. attack sampling driven by (AttackId, InstanceId, Frame) only,
//   4. the HitStop freeze/unfreeze (display holds, no frozen-span replay),
//   5. the (ActorId, AttackInstanceId, EventId) dedup incl. low-framerate
//      crossed intervals and instance switches,
//   6. cancel/interrupt/reset/death/revive leave no residue,
//   7. the component wiring (SetSources + SetDNFMVisualTarget + tick feed)
//      and the gate=0 no-op face,
//   8. the SeekToDisplayFrame primitive, and
//   9. the pawn gate binding (gate 1 binds the presenter, gate 0 unbinds).
#include "Misc/AutomationTest.h"

#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Character/DNFMCharacterVisualSet.h"
#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatPresentationComponent.h"
#include "../Combat/DNFMCombatVisualPresenter.h"
#include "../PrototypeCharacter.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M6_023_Tests
{
    // A mock manifest honoring the frozen M6-020 schema: the non-attack state
    // actions (rest/move/jump/damage1/damage2/down/overturn/die) plus the
    // attack1 clip of the plan's worked example (3 frames [100, 80, 220]).
    // attack1 frame 0 and frame 1 carry events so the walk order and the
    // dedup are observable.
    static const TCHAR* const MockManifest = TEXT(R"raw({
  "generator": "Scripts/DNFM/prepare_fighter_visual_set.py",
  "character": "fighter",
  "outfit": "0000a",
  "pages": [
    {"file": "mock_page_0.png", "sha256": "mock0", "size": [512, 512]}
  ],
  "actions": [
    {"action_id": "rest", "source_ani": "script/character/fighter/animation/rest.ani", "source_sha256": "h-rest", "loop": true,
     "frames": [
       {"index": 0, "duration_ms": 160, "page": 0, "uv_rect_px": [0, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32],
        "events": [{"type": "audio", "name": "snd_idle"}]},
       {"index": 1, "duration_ms": 160, "page": 0, "uv_rect_px": [40, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "move", "source_ani": "script/character/fighter/animation/move.ani", "source_sha256": "h-move", "loop": true,
     "frames": [
       {"index": 0, "duration_ms": 100, "page": 0, "uv_rect_px": [80, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]},
       {"index": 1, "duration_ms": 100, "page": 0, "uv_rect_px": [120, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "jump", "source_ani": "script/character/fighter/animation/jump.ani", "source_sha256": "h-jump", "loop": false,
     "frames": [
       {"index": 0, "duration_ms": 100, "page": 0, "uv_rect_px": [160, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]},
       {"index": 1, "duration_ms": 500, "page": 0, "uv_rect_px": [200, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "attack1", "source_ani": "script/character/fighter/animation/attack1.ani", "source_sha256": "h-attack1", "loop": false,
     "frames": [
       {"index": 0, "duration_ms": 100, "page": 0, "uv_rect_px": [240, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32],
        "events": [{"type": "audio", "name": "snd_swing0"}]},
       {"index": 1, "duration_ms": 80, "page": 0, "uv_rect_px": [280, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32],
        "events": [{"type": "audio", "name": "snd_swing1"}, {"type": "fx", "name": "fx_slash"}]},
       {"index": 2, "duration_ms": 220, "page": 0, "uv_rect_px": [320, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "damage1", "source_ani": "script/character/fighter/animation/damage1.ani", "source_sha256": "h-damage1", "loop": false,
     "frames": [
       {"index": 0, "duration_ms": 1000, "page": 0, "uv_rect_px": [360, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "damage2", "source_ani": "script/character/fighter/animation/damage2.ani", "source_sha256": "h-damage2", "loop": false,
     "frames": [
       {"index": 0, "duration_ms": 1000, "page": 0, "uv_rect_px": [400, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "down", "source_ani": "script/character/fighter/animation/down.ani", "source_sha256": "h-down", "loop": false,
     "frames": [
       {"index": 0, "duration_ms": 1000, "page": 0, "uv_rect_px": [0, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "overturn", "source_ani": "script/character/fighter/animation/overturn.ani", "source_sha256": "h-overturn", "loop": false,
     "frames": [
       {"index": 0, "duration_ms": 1000, "page": 0, "uv_rect_px": [40, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]},
    {"action_id": "die", "source_ani": "script/character/fighter/animation/die.ani", "source_sha256": "h-die", "loop": false,
     "frames": [
       {"index": 0, "duration_ms": 300, "page": 0, "uv_rect_px": [80, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]},
       {"index": 1, "duration_ms": 270, "page": 0, "uv_rect_px": [120, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}
     ]}
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

    /** A world-less visual component bound to the mock set (tests drive it). */
    static UDNFMCharacterVisualComponent* NewMockVisual(FAutomationTestBase& Test, UDNFMCharacterVisualSet* Set)
    {
        UDNFMCharacterVisualComponent* Visual = NewObject<UDNFMCharacterVisualComponent>(GetTransientPackage());
        Visual->SetVisualSet(Set);
        return Visual;
    }

    /** Fact factory for the presenter unit tests. */
    static FDNFMCombatVisualPresenter::FVisualFacts MakeFacts(ECombatActionState State,
        FName AttackId, uint64 InstanceId, int32 Frame,
        bool bDead = false, bool bAirborne = false, bool bGroundMoving = false)
    {
        FDNFMCombatVisualPresenter::FVisualFacts Facts;
        Facts.Combat.ActionState = State;
        Facts.Combat.AttackId = AttackId;
        Facts.Combat.InstanceId = InstanceId;
        Facts.Combat.Frame = Frame;
        Facts.bDead = bDead;
        Facts.bAirborne = bAirborne;
        Facts.bGroundMoving = bGroundMoving;
        return Facts;
    }

    // The frozen combat-attacks.json facts the mapping table must respect
    // (display only; the file itself is never read or written at runtime).
    struct FFrozenAttackFact
    {
        FName AttackId;
        int32 DurationFrames;
        int32 ActiveStart;
        int32 ActiveEnd;
    };

    static const TArray<FFrozenAttackFact>& FrozenFacts()
    {
        static const TArray<FFrozenAttackFact> Facts = {
            {FName(TEXT("light_01")), 26, 7, 11},
            {FName(TEXT("light_02")), 32, 9, 14},
            {FName(TEXT("launcher")), 40, 12, 17},
            {FName(TEXT("aerial_01")), 28, 6, 10},
            {FName(TEXT("singlekick")), 24, 11, 15},
            {FName(TEXT("damagelowkick")), 24, 11, 15},
            {FName(TEXT("throwsand")), 28, 13, 17},
            {FName(TEXT("earthbreak")), 30, 11, 14},
            {FName(TEXT("stomp")), 22, 11, 14}
        };
        return Facts;
    }

    /** The bare combat rig the attack-driven tests share (no world needed). */
    struct FCombatRig
    {
        UAttackCatalog* Catalog = nullptr;
        UCombatComponent* Combat = nullptr;
        UDNFMCharacterVisualSet* Set = nullptr;
        UDNFMCharacterVisualComponent* Visual = nullptr;
        FDNFMCombatVisualPresenter* Presenter = nullptr;
        TUniquePtr<FDNFMCombatVisualPresenter> PresenterOwner;

        bool Build(FAutomationTestBase& Test)
        {
            Set = BuildMockSet(Test, Error);
            if (Set == nullptr)
            {
                return false;
            }
            UAttackDefinition* Light01 = NewObject<UAttackDefinition>();
            Light01->AttackId = FName(TEXT("light_01"));
            Light01->DurationFrames = 26;
            Light01->ActiveWindow.StartFrame = 7;
            Light01->ActiveWindow.EndFrame = 11;
            UAttackDefinition* Light02 = NewObject<UAttackDefinition>();
            Light02->AttackId = FName(TEXT("light_02"));
            Light02->DurationFrames = 32;
            Light02->ActiveWindow.StartFrame = 9;
            Light02->ActiveWindow.EndFrame = 14;
            Catalog = NewObject<UAttackCatalog>();
            TArray<UAttackDefinition*> Definitions = {Light01, Light02};
            FText CatalogError;
            if (!Test.TestTrue(TEXT("the probe catalog builds"),
                Catalog->BuildFromDefinitions(Definitions, CatalogError)))
            {
                Test.AddError(FString::Printf(TEXT("probe catalog failed: %s"), *CatalogError.ToString()));
                return false;
            }
            Combat = NewObject<UCombatComponent>();
            Combat->InitializeFromCatalog(Catalog);
            Visual = NewMockVisual(Test, Set);
            PresenterOwner = MakeUnique<FDNFMCombatVisualPresenter>();
            Presenter = PresenterOwner.Get();
            Presenter->SetVisualTarget(Visual);
            return true;
        }

        /** Advances one 60 Hz logic frame and re-syncs the display. */
        void StepLogicFrame()
        {
            Combat->TickCombat(1.0f / 60.0f);
            SyncDisplay();
        }

        /** The presenter Step from the live combat snapshot (no other facts). */
        void SyncDisplay()
        {
            FDNFMCombatVisualPresenter::FVisualFacts Facts;
            Facts.Combat = Combat->GetSnapshot();
            Facts.bDead = Combat->IsDead();
            Presenter->Step(Facts);
        }

        FString Error;
    };
}

using namespace UE::UEMMO::Tasks::M6_023;
using namespace UE::UEMMO::Tasks::M6_023_Tests;

// 1) The frozen mapping table: every hit-pose segment covers its attack's
// active window exactly, and the segments tile [0, DurationFrames) contiguously.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_MappingTableMatchesActiveWindows,
    "UEMMO.Tasks.M6_023.MappingTableMatchesActiveWindows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_MappingTableMatchesActiveWindows::RunTest(const FString& Parameters)
{
    const TArray<FDNFMAttackDisplayMapping> Table = MakeDefaultAttackDisplayMappings();
    TestEqual(TEXT("one mapping row per known attack"), Table.Num(), FrozenFacts().Num());

    for (const FFrozenAttackFact& Fact : FrozenFacts())
    {
        const FDNFMAttackDisplayMapping* Mapping = FindAttackDisplayMapping(Table, Fact.AttackId);
        if (!TestNotNull(FString::Printf(TEXT("%s has a mapping row"), *Fact.AttackId.ToString()), Mapping))
        {
            continue;
        }
        // Coverage: the segments tile [0, DurationFrames) without holes.
        int32 ExpectedStart = 0;
        for (const FDNFMAttackDisplaySegment& Row : Mapping->Segments)
        {
            TestEqual(FString::Printf(TEXT("%s segment starts at %d"), *Fact.AttackId.ToString(), ExpectedStart),
                Row.LogicStartFrame, ExpectedStart);
            TestTrue(FString::Printf(TEXT("%s segment is non-empty"), *Fact.AttackId.ToString()),
                Row.LogicEndFrame > Row.LogicStartFrame);
            ExpectedStart = Row.LogicEndFrame;
        }
        TestEqual(FString::Printf(TEXT("%s segments cover the full duration"), *Fact.AttackId.ToString()),
            ExpectedStart, Fact.DurationFrames);

        // The hard constraint: the hit-pose row IS the active window, so the
        // visual hit pose always falls inside the existing damage window.
        bool bFoundHitRow = false;
        for (const FDNFMAttackDisplaySegment& Row : Mapping->Segments)
        {
            if (Row.bHitPose)
            {
                TestFalse(FString::Printf(TEXT("%s has at most one hit row"), *Fact.AttackId.ToString()), bFoundHitRow);
                TestEqual(FString::Printf(TEXT("%s hit row starts at the active window"), *Fact.AttackId.ToString()),
                    Row.LogicStartFrame, Fact.ActiveStart);
                TestEqual(FString::Printf(TEXT("%s hit row ends at the active window"), *Fact.AttackId.ToString()),
                    Row.LogicEndFrame, Fact.ActiveEnd);
                bFoundHitRow = true;
            }
        }
        TestTrue(FString::Printf(TEXT("%s declares its hit pose"), *Fact.AttackId.ToString()), bFoundHitRow);

        // Display frame indices stay inside the source clip's frame count
        // (the M6-019 frame-count facts the placeholders encode).
        for (const FDNFMAttackDisplaySegment& Row : Mapping->Segments)
        {
            TestTrue(FString::Printf(TEXT("%s row's display frame is non-negative"), *Fact.AttackId.ToString()),
                Row.DisplayFrameIndex >= 0);
        }
    }

    // The pure sampling: clamped at both ends, exact inside the rows.
    const FDNFMAttackDisplayMapping* Light01 = FindAttackDisplayMapping(Table, FName(TEXT("light_01")));
    if (TestNotNull(TEXT("light_01 has a mapping row"), Light01))
    {
        TestEqual(TEXT("pre-first-tick Frame (-1) reads as the first row"),
            FindDisplayFrameForLogicFrame(*Light01, -1), 0);
        TestEqual(TEXT("frame 6 is still the windup"), FindDisplayFrameForLogicFrame(*Light01, 6), 0);
        TestEqual(TEXT("frame 7 is the hit pose"), FindDisplayFrameForLogicFrame(*Light01, 7), 1);
        TestEqual(TEXT("frame 10 is still the hit pose"), FindDisplayFrameForLogicFrame(*Light01, 10), 1);
        TestEqual(TEXT("frame 11 is the recovery"), FindDisplayFrameForLogicFrame(*Light01, 11), 2);
        TestEqual(TEXT("an overrun clamps to the last row"), FindDisplayFrameForLogicFrame(*Light01, 999), 2);
    }
    FDNFMAttackDisplayMapping Empty;
    TestEqual(TEXT("an empty mapping samples to none"), FindDisplayFrameForLogicFrame(Empty, 5), INDEX_NONE);
    TestEqual(TEXT("an unknown attack id has no mapping"),
        FindAttackDisplayMapping(Table, FName(TEXT("not_an_attack"))) == nullptr, true);
    return true;
}

// 2) The display priority (plan 6.1): death -> down/getup/hit -> attack ->
// jump -> ground move/idle; death preempts a running attack; death holds its
// end frame; the revive edge clears the old display instance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_DisplayStatePriority,
    "UEMMO.Tasks.M6_023.DisplayStatePriority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_DisplayStatePriority::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    // Ground idle / move / jump baselines.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("a free grounded owner idles"), Visual->GetCurrentActionId(), IdleActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, true));
    TestEqual(TEXT("a moving owner plays the move loop"), Visual->GetCurrentActionId(), MoveActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false));
    TestEqual(TEXT("an airborne owner plays the jump clip"), Visual->GetCurrentActionId(), JumpActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("landing returns to idle"), Visual->GetCurrentActionId(), IdleActionId);

    // The reaction states (each plays once and the state owns its end).
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("hit stun shows the hit pose"), Visual->GetCurrentActionId(), HitActionId);
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("a repeated stun sync never restarts the hit pose"), Visual->GetCurrentActionId(), HitActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Knockdown, NAME_None, 0, -1));
    TestEqual(TEXT("knockdown shows the down pose"), Visual->GetCurrentActionId(), DownActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    TestEqual(TEXT("recovering shows the getup pose"), Visual->GetCurrentActionId(), RecoverActionId);

    // Death preempts everything and holds its end frame.
    Presenter.Step(MakeFacts(ECombatActionState::Attacking, FName(TEXT("light_01")), 7, 8, true));
    TestEqual(TEXT("death preempts a running attack"), Visual->GetCurrentActionId(), DeathActionId);
    Visual->TickVisual(5.0f); // way past the 570 ms die clip
    TestEqual(TEXT("the death clip holds its end frame"), Visual->GetCurrentFrameIndex(), 1);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, true));
    TestEqual(TEXT("a steady death never restarts the clip"), Visual->GetCurrentActionId(), DeathActionId);
    TestEqual(TEXT("the death end frame stays held"), Visual->GetCurrentFrameIndex(), 1);

    // The revive edge: the old display instance dies entirely and the same
    // sync rebuilds the locomotion display from scratch (frame 0, not the
    // death end frame).
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false));
    TestEqual(TEXT("a revived owner idles again"), Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the revived display restarts at frame 0"), Visual->GetCurrentFrameIndex(), 0);

    // Reaction attacks are refused by combat, but the display also honors the
    // priority: an attack snapshot under a reaction state is not reachable
    // (the state switch above covers it); the attack display is tested next.
    return true;
}

// 3) Attack sampling: the displayed frame is a pure function of the combat
// 60 Hz logic frame; the instance id switches cleanly between two instances.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_AttackSamplingFollowsLogicFrame,
    "UEMMO.Tasks.M6_023.AttackSamplingFollowsLogicFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_AttackSamplingFollowsLogicFrame::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 FirstInstance = Combat->GetSnapshot().InstanceId;
    TestTrue(TEXT("instance ids are minted from 1"), FirstInstance >= 1);
    Rig.SyncDisplay();
    TestEqual(TEXT("the attack display uses the mapped source clip"),
        Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    TestEqual(TEXT("the presenter presents the combat instance"),
        Presenter->GetPresentedInstanceId(), FirstInstance);

    // Frames 0..6: windup pose; 7..10: hit pose; 11..24: recovery (the step
    // that would land frame 25 = DurationFrames-1 ends the attack).
    for (int32 LogicFrame = 0; LogicFrame < 25; ++LogicFrame)
    {
        Rig.StepLogicFrame(); // this tick lands on LogicFrame
        const int32 ExpectedDisplay = (LogicFrame < 7) ? 0 : (LogicFrame < 11) ? 1 : 2;
        TestEqual(FString::Printf(TEXT("logic frame %d shows display frame %d"), LogicFrame, ExpectedDisplay),
            Visual->GetCurrentFrameIndex(), ExpectedDisplay);
        TestEqual(TEXT("the attack display is still presenting"),
            Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    }
    Rig.StepLogicFrame(); // the step that reaches DurationFrames-1 finishes
    TestEqual(TEXT("the attack returned to Free"), Combat->GetSnapshot().ActionState, ECombatActionState::Free);
    Rig.SyncDisplay();
    TestEqual(TEXT("a finished attack returns to the idle display"), Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the presented instance dropped to non-attack"), Presenter->GetPresentedInstanceId(), 0ull);

    // A second instance of the same attack: a fresh display instance with its
    // own event space (no residue from the first).
    TestTrue(TEXT("light_01 starts again"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 SecondInstance = Combat->GetSnapshot().InstanceId;
    TestTrue(TEXT("instance ids never repeat"), SecondInstance > FirstInstance);
    Rig.SyncDisplay();
    TestEqual(TEXT("the new instance presents the attack clip again"),
        Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    TestEqual(TEXT("the presenter follows the new instance"),
        Presenter->GetPresentedInstanceId(), SecondInstance);
    TestEqual(TEXT("the new instance restarts at the first display frame"),
        Visual->GetCurrentFrameIndex(), 0);

    // Wall time never moves the attack display: a huge TickVisual on the
    // attack clip changes nothing (the logic frame owns the time).
    Visual->TickVisual(10.0f);
    TestEqual(TEXT("real time cannot advance the attack display"),
        Visual->GetCurrentFrameIndex(), 0);
    return true;
}

// 4) The HitStop freeze: real-time display holds while frozen, no frozen-span
// event replay when it lifts; a presented attack keeps its time external.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_HitStopFreezesDisplay,
    "UEMMO.Tasks.M6_023.HitStopFreezesDisplay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_HitStopFreezesDisplay::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    // A real-time clip: the idle loop freezes and resumes without replay.
    Rig.SyncDisplay(); // Free -> rest
    TestEqual(TEXT("idle plays the rest loop"), Visual->GetCurrentActionId(), IdleActionId);
    Presenter->NotifyHitStopFrozen(true);
    TestTrue(TEXT("the freeze reaches the visual playback"), Visual->IsPlaybackFrozen());
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("a frozen idle never advances"), Visual->GetCurrentFrameIndex(), 0);
    Presenter->NotifyHitStopFrozen(false);
    TestFalse(TEXT("the unfreeze reaches the visual playback"), Visual->IsPlaybackFrozen());
    Visual->TickVisual(0.17); // past the 160 ms first frame
    TestEqual(TEXT("the unfrozen idle resumes from the frozen point"),
        Visual->GetCurrentFrameIndex(), 1);

    // An attack display: the time is external for the whole instance, so the
    // display frame moves only with the logic frame - a frozen or unfrozen
    // TickVisual can never drift it, and no frozen-span event can replay.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    Presenter->NotifyHitStopFrozen(true);
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("a frozen attack display holds its frame"),
        Visual->GetCurrentFrameIndex(), 0);
    Presenter->NotifyHitStopFrozen(false);
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("an attack display ignores wall time even unfrozen"),
        Visual->GetCurrentFrameIndex(), 0);
    // The logic frame advances the pose again after the freeze.
    Rig.StepLogicFrame(); // frame 0
    Rig.StepLogicFrame(); // frame 1
    TestEqual(TEXT("the logic frame still drives the pose after the freeze"),
        Visual->GetCurrentFrameIndex(), 0);
    return true;
}

// 5) The event dedup: the (ActorId, AttackInstanceId, EventId) tuple fires
// once per instance; low-framerate logic jumps walk every crossed display
// frame in order; instance switches re-arm the key space.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_EventDedupAndCrossedIntervals,
    "UEMMO.Tasks.M6_023.EventDedupAndCrossedIntervals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_EventDedupAndCrossedIntervals::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    // (a) Loop re-crossings dedup: the idle loop wraps its frame-0 event, but
    // the presenter dispatches it exactly once for the session-scoped display.
    Rig.SyncDisplay();
    Visual->TickVisual(0.16 * 6); // six rest cycles
    int32 IdleEvents = 0;
    for (const FDNFMRecordedVisualEvent& Record : Presenter->GetDedupedEventHistory())
    {
        if (Record.ActionName == IdleActionId)
        {
            ++IdleEvents;
        }
    }
    TestEqual(TEXT("the looping idle's event dispatched exactly once"), IdleEvents, 1);

    // (b) A low-framerate logic jump: one 13-frame combat step crosses both
    // the windup->hit and hit->recovery boundaries in a single sync.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 FirstInstance = Combat->GetSnapshot().InstanceId;
    Rig.SyncDisplay(); // enter at frame -1 -> display 0 (its event fires now)
    Combat->TickCombat(13.0f / 60.0f); // -1 -> frame 12 in ONE step
    Rig.SyncDisplay();                 // the seek walks display 1 then 2
    int32 Swing0 = 0;
    int32 Swing1 = 0;
    int32 Slash = 0;
    for (const FDNFMRecordedVisualEvent& Record : Presenter->GetDedupedEventHistory())
    {
        if (Record.ActionName != FName(TEXT("attack1")))
        {
            continue;
        }
        if (Record.EventName == FName(TEXT("snd_swing0"))) { ++Swing0; }
        if (Record.EventName == FName(TEXT("snd_swing1"))) { ++Swing1; }
        if (Record.EventName == FName(TEXT("fx_slash"))) { ++Slash; }
    }
    TestEqual(TEXT("the frame-0 event fired once on entry"), Swing0, 1);
    TestEqual(TEXT("the crossed frame-1 audio fired exactly once"), Swing1, 1);
    TestEqual(TEXT("the crossed frame-1 fx fired exactly once"), Slash, 1);

    // (c) An instance switch re-arms the key space: the same events dispatch
    // again under the new instance id, and the recorded rows carry it.
    for (int32 Frame = 0; Frame < 20; ++Frame)
    {
        Rig.StepLogicFrame(); // run the first instance out (frame 11 -> Free)
    }
    TestTrue(TEXT("light_02 chains"), Combat->TryStartAttack(FName(TEXT("light_02")), 1));
    // light_02 maps to attack2, which the mock set does not carry: the honest
    // fallback display runs and the attack1 events cannot leak into it.
    Rig.SyncDisplay();
    TestEqual(TEXT("an unmapped-in-set attack falls back to the idle display"),
        Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the fallback drops the presented instance"),
        Presenter->GetPresentedInstanceId(), 0ull);
    for (int32 Guard = 0; Guard < 64 && Combat->GetSnapshot().ActionState == ECombatActionState::Attacking; ++Guard)
    {
        Rig.StepLogicFrame(); // run the light_02 instance out
    }

    TestTrue(TEXT("light_01 starts once more"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 ThirdInstance = Combat->GetSnapshot().InstanceId;
    TestTrue(TEXT("the third instance is fresh"), ThirdInstance > FirstInstance);
    Rig.SyncDisplay();
    // After the third entry the frame-0 event exists for both instances.
    int32 Swing0First = 0;
    int32 Swing0Third = 0;
    for (const FDNFMRecordedVisualEvent& Record : Presenter->GetDedupedEventHistory())
    {
        if (Record.ActionName == FName(TEXT("attack1")) && Record.EventName == FName(TEXT("snd_swing0")))
        {
            if (Record.AttackInstanceId == FirstInstance) { ++Swing0First; }
            if (Record.AttackInstanceId == ThirdInstance) { ++Swing0Third; }
        }
    }
    TestEqual(TEXT("instance 1's swing0 dispatched once"), Swing0First, 1);
    TestEqual(TEXT("instance 3's swing0 dispatched once (no residue either way)"), Swing0Third, 1);
    return true;
}

// 6) Cancel/interrupt/reset/death leave no residue on the display or the
// event space.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_CancelInterruptResetNoResidue,
    "UEMMO.Tasks.M6_023.CancelInterruptResetNoResidue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_CancelInterruptResetNoResidue::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    // Cancel (no OnFinished): the display leaves the attack immediately.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    TestEqual(TEXT("the attack display runs"), Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    Combat->CancelCurrentAttack(FName(TEXT("M6_023_test")));
    Rig.SyncDisplay();
    TestEqual(TEXT("a cancelled attack returns to idle display"), Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the cancelled instance is forgotten"), Presenter->GetPresentedInstanceId(), 0ull);

    // ResetCombat (the teardown face): same clean exit.
    TestTrue(TEXT("light_01 restarts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    Combat->ResetCombat();
    Rig.SyncDisplay();
    TestEqual(TEXT("a reset teardown returns to idle display"), Visual->GetCurrentActionId(), IdleActionId);

    // Death: the die clip replaces the attack and holds; the facts-driven
    // revive edge clears it (the health lifecycle owns the revival).
    TestTrue(TEXT("light_01 starts for the death case"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    Combat->SetDead(true);
    Rig.SyncDisplay();
    TestEqual(TEXT("death preempts the attack display"), Visual->GetCurrentActionId(), DeathActionId);
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("the death display holds its end frame"), Visual->GetCurrentFrameIndex(), 1);
    Combat->SetDead(false); // a revival (ResetHealth) opened a new life
    Rig.SyncDisplay();
    // The in-flight attack fact survives a revival (death never interrupted
    // it): the display re-enters the mapped clip fresh instead of resuming
    // the death frame.
    TestEqual(TEXT("the revived display re-enters the running attack"),
        Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    TestEqual(TEXT("the re-entered display restarts at the mapped frame"),
        Visual->GetCurrentFrameIndex(), 0);
    for (int32 Guard = 0; Guard < 64 && Combat->GetSnapshot().ActionState == ECombatActionState::Attacking; ++Guard)
    {
        Rig.StepLogicFrame();
    }
    TestEqual(TEXT("the finished attack returns to idle display"), Visual->GetCurrentActionId(), IdleActionId);
    return true;
}

// 7) The component wiring: SetSources + SetDNFMVisualTarget + TickComponent
// feed the coordinator; the gate=0 face (no target bound) is a full no-op and
// the 3D montage seams keep dispatching.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_PresentationComponentWiring,
    "UEMMO.Tasks.M6_023.PresentationComponentWiring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_PresentationComponentWiring::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UAttackDefinition* Light01 = NewObject<UAttackDefinition>();
    Light01->AttackId = FName(TEXT("light_01"));
    Light01->DurationFrames = 26;
    Light01->ActiveWindow.StartFrame = 7;
    Light01->ActiveWindow.EndFrame = 11;
    UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
    FText CatalogError;
    if (!TestTrue(TEXT("the probe catalog builds"),
        Catalog->BuildFromDefinitions({Light01}, CatalogError)))
    {
        return false;
    }
    UCombatComponent* Combat = NewObject<UCombatComponent>();
    Combat->InitializeFromCatalog(Catalog);
    UCombatPresentationComponent* Presentation = NewObject<UCombatPresentationComponent>();
    Presentation->SetSources(Combat, nullptr); // null mesh: the 3D path no-ops safely
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    Presentation->SetDNFMVisualTarget(Visual);
    TestTrue(TEXT("the presenter binds the picture surface"),
        Presentation->GetDNFMVisualPresenter().GetVisualTarget() == Visual);

    // Fact provider injection: the bare component has no owner movement, so
    // the tests pin the airborne/ground-move facts explicitly.
    bool bAirborne = false;
    bool bGroundMoving = false;
    Presentation->SetDNFMFactProviders(
        [&bAirborne]() { return bAirborne; },
        [&bGroundMoving]() { return bGroundMoving; });

    const auto Tick = [&](float DeltaSeconds)
    {
        Presentation->TickComponent(DeltaSeconds, LEVELTICK_All, nullptr);
    };

    // Idle by default; the injected move fact switches the loop.
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the tick feed idles the display"), Visual->GetCurrentActionId(), IdleActionId);
    bGroundMoving = true;
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the tick feed moves the display"), Visual->GetCurrentActionId(), MoveActionId);
    bGroundMoving = false;
    bAirborne = true;
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the tick feed jumps the display"), Visual->GetCurrentActionId(), JumpActionId);
    bAirborne = false;

    // The attack sampling rides the real delegates (OnStarted) + tick sync.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    for (int32 Frame = -1; Frame < 8; ++Frame)
    {
        Tick(1.0f / 60.0f);
        Combat->TickCombat(1.0f / 60.0f);
    }
    TestEqual(TEXT("the tick-fed display reaches the hit pose"),
        Visual->GetCurrentFrameIndex(), 1);

    // The HitStop event face: RequestHitStop broadcasts and the presenter
    // freezes the real-time clips (the 3D pause seam still records too).
    Combat->CancelCurrentAttack(FName(TEXT("M6_023_wiring")));
    Tick(1.0f / 60.0f); // back to the idle display
    Combat->RequestHitStop(0.05f);
    TestTrue(TEXT("the hit stop freezes the DNFM playback"), Visual->IsPlaybackFrozen());
    TestTrue(TEXT("the 3D hit stop seam still dispatches"),
        Presentation->GetHitStopPauseDispatchHistory().Contains(true));
    Visual->TickVisual(1.0f);
    TestEqual(TEXT("the frozen idle holds"), Visual->GetCurrentFrameIndex(), 0);
    for (int32 Step = 0; Step < 6 && Combat->IsHitStopActive(); ++Step)
    {
        Combat->TickCombat(1.0f / 60.0f); // real-time countdown, no world needed
    }
    TestFalse(TEXT("the hit stop ended"), Combat->IsHitStopActive());
    Tick(1.0f / 60.0f);
    TestFalse(TEXT("the DNFM playback unfreezes with the hit stop"), Visual->IsPlaybackFrozen());

    // The gate=0 no-op face: an unbound presentation never touches the visual.
    UCombatPresentationComponent* GateOff = NewObject<UCombatPresentationComponent>();
    GateOff->SetSources(Combat, nullptr);
    UDNFMCharacterVisualComponent* UntouchedVisual = NewMockVisual(*this, Set);
    TestTrue(TEXT("an unbound presentation is the no-op face"),
        GateOff->GetDNFMVisualPresenter().GetVisualTarget() == nullptr);
    GateOff->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    TestFalse(TEXT("the gate-off path never starts a display"), UntouchedVisual->IsVisualActive());
    TestTrue(TEXT("light_01 starts under the gate-off presentation"),
        Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    GateOff->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    Combat->TickCombat(1.0f / 60.0f);
    GateOff->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    TestFalse(TEXT("the gate-off path still never starts a display"), UntouchedVisual->IsVisualActive());
    return true;
}

// 8) The SeekToDisplayFrame primitive: forward walks fire crossed events in
// order, backward walks stay silent, elapsed pins to the target frame start,
// and a frozen playback still accepts explicit positions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_SeekToDisplayFramePrimitive,
    "UEMMO.Tasks.M6_023.SeekToDisplayFramePrimitive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_SeekToDisplayFramePrimitive::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    TArray<FString> Fired;
    Visual->OnVisualFrameEvent.AddLambda([&Fired](uint64, FName Action, int32 Frame, FName, FName Event)
    {
        Fired.Add(FString::Printf(TEXT("%s|%d|%s"), *Action.ToString(), Frame, *Event.ToString()));
    });

    TestFalse(TEXT("a seek without playback refuses"), Visual->SeekToDisplayFrame(0));
    TestTrue(TEXT("attack1 starts"), Visual->SetAction(FName(TEXT("attack1"))));
    Fired.Reset(); // the SetAction frame-0 events are the M6-021 contract, not this test's subject

    TestTrue(TEXT("a seek to the current frame is idempotent"), Visual->SeekToDisplayFrame(0));
    TestEqual(TEXT("the idempotent seek fired nothing"), Fired.Num(), 0);
    TestFalse(TEXT("an out-of-range seek refuses"), Visual->SeekToDisplayFrame(3));
    TestEqual(TEXT("the refused seek fired nothing"), Fired.Num(), 0);

    // Forward 0 -> 2 walks frames 1 and 2 in order (frame 1 carries two
    // events; frame 2 carries none).
    TestTrue(TEXT("the forward seek lands"), Visual->SeekToDisplayFrame(2));
    TestEqual(TEXT("the walk reported frame 1's two events"), Fired.Num(), 2);
    if (Fired.Num() == 2)
    {
        TestEqual(TEXT("the walk order is the manifest order"), Fired[0], FString(TEXT("attack1|1|snd_swing1")));
        TestEqual(TEXT("the second event row follows"), Fired[1], FString(TEXT("attack1|1|fx_slash")));
    }
    // Elapsed pinned to the target frame start (220 ms frame): a short
    // real-time advance stays, a full one crosses the end hold (frame 2).
    Visual->TickVisual(0.1);
    TestEqual(TEXT("the pinned elapsed continues from the seeked frame"),
        Visual->GetCurrentFrameIndex(), 2);

    // Backward seek on a once-clip: the frame applies without fake re-fires.
    Fired.Reset();
    TestTrue(TEXT("the backward seek applies"), Visual->SeekToDisplayFrame(0));
    TestEqual(TEXT("a backward seek fires nothing"), Fired.Num(), 0);
    TestEqual(TEXT("the backward seek moved the frame"), Visual->GetCurrentFrameIndex(), 0);

    // A frozen playback accepts explicit positions (time is frozen, not the
    // display): the presenter can switch poses during a freeze.
    Visual->SetPlaybackFrozen(true);
    Fired.Reset();
    TestTrue(TEXT("a frozen seek still lands"), Visual->SeekToDisplayFrame(1));
    TestEqual(TEXT("the frozen seek fired the crossed frame's events"), Fired.Num(), 2);
    TestEqual(TEXT("the frozen seek moved the frame"), Visual->GetCurrentFrameIndex(), 1);
    // And the frozen self-tick does not move past it.
    Visual->TickVisual(1.0f);
    TestEqual(TEXT("the frozen playback never self-advances"), Visual->GetCurrentFrameIndex(), 1);
    Visual->SetPlaybackFrozen(false);
    return true;
}

// 9) The pawn gate binding: gate 1 binds the presenter target, gate 0
// unbinds it and restores the 3D hero (the M6-021 teardown keeps its shape).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_PawnGateBindsPresenter,
    "UEMMO.Tasks.M6_023.PawnGateBindsPresenter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_PawnGateBindsPresenter::RunTest(const FString& Parameters)
{
    IConsoleVariable* Gate = IConsoleManager::Get().FindConsoleVariable(TEXT("UEMMO.PlayerDNFMVisual"));
    if (!TestNotNull(TEXT("the DNFM gate exists"), Gate))
    {
        return true;
    }
    const int32 Original = Gate->GetInt();
    Gate->Set(1, ECVF_SetByCode);
    FTestWorldWrapper Wrapper;
    if (!TestTrue(TEXT("the manually ticked test world is created"),
        Wrapper.CreateTestWorld(EWorldType::Game)))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    UWorld* World = Wrapper.GetTestWorld();
    if (!TestNotNull(TEXT("the test world is available"), World)
        || !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    FActorSpawnParameters Params;
    APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
        APrototypeCharacter::StaticClass(), FVector(96000.0, 75000.0, 600.0), FRotator::ZeroRotator, Params);
    if (!TestNotNull(TEXT("the player exists"), Player))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    UCombatPresentationComponent* Presentation = Player->FindComponentByClass<UCombatPresentationComponent>();
    if (!TestNotNull(TEXT("the pawn carries the presentation component"), Presentation))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    TestTrue(TEXT("gate 1 activated the DNFM mode"), Player->IsDNFMVisualActive());
    TestTrue(TEXT("the pawn gate bound the presenter target"),
        Presentation->GetDNFMVisualTarget() == Player->GetDNFMVisual());

    // The runtime teardown unbinds the presenter and restores the 3D hero.
    Gate->Set(0, ECVF_SetByCode);
    Player->ApplyDNFMVisualIfEnabled();
    TestFalse(TEXT("the runtime gate-off tears the mode down"), Player->IsDNFMVisualActive());
    TestTrue(TEXT("the gate-off unbound the presenter target"),
        Presentation->GetDNFMVisualTarget() == nullptr);
    TestTrue(TEXT("the 3D mesh returns"), Player->GetMesh()->IsVisible());
    Gate->Set(Original, ECVF_SetByCode);
    return true;
}

#endif
