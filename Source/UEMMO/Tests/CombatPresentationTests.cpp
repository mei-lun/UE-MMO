#include "Misc/AutomationTest.h"

#include "CombatPresentationTestDoubles.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// M1-032: attack montage playback ownership tests.
// - pure play-rate mapping (card step 1),
// - asset persistence + total logic duration of the four generated montages,
// - dispatch semantics (Started/Finished/Reset/instance switch) verified on
//   the URecordingPresentation seam, not real playback,
// - crash safety with a null mesh / AnimInstance.
namespace UE::UEMMO::Tasks::M1_032
{
	// Ids stay TCHAR literals at namespace scope; FNames are only built inside
	// test bodies (function-local), never before the name pool exists.
	const TCHAR* const ProbeAttackIdText = TEXT("probe_strike");
	const TCHAR* const LightAttackIdText = TEXT("light_01");

	constexpr float RateTolerance = 0.001f;
	constexpr float DurationTolerance = 0.0001f;
	// Card acceptance: total logic duration within 10% of DurationFrames / 60.
	constexpr float LogicDurationRelativeTolerance = 0.10f;

	// The four montage object paths follow MNT_<AttackId> from Data/
	// combat-attacks.json attack ids (M1-008 contract values).
	const TCHAR* const MontageObjectPaths[] =
	{
		TEXT("/Game/UEMMO/Animation/Montages/MNT_light_01.MNT_light_01"),
		TEXT("/Game/UEMMO/Animation/Montages/MNT_light_02.MNT_light_02"),
		TEXT("/Game/UEMMO/Animation/Montages/MNT_launcher.MNT_launcher"),
		TEXT("/Game/UEMMO/Animation/Montages/MNT_aerial_01.MNT_aerial_01")
	};

	const TCHAR* const MontageLabels[] =
	{
		TEXT("MNT_light_01"),
		TEXT("MNT_light_02"),
		TEXT("MNT_launcher"),
		TEXT("MNT_aerial_01")
	};

	const TCHAR* const DefinitionObjectPaths[] =
	{
		TEXT("/Game/UEMMO/Combat/Definitions/DA_light_01.DA_light_01"),
		TEXT("/Game/UEMMO/Combat/Definitions/DA_light_02.DA_light_02"),
		TEXT("/Game/UEMMO/Combat/Definitions/DA_launcher.DA_launcher"),
		TEXT("/Game/UEMMO/Combat/Definitions/DA_aerial_01.DA_aerial_01")
	};

	// Bundle of the synthetic combat setup used by the dispatch tests: one
	// 60-frame probe attack in a catalog, one combat component driving it, and
	// one recording presenter bound to it (null mesh: playback is recorded).
	struct FProbeSession
	{
		UAttackCatalog* Catalog = nullptr;
		UAttackDefinition* Definition = nullptr;
		UCombatComponent* Combat = nullptr;
		URecordingPresentation* Presentation = nullptr;
	};

	FProbeSession NewProbeSession(FAutomationTestBase& Test, int32 ProbeDurationFrames)
	{
		FProbeSession Session;
		Session.Definition = NewObject<UAttackDefinition>();
		Session.Definition->AttackId = FName(ProbeAttackIdText);
		Session.Definition->DurationFrames = ProbeDurationFrames;

		Session.Catalog = NewObject<UAttackCatalog>();
		TArray<UAttackDefinition*> Definitions;
		Definitions.Add(Session.Definition);
		FText CatalogError;
		if (!Session.Catalog->BuildFromDefinitions(Definitions, CatalogError))
		{
			Test.AddError(FString::Printf(TEXT("probe catalog build failed: %s"), *CatalogError.ToString()));
		}

		Session.Combat = NewObject<UCombatComponent>();
		Session.Combat->InitializeFromCatalog(Session.Catalog);

		Session.Presentation = NewObject<URecordingPresentation>();
		Session.Presentation->ProbeDefinition = Session.Definition;
		Session.Presentation->TokenMontage = NewObject<UAnimMontage>();
		Session.Presentation->SetSources(Session.Combat, nullptr);
		return Session;
	}
}

using namespace UE::UEMMO::Tasks::M1_032;

void URecordingPresentation::PlayAttackMontage(FName AttackId, UAnimMontage* Montage, float PlayRate)
{
	PlayedIds.Add(AttackId);
	PlayedRates.Add(PlayRate);
}

void URecordingPresentation::StopAttackMontage(FName AttackId, UAnimMontage* Montage)
{
	StoppedIds.Add(AttackId);
}

UAnimMontage* URecordingPresentation::FindMontageForAttack(FName AttackId)
{
	return (AttackId == FName(ProbeAttackIdText)) ? TokenMontage.Get() : Super::FindMontageForAttack(AttackId);
}

const UAttackDefinition* URecordingPresentation::FindDefinitionForAttack(FName AttackId)
{
	return (AttackId == FName(ProbeAttackIdText)) ? ProbeDefinition.Get() : Super::FindDefinitionForAttack(AttackId);
}

// Pure mapping (card step 1): 26 frames at 60 Hz = 0.4333 s of logic time, so
// a 1 s montage must play at ~2.3077x to finish with the attack clock.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_PlayRateMapsMontageToDurationFrames,
	"UEMMO.Tasks.M1_032.PlayRateMapsMontageToDurationFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_PlayRateMapsMontageToDurationFrames::RunTest(const FString& Parameters)
{
	UAttackDefinition* Light = NewObject<UAttackDefinition>();
	Light->DurationFrames = 26;
	const float LightRate = UCombatPresentationComponent::ComputeMontagePlayRate(*Light, 1.0f);
	TestTrue(TEXT("26 frames maps a 1.0 s montage to ~2.3077"),
		FMath::IsNearlyEqual(LightRate, 2.3077f, RateTolerance));
	TestTrue(TEXT("26 frame rate reproduces DurationFrames/60 total logic duration"),
		FMath::IsNearlyEqual(1.0f / LightRate, 26.0f / 60.0f, DurationTolerance));

	UAttackDefinition* Light02 = NewObject<UAttackDefinition>();
	Light02->DurationFrames = 32;
	TestTrue(TEXT("32 frames maps a 1.0 s montage to 1.875"),
		FMath::IsNearlyEqual(UCombatPresentationComponent::ComputeMontagePlayRate(*Light02, 1.0f), 1.875f, RateTolerance));

	// Degradation: broken inputs fall back to rate 1.0 instead of dividing by
	// zero (the once-per-process diagnostic fires but is not asserted here).
	UAttackDefinition* Zero = NewObject<UAttackDefinition>();
	Zero->DurationFrames = 0;
	TestTrue(TEXT("DurationFrames=0 falls back to rate 1.0"),
		FMath::IsNearlyEqual(UCombatPresentationComponent::ComputeMontagePlayRate(*Zero, 1.0f), 1.0f, RateTolerance));
	Zero->DurationFrames = -3;
	TestTrue(TEXT("negative DurationFrames falls back to rate 1.0"),
		FMath::IsNearlyEqual(UCombatPresentationComponent::ComputeMontagePlayRate(*Zero, 1.0f), 1.0f, RateTolerance));
	Zero->DurationFrames = 26;
	TestTrue(TEXT("non-positive montage length falls back to rate 1.0"),
		FMath::IsNearlyEqual(UCombatPresentationComponent::ComputeMontagePlayRate(*Zero, 0.0f), 1.0f, RateTolerance));
	return true;
}

// The four generated montages exist on disk and load as UAnimMontage in a
// fresh process (card acceptance: new-process load, non-empty content).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_AllFourMontagesLoadInFreshProcess,
	"UEMMO.Tasks.M1_032.AllFourMontagesLoadInFreshProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_AllFourMontagesLoadInFreshProcess::RunTest(const FString& Parameters)
{
	for (int32 Index = 0; Index < 4; ++Index)
	{
		UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, MontageObjectPaths[Index]);
		if (TestNotNull(FString::Printf(TEXT("%s loads as UAnimMontage in a fresh process"), MontageLabels[Index]), Montage))
		{
			TestTrue(FString::Printf(TEXT("%s has a positive play length"), MontageLabels[Index]),
				Montage->GetPlayLength() > 0.0f);
			TestTrue(FString::Printf(TEXT("%s has a slot track"), MontageLabels[Index]),
				Montage->SlotAnimTracks.Num() >= 1);
		}
	}
	return true;
}

// Card acceptance: montage length compressed by the play-rate function lands
// on the definition's DurationFrames / 60 (within 10%) for every entry, and
// each montage's slot segment references the definition's configured source
// animation (the four JSON entries are full clips, so segments stay full).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_MontageTotalLogicDurationMatchesDefinition,
	"UEMMO.Tasks.M1_032.MontageTotalLogicDurationMatchesDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_MontageTotalLogicDurationMatchesDefinition::RunTest(const FString& Parameters)
{
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const TCHAR* const Label = MontageLabels[Index];
		UAttackDefinition* Definition = LoadObject<UAttackDefinition>(nullptr, DefinitionObjectPaths[Index]);
		UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, MontageObjectPaths[Index]);
		if (!TestNotNull(FString::Printf(TEXT("%s definition loads"), Label), Definition) ||
			!TestNotNull(FString::Printf(TEXT("%s loads"), Label), Montage))
		{
			continue;
		}

		const float MontageLength = Montage->GetPlayLength();
		const float PlayRate = UCombatPresentationComponent::ComputeMontagePlayRate(*Definition, MontageLength);
		const float TotalLogicDuration = MontageLength / PlayRate;
		const float ExpectedDuration = static_cast<float>(Definition->DurationFrames) / 60.0f;
		TestTrue(FString::Printf(TEXT("%s total logic duration (montage %.4f s / rate %.4f = %.4f s) is DurationFrames/60 = %.4f s within 10%%"),
				Label, MontageLength, PlayRate, TotalLogicDuration, ExpectedDuration),
			FMath::IsNearlyEqual(TotalLogicDuration, ExpectedDuration, ExpectedDuration * LogicDurationRelativeTolerance));

		// The montage segment must reference exactly the definition's soft
		// animation reference (the asset the JSON animation_path names).
		if (Montage->SlotAnimTracks.Num() > 0)
		{
			const FAnimTrack& Track = Montage->SlotAnimTracks[0].AnimTrack;
			if (TestTrue(FString::Printf(TEXT("%s slot track has a segment"), Label), Track.AnimSegments.Num() > 0))
			{
				UAnimSequenceBase* SegmentAnim = Track.AnimSegments[0].GetAnimReference();
				UAnimSequence* ExpectedAnim = Cast<UAnimSequence>(Definition->Animation.ToSoftObjectPath().TryLoad());
				TestTrue(FString::Printf(TEXT("%s segment references the definition's animation"), Label),
					SegmentAnim != nullptr && SegmentAnim == ExpectedAnim);
			}
		}
	}
	return true;
}

// Started dispatch: a successful TryStartAttack plays the attack's montage
// once, at a finite non-negative rate, and stops nothing yet.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_StartedDispatchesMontagePlay,
	"UEMMO.Tasks.M1_032.StartedDispatchesMontagePlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_StartedDispatchesMontagePlay::RunTest(const FString& Parameters)
{
	const FProbeSession Session = NewProbeSession(*this, 60);
	TestTrue(TEXT("probe attack starts"), Session.Combat->TryStartAttack(FName(ProbeAttackIdText), 1));

	TestEqual(TEXT("Started dispatches exactly one play"), Session.Presentation->PlayedIds.Num(), 1);
	if (Session.Presentation->PlayedIds.Num() == 1)
	{
		TestEqual(TEXT("played attack id is the running attack"), Session.Presentation->PlayedIds[0], FName(ProbeAttackIdText));
		TestTrue(TEXT("dispatched play rate is finite and non-negative"),
			FMath::IsFinite(Session.Presentation->PlayedRates[0]) && Session.Presentation->PlayedRates[0] >= 0.0f);
	}
	TestEqual(TEXT("start stops nothing"), Session.Presentation->StoppedIds.Num(), 0);
	return true;
}

// Tick fallback: without any delegate replay the presenter re-syncs from the
// snapshot. This also covers the same-tick instance switch used by combo
// chaining: a changed InstanceId re-presents the new instance exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_TickResyncReplaysChangedInstance,
	"UEMMO.Tasks.M1_032.TickResyncReplaysChangedInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_TickResyncReplaysChangedInstance::RunTest(const FString& Parameters)
{
	const FProbeSession Session = NewProbeSession(*this, 60);
	TestTrue(TEXT("probe attack starts"), Session.Combat->TryStartAttack(FName(ProbeAttackIdText), 1));
	TestEqual(TEXT("precondition: one play from the Started event"), Session.Presentation->PlayedIds.Num(), 1);

	// Forget the presenting state without touching the running combat
	// instance: the next tick must observe the snapshot still attacking with
	// InstanceId 1 (nonzero, unrepresented) and re-present it.
	Session.Presentation->SetSources(Session.Combat, nullptr);
	Session.Presentation->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("tick fallback re-presents the running instance"), Session.Presentation->PlayedIds.Num(), 2);

	// An unchanged Attacking snapshot must not restart playback every tick.
	Session.Presentation->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("steady-state attacking snapshot stays idempotent"), Session.Presentation->PlayedIds.Num(), 2);
	return true;
}

// Card acceptance (component level): a torn-down action (ResetCombat, and by
// extension every interrupt-to-free path) never broadcasts Finished, so the
// stop must come from the snapshot sync: the montage stops on the next tick
// and the presenter stays idle afterwards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_ResetStopsMontageOnTickFallback,
	"UEMMO.Tasks.M1_032.ResetStopsMontageOnTickFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_ResetStopsMontageOnTickFallback::RunTest(const FString& Parameters)
{
	const FProbeSession Session = NewProbeSession(*this, 60);
	TestTrue(TEXT("probe attack starts"), Session.Combat->TryStartAttack(FName(ProbeAttackIdText), 1));
	TestEqual(TEXT("precondition: one play from the Started event"), Session.Presentation->PlayedIds.Num(), 1);

	// Reset is a teardown without OnFinished: no stop may be dispatched yet.
	Session.Combat->ResetCombat();
	TestEqual(TEXT("reset alone dispatches no stop (no event exists)"), Session.Presentation->StoppedIds.Num(), 0);

	Session.Presentation->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("tick fallback stops the montage after a reset"), Session.Presentation->StoppedIds.Num(), 1);
	if (Session.Presentation->StoppedIds.Num() == 1)
	{
		TestEqual(TEXT("stopped attack id is the torn-down attack"), Session.Presentation->StoppedIds[0], FName(ProbeAttackIdText));
	}

	// Free steady state: repeated ticks neither stop nor replay anything.
	Session.Presentation->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("free steady state dispatches nothing further"), Session.Presentation->StoppedIds.Num(), 1);
	TestEqual(TEXT("free steady state replays nothing"), Session.Presentation->PlayedIds.Num(), 1);
	return true;
}

// Natural end: the single Finished broadcast (which fires after the combat
// snapshot is already Free) stops the montage exactly once; replaying the
// same attack afterwards starts a new instance and therefore a new play.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_NaturalFinishStopsMontageOnce,
	"UEMMO.Tasks.M1_032.NaturalFinishStopsMontageOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_NaturalFinishStopsMontageOnce::RunTest(const FString& Parameters)
{
	const FProbeSession Session = NewProbeSession(*this, 60);
	TestTrue(TEXT("probe attack starts"), Session.Combat->TryStartAttack(FName(ProbeAttackIdText), 1));
	TestEqual(TEXT("precondition: one play from the Started event"), Session.Presentation->PlayedIds.Num(), 1);

	// The fixed 60 Hz combat clock caps every TickCombat at 8 advanced frames
	// (interface contract section 2: max 8 steps, backlog retained), so the
	// 60-frame attack needs several calls; loop until the instance finishes.
	int32 CombatTicks = 0;
	while (Session.Combat->GetSnapshot().ActionState == ECombatActionState::Attacking && CombatTicks < 120)
	{
		Session.Combat->TickCombat(1.0f / 60.0f);
		++CombatTicks;
	}
	TestTrue(TEXT("attack finished within the tick budget"), CombatTicks < 120);
	TestEqual(TEXT("Finished stops the montage exactly once"), Session.Presentation->StoppedIds.Num(), 1);
	if (Session.Presentation->StoppedIds.Num() == 1)
	{
		TestEqual(TEXT("stopped attack id is the finished attack"), Session.Presentation->StoppedIds[0], FName(ProbeAttackIdText));
	}

	TestTrue(TEXT("probe attack restarts after the finish"), Session.Combat->TryStartAttack(FName(ProbeAttackIdText), 1));
	TestEqual(TEXT("restart dispatches a second play"), Session.Presentation->PlayedIds.Num(), 2);
	return true;
}

// Crash safety: the default (real) component with a null mesh target must
// tolerate a full start/finish/reset cycle. The real light_01 catalog entry
// exercises the default lookup seams: with the montage missing (stage 1) it
// skips with a one-time diagnostic; once generated, the default playback seam
// no-ops on the null mesh without crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_032_NullMeshLifecycleNeverCrashes,
	"UEMMO.Tasks.M1_032.NullMeshLifecycleNeverCrashes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_032_NullMeshLifecycleNeverCrashes::RunTest(const FString& Parameters)
{
	UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
	TArray<FString> AssetPaths;
	AssetPaths.Add(TEXT("/Game/UEMMO/Combat/Definitions/DA_light_01.DA_light_01"));
	FText CatalogError;
	if (!TestTrue(TEXT("light_01 catalog builds from the generated asset"),
		Catalog->InitializeFromPaths(AssetPaths, CatalogError)))
	{
		AddError(FString::Printf(TEXT("catalog init failed: %s"), *CatalogError.ToString()));
		return true;
	}

	UCombatComponent* Combat = NewObject<UCombatComponent>();
	Combat->InitializeFromCatalog(Catalog);

	UCombatPresentationComponent* Presentation = NewObject<UCombatPresentationComponent>();
	Presentation->SetSources(Combat, nullptr);

	TestTrue(TEXT("real attack starts with a null mesh target"), Combat->TryStartAttack(FName(LightAttackIdText), 1));
	Presentation->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	Combat->TickCombat(1.0f);
	Combat->ResetCombat();
	Presentation->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	AddInfo(TEXT("null-mesh lifecycle completed without crashing"));
	return true;
}

#endif
