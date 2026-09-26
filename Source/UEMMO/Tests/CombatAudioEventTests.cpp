#include "Misc/AutomationTest.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatPresentationComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "Sound/SoundWave.h"

#if WITH_DEV_AUTOMATION_TESTS

// M1-034: combat audio event dispatch tests. The dispatcher is the evidence
// surface: assertions cover accepted request counts and accept/reject
// decisions only. No test claims a sound was heard (NullRHI/nosound runs must
// not claim audibility); the play seam without a world dispatches the request
// and stays silent, which is exactly what these tests observe.
namespace UE::UEMMO::Tasks::M1_034
{
	// Per-TU unique probe id text (FName built inside test bodies only).
	const TCHAR* const ProbeAttackIdText = TEXT("probe_strike_m1_034");

	constexpr double HitLocationX = 100.0;

	// A Hit request with a fixed dedup tuple. TargetActor stays null so the
	// target-id part is the explicit 77 for every event in these tests.
	FCombatAudioEvent MakeHitAudioEvent(uint64 InstigatorId, uint64 AttackInstanceId, int32 HitGroupId)
	{
		FCombatAudioEvent Event;
		Event.Type = ECombatAudioEventType::Hit;
		Event.WorldLocation = FVector(HitLocationX, 0.0, 90.0);
		Event.TargetId = 77;
		Event.InstigatorId = InstigatorId;
		Event.AttackInstanceId = AttackInstanceId;
		Event.HitGroupId = HitGroupId;
		return Event;
	}

	// A Land request keyed on (actor id, landing round epoch).
	FCombatAudioEvent MakeLandAudioEvent(uint64 ActorKey, uint64 LandingEpoch)
	{
		FCombatAudioEvent Event;
		Event.Type = ECombatAudioEventType::Land;
		Event.WorldLocation = FVector(0.0, 0.0, 0.0);
		Event.TargetId = ActorKey;
		Event.LandingEpoch = LandingEpoch;
		return Event;
	}

	// One accepted hit as the combat component broadcasts it (M1-019 shape).
	FCombatHit MakeCombatHit(uint64 AttackInstanceId)
	{
		FCombatHit Hit;
		Hit.InstigatorId = 1;
		Hit.AttackInstanceId = AttackInstanceId;
		Hit.HitGroupId = 0;
		Hit.WorldHitLocation = FVector(HitLocationX, 0.0, 90.0);
		Hit.AttackId = FName(ProbeAttackIdText);
		return Hit;
	}
}

using namespace UE::UEMMO::Tasks::M1_034;

// One hit event produces exactly one accepted request; a same-key redelivery
// inside the dedup window is rejected; a new attack instance key is accepted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_034_DispatcherAcceptsSingleHitRequestPerKey,
	"UEMMO.Tasks.M1_034.DispatcherAcceptsSingleHitRequestPerKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_034_DispatcherAcceptsSingleHitRequestPerKey::RunTest(const FString& Parameters)
{
	FCombatAudioDispatcher Dispatcher;
	Dispatcher.SetClockSeconds(10.0);

	const FCombatAudioEvent Hit = MakeHitAudioEvent(1, 5, 0);
	TestTrue(TEXT("the first hit request is accepted"), Dispatcher.Submit(Hit));
	TestEqual(TEXT("one accepted hit request"), Dispatcher.GetAcceptedCount(), 1);

	TestFalse(TEXT("the same key redelivered at the same clock is rejected"), Dispatcher.Submit(Hit));
	TestEqual(TEXT("a redelivered key stays one accepted request"), Dispatcher.GetAcceptedCount(), 1);

	const FCombatAudioEvent OtherInstance = MakeHitAudioEvent(1, 6, 0);
	TestTrue(TEXT("a different attack instance key is accepted"), Dispatcher.Submit(OtherInstance));
	TestEqual(TEXT("two accepted hit requests after a new key"), Dispatcher.GetAcceptedCount(), 2);

	if (Dispatcher.GetAcceptedEvents().Num() == 2)
	{
		TestEqual(TEXT("accepted event keeps its attack instance id"),
			Dispatcher.GetAcceptedEvents()[0].AttackInstanceId, static_cast<uint64>(5));
		TestTrue(TEXT("accepted event keeps its world location"),
			Dispatcher.GetAcceptedEvents()[0].WorldLocation.Equals(FVector(HitLocationX, 0.0, 90.0)));
	}
	return true;
}

// Component wiring: the HitConfirmed broadcast becomes exactly one accepted
// Hit request through the presentation component; a repeated broadcast of the
// same hit instance does not duplicate it; a new instance gets its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_034_HitConfirmedDispatchesHitAudioRequest,
	"UEMMO.Tasks.M1_034.HitConfirmedDispatchesHitAudioRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_034_HitConfirmedDispatchesHitAudioRequest::RunTest(const FString& Parameters)
{
	UCombatComponent* Combat = NewObject<UCombatComponent>();
	UCombatPresentationComponent* Presentation = NewObject<UCombatPresentationComponent>();
	Presentation->SetSources(Combat, nullptr);

	TestEqual(TEXT("no hit confirmed yet means no audio request"),
		Presentation->GetAudioDispatcher().GetAcceptedCount(), 0);

	Combat->OnHitConfirmed.Broadcast(MakeCombatHit(5));
	TestEqual(TEXT("one accepted hit broadcasts one audio request"),
		Presentation->GetAudioDispatcher().GetAcceptedCount(), 1);
	TestEqual(TEXT("the accepted request reached the play seam"),
		Presentation->GetDispatchedAudioPlayCount(), 1);

	Combat->OnHitConfirmed.Broadcast(MakeCombatHit(5));
	TestEqual(TEXT("the same hit instance sounds once"),
		Presentation->GetAudioDispatcher().GetAcceptedCount(), 1);
	TestEqual(TEXT("the duplicate dispatched no extra playback"),
		Presentation->GetDispatchedAudioPlayCount(), 1);

	Combat->OnHitConfirmed.Broadcast(MakeCombatHit(6));
	TestEqual(TEXT("a new hit instance gets its own request"),
		Presentation->GetAudioDispatcher().GetAcceptedCount(), 2);
	return true;
}

// A fully missed attack (no accepted damage application, no HitConfirmed
// broadcast) leaves the audio dispatcher empty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_034_MissedAttackRecordsNoAudioRequest,
	"UEMMO.Tasks.M1_034.MissedAttackRecordsNoAudioRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_034_MissedAttackRecordsNoAudioRequest::RunTest(const FString& Parameters)
{
	// Probe catalog with one 60-frame attack, presentation bound, no world:
	// the active-window hit query stays a no-op, so no OnHitConfirmed can
	// broadcast and the whole attack must record nothing.
	UAttackDefinition* Definition = NewObject<UAttackDefinition>();
	Definition->AttackId = FName(ProbeAttackIdText);
	Definition->DurationFrames = 60;

	UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
	TArray<UAttackDefinition*> Definitions;
	Definitions.Add(Definition);
	FText CatalogError;
	if (!TestTrue(TEXT("probe catalog builds"), Catalog->BuildFromDefinitions(Definitions, CatalogError)))
	{
		AddError(FString::Printf(TEXT("catalog build failed: %s"), *CatalogError.ToString()));
		return true;
	}

	UCombatComponent* Combat = NewObject<UCombatComponent>();
	Combat->InitializeFromCatalog(Catalog);
	UCombatPresentationComponent* Presentation = NewObject<UCombatPresentationComponent>();
	Presentation->SetSources(Combat, nullptr);

	TestTrue(TEXT("probe attack starts"), Combat->TryStartAttack(FName(ProbeAttackIdText), 1));
	int32 CombatTicks = 0;
	while (Combat->GetSnapshot().ActionState == ECombatActionState::Attacking && CombatTicks < 120)
	{
		Combat->TickCombat(1.0f / 60.0f);
		++CombatTicks;
	}
	TestTrue(TEXT("attack finished within the tick budget"), CombatTicks < 120);

	TestEqual(TEXT("a fully missed attack records no audio request"),
		Presentation->GetAudioDispatcher().GetAcceptedCount(), 0);
	TestEqual(TEXT("a fully missed attack dispatches no playback"),
		Presentation->GetDispatchedAudioPlayCount(), 0);
	return true;
}

// Landing dedup: one landing round (actor + epoch) is one request; the same
// round redelivered is rejected; the next round (epoch advanced) is accepted.
// Covered on the dispatcher directly and through the enemy testable entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_034_LandingRoundDeduplicatedPerActorAndEpoch,
	"UEMMO.Tasks.M1_034.LandingRoundDeduplicatedPerActorAndEpoch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_034_LandingRoundDeduplicatedPerActorAndEpoch::RunTest(const FString& Parameters)
{
	FCombatAudioDispatcher Dispatcher;
	Dispatcher.SetClockSeconds(0.0);

	const FCombatAudioEvent Land = MakeLandAudioEvent(999, 1);
	TestTrue(TEXT("the first landing round request is accepted"), Dispatcher.Submit(Land));
	TestEqual(TEXT("one accepted land request"), Dispatcher.GetAcceptedCount(), 1);
	TestFalse(TEXT("the same landing round redelivered is rejected"), Dispatcher.Submit(Land));
	TestEqual(TEXT("one landing round stays one request"), Dispatcher.GetAcceptedCount(), 1);

	const FCombatAudioEvent NextRound = MakeLandAudioEvent(999, 2);
	TestTrue(TEXT("a new landing round with an advanced epoch is accepted"), Dispatcher.Submit(NextRound));
	TestEqual(TEXT("two landing rounds are two requests"), Dispatcher.GetAcceptedCount(), 2);

	// Enemy-level: the testable landing entry with an explicit clock value.
	ATrainingEnemy* Enemy = NewObject<ATrainingEnemy>();
	TestTrue(TEXT("enemy first landing round dispatch is accepted"), Enemy->SubmitLandingAudio(1, 5.0));
	TestFalse(TEXT("enemy same landing round re-submission is rejected"), Enemy->SubmitLandingAudio(1, 5.02));
	TestTrue(TEXT("enemy new landing round dispatch is accepted"), Enemy->SubmitLandingAudio(2, 5.04));
	TestEqual(TEXT("enemy recorded two landing requests"), Enemy->GetAcceptedLandingAudioCount(), 2);
	return true;
}

// Concurrency cap: twelve distinct keys submitted in the same window produce
// exactly eight accepted requests; after the window expires a new one passes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_034_ConcurrencyCapAcceptsOnlyEightRequestsInWindow,
	"UEMMO.Tasks.M1_034.ConcurrencyCapAcceptsOnlyEightRequestsInWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_034_ConcurrencyCapAcceptsOnlyEightRequestsInWindow::RunTest(const FString& Parameters)
{
	FCombatAudioDispatcher Dispatcher;
	Dispatcher.SetClockSeconds(0.0);

	int32 Accepted = 0;
	for (int32 Index = 1; Index <= 12; ++Index)
	{
		if (Dispatcher.Submit(MakeHitAudioEvent(1, static_cast<uint64>(Index), 0)))
		{
			++Accepted;
		}
	}
	TestEqual(TEXT("only the first eight requests are accepted inside the window"), Accepted, 8);
	TestEqual(TEXT("accepted history holds the eight in-window requests"), Dispatcher.GetAcceptedCount(), 8);

	Dispatcher.SetClockSeconds(0.3);
	TestTrue(TEXT("after the concurrency window expires a new request is accepted"),
		Dispatcher.Submit(MakeHitAudioEvent(1, 13, 0)));
	return true;
}

// Minimum repeat interval on the injected clock: the same key is rejected
// inside 30 ms and accepted after 60 ms (interval 50 ms default).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_034_MinRepeatIntervalGatesSameKeyOnInjectedClock,
	"UEMMO.Tasks.M1_034.MinRepeatIntervalGatesSameKeyOnInjectedClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_034_MinRepeatIntervalGatesSameKeyOnInjectedClock::RunTest(const FString& Parameters)
{
	FCombatAudioDispatcher Dispatcher;
	const FCombatAudioEvent Hit = MakeHitAudioEvent(1, 5, 0);

	Dispatcher.SetClockSeconds(0.0);
	TestTrue(TEXT("the first submission is accepted at t=0"), Dispatcher.Submit(Hit));

	Dispatcher.SetClockSeconds(0.03);
	TestFalse(TEXT("the same key inside 30 ms is rejected"), Dispatcher.Submit(Hit));

	Dispatcher.SetClockSeconds(0.06);
	TestTrue(TEXT("the same key after 60 ms is accepted"), Dispatcher.Submit(Hit));

	Dispatcher.SetClockSeconds(0.09);
	TestFalse(TEXT("the same key within 30 ms of the re-accept is rejected"), Dispatcher.Submit(Hit));

	TestEqual(TEXT("two accepted submissions for the same key overall"), Dispatcher.GetAcceptedCount(), 2);
	return true;
}

// Missing sound asset: the request stays accepted, playback is skipped with
// one diagnostic, and neither the hit nor the landing path crashes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_034_MissingSoundAssetSkipsPlaybackWithoutCrash,
	"UEMMO.Tasks.M1_034.MissingSoundAssetSkipsPlaybackWithoutCrash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_034_MissingSoundAssetSkipsPlaybackWithoutCrash::RunTest(const FString& Parameters)
{
	UCombatComponent* Combat = NewObject<UCombatComponent>();
	UCombatPresentationComponent* Presentation = NewObject<UCombatPresentationComponent>();
	Presentation->SetAudioSounds(TSoftObjectPtr<USoundWave>(), TSoftObjectPtr<USoundWave>());
	Presentation->SetSources(Combat, nullptr);

	Combat->OnHitConfirmed.Broadcast(MakeCombatHit(5));
	TestEqual(TEXT("missing asset: the hit request stays accepted"),
		Presentation->GetAudioDispatcher().GetAcceptedCount(), 1);
	TestEqual(TEXT("missing asset: hit playback is skipped"),
		Presentation->GetDispatchedAudioPlayCount(), 0);

	ATrainingEnemy* Enemy = NewObject<ATrainingEnemy>();
	Enemy->SetLandSound(TSoftObjectPtr<USoundWave>());
	TestTrue(TEXT("missing asset: the landing request stays accepted"), Enemy->SubmitLandingAudio(1, 0.0));
	TestEqual(TEXT("missing asset: landing playback is skipped"), Enemy->GetDispatchedLandingAudioCount(), 0);
	TestEqual(TEXT("missing asset: landing dedup still records one request"),
		Enemy->GetAcceptedLandingAudioCount(), 1);
	return true;
}

#endif
