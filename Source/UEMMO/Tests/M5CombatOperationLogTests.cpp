// M5-053: the unified shot event-chain log. The suite locks the card's
// requirements on the OperationLogSubsystem's chain face:
// 1. One applied lethal hitscan leaves, under its (Epoch, Source, Shot) chain
//    id, the ordered rows ShotCommitted -> Admission(Approved) -> Hit ->
//    Damage (the exact applied value as the numeric payload) -> Death, with
//    the request's pellet/target fields and the committing caller's config
//    revision on every row; a staggering shot additionally records the
//    Reaction row; EndShotChain appends the Cleanup row and the chain stays
//    queryable.
// 2. Refused requests (same-faction friendly fire, a dead target, a stale
//    epoch) record ONE named refusal row with no damage row; a repeated
//    identical refusal merges into the first row's RepeatCount instead of
//    duplicating (the old-epoch rejection stays visible).
// 3. Dedup and sampling never delete error chains: repeated verdicts merge,
//    the Projectile stage's Info rows follow the sampling stride, and
//    refusal/critical rows are recorded even when the stride would drop them
//    and even after the chain ended.
// 4. The retention policy: EndShotChain keeps the chain queryable; the
//    active-chain LRU cap force-closes the oldest open chain (ActiveEvicted,
//    still queryable); the tracked-chain window finally hard-drops the oldest
//    ended chains while the active population stays capped and recent chains
//    stay queryable.
// 5. The opening metadata (weapon definition, revision, best-effort driver/
//    vehicle name snapshots) rides the chain summary, and every degenerate
//    call (unusable identity, unknown chain, stride clamp) is a named no-op.
//
// Harness: a real UGameInstance per test (the M3-023 session pattern) so the
// production subsystem collection instantiates UOperationLogSubsystem, plus
// the M5-012 scene pattern (registry + bound ledger + two registered actors)
// for the ApplyUnifiedHit drives. The applier resolves the log through the
// target actor's world -> game instance, exactly like production.

#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Combat/System/UnifiedHitApplier.h"
#include "../Logging/OperationLogSubsystem.h"
#include "../Weapons/WeaponComponent.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_053
{
	// World names are uniquified: several suites live in one process and two
	// of this suite's own sessions can be alive in one test.
	static int32 M5_053_SessionCounter = 0;

	/**
	 * One real UGameInstance + its world (the M3-023 session harness): Create()
	 * initializes the instance, so the production subsystem collection
	 * instantiates UOperationLogSubsystem. TearDown() follows the engine
	 * FTestWorldWrapper::DestroyTestWorld order.
	 */
	struct FM5_053_Session
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameInstance* GameInstance = nullptr;
		UOperationLogSubsystem* OpLog = nullptr;

		static FM5_053_Session Create(FAutomationTestBase& Test)
		{
			FM5_053_Session Session;
			++M5_053_SessionCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M5_053_TestWorld_%d"), M5_053_SessionCounter));
			Session.World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
			if (!Test.TestNotNull(TEXT("a private test world is available"), Session.World))
			{
				return Session;
			}
			Session.GameInstance = NewObject<UGameInstance>(GEngine);
			Session.Context = &GEngine->CreateNewWorldContext(EWorldType::Game);
			Session.Context->OwningGameInstance = Session.GameInstance;
			Session.World->SetGameInstance(Session.GameInstance);
			Session.Context->SetCurrentWorld(Session.World);
			// The production path that creates every registered
			// UGameInstanceSubsystem (engine FTestWorldWrapper precedent).
			Session.GameInstance->Init();
			Session.OpLog = Session.GameInstance->GetSubsystem<UOperationLogSubsystem>();
			Test.TestNotNull(TEXT("the operation log subsystem exists in the game instance's collection"), Session.OpLog);
			return Session;
		}

		void TearDown()
		{
			if (World)
			{
				World->RemoveFromRoot();
				if (GameInstance)
				{
					GameInstance->Shutdown();
					GameInstance = nullptr;
				}
				if (Context)
				{
					GEngine->DestroyWorldContext(World);
					Context = nullptr;
				}
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				World = nullptr;
			}
		}
	};

	/** Reads the production log file through the subsystem's own path. */
	static bool M5_053_ReadLog(const UOperationLogSubsystem* OpLog, FString& OutContent)
	{
		return OpLog != nullptr
			&& !OpLog->GetLogFilePath().IsEmpty()
			&& FFileHelper::LoadFileToString(OutContent, *OpLog->GetLogFilePath());
	}

	/** Spawns a plain actor with a health component and, optionally, a combat component (the M5-012 pattern). */
	static AActor* M5_053_SpawnCombatActor(UWorld& World, const FVector& Location, bool bWithCombat)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_053_Health"));
		Health->RegisterComponent();
		if (bWithCombat)
		{
			UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_053_Combat"));
			Combat->RegisterComponent();
		}
		return Actor;
	}

	/**
	 * One scene: a registry with the ledger bound to it, one attacker actor
	 * and one target actor (health always, combat for the legacy victim path)
	 * registered with their factions.
	 */
	struct FM5_053_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{&Registry};
		FEntityId AttackerId = InvalidCombatEntityId;
		FEntityId TargetId = InvalidCombatEntityId;
		AActor* Attacker = nullptr;
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;

		bool Build(FAutomationTestBase& Test, UWorld& World, const TCHAR* AttackerFaction, const TCHAR* TargetFaction)
		{
			Attacker = M5_053_SpawnCombatActor(World, FVector(6100.0, 0.0, 100.0), /*bWithCombat*/ true);
			Target = M5_053_SpawnCombatActor(World, FVector(6300.0, 0.0, 100.0), /*bWithCombat*/ true);
			if (!Test.TestTrue(TEXT("precondition: the attacker spawns"), Attacker != nullptr)
				|| !Test.TestTrue(TEXT("precondition: the target spawns"), Target != nullptr))
			{
				return false;
			}
			FCombatEntityMetadata AttackerMeta;
			AttackerMeta.Faction = AttackerFaction;
			AttackerMeta.Category = TEXT("attacker");
			FCombatEntityMetadata TargetMeta;
			TargetMeta.Faction = TargetFaction;
			TargetMeta.Category = TEXT("target");
			const FCombatEpoch Epoch = Registry.GetCurrentEpoch();
			AttackerId = Registry.RegisterEntity(Attacker, AttackerMeta, Epoch);
			TargetId = Registry.RegisterEntity(Target, TargetMeta, Epoch);
			if (!Test.TestTrue(TEXT("precondition: the attacker registers"), IsValidCombatEntityId(AttackerId))
				|| !Test.TestTrue(TEXT("precondition: the target registers"), IsValidCombatEntityId(TargetId)))
			{
				return false;
			}
			TargetHealth = Target->FindComponentByClass<UHealthComponent>();
			return Test.TestTrue(TEXT("precondition: the target carries a health component"), TargetHealth != nullptr);
		}

		/** Draws the next value of the source's common ActionSequence (the 010 registry). */
		FShotId AllocateShot(FEntityId SourceEntityId)
		{
			return Registry.AllocateActionSequence(SourceEntityId, Registry.GetCurrentEpoch());
		}
	};

	/** A light_01-like profile: 10 damage with the full stagger request. */
	static FDamageProfile MakeStunProfile()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("m5_053_stun");
		Profile.BaseDamage = 10.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.22f;
		Profile.KnockbackCmPerSecond = 90.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** A lethal profile with NO control request (no Reaction row is expected). */
	static FDamageProfile MakeLethalProfile()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("m5_053_lethal");
		Profile.BaseDamage = 1000.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.0f;
		Profile.KnockbackCmPerSecond = 0.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.0f;
		return Profile;
	}

	/** The legacy normal target: every gate open, no immunity, no poise. */
	static FTargetReaction MakeNormalTarget()
	{
		FTargetReaction Target;
		Target.PolicyId = TEXT("normal");
		return Target;
	}

	/** A fully populated request from the scene against the target. */
	static FUnifiedHitRequest MakeHitRequest(const FM5_053_Scene& Scene, const FDamageProfile& Profile,
		FShotId ShotId, FPelletIndex PelletIndex = 0)
	{
		FUnifiedHitRequest Request;
		Request.Epoch = Scene.Registry.GetCurrentEpoch();
		Request.AttackerEntityId = Scene.AttackerId;
		Request.ShotId = ShotId;
		Request.PelletIndex = PelletIndex;
		Request.TargetEntityId = Scene.TargetId;
		Request.TargetActor = Scene.Target;
		Request.Attack = Profile;
		Request.TargetReaction = MakeNormalTarget();
		Request.AttackPower = 0.0f;
		Request.HitLocation = FVector(6200.0, 0.0, 130.0);
		return Request;
	}

	/** The committing caller's opening snapshot (the WeaponComponent commit wiring's exact face). */
	static FShotContext MakeShotContext(FCombatEpoch Epoch, FEntityId Source, FShotId ShotId)
	{
		FShotContext Shot;
		Shot.Epoch = Epoch;
		Shot.SourceEntityId = Source;
		Shot.ShotId = ShotId;
		Shot.WeaponDefinitionId = FName(TEXT("smg_m5_053"));
		Shot.PelletCount = 1;
		Shot.FireMode = EWeaponFireMode::Hitscan;
		Shot.LocalShotSequence = ShotId;
		return Shot;
	}

	/** True when no row of the chain carries the damage stage (a refusal leaves none). */
	static bool M5_053_HasNoDamageRow(const TArray<FShotChainEvent>& Events)
	{
		for (const FShotChainEvent& Event : Events)
		{
			if (Event.Stage == EShotChainStage::Damage)
			{
				return false;
			}
		}
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M5_053;

// -- 1. The applied shot chain: ordered stages, exact fields, release cleanup --

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_053AppliedShotChainRecordsStagesInOrder,
	"UEMMO.Tasks.M5_053.AppliedShotChainRecordsStagesInOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_053AppliedShotChainRecordsStagesInOrder::RunTest(const FString& Parameters)
{
	FM5_053_Session Session = FM5_053_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}
	// A pinned time makes the RecordedAt domain deterministic (the M3-023
	// injectable now provider drives the chain timestamps too).
	const FDateTime PinnedTime(2026, 10, 28, 12, 0, 0, 0);
	Session.OpLog->SetNowProvider([PinnedTime]() { return PinnedTime; });

	FM5_053_Scene Scene;
	if (!Scene.Build(*this, *Session.World, TEXT("Player"), TEXT("Enemy")))
	{
		Session.TearDown();
		return true;
	}

	// -- Shot 1: a non-lethal staggering hit (the Reaction row case) ----------
	const FCombatEpoch Epoch = Scene.Registry.GetCurrentEpoch();
	const FShotId StunShotId = Scene.AllocateShot(Scene.AttackerId);
	FShotContext StunShot = MakeShotContext(Epoch, Scene.AttackerId, StunShotId);
	TestTrue(TEXT("BeginShotChain accepts the committed shot context"),
		Session.OpLog->BeginShotChain(StunShot, TEXT("rev-m5-053-a"), TEXT("PlayerChar"), FString()));
	TestEqual(TEXT("one open chain exists after the commit"),
		Session.OpLog->GetNumActiveShotChains(), 1);

	const FUnifiedHitOutcome StunOutcome = ApplyUnifiedHit(
		MakeHitRequest(Scene, MakeStunProfile(), StunShotId), Scene.Ledger);
	TestFalse(TEXT("precondition: the stun hit was not blocked"), StunOutcome.bWasBlocked);
	TestTrue(TEXT("precondition: the stun hit's stagger was accepted"), StunOutcome.Control.bStagger);

	const TArray<FShotChainEvent> StunEvents = Session.OpLog->GetShotChainEvents(Epoch, Scene.AttackerId, StunShotId);
	if (!TestEqual(TEXT("the stun chain records commit/admission/hit/damage/reaction"), StunEvents.Num(), 5))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("row 0 is the shot commit"), StunEvents[0].Stage, EShotChainStage::ShotCommitted);
	TestEqual(TEXT("row 1 is the admission"), StunEvents[1].Stage, EShotChainStage::Admission);
	TestEqual(TEXT("row 2 is the hit"), StunEvents[2].Stage, EShotChainStage::Hit);
	TestEqual(TEXT("row 3 is the damage"), StunEvents[3].Stage, EShotChainStage::Damage);
	TestEqual(TEXT("row 4 is the reaction"), StunEvents[4].Stage, EShotChainStage::Reaction);
	TestTrue(TEXT("the admission row approved the hit"), StunEvents[1].Result == FName(TEXT("Approved")));
	TestTrue(TEXT("the hit row connected"), StunEvents[2].Result == FName(TEXT("Connected")));
	TestTrue(TEXT("the damage row applied"), StunEvents[3].Result == FName(TEXT("Applied")));
	TestTrue(TEXT("the reaction row accepted"), StunEvents[4].Result == FName(TEXT("Accepted")));
	TestTrue(TEXT("the reaction row names the accepted stagger and no launch/knockdown"),
		StunEvents[4].Detail.Contains(TEXT("stagger=1")) && StunEvents[4].Detail.Contains(TEXT("launch=0"))
		&& StunEvents[4].Detail.Contains(TEXT("knockdown=0")));
	TestTrue(TEXT("the damage row carries the applied damage as its numeric payload"),
		StunEvents[3].bHasNumericValue && FMath::IsNearlyEqual(StunEvents[3].NumericValue, 10.0));
	TestEqual(TEXT("the applied damage reached the target's health component"),
		Scene.TargetHealth->GetHealth(), 90.0f);

	// -- Shot 2: the lethal hit (the Death row case) ---------------------------
	const FShotId LethalShotId = Scene.AllocateShot(Scene.AttackerId);
	FShotContext LethalShot = MakeShotContext(Epoch, Scene.AttackerId, LethalShotId);
	TestTrue(TEXT("BeginShotChain accepts the lethal shot context"),
		Session.OpLog->BeginShotChain(LethalShot, TEXT("rev-m5-053-a"), TEXT("PlayerChar"), FString()));

	const FUnifiedHitOutcome LethalOutcome = ApplyUnifiedHit(
		MakeHitRequest(Scene, MakeLethalProfile(), LethalShotId), Scene.Ledger);
	TestFalse(TEXT("precondition: the lethal hit was not blocked"), LethalOutcome.bWasBlocked);
	TestTrue(TEXT("precondition: the target died from the lethal hit"), LethalOutcome.bTargetDied);

	const TArray<FShotChainEvent> LethalEvents = Session.OpLog->GetShotChainEvents(Epoch, Scene.AttackerId, LethalShotId);
	if (!TestEqual(TEXT("the lethal chain records commit/admission/hit/damage/death"), LethalEvents.Num(), 5))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("lethal row 0 is the shot commit"), LethalEvents[0].Stage, EShotChainStage::ShotCommitted);
	TestEqual(TEXT("lethal row 1 is the admission"), LethalEvents[1].Stage, EShotChainStage::Admission);
	TestEqual(TEXT("lethal row 2 is the hit"), LethalEvents[2].Stage, EShotChainStage::Hit);
	TestEqual(TEXT("lethal row 3 is the damage"), LethalEvents[3].Stage, EShotChainStage::Damage);
	TestEqual(TEXT("lethal row 4 is the death"), LethalEvents[4].Stage, EShotChainStage::Death);
	TestTrue(TEXT("the death row carries the Died verdict"), LethalEvents[4].Result == FName(TEXT("Died")));
	TestEqual(TEXT("the death row is critical severity"), LethalEvents[4].Severity, EShotChainSeverity::Critical);

	// Field identity: every row carries the request's chain identity and the
	// damage row carries the request's pellet/target and the exact outcome value.
	bool bAllRowsCarryChainIdentity = true;
	for (const FShotChainEvent& Event : LethalEvents)
	{
		bAllRowsCarryChainIdentity = bAllRowsCarryChainIdentity
			&& Event.Epoch == Epoch
			&& Event.SourceEntityId == Scene.AttackerId
			&& Event.ShotId == LethalShotId;
	}
	TestTrue(TEXT("every lethal-chain row carries the request's epoch/source/shot identity"), bAllRowsCarryChainIdentity);
	TestEqual(TEXT("the damage row carries the request's pellet index"),
		static_cast<int32>(LethalEvents[3].PelletIndex), 0);
	TestEqual(TEXT("the damage row carries the request's target id"), LethalEvents[3].TargetId, Scene.TargetId);
	TestTrue(TEXT("the damage row's numeric payload equals the outcome's applied damage"),
		LethalEvents[3].bHasNumericValue
		&& FMath::IsNearlyEqual(LethalEvents[3].NumericValue, static_cast<double>(LethalOutcome.DamageApplied)));
	// The applied damage is the POOL CLAMP of the request: the stun hit above
	// already dropped the 100 pool to 90, so the lethal request applies the
	// remaining 90 (the applied value always equals the actual pool drop).
	TestEqual(TEXT("the lethal hit applied the remaining health pool"), LethalOutcome.DamageApplied, 90.0f);
	TestTrue(TEXT("the damage row repeats the chain's config revision"),
		LethalEvents[3].ConfigRevision == TEXT("rev-m5-053-a"));
	TestTrue(TEXT("the death row carries the injected now provider's timestamp"),
		LethalEvents[4].RecordedAt == PinnedTime);

	// The chain summary rides the opening metadata while the chain is open.
	FShotChainSummary Summary;
	TestTrue(TEXT("the open chain summary resolves"),
		Session.OpLog->FindShotChainSummary(Epoch, Scene.AttackerId, LethalShotId, Summary));
	TestFalse(TEXT("the chain is still open before the release"), Summary.bEnded);
	TestTrue(TEXT("the summary carries the weapon definition from the shot context"),
		Summary.WeaponDefinitionId == FName(TEXT("smg_m5_053")));
	TestTrue(TEXT("the summary carries the config revision"), Summary.ConfigRevision == TEXT("rev-m5-053-a"));
	TestTrue(TEXT("the summary carries the best-effort driver snapshot"), Summary.DriverName == TEXT("PlayerChar"));
	TestEqual(TEXT("no rows were dropped on the lethal chain"), Summary.DroppedInfoCount, 0);

	// The release closes the chain with its Cleanup row; the query stays.
	TestTrue(TEXT("EndShotChain closes the open chain"),
		Session.OpLog->EndShotChain(Epoch, Scene.AttackerId, LethalShotId));
	const TArray<FShotChainEvent> EndedEvents = Session.OpLog->GetShotChainEvents(Epoch, Scene.AttackerId, LethalShotId);
	TestEqual(TEXT("the cleanup row is the sixth row of the released chain"), EndedEvents.Num(), 6);
	if (EndedEvents.Num() == 6)
	{
		TestEqual(TEXT("the last row is the cleanup"), EndedEvents[5].Stage, EShotChainStage::Cleanup);
		TestTrue(TEXT("the cleanup row carries the Released verdict"),
			EndedEvents[5].Result == FName(TEXT("Released")));
	}
	FShotChainSummary EndedSummary;
	TestTrue(TEXT("the released chain summary still resolves"),
		Session.OpLog->FindShotChainSummary(Epoch, Scene.AttackerId, LethalShotId, EndedSummary));
	TestTrue(TEXT("the released chain reports ended"), EndedSummary.bEnded);
	TestEqual(TEXT("the released chain ended by release"), EndedSummary.EndReason, EShotChainEndReason::Released);

	Session.TearDown();
	return true;
}

// -- 2. Refused requests: one named refusal row, no damage row, merged repeats --

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_053RefusedRequestsCarryReasonAndNoDamageRows,
	"UEMMO.Tasks.M5_053.RefusedRequestsCarryReasonAndNoDamageRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_053RefusedRequestsCarryReasonAndNoDamageRows::RunTest(const FString& Parameters)
{
	FM5_053_Session Session = FM5_053_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Same-faction attacker/target: the friendly-fire filter refuses before
	// anything is applied; the chain auto-opens (no weapon-mount commit).
	FM5_053_Scene Scene;
	if (!Scene.Build(*this, *Session.World, TEXT("Player"), TEXT("Player")))
	{
		Session.TearDown();
		return true;
	}
	const FCombatEpoch Epoch = Scene.Registry.GetCurrentEpoch();
	const FShotId FriendlyShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitOutcome FriendlyOutcome = ApplyUnifiedHit(
		MakeHitRequest(Scene, MakeStunProfile(), FriendlyShotId), Scene.Ledger);
	TestTrue(TEXT("precondition: the friendly hit was refused"), FriendlyOutcome.bWasBlocked);
	TestTrue(TEXT("precondition: the friendly hit was refused as SameFaction"),
		FriendlyOutcome.RefuseReason == EUnifiedHitRefuseReason::SameFaction);

	const TArray<FShotChainEvent> FriendlyEvents = Session.OpLog->GetShotChainEvents(Epoch, Scene.AttackerId, FriendlyShotId);
	if (!TestEqual(TEXT("the friendly-fire chain carries exactly the refusal row"), FriendlyEvents.Num(), 1))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("the refusal row is an admission row"), FriendlyEvents[0].Stage, EShotChainStage::Admission);
	TestEqual(TEXT("the refusal row is refusal severity"), FriendlyEvents[0].Severity, EShotChainSeverity::Refusal);
	TestTrue(TEXT("the refusal row's verdict names the applier reason"),
		FriendlyEvents[0].Result == FName(TEXT("SameFaction")));
	TestTrue(TEXT("the refusal detail names the decision and the reason"),
		FriendlyEvents[0].Detail.Contains(TEXT("decision=Block_Ignore"))
		&& FriendlyEvents[0].Detail.Contains(TEXT("refuse=SameFaction")));
	TestTrue(TEXT("the auto-opened chain carries no revision"), FriendlyEvents[0].ConfigRevision.IsEmpty());
	TestTrue(TEXT("the refused request recorded no damage row"), M5_053_HasNoDamageRow(FriendlyEvents));

	// Dead target: the death gate refuses the whole hit.
	Scene.TargetHealth->ApplyDamage(1000.0f);
	TestFalse(TEXT("precondition: the target is dead"), Scene.TargetHealth->IsAlive());
	const FShotId DeadShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitOutcome DeadOutcome = ApplyUnifiedHit(
		MakeHitRequest(Scene, MakeStunProfile(), DeadShotId), Scene.Ledger);
	TestTrue(TEXT("precondition: the dead-target hit was refused as TargetDead"),
		DeadOutcome.RefuseReason == EUnifiedHitRefuseReason::TargetDead);
	const TArray<FShotChainEvent> DeadEvents = Session.OpLog->GetShotChainEvents(Epoch, Scene.AttackerId, DeadShotId);
	if (!TestEqual(TEXT("the dead-target chain carries exactly the refusal row"), DeadEvents.Num(), 1))
	{
		Session.TearDown();
		return true;
	}
	TestTrue(TEXT("the dead-target refusal names the reason"),
		DeadEvents[0].Result == FName(TEXT("TargetDead")));
	TestTrue(TEXT("the dead-target request recorded no damage row"), M5_053_HasNoDamageRow(DeadEvents));

	// Stale epoch: the world moved on (the registry bumped its generation) and
	// the request still names the old one - the ledger refuses it visibly. The
	// shot id is hand-picked: after the bump the registry wiped the fire source,
	// so AllocateShot would return the invalid sentinel (the chain key only
	// needs a non-zero id; the ledger checks the epoch before the entities).
	const FCombatEpoch NextEpoch = Scene.Registry.BeginNextWorldEpoch();
	Scene.TargetHealth->ResetHealth(); // the epoch face, not the dead-target face, must refuse here
	const FShotId StaleShotId = 3;
	FUnifiedHitRequest StaleRequest = MakeHitRequest(Scene, MakeStunProfile(), StaleShotId);
	StaleRequest.Epoch = Epoch; // the pre-bump generation
	const FUnifiedHitOutcome StaleOutcome = ApplyUnifiedHit(StaleRequest, Scene.Ledger);
	TestTrue(TEXT("precondition: the stale-epoch hit was refused"), StaleOutcome.bWasBlocked);
	TestTrue(TEXT("precondition: the stale-epoch hit was refused by the ledger"),
		StaleOutcome.RefuseReason == EUnifiedHitRefuseReason::LedgerRefused);
	TestTrue(TEXT("precondition: the ledger reason is the stale epoch"),
		StaleOutcome.LedgerReason == EHitLedgerRejectReason::StaleEpoch);

	// The chain carries the request's OWN (stale) epoch - that is exactly the
	// visible old-epoch rejection the card asks for.
	const TArray<FShotChainEvent> StaleEvents = Session.OpLog->GetShotChainEvents(Epoch, Scene.AttackerId, StaleShotId);
	if (!TestEqual(TEXT("the stale-epoch chain carries exactly the refusal row"), StaleEvents.Num(), 1))
	{
		Session.TearDown();
		return true;
	}
	TestTrue(TEXT("the stale-epoch refusal names the ledger face"),
		StaleEvents[0].Result == FName(TEXT("LedgerRefused")));
	TestTrue(TEXT("the stale-epoch refusal detail carries the ledger's StaleEpoch reason"),
		StaleEvents[0].Detail.Contains(TEXT("ledger=StaleEpoch")));
	TestTrue(TEXT("the stale-epoch request recorded no damage row"), M5_053_HasNoDamageRow(StaleEvents));

	// A repeated identical refusal merges into the first row's RepeatCount
	// (the old-epoch rejection stays visible without duplicate rows).
	const FUnifiedHitOutcome RepeatOutcome = ApplyUnifiedHit(StaleRequest, Scene.Ledger);
	TestTrue(TEXT("precondition: the repeated stale-epoch hit was refused again"), RepeatOutcome.bWasBlocked);
	// The chain lives under the request's OWN (stale) epoch - the repeat query
	// reads the same key the first refusal opened.
	const TArray<FShotChainEvent> MergedEvents = Session.OpLog->GetShotChainEvents(Epoch, Scene.AttackerId, StaleShotId);
	TestEqual(TEXT("the repeated refusal did not add a row"), MergedEvents.Num(), 1);
	if (MergedEvents.Num() == 1)
	{
		TestEqual(TEXT("the refusal row counted its repeat"), MergedEvents[0].RepeatCount, 2u);
	}

	Session.TearDown();
	return true;
}

// -- 3. Dedup and sampling: repeats merge, errors are never sampled or dropped --

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_053DuplicateRowsMergeAndErrorsNeverSampled,
	"UEMMO.Tasks.M5_053.DuplicateRowsMergeAndErrorsNeverSampled",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_053DuplicateRowsMergeAndErrorsNeverSampled::RunTest(const FString& Parameters)
{
	FM5_053_Session Session = FM5_053_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// The sampling stride 3: of the distinct per-frame projectile checkpoints
	// only the 1st and the 4th are recorded; refusal/critical rows ignore it.
	Session.OpLog->SetInfoSampleStride(3);
	TestEqual(TEXT("the stride setter reports the configured stride"),
		Session.OpLog->GetInfoSampleStride(), 3);

	const FCombatEpoch Epoch = 7;
	const FEntityId Source = 11;
	const FShotId Shot = 13;
	TestTrue(TEXT("BeginShotChain accepts a logic-only chain identity"),
		Session.OpLog->BeginShotChain(MakeShotContext(Epoch, Source, Shot), TEXT("rev-m5-053-s"), FString(), FString()));

	auto LogProjectile = [Session](EShotChainSeverity Severity, const TCHAR* Result)
	{
		FShotStageRecord Record;
		Record.Stage = EShotChainStage::Projectile;
		Record.Severity = Severity;
		Record.PelletIndex = 0;
		Record.TargetId = 5;
		Record.Result = FName(Result);
		Record.Detail = FString::Printf(TEXT("projectile checkpoint %s"), Result);
		return Session.OpLog->LogShotStage(Epoch, Source, Shot, Record);
	};

	// Five distinct Info checkpoints -> occurrences 1 and 4 recorded, 2/3/5 dropped.
	int32 RecordedInfo = 0;
	for (int32 Index = 1; Index <= 5; ++Index)
	{
		const bool bRecorded = LogProjectile(EShotChainSeverity::Info,
			*FString::Printf(TEXT("Checkpoint%d"), Index));
		RecordedInfo += bRecorded ? 1 : 0;
	}
	TestEqual(TEXT("the sampling stride recorded exactly the 1st and 4th checkpoints"), RecordedInfo, 2);

	// Refusal and critical rows ignore the stride entirely.
	int32 RecordedRefusals = 0;
	for (int32 Index = 1; Index <= 3; ++Index)
	{
		RecordedRefusals += LogProjectile(EShotChainSeverity::Refusal,
			*FString::Printf(TEXT("Refusal%d"), Index)) ? 1 : 0;
	}
	TestEqual(TEXT("refusal rows are never sampled"), RecordedRefusals, 3);
	TestTrue(TEXT("a critical row is recorded despite the stride"),
		LogProjectile(EShotChainSeverity::Critical, TEXT("ProjectileCrash")));

	FShotChainSummary Summary;
	TestTrue(TEXT("the chain summary resolves"), Session.OpLog->FindShotChainSummary(Epoch, Source, Shot, Summary));
	// Rows: 1 commit + 2 sampled checkpoints + 3 refusals + 1 critical = 7.
	TestEqual(TEXT("the chain recorded exactly the sampled info, all refusals and the critical row"),
		Summary.EventCount, 7);
	TestEqual(TEXT("the sampling drops are auditable on the summary"), Summary.DroppedInfoCount, 3);

	// Dedup: a repeated identical refusal merges (no new row, RepeatCount grows).
	TestTrue(TEXT("a repeated refusal call reports recorded (merged)"),
		LogProjectile(EShotChainSeverity::Refusal, TEXT("Refusal1")));
	TArray<FShotChainEvent> Events = Session.OpLog->GetShotChainEvents(Epoch, Source, Shot);
	TestEqual(TEXT("the repeated refusal did not add a row"), Events.Num(), 7);
	int32 Refusal1Repeat = 0;
	for (const FShotChainEvent& Event : Events)
	{
		if (Event.Result == FName(TEXT("Refusal1")))
		{
			Refusal1Repeat = static_cast<int32>(Event.RepeatCount);
		}
	}
	TestEqual(TEXT("the merged refusal row carries the repeat count"), Refusal1Repeat, 2);

	// EndShot: the completed chain stays queryable; a late Info row is dropped
	// (counted) while a late refusal row still lands on the ended chain.
	TestTrue(TEXT("EndShotChain closes the chain"), Session.OpLog->EndShotChain(Epoch, Source, Shot));
	Events = Session.OpLog->GetShotChainEvents(Epoch, Source, Shot);
	TestEqual(TEXT("the cleanup row closed the chain"), Events.Num(), 8);
	TestEqual(TEXT("the last row is the cleanup"), Events[7].Stage, EShotChainStage::Cleanup);

	// Isolate the post-end gate from sampling: stride 1 means the late Info
	// row below passes the sampling gate and is dropped by the ended gate.
	Session.OpLog->SetInfoSampleStride(1);
	FShotStageRecord LateInfo;
	LateInfo.Stage = EShotChainStage::Projectile;
	LateInfo.Severity = EShotChainSeverity::Info;
	LateInfo.Result = FName(TEXT("LateCheckpoint"));
	TestFalse(TEXT("a late Info row on an ended chain is dropped"),
		Session.OpLog->LogShotStage(Epoch, Source, Shot, LateInfo));

	FShotStageRecord LateRefusal;
	LateRefusal.Stage = EShotChainStage::Admission;
	LateRefusal.Severity = EShotChainSeverity::Refusal;
	LateRefusal.PelletIndex = 0;
	LateRefusal.TargetId = 5;
	LateRefusal.Result = FName(TEXT("LateLedgerRefused"));
	LateRefusal.Detail = TEXT("late ledger refusal after the shot ended");
	TestTrue(TEXT("a late refusal row still lands on the ended chain"),
		Session.OpLog->LogShotStage(Epoch, Source, Shot, LateRefusal));
	Events = Session.OpLog->GetShotChainEvents(Epoch, Source, Shot);
	TestEqual(TEXT("the ended chain grew only by the refusal row"), Events.Num(), 9);
	TestEqual(TEXT("the dropped late info is counted on the ended chain"),
		Session.OpLog->FindShotChainSummary(Epoch, Source, Shot, Summary) ? Summary.DroppedInfoCount : -1, 4);

	Session.TearDown();
	return true;
}

// -- 4. Retention: released chains stay queryable; the caps evict in LRU order --

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_053EndShotChainRetainsQueryAndLruEvictsOldest,
	"UEMMO.Tasks.M5_053.EndShotChainRetainsQueryAndLruEvictsOldest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_053EndShotChainRetainsQueryAndLruEvictsOldest::RunTest(const FString& Parameters)
{
	FM5_053_Session Session = FM5_053_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	const FCombatEpoch Epoch = 7;
	const FEntityId Source = 11;

	// Part 1: the released chain keeps its query and echoes the text summary.
	const FShotId ReleasedShot = 101;
	TestTrue(TEXT("the released chain begins"),
		Session.OpLog->BeginShotChain(MakeShotContext(Epoch, Source, ReleasedShot), TEXT("rev-r"), FString(), FString()));
	FShotStageRecord HitRow;
	HitRow.Stage = EShotChainStage::Hit;
	HitRow.PelletIndex = 0;
	HitRow.TargetId = 5;
	HitRow.Result = FName(TEXT("Connected"));
	TestTrue(TEXT("the released chain recorded its hit row"),
		Session.OpLog->LogShotStage(Epoch, Source, ReleasedShot, HitRow));
	TestTrue(TEXT("EndShotChain releases the chain"),
		Session.OpLog->EndShotChain(Epoch, Source, ReleasedShot));
	FShotChainSummary ReleasedSummary;
	TestTrue(TEXT("the released chain is still queryable"),
		Session.OpLog->FindShotChainSummary(Epoch, Source, ReleasedShot, ReleasedSummary));
	TestEqual(TEXT("the released chain carries commit + hit + cleanup"),
		ReleasedSummary.EventCount, 3);
	FString LogContent;
	TestTrue(TEXT("the text log file is readable after the release"), M5_053_ReadLog(Session.OpLog, LogContent));
	TestTrue(TEXT("the text file received the chain-end summary line"),
		LogContent.Contains(TEXT("ShotChain End: epoch=7 source=11 shot=101 events=3 dropped=0 reason=Released")));

	// Part 2: the active-chain cap force-closes the oldest OPEN chain (LRU).
	for (FShotId ShotId = 102; ShotId <= 165; ++ShotId)
	{
		TestTrue(TEXT("a fresh open chain begins"),
			Session.OpLog->BeginShotChain(MakeShotContext(Epoch, Source, ShotId), FString(), FString(), FString()));
	}
	TestEqual(TEXT("the active population sits at the cap before the overflow"),
		Session.OpLog->GetNumActiveShotChains(), 64);
	TestTrue(TEXT("the oldest open chain begins and forces the eviction of shot 102's chain"),
		Session.OpLog->BeginShotChain(MakeShotContext(Epoch, Source, 166), FString(), FString(), FString()));
	FShotChainSummary EvictedSummary;
	TestTrue(TEXT("the force-closed chain is still tracked"),
		Session.OpLog->FindShotChainSummary(Epoch, Source, 102, EvictedSummary));
	TestTrue(TEXT("the force-closed chain reports ended"), EvictedSummary.bEnded);
	TestEqual(TEXT("the force-closed chain ended by active eviction"),
		EvictedSummary.EndReason, EShotChainEndReason::ActiveEvicted);
	TestTrue(TEXT("the force-closed chain's rows stay queryable"),
		Session.OpLog->GetShotChainEvents(Epoch, Source, 102).Num() >= 2);

	// Part 3: the tracked-chain window finally hard-drops the oldest ended
	// chains; the caps hold throughout and recent chains stay queryable.
	bool bCapsHeld = true;
	bool bOldestEvictedDropped = false;
	bool bRecentChainQueryable = false;
	for (FShotId ShotId = 167; ShotId <= 400; ++ShotId)
	{
		TestTrue(TEXT("the eviction loop begins a chain"),
			Session.OpLog->BeginShotChain(MakeShotContext(Epoch, Source, ShotId), FString(), FString(), FString()));
		bCapsHeld = bCapsHeld
			&& Session.OpLog->GetNumActiveShotChains() <= 64
			&& Session.OpLog->GetNumTrackedShotChains() <= 192;
		if (!Session.OpLog->FindShotChainSummary(Epoch, Source, 102, EvictedSummary))
		{
			bOldestEvictedDropped = true;
			FShotChainSummary RecentSummary;
			bRecentChainQueryable = Session.OpLog->FindShotChainSummary(Epoch, Source, ShotId, RecentSummary)
				&& !RecentSummary.bEnded;
			break;
		}
	}
	TestTrue(TEXT("the active and tracked caps held during the eviction storm"), bCapsHeld);
	TestTrue(TEXT("the oldest evicted chain was finally hard-dropped (no longer findable)"), bOldestEvictedDropped);
	TestTrue(TEXT("the most recent chain stays open and queryable"), bRecentChainQueryable);
	TestEqual(TEXT("the released chain of part 1 was dropped with the retention window"),
		Session.OpLog->GetShotChainEvents(Epoch, Source, ReleasedShot).Num(), 0);

	Session.TearDown();
	return true;
}

// -- 5. Opening metadata, auto-open and degenerate calls ------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_053ChainMetaCarriesActorNamesAndSafeDegenerateCalls,
	"UEMMO.Tasks.M5_053.ChainMetaCarriesActorNamesAndSafeDegenerateCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_053ChainMetaCarriesActorNamesAndSafeDegenerateCalls::RunTest(const FString& Parameters)
{
	FM5_053_Session Session = FM5_053_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	const FCombatEpoch Epoch = 7;
	const FEntityId Source = 11;

	// The opening metadata rides the summary (the vehicle mount may pass the
	// hull and its seat occupant; a pawn-held mount passes the wielder).
	const FShotId MetaShot = 201;
	FShotContext MetaShotContext = MakeShotContext(Epoch, Source, MetaShot);
	MetaShotContext.PelletCount = 8;
	MetaShotContext.FireMode = EWeaponFireMode::Projectile;
	TestTrue(TEXT("BeginShotChain accepts the metadata-rich commit"),
		Session.OpLog->BeginShotChain(MetaShotContext, TEXT("rev-m5-053-meta"), TEXT("SeatDriver"), TEXT("HullVehicle")));
	FShotChainSummary MetaSummary;
	TestTrue(TEXT("the metadata chain resolves"),
		Session.OpLog->FindShotChainSummary(Epoch, Source, MetaShot, MetaSummary));
	TestTrue(TEXT("the summary carries the driver snapshot"), MetaSummary.DriverName == TEXT("SeatDriver"));
	TestTrue(TEXT("the summary carries the vehicle snapshot"), MetaSummary.VehicleName == TEXT("HullVehicle"));
	TestTrue(TEXT("the summary carries the revision"), MetaSummary.ConfigRevision == TEXT("rev-m5-053-meta"));
	TestTrue(TEXT("the commit row names the weapon and the pellet count"),
		Session.OpLog->GetShotChainEvents(Epoch, Source, MetaShot)[0].Detail.Contains(TEXT("weapon=smg_m5_053"))
		&& Session.OpLog->GetShotChainEvents(Epoch, Source, MetaShot)[0].Detail.Contains(TEXT("pellets=8")));

	// A hit pipeline that runs without the weapon-mount commit auto-opens its
	// chain (empty metadata) and stays queryable.
	const FShotId AutoShot = 202;
	FShotStageRecord AutoRow;
	AutoRow.Stage = EShotChainStage::Hit;
	AutoRow.PelletIndex = 0;
	AutoRow.TargetId = 9;
	AutoRow.Result = FName(TEXT("Connected"));
	TestTrue(TEXT("LogShotStage auto-opens an unknown chain"),
		Session.OpLog->LogShotStage(Epoch, Source, AutoShot, AutoRow));
	FShotChainSummary AutoSummary;
	TestTrue(TEXT("the auto-opened chain resolves"),
		Session.OpLog->FindShotChainSummary(Epoch, Source, AutoShot, AutoSummary));
	TestTrue(TEXT("the auto-opened chain carries no revision"), AutoSummary.ConfigRevision.IsEmpty());
	TestEqual(TEXT("the auto-opened chain recorded its row"), AutoSummary.EventCount, 1);

	// Degenerate calls are named no-ops: an unusable identity, an unknown
	// chain, a stride clamp and a double end all refuse without side effects.
	const int32 TrackedBefore = Session.OpLog->GetNumTrackedShotChains();
	TestFalse(TEXT("BeginShotChain refuses a zero shot id"),
		Session.OpLog->BeginShotChain(MakeShotContext(Epoch, Source, InvalidCombatShotId), FString(), FString(), FString()));
	TestFalse(TEXT("LogShotStage refuses a zero epoch"),
		Session.OpLog->LogShotStage(InvalidCombatEpoch, Source, 203, AutoRow));
	TestFalse(TEXT("EndShotChain refuses an unknown chain"),
		Session.OpLog->EndShotChain(Epoch, Source, 999));
	TestEqual(TEXT("no degenerate call created or removed a chain"),
		Session.OpLog->GetNumTrackedShotChains(), TrackedBefore);
	TestEqual(TEXT("the query of an unknown chain is empty"),
		Session.OpLog->GetShotChainEvents(Epoch, Source, 999).Num(), 0);
	FShotChainSummary UnknownSummary;
	TestFalse(TEXT("the summary of an unknown chain refuses"),
		Session.OpLog->FindShotChainSummary(Epoch, Source, 999, UnknownSummary));
	TestTrue(TEXT("EndShotChain ends the auto-opened chain"),
		Session.OpLog->EndShotChain(Epoch, Source, AutoShot));
	TestFalse(TEXT("EndShotChain refuses an already ended chain"),
		Session.OpLog->EndShotChain(Epoch, Source, AutoShot));
	Session.OpLog->SetInfoSampleStride(0);
	TestEqual(TEXT("a zero stride clamps to record-everything"),
		Session.OpLog->GetInfoSampleStride(), 1);

	Session.TearDown();
	return true;
}

#endif
