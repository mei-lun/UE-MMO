// M5-012: the unified hit application entry (interface contract section 1,
// owner 012). Pins the single game-thread submission that validates identity
// and context first, commits the ledger dedup, resolves the damage through
// the injectable calculator (the M5-011 resolver by default), applies it
// through the target's HealthComponent and decides the hit's control from
// the target reaction policy. Duplicates, stale epochs, teammates, dead
// targets, get-up protected targets, unusable keys, unbound ledgers and
// illegal damage contexts are all refused with explicit traceable reasons
// and never leave half a transaction (health, control or dedup) behind.
// The old CombatComponent victim path stays the only action state machine:
// an accepted hit reaches it through NotifyHitReceived, never around it.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Combat/System/UnifiedHitApplier.h"

#include <limits>
#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Compile-time pins: the outcome is a pointer-free value snapshot (interface
// contract section 0.5), the request carries only a weak reference and the
// applier refuse reasons stay an append-only uint8 enum with 0 = None.
// ---------------------------------------------------------------------------

static_assert(std::is_trivially_copyable_v<FUnifiedHitOutcome>, "FUnifiedHitOutcome must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FUnifiedHitControlOutcome>, "FUnifiedHitControlOutcome must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FUnifiedHitRequest::TargetActor)>, "FUnifiedHitRequest::TargetActor must stay a weak reference, never a raw pointer");
static_assert(!std::is_pointer_v<decltype(FUnifiedHitRequest::Epoch)>, "FUnifiedHitRequest::Epoch must stay a value type");
static_assert(!std::is_pointer_v<decltype(FUnifiedHitRequest::AttackPower)>, "FUnifiedHitRequest::AttackPower must stay a value type");
static_assert(std::is_same_v<std::underlying_type_t<EUnifiedHitRefuseReason>, uint8>, "EUnifiedHitRefuseReason stays an append-only uint8 enum");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::None) == 0, "EUnifiedHitRefuseReason::None must stay 0");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::UnusableKey) == 1, "EUnifiedHitRefuseReason::UnusableKey must stay 1");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::InvalidTargetActor) == 2, "EUnifiedHitRefuseReason::InvalidTargetActor must stay 2");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::MissingHealthComponent) == 3, "EUnifiedHitRefuseReason::MissingHealthComponent must stay 3");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::TargetDead) == 4, "EUnifiedHitRefuseReason::TargetDead must stay 4");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::GetUpProtection) == 5, "EUnifiedHitRefuseReason::GetUpProtection must stay 5");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::SameFaction) == 6, "EUnifiedHitRefuseReason::SameFaction must stay 6");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::LedgerRefused) == 7, "EUnifiedHitRefuseReason::LedgerRefused must stay 7");
static_assert(static_cast<uint8>(EUnifiedHitRefuseReason::DamageContextRefused) == 8, "EUnifiedHitRefuseReason::DamageContextRefused must stay 8");

namespace UE::UEMMO::Tasks::M5_012
{
	/** The legacy light_01 numbers (M5-003 pinned values). */
	static FDamageProfile MakeLight01()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("light_01");
		Profile.BaseDamage = 10.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.22f;
		Profile.KnockbackCmPerSecond = 90.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** The legacy launcher numbers (M5-003 pinned values). */
	static FDamageProfile MakeLauncher()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("launcher");
		Profile.BaseDamage = 18.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 1.0f;
		Profile.KnockbackCmPerSecond = 70.0f;
		Profile.LaunchCmPerSecond = 700.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** The legacy normal target: every gate open, no immunity, no poise. */
	static FTargetReaction MakeNormalTarget()
	{
		FTargetReaction Target;
		Target.PolicyId = TEXT("normal");
		return Target;
	}

	/** World acquisition, M1-019 pattern: a private temp world, GWorld fallback. */
	static UWorld* M5_012_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_012_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_012 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_012 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_012_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_012_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	/** Spawns a plain actor with a health component and, optionally, a combat component. */
	static AActor* M5_012_SpawnCombatActor(UWorld& World, const FVector& Location, bool bWithCombat)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_012_Health"));
		Health->RegisterComponent();
		if (bWithCombat)
		{
			UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_012_Combat"));
			Combat->RegisterComponent();
		}
		return Actor;
	}

	/**
	 * One scene: a registry with the ledger bound to it, one attacker actor
	 * and one target actor (health always, combat optional) registered with
	 * their factions. Both actors carry a combat component so the legacy
	 * victim path is in the loop unless a test opts out.
	 */
	struct FM5_012_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{&Registry};
		FEntityId AttackerId = InvalidCombatEntityId;
		FEntityId TargetId = InvalidCombatEntityId;
		AActor* Attacker = nullptr;
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;
		UCombatComponent* TargetCombat = nullptr;

		bool Build(FAutomationTestBase& Test, UWorld& World,
			const TCHAR* AttackerFaction, const TCHAR* TargetFaction, bool bTargetWithCombat = true)
		{
			Attacker = M5_012_SpawnCombatActor(World, FVector(100.0, 0.0, 100.0), /*bWithCombat*/ true);
			Target = M5_012_SpawnCombatActor(World, FVector(200.0, 0.0, 100.0), bTargetWithCombat);
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
			TargetCombat = Target->FindComponentByClass<UCombatComponent>();
			return Test.TestTrue(TEXT("precondition: the target carries a health component"), TargetHealth != nullptr);
		}

		/** Draws the next value of the source's common ActionSequence (the 010 registry). */
		FShotId AllocateShot(FEntityId SourceEntityId)
		{
			return Registry.AllocateActionSequence(SourceEntityId, Registry.GetCurrentEpoch());
		}
	};

	/** A fully populated request from the scene: light_01 against the target. */
	static FUnifiedHitRequest MakeHitRequest(const FM5_012_Scene& Scene, const FDamageProfile& Profile,
		const FTargetReaction& Policy, FShotId ShotId, FPelletIndex PelletIndex = 0)
	{
		FUnifiedHitRequest Request;
		Request.Epoch = Scene.Registry.GetCurrentEpoch();
		Request.AttackerEntityId = Scene.AttackerId;
		Request.ShotId = ShotId;
		Request.PelletIndex = PelletIndex;
		Request.TargetEntityId = Scene.TargetId;
		Request.TargetActor = Scene.Target;
		Request.Attack = Profile;
		Request.TargetReaction = Policy;
		Request.AttackPower = 0.0f;
		Request.HitLocation = FVector(150.0, 0.0, 130.0);
		return Request;
	}
}

using namespace UE::UEMMO::Tasks::M5_012;

// ---------------------------------------------------------------------------
// AppliedHitDamagesControlsBridges
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012AppliedHitDamagesControlsBridges,
	"UEMMO.Tasks.M5_012.AppliedHitDamagesControlsBridges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012AppliedHitDamagesControlsBridges::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}

	const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
	const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);

	// The applied hit reports the legacy light_01 damage with no immunity,
	// no refusal and no reason on any face.
	TestEqual(TEXT("the applied hit deals the legacy light_01 damage"), Outcome.DamageApplied, 10.0f);
	TestEqual(TEXT("the applied hit decides Apply"), Outcome.Decision, EHitDecision::Apply);
	TestFalse(TEXT("the applied hit is not immune"), Outcome.bWasImmune);
	TestFalse(TEXT("the applied hit is not blocked"), Outcome.bWasBlocked);
	TestFalse(TEXT("the applied hit is not lethal"), Outcome.bTargetDied);
	TestTrue(TEXT("the applied hit carries no refuse reason"),
		Outcome.RefuseReason == EUnifiedHitRefuseReason::None);
	TestTrue(TEXT("the applied hit carries no ledger reason"),
		Outcome.LedgerReason == EHitLedgerRejectReason::None);
	TestTrue(TEXT("the applied hit carries no damage block reason"),
		Outcome.DamageBlockReason == EDamageBlockReason::None);

	// Control: light_01 requests only a stagger; the normal target accepts it.
	TestTrue(TEXT("the light_01 stagger is accepted"), Outcome.Control.bStagger);
	TestFalse(TEXT("light_01 requests no launch"), Outcome.Control.bLaunch);
	TestFalse(TEXT("no accepted launch implies no knockdown"), Outcome.Control.bKnockdown);

	// The damage went through the target's HealthComponent (no second pool).
	TestEqual(TEXT("the target lost exactly the applied health"), Scene.TargetHealth->GetHealth(), 90.0f);
	TestEqual(TEXT("the health component removed exactly the reported damage"),
		Scene.TargetHealth->GetMaxHealth() - Scene.TargetHealth->GetHealth(), Outcome.DamageApplied);

	// The legacy victim path owns the state machine: the accepted hit reached
	// it through NotifyHitReceived and stunned the target.
	if (TestNotNull(TEXT("precondition: the target carries a combat component"), Scene.TargetCombat))
	{
		TestTrue(TEXT("the accepted hit stunned the victim through the legacy path"),
			Scene.TargetCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	}

	// Bridging rules: actual damage > 0 bridges the legacy OnHitConfirmed
	// path; this hit is not the pure-control path.
	TestTrue(TEXT("an applied damage bridges the legacy hit-confirmed path"), Outcome.BridgesLegacyHitConfirmed());
	TestFalse(TEXT("an applied damage is not the pure-control path"), Outcome.TakesPureControlPath());

	// The ledger recorded exactly one event and the shot's one control.
	TestEqual(TEXT("the ledger recorded one hit event"), Scene.Ledger.GetNumRecordedEvents(), 1);
	TestEqual(TEXT("the ledger accepted one shot control"), Scene.Ledger.GetNumAcceptedControls(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// DuplicateKeyReapplyIsRefused
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012DuplicateKeyReapplyIsRefused,
	"UEMMO.Tasks.M5_012.DuplicateKeyReapplyIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012DuplicateKeyReapplyIsRefused::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}

	const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
	const FUnifiedHitOutcome First = ApplyUnifiedHit(Request, Scene.Ledger);
	TestTrue(TEXT("precondition: the first application applied its damage"), First.DamageApplied > 0.0f);
	const float HealthAfterFirst = Scene.TargetHealth->GetHealth();

	// The exact re-send of the same event key is refused with an explicit
	// traceable reason and applies nothing.
	const FUnifiedHitOutcome Second = ApplyUnifiedHit(Request, Scene.Ledger);
	TestEqual(TEXT("the duplicate key is refused as Block_Ignore"), Second.Decision, EHitDecision::Block_Ignore);
	TestTrue(TEXT("the duplicate key is blocked"), Second.bWasBlocked);
	TestTrue(TEXT("the duplicate key names the ledger as the refusing face"),
		Second.RefuseReason == EUnifiedHitRefuseReason::LedgerRefused);
	TestTrue(TEXT("the duplicate key carries the ledger's DuplicateEvent reason"),
		Second.LedgerReason == EHitLedgerRejectReason::DuplicateEvent);
	TestEqual(TEXT("the duplicate key applies zero damage"), Second.DamageApplied, 0.0f);
	TestFalse(TEXT("the duplicate key accepts no control"), Second.Control.bStagger || Second.Control.bLaunch || Second.Control.bKnockdown);
	TestFalse(TEXT("the duplicate key is not an immunity"), Second.bWasImmune);
	TestFalse(TEXT("a refused duplicate bridges no legacy path"),
		Second.BridgesLegacyHitConfirmed() || Second.TakesPureControlPath());

	// No half transaction: the health is untouched and the ledger grew by
	// nothing (the duplicate consumed no new key).
	TestEqual(TEXT("the duplicate re-send removed no health"), Scene.TargetHealth->GetHealth(), HealthAfterFirst);
	TestEqual(TEXT("the ledger still holds exactly one event"), Scene.Ledger.GetNumRecordedEvents(), 1);
	TestEqual(TEXT("the ledger still holds exactly one control"), Scene.Ledger.GetNumAcceptedControls(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// StaleEpochIsRefused
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012StaleEpochIsRefused,
	"UEMMO.Tasks.M5_012.StaleEpochIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012StaleEpochIsRefused::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}

	const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);

	// A request naming a future epoch is refused before anything applies.
	{
		FUnifiedHitRequest Future = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		Future.Epoch = Scene.Registry.GetCurrentEpoch() + 1;
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Future, Scene.Ledger);
		TestEqual(TEXT("a future-epoch request is refused as Block_Ignore"), Outcome.Decision, EHitDecision::Block_Ignore);
		TestTrue(TEXT("a future-epoch request names the ledger"), Outcome.RefuseReason == EUnifiedHitRefuseReason::LedgerRefused);
		TestTrue(TEXT("a future-epoch request carries StaleEpoch"),
			Outcome.LedgerReason == EHitLedgerRejectReason::StaleEpoch);
		TestEqual(TEXT("a future-epoch request applies zero damage"), Outcome.DamageApplied, 0.0f);
		TestEqual(TEXT("a future-epoch request records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
		TestEqual(TEXT("a future-epoch request removes no health"), Scene.TargetHealth->GetHealth(), 100.0f);
	}

	// A world rebuild invalidates the old generation as a whole: the very same
	// (old epoch, old ids) request is refused with StaleEpoch even though the
	// epoch counter only moved by one.
	const FCombatEpoch OldEpoch = Scene.Registry.GetCurrentEpoch();
	Scene.Registry.BeginNextWorldEpoch();
	{
		FUnifiedHitRequest Stale = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		Stale.Epoch = OldEpoch;
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Stale, Scene.Ledger);
		TestTrue(TEXT("a post-rebuild request of the old generation is refused"),
			Outcome.bWasBlocked && Outcome.RefuseReason == EUnifiedHitRefuseReason::LedgerRefused);
		TestTrue(TEXT("the post-rebuild refusal names StaleEpoch"),
			Outcome.LedgerReason == EHitLedgerRejectReason::StaleEpoch);
		TestEqual(TEXT("the post-rebuild refusal applies zero damage"), Outcome.DamageApplied, 0.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// TeammateGetUpProtectedAndDeadTargetsAreFiltered
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012TeammateGetUpProtectedAndDeadTargetsAreFiltered,
	"UEMMO.Tasks.M5_012.TeammateGetUpProtectedAndDeadTargetsAreFiltered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012TeammateGetUpProtectedAndDeadTargetsAreFiltered::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. Same non-empty faction: the friendly fire filter refuses the whole
	// hit before any application (no damage, no control, no ledger record).
	{
		FM5_012_Scene FriendlyScene;
		if (!FriendlyScene.Build(*this, *World, TEXT("Enemy"), TEXT("Enemy")))
		{
			return true;
		}
		const FShotId ShotId = FriendlyScene.AllocateShot(FriendlyScene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(FriendlyScene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, FriendlyScene.Ledger);
		TestEqual(TEXT("a teammate hit is refused as Block_Ignore"), Outcome.Decision, EHitDecision::Block_Ignore);
		TestTrue(TEXT("a teammate hit is blocked"), Outcome.bWasBlocked);
		TestTrue(TEXT("a teammate hit names the faction filter"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::SameFaction);
		TestEqual(TEXT("a teammate hit applies zero damage"), Outcome.DamageApplied, 0.0f);
		TestEqual(TEXT("a teammate hit removes no health"), FriendlyScene.TargetHealth->GetHealth(), 100.0f);
		TestEqual(TEXT("a teammate hit records no ledger event"), FriendlyScene.Ledger.GetNumRecordedEvents(), 0);
		TestTrue(TEXT("a teammate hit leaves the victim untouched"),
			FriendlyScene.TargetCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	}

	// 2. An unaligned attacker (empty faction) is not friendly fire.
	{
		FM5_012_Scene UnalignedScene;
		if (!UnalignedScene.Build(*this, *World, TEXT(""), TEXT("Enemy")))
		{
			return true;
		}
		const FShotId ShotId = UnalignedScene.AllocateShot(UnalignedScene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(UnalignedScene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, UnalignedScene.Ledger);
		TestFalse(TEXT("an unaligned attacker is not faction filtered"), Outcome.bWasBlocked);
		TestEqual(TEXT("an unaligned attacker applies its damage"), Outcome.DamageApplied, 10.0f);
	}

	// 3. Get-up protection: a target inside its landing recovery refuses the
	// whole hit with Block_Invulnerable (the legacy unhittable recovery).
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		Scene.TargetCombat->SetInputClockSeconds(10.0);
		TestTrue(TEXT("precondition: the target enters its landing recovery"),
			Scene.TargetCombat->BeginLandingRecovery(10.0));
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestEqual(TEXT("a get-up protected target refuses the hit as Block_Invulnerable"),
			Outcome.Decision, EHitDecision::Block_Invulnerable);
		TestTrue(TEXT("the get-up protection refusal names its reason"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::GetUpProtection);
		TestEqual(TEXT("the get-up protected target takes no damage"), Scene.TargetHealth->GetHealth(), 100.0f);
		TestEqual(TEXT("the get-up protection records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
	}

	// 4. A dead target refuses the whole hit: no damage, no presentation, no
	// ledger record (a refused hit never poisons a later instance).
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		Scene.TargetHealth->ApplyDamage(Scene.TargetHealth->GetMaxHealth());
		TestTrue(TEXT("precondition: the target starts dead"), !Scene.TargetHealth->IsAlive());
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestEqual(TEXT("a dead target is refused as Block_Ignore"), Outcome.Decision, EHitDecision::Block_Ignore);
		TestTrue(TEXT("the dead target refusal names its reason"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::TargetDead);
		TestEqual(TEXT("a dead target takes no damage"), Outcome.DamageApplied, 0.0f);
		TestEqual(TEXT("a dead target records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
		TestFalse(TEXT("a refused dead-target hit bridges no legacy path"),
			Outcome.BridgesLegacyHitConfirmed() || Outcome.TakesPureControlPath());
	}
	return true;
}

// ---------------------------------------------------------------------------
// LethalHitDiesOnceAndReentryIsTraced
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012LethalHitDiesOnceAndReentryIsTraced,
	"UEMMO.Tasks.M5_012.LethalHitDiesOnceAndReentryIsTraced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012LethalHitDiesOnceAndReentryIsTraced::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}

	// A lethal profile (one hit removes the full pool).
	FDamageProfile Lethal = MakeLight01();
	Lethal.DamageProfileId = TEXT("lethal_probe");
	Lethal.BaseDamage = 999.0f;

	const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
	FUnifiedHitRequest Request = MakeHitRequest(Scene, Lethal, MakeNormalTarget(), ShotId);

	// Callback reentry: while the death event is still broadcasting, a
	// re-entrant ApplyUnifiedHit of the SAME key must be refused with a
	// traceable reason instead of applying a second time.
	int32 DiedCount = 0;
	TOptional<FUnifiedHitOutcome> ReentrantOutcome;
	Scene.TargetHealth->OnDied.AddLambda([&](void)
	{
		++DiedCount;
		ReentrantOutcome = ApplyUnifiedHit(Request, Scene.Ledger);
	});

	const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
	TestEqual(TEXT("the lethal hit applies the full remaining pool"), Outcome.DamageApplied, 100.0f);
	TestTrue(TEXT("the lethal hit reports the target death"), Outcome.bTargetDied);
	TestEqual(TEXT("the target health is zero"), Scene.TargetHealth->GetHealth(), 0.0f);
	TestEqual(TEXT("the death event broadcast exactly once"), DiedCount, 1);
	TestTrue(TEXT("the lethal hit bridges the legacy hit-confirmed path"), Outcome.BridgesLegacyHitConfirmed());

	// The legacy victim path marked the combat component dead (death has
	// priority over every stun request).
	TestTrue(TEXT("the legacy path marked the victim dead"), Scene.TargetCombat->IsDead());

	// The re-entrant call was refused with an explicit reason and applied
	// nothing: the health stays zero and no second death event fired.
	if (TestTrue(TEXT("the reentrant call was answered"), ReentrantOutcome.IsSet()))
	{
		TestTrue(TEXT("the reentrant call was refused"), ReentrantOutcome->bWasBlocked);
		TestTrue(TEXT("the reentrant call names its refusal face"),
			ReentrantOutcome->RefuseReason == EUnifiedHitRefuseReason::TargetDead
			|| ReentrantOutcome->RefuseReason == EUnifiedHitRefuseReason::LedgerRefused);
		TestEqual(TEXT("the reentrant call applied zero damage"), ReentrantOutcome->DamageApplied, 0.0f);
	}
	TestEqual(TEXT("no second death event fired after the reentry"), DiedCount, 1);

	// A post-mortem re-send of a NEW key on the corpse is refused as well.
	const FShotId SecondShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitRequest CorpseRequest = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), SecondShotId);
	const FUnifiedHitOutcome CorpseOutcome = ApplyUnifiedHit(CorpseRequest, Scene.Ledger);
	TestTrue(TEXT("a hit on a corpse is refused"), CorpseOutcome.bWasBlocked);
	TestTrue(TEXT("the corpse hit names TargetDead"),
		CorpseOutcome.RefuseReason == EUnifiedHitRefuseReason::TargetDead);
	TestEqual(TEXT("the death event still fired exactly once"), DiedCount, 1);
	return true;
}

// ---------------------------------------------------------------------------
// SuperArmorTakesDamageWithoutControl
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012SuperArmorTakesDamageWithoutControl,
	"UEMMO.Tasks.M5_012.SuperArmorTakesDamageWithoutControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012SuperArmorTakesDamageWithoutControl::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. A blanket control-immune target (the super armor policy shape):
	// full damage, zero control.
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		FTargetReaction SuperArmor = MakeNormalTarget();
		SuperArmor.PolicyId = TEXT("super_armor");
		SuperArmor.bImmuneControl = true;
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLauncher(), SuperArmor, ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestFalse(TEXT("the super armor hit is not refused"), Outcome.bWasBlocked);
		TestEqual(TEXT("the super armor target takes the full launcher damage"), Outcome.DamageApplied, 18.0f);
		TestFalse(TEXT("the super armor target takes no fake immunity"), Outcome.bWasImmune);
		TestFalse(TEXT("the super armor target accepts no stagger"), Outcome.Control.bStagger);
		TestFalse(TEXT("the super armor target accepts no launch"), Outcome.Control.bLaunch);
		TestFalse(TEXT("the super armor target accepts no knockdown"), Outcome.Control.bKnockdown);
		if (TestNotNull(TEXT("precondition: the target carries a combat component"), Scene.TargetCombat))
		{
			TestTrue(TEXT("the super armor victim keeps its action state"),
				Scene.TargetCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		}
		TestTrue(TEXT("the super armor hit bridges the legacy damage path"), Outcome.BridgesLegacyHitConfirmed());
		TestFalse(TEXT("the super armor hit is not the pure-control path"), Outcome.TakesPureControlPath());
	}

	// 2. A gate-closed heavy target shape: the launch gate refuses the launch
	// while the damage stands.
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		FTargetReaction Heavy = MakeNormalTarget();
		Heavy.PolicyId = TEXT("heavy");
		Heavy.bAllowLaunch = false;
		Heavy.PoiseMax = 120.0f;
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLauncher(), Heavy, ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestFalse(TEXT("the heavy target's hit is not refused"), Outcome.bWasBlocked);
		TestEqual(TEXT("the heavy target takes the full launcher damage"), Outcome.DamageApplied, 18.0f);
		TestTrue(TEXT("the heavy target still accepts the stagger"), Outcome.Control.bStagger);
		TestFalse(TEXT("the heavy target refuses the launch"), Outcome.Control.bLaunch);
		TestFalse(TEXT("a refused launch implies no knockdown"), Outcome.Control.bKnockdown);
	}
	return true;
}

// ---------------------------------------------------------------------------
// DamageImmuneTargetTakesPureControlPath
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012DamageImmuneTargetTakesPureControlPath,
	"UEMMO.Tasks.M5_012.DamageImmuneTargetTakesPureControlPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012DamageImmuneTargetTakesPureControlPath::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}

	FTargetReaction DamageImmune = MakeNormalTarget();
	DamageImmune.PolicyId = TEXT("ghost");
	DamageImmune.bImmuneDamage = true;
	const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLauncher(), DamageImmune, ShotId);
	const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);

	// The immune hit connects but applies exactly zero (never the legacy
	// min-1 fake damage) and is not a refusal.
	TestEqual(TEXT("the damage-immune target takes exactly zero"), Outcome.DamageApplied, 0.0f);
	TestTrue(TEXT("the outcome reports the immunity"), Outcome.bWasImmune);
	TestFalse(TEXT("the immunity is not a refusal"), Outcome.bWasBlocked);
	TestEqual(TEXT("the immune hit still decides Apply"), Outcome.Decision, EHitDecision::Apply);
	TestEqual(TEXT("the immune target keeps its full health"), Scene.TargetHealth->GetHealth(), 100.0f);

	// The control side stands: the launcher's stagger/launch/knockdown are
	// all accepted for the normal gates of this policy.
	TestTrue(TEXT("the immune target accepts the stagger"), Outcome.Control.bStagger);
	TestTrue(TEXT("the immune target accepts the launch"), Outcome.Control.bLaunch);
	TestTrue(TEXT("the immune target accepts the knockdown"), Outcome.Control.bKnockdown);

	// The bridging rules: zero damage never bridges OnHitConfirmed; the
	// accepted control routes the hit through the pure-control path instead.
	TestFalse(TEXT("a zero-damage hit bridges no legacy damage event"), Outcome.BridgesLegacyHitConfirmed());
	TestTrue(TEXT("the pure-control path takes over"), Outcome.TakesPureControlPath());

	// The pure control went through the legacy victim path: the target is
	// stunned by the accepted stagger request.
	TestTrue(TEXT("the pure-control hit stunned the victim through the legacy path"),
		Scene.TargetCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);

	// The event and the shot control were committed exactly once.
	TestEqual(TEXT("the ledger recorded the immune hit event"), Scene.Ledger.GetNumRecordedEvents(), 1);
	TestEqual(TEXT("the ledger accepted the shot control"), Scene.Ledger.GetNumAcceptedControls(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// ControlPolicyGatesAndPelletControlDedup
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012ControlPolicyGatesAndPelletControlDedup,
	"UEMMO.Tasks.M5_012.ControlPolicyGatesAndPelletControlDedup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012ControlPolicyGatesAndPelletControlDedup::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	// A knockdown-gated target: the launch lands but the landing knockdown
	// is refused.
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		FTargetReaction NoKnockdown = MakeNormalTarget();
		NoKnockdown.PolicyId = TEXT("no_down");
		NoKnockdown.bAllowKnockdown = false;
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLauncher(), NoKnockdown, ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestTrue(TEXT("the knockdown-gated target still accepts the launch"), Outcome.Control.bLaunch);
		TestTrue(TEXT("the knockdown-gated target still accepts the stagger"), Outcome.Control.bStagger);
		TestFalse(TEXT("the knockdown-gated target refuses the knockdown"), Outcome.Control.bKnockdown);
		TestEqual(TEXT("the knockdown gate does not touch the damage"), Outcome.DamageApplied, 18.0f);
	}

	// Multi-pellet control dedup: pellet 0 applies the shot's control; pellet
	// 1 damages again through its own key but the shot's control on the
	// target is consumed exactly once.
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Pellet0 = MakeHitRequest(Scene, MakeLauncher(), MakeNormalTarget(), ShotId, /*PelletIndex*/ 0);
		const FUnifiedHitRequest Pellet1 = MakeHitRequest(Scene, MakeLauncher(), MakeNormalTarget(), ShotId, /*PelletIndex*/ 1);

		const FUnifiedHitOutcome First = ApplyUnifiedHit(Pellet0, Scene.Ledger);
		TestTrue(TEXT("precondition: pellet 0 applied its damage"), First.DamageApplied > 0.0f);
		TestTrue(TEXT("precondition: pellet 0 applied the shot control"),
			First.Control.bStagger && First.Control.bLaunch && First.Control.bKnockdown);

		const FUnifiedHitOutcome Second = ApplyUnifiedHit(Pellet1, Scene.Ledger);
		TestEqual(TEXT("pellet 1 is its own event and applies its damage"), Second.DamageApplied, 18.0f);
		TestFalse(TEXT("pellet 1 is not refused"), Second.bWasBlocked);
		TestFalse(TEXT("pellet 1 cannot repeat the shot control"), Second.Control.bStagger || Second.Control.bLaunch || Second.Control.bKnockdown);
		TestEqual(TEXT("the target took both pellets' damage"), Scene.TargetHealth->GetHealth(), 100.0f - 36.0f);
		TestEqual(TEXT("the ledger recorded both pellet events"), Scene.Ledger.GetNumRecordedEvents(), 2);
		TestEqual(TEXT("the ledger holds exactly one shot control"), Scene.Ledger.GetNumAcceptedControls(), 1);
	}
	return true;
}

// ---------------------------------------------------------------------------
// InvalidTargetActorSkipsSafely
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012InvalidTargetActorSkipsSafely,
	"UEMMO.Tasks.M5_012.InvalidTargetActorSkipsSafely",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012InvalidTargetActorSkipsSafely::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}
	const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);

	// A null weak reference skips safely: nothing applies, nothing records.
	{
		FUnifiedHitRequest NullTarget = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		NullTarget.TargetActor = nullptr;
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(NullTarget, Scene.Ledger);
		TestEqual(TEXT("a null target actor is refused as Block_Ignore"), Outcome.Decision, EHitDecision::Block_Ignore);
		TestTrue(TEXT("the null target actor names its reason"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::InvalidTargetActor);
		TestEqual(TEXT("the null target actor applies zero damage"), Outcome.DamageApplied, 0.0f);
		TestEqual(TEXT("the null target actor records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
	}

	// A destroyed actor behind a lingering weak reference skips safely too.
	{
		AActor* Doomed = M5_012_SpawnCombatActor(*World, FVector(300.0, 0.0, 100.0), /*bWithCombat*/ false);
		if (!TestNotNull(TEXT("precondition: the doomed actor spawns"), Doomed))
		{
			return true;
		}
		FCombatEntityMetadata DoomedMeta;
		DoomedMeta.Faction = TEXT("Enemy");
		DoomedMeta.Category = TEXT("target");
		const FEntityId DoomedId = Scene.Registry.RegisterEntity(Doomed, DoomedMeta, Scene.Registry.GetCurrentEpoch());
		TestTrue(TEXT("precondition: the doomed actor registers"), IsValidCombatEntityId(DoomedId));

		FUnifiedHitRequest DoomedRequest = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		DoomedRequest.TargetEntityId = DoomedId;
		DoomedRequest.TargetActor = Doomed;
		Doomed->Destroy();

		TWeakObjectPtr<AActor> Lingering = Doomed;
		TestTrue(TEXT("precondition: the destroyed actor leaves a stale weak reference"), !Lingering.IsValid());

		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(DoomedRequest, Scene.Ledger);
		TestTrue(TEXT("the stale target actor skips safely"), Outcome.bWasBlocked);
		TestTrue(TEXT("the stale target actor names its reason"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::InvalidTargetActor);
		TestEqual(TEXT("the stale target actor applies zero damage"), Outcome.DamageApplied, 0.0f);
		TestEqual(TEXT("the stale target actor records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
	}

	// A target actor that does not match the registered entity the key names
	// is a context mismatch and is refused before anything applies.
	{
		FUnifiedHitRequest Mismatch = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		Mismatch.TargetActor = Scene.Attacker; // the key still names the target entity
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Mismatch, Scene.Ledger);
		TestTrue(TEXT("a mismatched target actor is refused"), Outcome.bWasBlocked);
		TestTrue(TEXT("the mismatched target actor names its reason"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::InvalidTargetActor);
		TestEqual(TEXT("the mismatched target actor records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
		TestEqual(TEXT("the mismatched target actor damages nobody"), Scene.Attacker->FindComponentByClass<UHealthComponent>()->GetHealth(), 100.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// IllegalContextsLeaveNoHalfState
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012IllegalContextsLeaveNoHalfState,
	"UEMMO.Tasks.M5_012.IllegalContextsLeaveNoHalfState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012IllegalContextsLeaveNoHalfState::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}
	const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);

	// Every unusable key is refused before the ledger and names its face.
	auto ExpectUnusable = [this, &Scene](const TCHAR* What, FUnifiedHitRequest Request)
	{
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		const FString Label = FString::Printf(TEXT("%s is refused"), What);
		TestTrue(Label, Outcome.bWasBlocked);
		TestTrue(FString::Printf(TEXT("%s names UnusableKey"), What),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::UnusableKey);
		TestEqual(FString::Printf(TEXT("%s applies zero damage"), What), Outcome.DamageApplied, 0.0f);
		TestEqual(FString::Printf(TEXT("%s records nothing"), What), Scene.Ledger.GetNumRecordedEvents(), 0);
	};

	{
		FUnifiedHitRequest ZeroEpoch = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		ZeroEpoch.Epoch = InvalidCombatEpoch;
		ExpectUnusable(TEXT("a zero epoch"), ZeroEpoch);
	}
	{
		FUnifiedHitRequest ZeroAttacker = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		ZeroAttacker.AttackerEntityId = InvalidCombatEntityId;
		ExpectUnusable(TEXT("a zero attacker id"), ZeroAttacker);
	}
	{
		FUnifiedHitRequest ZeroShot = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), InvalidCombatShotId);
		ExpectUnusable(TEXT("a zero shot id"), ZeroShot);
	}
	{
		FUnifiedHitRequest ZeroTarget = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		ZeroTarget.TargetEntityId = InvalidCombatTargetId;
		ExpectUnusable(TEXT("a zero target id"), ZeroTarget);
	}
	{
		FUnifiedHitRequest BadPellet = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId, InvalidCombatPelletIndex);
		ExpectUnusable(TEXT("the pellet sentinel"), BadPellet);
	}

	// A ledger without a bound registry refuses with UnboundRegistry.
	{
		FHitLedger Unbound;
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Unbound);
		TestTrue(TEXT("an unbound ledger is refused"), Outcome.bWasBlocked);
		TestTrue(TEXT("the unbound ledger names its face"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::LedgerRefused);
		TestTrue(TEXT("the unbound ledger carries UnboundRegistry"),
			Outcome.LedgerReason == EHitLedgerRejectReason::UnboundRegistry);
		TestEqual(TEXT("the unbound ledger applied zero damage"), Outcome.DamageApplied, 0.0f);
	}

	// An illegal damage context (the resolver refuses) is refused AFTER the
	// identity validation but BEFORE the ledger commit: the transaction
	// leaves no half dedup and no half damage.
	{
		FDamageProfile NaNProfile = MakeLight01();
		NaNProfile.BaseDamage = std::numeric_limits<float>::quiet_NaN();
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, NaNProfile, MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestTrue(TEXT("a NaN damage context is refused"), Outcome.bWasBlocked);
		TestTrue(TEXT("the NaN damage context names the resolver face"),
			Outcome.RefuseReason == EUnifiedHitRefuseReason::DamageContextRefused);
		TestTrue(TEXT("the NaN damage context carries NonFiniteInput"),
			Outcome.DamageBlockReason == EDamageBlockReason::NonFiniteInput);
		TestEqual(TEXT("the NaN damage context applies zero damage"), Outcome.DamageApplied, 0.0f);
		TestEqual(TEXT("the NaN damage context records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
		TestEqual(TEXT("the NaN damage context removes no health"), Scene.TargetHealth->GetHealth(), 100.0f);
		TestFalse(TEXT("the NaN damage context accepts no control"),
			Outcome.Control.bStagger || Outcome.Control.bLaunch || Outcome.Control.bKnockdown);
	}

	// A negative attack power is refused the same way with its own reason.
	{
		FUnifiedHitRequest Negative = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		Negative.AttackPower = -1.0f;
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Negative, Scene.Ledger);
		TestTrue(TEXT("a negative attack power is refused"), Outcome.bWasBlocked);
		TestTrue(TEXT("the negative attack power carries NegativeInput"),
			Outcome.DamageBlockReason == EDamageBlockReason::NegativeInput);
		TestEqual(TEXT("the negative attack power records no ledger event"), Scene.Ledger.GetNumRecordedEvents(), 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// DefaultCalculatorReproducesLegacyDamageAndIsInjectable
// ---------------------------------------------------------------------------

namespace UE::UEMMO::Tasks::M5_012
{
	/** A counting fake calculator: proves the computation seam is injectable. */
	struct FM5_012_FixedCalculator final : IUnifiedHitDamageCalculator
	{
		mutable int32 CallCount = 0;
		FDamageOutcome Fixed;

		virtual FDamageOutcome ResolveDamage(const FUnifiedHitRequest& /*Request*/) const override
		{
			++CallCount;
			return Fixed;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012DefaultCalculatorReproducesLegacyDamageAndIsInjectable,
	"UEMMO.Tasks.M5_012.DefaultCalculatorReproducesLegacyDamageAndIsInjectable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012DefaultCalculatorReproducesLegacyDamageAndIsInjectable::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. The default calculator reproduces the legacy light_01 damage on a
	// defense-free target (no combat component = the legacy 0 defense).
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy"), /*bTargetWithCombat*/ false))
		{
			return true;
		}
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestEqual(TEXT("the default calculator reproduces the legacy light_01 damage"), Outcome.DamageApplied, 10.0f);
	}

	// 2. The defense comes from the target's combat component (the legacy
	// M3-010 lookup): Defense 25 turns light_01's 10 into 8.
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		Scene.TargetCombat->SetCombatStats(0.0f, 25.0f);
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger);
		TestEqual(TEXT("the default calculator reads the target defense (25 turns 10 into 8)"), Outcome.DamageApplied, 8.0f);

		// 3. The request's AttackPower is the attacker context: (10+5)*100/125=12.
		const FShotId SecondShotId = Scene.AllocateShot(Scene.AttackerId);
		FUnifiedHitRequest Powered = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), SecondShotId);
		Powered.AttackPower = 5.0f;
		const FUnifiedHitOutcome PoweredOutcome = ApplyUnifiedHit(Powered, Scene.Ledger);
		TestEqual(TEXT("attack power 5 against defense 25 deals 12"), PoweredOutcome.DamageApplied, 12.0f);
	}

	// 4. The computation seam is injectable: a fake calculator answers every
	// hit exactly once with its fixed outcome.
	{
		FM5_012_Scene Scene;
		if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
		{
			return true;
		}
		FM5_012_FixedCalculator Calculator;
		Calculator.Fixed.FinalDamage = 42.0f;
		Calculator.Fixed.bStaggerAccepted = true;
		const FShotId ShotId = Scene.AllocateShot(Scene.AttackerId);
		const FUnifiedHitRequest Request = MakeHitRequest(Scene, MakeLight01(), MakeNormalTarget(), ShotId);
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, Scene.Ledger, Calculator);
		TestEqual(TEXT("the injected calculator's damage is applied"), Outcome.DamageApplied, 42.0f);
		TestTrue(TEXT("the injected calculator's control summary is honored"), Outcome.Control.bStagger);
		TestEqual(TEXT("the injected calculator was consulted exactly once"), Calculator.CallCount, 1);
		TestEqual(TEXT("the injected damage reached the health component"), Scene.TargetHealth->GetHealth(), 58.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// DeathResistantTargetSurvivesAtOneHealth
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_012DeathResistantTargetSurvivesAtOneHealth,
	"UEMMO.Tasks.M5_012.DeathResistantTargetSurvivesAtOneHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_012DeathResistantTargetSurvivesAtOneHealth::RunTest(const FString& Parameters)
{
	UWorld* World = M5_012_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_012_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_012_Scene Scene;
	if (!Scene.Build(*this, *World, TEXT("Player"), TEXT("Enemy")))
	{
		return true;
	}

	FTargetReaction DeathResistant = MakeNormalTarget();
	DeathResistant.PolicyId = TEXT("boss");
	DeathResistant.bDeathResistant = true;

	FDamageProfile Lethal = MakeLight01();
	Lethal.DamageProfileId = TEXT("lethal_probe");
	Lethal.BaseDamage = 999.0f;

	// The first lethal-sized hit is capped so one health point survives: the
	// death-resistant target takes damage but never dies from it.
	const FShotId FirstShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitRequest First = MakeHitRequest(Scene, Lethal, DeathResistant, FirstShotId);
	const FUnifiedHitOutcome FirstOutcome = ApplyUnifiedHit(First, Scene.Ledger);
	TestFalse(TEXT("the death-resistant hit is not refused"), FirstOutcome.bWasBlocked);
	TestEqual(TEXT("the death-resistant target took the capped damage"), FirstOutcome.DamageApplied, 99.0f);
	TestEqual(TEXT("the death-resistant target survives at exactly one health"), Scene.TargetHealth->GetHealth(), 1.0f);
	TestTrue(TEXT("the death-resistant target is still alive"), Scene.TargetHealth->IsAlive());
	TestFalse(TEXT("the capped hit did not kill"), FirstOutcome.bTargetDied);

	// A second lethal-sized hit at one health is capped to zero: the target
	// takes no fake damage and stays alive; the accepted control rides the
	// pure-control path.
	const FShotId SecondShotId = Scene.AllocateShot(Scene.AttackerId);
	const FUnifiedHitRequest Second = MakeHitRequest(Scene, Lethal, DeathResistant, SecondShotId);
	const FUnifiedHitOutcome SecondOutcome = ApplyUnifiedHit(Second, Scene.Ledger);
	TestFalse(TEXT("the capped follow-up is not refused"), SecondOutcome.bWasBlocked);
	TestEqual(TEXT("the capped follow-up applies zero damage"), SecondOutcome.DamageApplied, 0.0f);
	TestTrue(TEXT("the capped follow-up leaves the target alive"), Scene.TargetHealth->IsAlive());
	TestFalse(TEXT("the capped follow-up did not kill"), SecondOutcome.bTargetDied);
	TestTrue(TEXT("the capped follow-up still applies its accepted control"), SecondOutcome.Control.bStagger);
	TestTrue(TEXT("the zero-damage control hit takes the pure-control path"), SecondOutcome.TakesPureControlPath());
	return true;
}

#endif
