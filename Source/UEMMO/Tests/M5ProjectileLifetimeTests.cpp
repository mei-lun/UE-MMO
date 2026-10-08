// M5-027: the projectile spawn transaction and lifetime (owner 027, the
// World-lifetime face). Pins the all-or-nothing volley spawn (a failed
// commit destroys what it prepared - never half a volley), the reservation
// cancel/release faces, the epoch cleanup (map unload / room exit / retry
// destroy every live actor and the count returns to baseline 0), the
// Pause-frozen LifeSpan timeout, the single-motion-source actor structure
// and the value-snapshot provenance that never reads source JSON.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include "../Combat/System/CombatEventTypes.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/ProjectileWorldService.h"

#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/Actor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_027
{
	/** A valid straight-line definition (a value snapshot; no catalog involved). */
	static FProjectileDefinition M5_027_MakeDefinition()
	{
		FProjectileDefinition Definition;
		Definition.ProjectileId = TEXT("bullet_linear_test");
		Definition.Motion = EProjectileMotion::Straight;
		Definition.SpeedCmS = 3000.0f;
		Definition.LifetimeS = 2.0f;
		Definition.DamageProfileId = TEXT("hitscan_test_round");
		Definition.PierceCount = 0;
		Definition.ExplosionRadiusCm = 0.0f;
		Definition.HomingTurnRateDegS = 0.0f;
		return Definition;
	}

	/** The committed-shot snapshot everything below perturbs. */
	static FShotContext M5_027_MakeShot(FCombatEpoch Epoch, FEntityId SourceId, FShotId ShotId, int32 PelletCount = 1)
	{
		FShotContext Shot;
		Shot.Epoch = Epoch;
		Shot.SourceEntityId = SourceId;
		Shot.ShotId = ShotId;
		Shot.WeaponInstanceId = FGuid(0xA1, 0xB2, 0xC3, 0xD4);
		Shot.WeaponDefinitionId = TEXT("pistol_test");
		Shot.FireMode = EWeaponFireMode::Projectile;
		Shot.DamageProfileId = TEXT("hitscan_test_round");
		Shot.ProjectileId = TEXT("bullet_linear_test");
		Shot.PelletCount = PelletCount;
		return Shot;
	}

	/** N straight +X pellets from index 0..N-1. */
	static TArray<FShotPellet> M5_027_MakePellets(int32 Count)
	{
		TArray<FShotPellet> Pellets;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FShotPellet Pellet;
			Pellet.PelletIndex = static_cast<FPelletIndex>(Index);
			Pellet.Direction = FVector(1.0, 0.0, 0.0);
			Pellets.Add(Pellet);
		}
		return Pellets;
	}

	/** World acquisition, the M5_012 pattern: private temp world, GWorld fallback. */
	static UWorld* M5_027_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_027_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_027 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_027 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_027_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_027_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	/**
	 * The production-shaped spawner bound to the test world, plus an optional
	 * failure injection: SpawnFailAfter = N spawns the Nth (0-based) pellet
	 * request null, emulating a mid-volley spawn refusal.
	 */
	struct FTestSpawner : IProjectileSpawner
	{
		UWorld* World = nullptr;
		int32 SpawnFailAfter = -1;
		int32 SpawnCalls = 0;
		int32 DestroyCalls = 0;
		TArray<ACombatProjectile*> Spawned;

		virtual ACombatProjectile* SpawnPellet(const FProjectileSpawnReservation& Reservation, const FShotPellet& Pellet) override
		{
			const int32 CallIndex = SpawnCalls++;
			if (World == nullptr || CallIndex == SpawnFailAfter)
			{
				return nullptr;
			}
			FActorSpawnParameters Params;
			ACombatProjectile* Actor = World->SpawnActor<ACombatProjectile>(ACombatProjectile::StaticClass(),
				Reservation.Origin, FRotator::ZeroRotator, Params);
			if (Actor == nullptr)
			{
				return nullptr;
			}
			Actor->Context.Epoch = Reservation.Epoch;
			Actor->Context.SourceEntityId = Reservation.Shot.SourceEntityId;
			Actor->Context.ShotId = Reservation.Shot.ShotId;
			Actor->Context.PelletIndex = Pellet.PelletIndex;
			Actor->Context.WeaponInstanceId = Reservation.Shot.WeaponInstanceId;
			Actor->Context.ProjectileInstanceId = Reservation.ProjectileInstanceId;
			Actor->Context.ConfigRevision = Reservation.ConfigRevision;
			Actor->Context.ProjectileId = Reservation.Definition.ProjectileId;
			Actor->LaunchSpeedCmS = Reservation.Definition.SpeedCmS;
			Actor->MotionGravityScale = Reservation.Definition.Motion == EProjectileMotion::Parabolic ? 1.0f : 0.0f;
			if (Actor->Movement != nullptr)
			{
				Actor->Movement->InitialSpeed = Reservation.Definition.SpeedCmS;
				Actor->Movement->MaxSpeed = Reservation.Definition.SpeedCmS;
				Actor->Movement->ProjectileGravityScale = Actor->MotionGravityScale;
			}
			Actor->SetLifeSpan(Reservation.Definition.LifetimeS);
			Spawned.Add(Actor);
			return Actor;
		}

		virtual void DestroyPellet(ACombatProjectile& PelletActor) override
		{
			++DestroyCalls;
			Spawned.Remove(&PelletActor);
			if (PelletActor.GetWorld() != nullptr && IsValid(&PelletActor))
			{
				PelletActor.Destroy();
			}
		}
	};

	/** One reserve+commit helper returning the reserve outcome. */
	static FProjectileReserveOutcome M5_027_Reserve(FAutomationTestBase& Test, FProjectileWorldService& Service,
		const FShotContext& Shot, const FProjectileDefinition& Definition, int32 PelletCount,
		FProjectileSpawnReservation& OutReservation, const TCHAR* What,
		const FString& ConfigRevision = TEXT("rev-test-abc-001"),
		const FVector& Origin = FVector(60000.0, 50000.0, 800.0))
	{
		const FProjectileReserveOutcome Outcome = Service.ReserveProjectiles(Shot, Definition, Origin,
			M5_027_MakePellets(PelletCount), ConfigRevision, OutReservation);
		if (!Outcome.bReserved)
		{
			Test.TestFalse(FString::Printf(TEXT("%s: the reservation was granted"), What), Outcome.bReserved);
		}
		return Outcome;
	}
}

using namespace UE::UEMMO::Tasks::M5_027;

// ---------------------------------------------------------------------------
// InvalidReserveNamedRefusal
// ---------------------------------------------------------------------------

// Every pre-reservation refusal is named and reserves nothing: a null world,
// an unusable shot identity, a pellet mismatch, a bad direction, a
// non-finite origin, an invalid definition and a full capacity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_027InvalidReserveNamedRefusal,
	"UEMMO.Tasks.M5_027.InvalidReserveNamedRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_027InvalidReserveNamedRefusal::RunTest(const FString& Parameters)
{
	UWorld* World = M5_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_027_WorldScope WorldScope;
	WorldScope.World = World;

	FProjectileWorldService Service(World);
	const FCombatEpoch Epoch = Service.GetCurrentEpoch();
	const FProjectileDefinition Definition = M5_027_MakeDefinition();

	// Null world: a service bound to nothing refuses with its own name.
	FProjectileWorldService NullService(nullptr);
	FProjectileSpawnReservation NullReservation;
	const FProjectileReserveOutcome NullOutcome = NullService.ReserveProjectiles(
		M5_027_MakeShot(Epoch, 1, 1, 1), Definition, FVector(60000.0, 50000.0, 800.0),
		M5_027_MakePellets(1), TEXT("rev"), NullReservation);
	TestFalse(TEXT("a null world refuses the reservation"), NullOutcome.bReserved);
	TestEqual(TEXT("a null world names its reject"), NullOutcome.Reject, EProjectileReserveReject::NullWorld);

	auto ReserveRejects = [&](const TCHAR* What, const FShotContext& Shot, const FProjectileDefinition& Def,
		int32 PelletCount, const FVector& Origin, EProjectileReserveReject Expected)
	{
		FProjectileSpawnReservation Reservation;
		const FProjectileReserveOutcome Outcome = Service.ReserveProjectiles(Shot, Def, Origin,
			M5_027_MakePellets(PelletCount), TEXT("rev"), Reservation);
		TestFalse(FString::Printf(TEXT("%s is refused"), What), Outcome.bReserved);
		TestEqual(FString::Printf(TEXT("%s names its reject"), What), Outcome.Reject, Expected);
		TestTrue(FString::Printf(TEXT("%s names the offending detail"), What), !Outcome.RejectDetail.IsEmpty());
	};

	ReserveRejects(TEXT("an invalid epoch"), M5_027_MakeShot(InvalidCombatEpoch, 1, 1, 1), Definition, 1,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::InvalidShotContext);
	ReserveRejects(TEXT("an invalid source"), M5_027_MakeShot(Epoch, InvalidCombatEntityId, 1, 1), Definition, 1,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::InvalidShotContext);
	ReserveRejects(TEXT("an invalid shot id"), M5_027_MakeShot(Epoch, 1, InvalidCombatShotId, 1), Definition, 1,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::InvalidShotContext);
	ReserveRejects(TEXT("a pellet count mismatch"), M5_027_MakeShot(Epoch, 1, 2, 3), Definition, 2,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::PelletCountMismatch);
	ReserveRejects(TEXT("an empty pellet list"), M5_027_MakeShot(Epoch, 1, 2, 0), Definition, 0,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::PelletCountMismatch);

	FProjectileDefinition BadSpeed = Definition;
	BadSpeed.SpeedCmS = 0.0f;
	ReserveRejects(TEXT("a zero-speed definition"), M5_027_MakeShot(Epoch, 1, 3, 1), BadSpeed, 1,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::InvalidDefinition);

	FProjectileDefinition BadHoming = Definition;
	BadHoming.Motion = EProjectileMotion::Homing;
	ReserveRejects(TEXT("a homing definition without turn rate"), M5_027_MakeShot(Epoch, 1, 4, 1), BadHoming, 1,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::InvalidDefinition);

	ReserveRejects(TEXT("a non-finite origin"), M5_027_MakeShot(Epoch, 1, 5, 1), Definition, 1,
		FVector(60000.0, std::numeric_limits<double>::quiet_NaN(), 800.0), EProjectileReserveReject::InvalidOrigin);

	// Capacity: fill the service to the hard cap with single-pellet volleys,
	// then the next reservation refuses with CapacityFull.
	FTestSpawner Spawner;
	Spawner.World = World;
	for (int32 Index = 0; Index < MaxLiveProjectiles; ++Index)
	{
		FProjectileSpawnReservation Reservation;
		const FShotContext Shot = M5_027_MakeShot(Epoch, 1, static_cast<FShotId>(100 + Index), 1);
		const FProjectileReserveOutcome Outcome = Service.ReserveProjectiles(Shot, Definition,
			FVector(60000.0, 50000.0, 800.0), M5_027_MakePellets(1), TEXT("rev"), Reservation);
		if (!TestTrue(FString::Printf(TEXT("capacity fill %d reserved"), Index), Outcome.bReserved))
		{
			return true;
		}
		if (!TestTrue(FString::Printf(TEXT("capacity fill %d committed"), Index), Service.CommitSpawn(Reservation, Spawner)))
		{
			return true;
		}
	}
	TestEqual(TEXT("the cap is reached"), Service.GetNumLiveProjectiles(), MaxLiveProjectiles);
	ReserveRejects(TEXT("a full capacity"), M5_027_MakeShot(Epoch, 1, 9999, 1), Definition, 1,
		FVector(60000.0, 50000.0, 800.0), EProjectileReserveReject::CapacityFull);

	return true;
}

// ---------------------------------------------------------------------------
// CommitAllOrNothingNoHalfVolley
// ---------------------------------------------------------------------------

// The transaction face: a full commit spawns every pellet actor carrying the
// same provenance; a commit whose Nth spawn fails destroys everything it
// prepared and lands the reservation in Failed - never half a volley.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_027CommitAllOrNothingNoHalfVolley,
	"UEMMO.Tasks.M5_027.CommitAllOrNothingNoHalfVolley",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_027CommitAllOrNothingNoHalfVolley::RunTest(const FString& Parameters)
{
	UWorld* World = M5_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_027_WorldScope WorldScope;
	WorldScope.World = World;

	FProjectileWorldService Service(World);
	const FCombatEpoch Epoch = Service.GetCurrentEpoch();
	const FProjectileDefinition Definition = M5_027_MakeDefinition();
	FTestSpawner Spawner;
	Spawner.World = World;

	// Full commit: three pellets, one reservation, one instance identity.
	FProjectileSpawnReservation Reservation;
	const FShotContext Shot = M5_027_MakeShot(Epoch, 7, 42, 3);
	if (!TestTrue(TEXT("the three-pellet volley reserved"),
		M5_027_Reserve(*this, Service, Shot, Definition, 3, Reservation, TEXT("the three-pellet volley")).bReserved))
	{
		return true;
	}
	TestTrue(TEXT("the reservation mints an instance identity"), Reservation.ProjectileInstanceId.IsValid());
	if (!TestTrue(TEXT("the three-pellet volley committed"), Service.CommitSpawn(Reservation, Spawner)))
	{
		return true;
	}
	TestEqual(TEXT("all three pellet actors spawned"), Spawner.Spawned.Num(), 3);
	TestEqual(TEXT("the live count names all three"), Service.GetNumLiveProjectiles(), 3);
	for (int32 Index = 0; Index < Spawner.Spawned.Num(); ++Index)
	{
		const ACombatProjectile* Actor = Spawner.Spawned[Index];
		TestEqual(FString::Printf(TEXT("pellet %d names the shot epoch"), Index), Actor->Context.Epoch, Epoch);
		TestEqual(FString::Printf(TEXT("pellet %d names the source"), Index), Actor->Context.SourceEntityId, 7ULL);
		TestEqual(FString::Printf(TEXT("pellet %d names the shot id"), Index), Actor->Context.ShotId, 42ULL);
		TestEqual(FString::Printf(TEXT("pellet %d names its pellet slot"), Index),
			static_cast<int32>(Actor->Context.PelletIndex), Index);
		TestTrue(FString::Printf(TEXT("pellet %d shares the volley instance id"), Index),
			Actor->Context.ProjectileInstanceId == Reservation.ProjectileInstanceId);
	}

	// Half-volley refusal: the second spawn request fails; the first actor is
	// destroyed, the live count returns to 0 and the reservation is Failed.
	FProjectileSpawnReservation HalfReservation;
	const FShotContext HalfShot = M5_027_MakeShot(Epoch, 7, 43, 3);
	if (!TestTrue(TEXT("the failing volley reserved"),
		M5_027_Reserve(*this, Service, HalfShot, Definition, 3, HalfReservation, TEXT("the failing volley")).bReserved))
	{
		return true;
	}
	FTestSpawner FailingSpawner;
	FailingSpawner.World = World;
	FailingSpawner.SpawnFailAfter = 1;
	ACombatProjectile* FirstActor = nullptr;
	const bool bCommitted = Service.CommitSpawn(HalfReservation, FailingSpawner);
	TestFalse(TEXT("the failing commit is refused"), bCommitted);
	TestTrue(TEXT("the failed commit destroyed the prepared actor"), FailingSpawner.DestroyCalls >= 1);
	if (FailingSpawner.Spawned.Num() > 0)
	{
		FirstActor = FailingSpawner.Spawned[0];
	}
	TestTrue(TEXT("no prepared actor survived"), FirstActor == nullptr || !IsValid(FirstActor));
	TestEqual(TEXT("the live count returned to the committed baseline"), Service.GetNumLiveProjectiles(), 3);
	TestEqual(TEXT("the reservation landed in Failed"), HalfReservation.State, EProjectileReservationState::Failed);
	TestTrue(TEXT("the failed commit names its detail"), !HalfReservation.RejectDetail.IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// CancelReservationLeavesNothing
// ---------------------------------------------------------------------------

// A reserved-but-uncommitted volley leaves nothing behind and the slots are
// reusable; a cancelled reservation can never commit, and a committed volley
// is never late-cancelled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_027CancelReservationLeavesNothing,
	"UEMMO.Tasks.M5_027.CancelReservationLeavesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_027CancelReservationLeavesNothing::RunTest(const FString& Parameters)
{
	UWorld* World = M5_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_027_WorldScope WorldScope;
	WorldScope.World = World;

	FProjectileWorldService Service(World);
	const FCombatEpoch Epoch = Service.GetCurrentEpoch();
	const FProjectileDefinition Definition = M5_027_MakeDefinition();
	FTestSpawner Spawner;
	Spawner.World = World;

	// Reserve then cancel: no actor, no slot leak.
	FProjectileSpawnReservation Reservation;
	if (!TestTrue(TEXT("the cancellation volley reserved"),
		M5_027_Reserve(*this, Service, M5_027_MakeShot(Epoch, 7, 51, 2), Definition, 2, Reservation, TEXT("the cancellation volley")).bReserved))
	{
		return true;
	}
	Service.CancelReservation(Reservation);
	TestEqual(TEXT("the cancelled volley spawned nothing"), Spawner.Spawned.Num(), 0);
	TestEqual(TEXT("the cancelled volley holds no slots"), Service.GetNumLiveProjectiles(), 0);
	TestEqual(TEXT("the reservation landed in Cancelled"), Reservation.State, EProjectileReservationState::Cancelled);

	// A cancelled reservation can never commit; the cancel is idempotent.
	Service.CancelReservation(Reservation);
	TestEqual(TEXT("a second cancel is a no-op"), Reservation.State, EProjectileReservationState::Cancelled);
	TestFalse(TEXT("a cancelled reservation refuses to commit"), Service.CommitSpawn(Reservation, Spawner));
	TestEqual(TEXT("the refused commit spawned nothing"), Spawner.Spawned.Num(), 0);

	// The released slots are reusable: a fresh volley commits normally.
	FProjectileSpawnReservation FreshReservation;
	if (!TestTrue(TEXT("the fresh volley reserved after cancel"),
		M5_027_Reserve(*this, Service, M5_027_MakeShot(Epoch, 7, 52, 2), Definition, 2, FreshReservation, TEXT("the fresh volley")).bReserved))
	{
		return true;
	}
	TestTrue(TEXT("the fresh volley committed"), Service.CommitSpawn(FreshReservation, Spawner));
	TestEqual(TEXT("the fresh volley spawned both pellets"), Spawner.Spawned.Num(), 2);
	TestEqual(TEXT("the fresh volley is live"), Service.GetNumLiveProjectiles(), 2);

	// A committed volley is never late-cancelled.
	Service.CancelReservation(FreshReservation);
	TestEqual(TEXT("a late cancel cannot un-commit"), FreshReservation.State, EProjectileReservationState::Committed);
	TestEqual(TEXT("the committed volley stays live"), Service.GetNumLiveProjectiles(), 2);
	return true;
}

// ---------------------------------------------------------------------------
// TimeoutAndEpochCleanup
// ---------------------------------------------------------------------------

// The lifetime faces: the committed actor's timeout is the definition's
// LifetimeS on the Pause-frozen world clock; BeginNextWorldEpoch destroys
// every live actor (count back to baseline 0) and a reservation minted in
// the old epoch can never commit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_027TimeoutAndEpochCleanup,
	"UEMMO.Tasks.M5_027.TimeoutAndEpochCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_027TimeoutAndEpochCleanup::RunTest(const FString& Parameters)
{
	UWorld* World = M5_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_027_WorldScope WorldScope;
	WorldScope.World = World;

	FProjectileWorldService Service(World);
	const FCombatEpoch Epoch = Service.GetCurrentEpoch();
	const FProjectileDefinition Definition = M5_027_MakeDefinition();
	FTestSpawner Spawner;
	Spawner.World = World;

	// Commit two volleys (one straight, one parabolic for the gravity face).
	FProjectileSpawnReservation StraightReservation;
	if (!TestTrue(TEXT("the straight volley reserved"),
		M5_027_Reserve(*this, Service, M5_027_MakeShot(Epoch, 7, 61, 1), Definition, 1, StraightReservation, TEXT("the straight volley")).bReserved))
	{
		return true;
	}
	if (!TestTrue(TEXT("the straight volley committed"), Service.CommitSpawn(StraightReservation, Spawner)))
	{
		return true;
	}

	FProjectileDefinition Parabolic = Definition;
	Parabolic.Motion = EProjectileMotion::Parabolic;
	FProjectileSpawnReservation ParabolicReservation;
	if (!TestTrue(TEXT("the parabolic volley reserved"),
		M5_027_Reserve(*this, Service, M5_027_MakeShot(Epoch, 7, 62, 1), Parabolic, 1, ParabolicReservation, TEXT("the parabolic volley")).bReserved))
	{
		return true;
	}
	if (!TestTrue(TEXT("the parabolic volley committed"), Service.CommitSpawn(ParabolicReservation, Spawner)))
	{
		return true;
	}
	TestEqual(TEXT("both volleys are live"), Service.GetNumLiveProjectiles(), 2);

	// The timeout face: LifeSpan carries the definition's lifetime; the
	// motion numbers ride the single movement component.
	for (const ACombatProjectile* Actor : Spawner.Spawned)
	{
		TestEqual(TEXT("the actor's lifespan is the definition's"), Actor->GetLifeSpan(), 2.0f);
		TestEqual(TEXT("the actor's launch speed is the definition's"), Actor->Movement->InitialSpeed, 3000.0f);
	}
	TestTrue(TEXT("the straight pellet carries no gravity"), Spawner.Spawned[0]->MotionGravityScale == 0.0f);
	TestTrue(TEXT("the parabolic pellet carries full gravity"), Spawner.Spawned[1]->MotionGravityScale == 1.0f);

	// The epoch face: rebuild the world generation; every actor is destroyed
	// and the live count returns to baseline 0.
	const FCombatEpoch OldEpoch = Service.GetCurrentEpoch();
	TArray<ACombatProjectile*> OldActors = Spawner.Spawned;

	// A reservation minted in the old epoch and held across the rebuild can
	// never commit: the commit face refuses it as stale.
	FProjectileSpawnReservation HeldReservation;
	const FProjectileReserveOutcome HeldOutcome = Service.ReserveProjectiles(
		M5_027_MakeShot(OldEpoch, 7, 63, 1), Definition, FVector(60000.0, 50000.0, 800.0),
		M5_027_MakePellets(1), TEXT("rev"), HeldReservation);
	if (!TestTrue(TEXT("the held volley reserved in the old epoch"), HeldOutcome.bReserved))
	{
		return true;
	}
	TestEqual(TEXT("the held reservation carries the old epoch"), HeldReservation.Epoch, OldEpoch);

	Service.BeginNextWorldEpoch();
	for (const ACombatProjectile* Actor : OldActors)
	{
		TestTrue(TEXT("the old-epoch actor was destroyed"), Actor == nullptr || !IsValid(Actor));
	}
	TestEqual(TEXT("the live count returned to baseline"), Service.GetNumLiveProjectiles(), 0);
	TestEqual(TEXT("the epoch advanced"), Service.GetCurrentEpoch(), OldEpoch + 1);

	TestFalse(TEXT("the held reservation cannot commit after the rebuild"), Service.CommitSpawn(HeldReservation, Spawner));
	TestTrue(TEXT("the refused commit names the stale epoch"), HeldReservation.RejectDetail.Contains(TEXT("stale epoch")));
	TestEqual(TEXT("the refused commit left the baseline"), Service.GetNumLiveProjectiles(), 0);
	TestEqual(TEXT("the held reservation is not committed"), HeldReservation.State, EProjectileReservationState::Reserved);

	// A shot naming the old epoch refuses at reserve in the new generation:
	// a stale volley never even mints a reservation.
	FProjectileSpawnReservation StaleReservation;
	const FProjectileReserveOutcome StaleOutcome = Service.ReserveProjectiles(
		M5_027_MakeShot(OldEpoch, 7, 64, 1), Definition, FVector(60000.0, 50000.0, 800.0),
		M5_027_MakePellets(1), TEXT("rev"), StaleReservation);
	TestFalse(TEXT("a stale-epoch shot refuses to reserve"), StaleOutcome.bReserved);
	TestEqual(TEXT("the stale-epoch reserve names its reject"), StaleOutcome.Reject, EProjectileReserveReject::StaleEpoch);
	TestTrue(TEXT("the stale-epoch reserve names its detail"), !StaleOutcome.RejectDetail.IsEmpty());

	// The new generation still serves its own shots.
	FProjectileSpawnReservation FreshReservation;
	const FCombatEpoch NewEpoch = Service.GetCurrentEpoch();
	const FProjectileReserveOutcome FreshOutcome = Service.ReserveProjectiles(
		M5_027_MakeShot(NewEpoch, 7, 65, 1), Definition, FVector(60000.0, 50000.0, 800.0),
		M5_027_MakePellets(1), TEXT("rev"), FreshReservation);
	TestTrue(TEXT("the new epoch still reserves"), FreshOutcome.bReserved);
	TestEqual(TEXT("the fresh reservation carries the new epoch"), FreshReservation.Epoch, NewEpoch);
	return true;
}

// ---------------------------------------------------------------------------
// SingleMotionSourceNoSecondTick
// ---------------------------------------------------------------------------

// The structural face: a spawned pellet actor has exactly one movement
// component (the single motion driver), never ticks for motion and carries
// one query body for the later sweep work.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_027SingleMotionSourceNoSecondTick,
	"UEMMO.Tasks.M5_027.SingleMotionSourceNoSecondTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_027SingleMotionSourceNoSecondTick::RunTest(const FString& Parameters)
{
	UWorld* World = M5_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_027_WorldScope WorldScope;
	WorldScope.World = World;

	FProjectileWorldService Service(World);
	const FCombatEpoch Epoch = Service.GetCurrentEpoch();
	const FProjectileDefinition Definition = M5_027_MakeDefinition();
	FTestSpawner Spawner;
	Spawner.World = World;

	FProjectileSpawnReservation Reservation;
	if (!TestTrue(TEXT("the structural volley reserved"),
		M5_027_Reserve(*this, Service, M5_027_MakeShot(Epoch, 7, 71, 1), Definition, 1, Reservation, TEXT("the structural volley")).bReserved))
	{
		return true;
	}
	if (!TestTrue(TEXT("the structural volley committed"), Service.CommitSpawn(Reservation, Spawner)))
	{
		return true;
	}
	if (!TestEqual(TEXT("one pellet actor spawned"), Spawner.Spawned.Num(), 1))
	{
		return true;
	}

	ACombatProjectile* Actor = Spawner.Spawned[0];
	TestFalse(TEXT("the actor never ticks for motion"), Actor->PrimaryActorTick.bCanEverTick);
	TArray<UProjectileMovementComponent*> ProjectileMovements;
	Actor->GetComponents(ProjectileMovements);
	TestEqual(TEXT("exactly one projectile movement component"), ProjectileMovements.Num(), 1);
	TArray<UMovementComponent*> Movements;
	Actor->GetComponents(Movements);
	TestEqual(TEXT("exactly one movement component of any kind"), Movements.Num(), 1);
	TestNotNull(TEXT("the actor carries one collision body"), Actor->Body.Get());
	TestTrue(TEXT("the movement drives the body"), Actor->Movement->UpdatedComponent == Actor->Body);
	TestEqual(TEXT("the collision body is query-enabled"), Actor->Body->GetCollisionEnabled(), ECollisionEnabled::QueryAndProbe);
	return true;
}

// ---------------------------------------------------------------------------
// ProvenanceWithoutSourceJson
// ---------------------------------------------------------------------------

// The provenance face: the reservation stamps the committed shot's instance
// ids, source and config revision straight through - any revision string is
// carried verbatim and no config/catalog/parser object takes part in the
// spawn path (the whole fixture builds values only).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_027ProvenanceWithoutSourceJson,
	"UEMMO.Tasks.M5_027.ProvenanceWithoutSourceJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_027ProvenanceWithoutSourceJson::RunTest(const FString& Parameters)
{
	UWorld* World = M5_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_027_WorldScope WorldScope;
	WorldScope.World = World;

	FProjectileWorldService Service(World);
	const FCombatEpoch Epoch = Service.GetCurrentEpoch();
	const FProjectileDefinition Definition = M5_027_MakeDefinition();
	FTestSpawner Spawner;
	Spawner.World = World;

	// A revision string from an arbitrary source is carried verbatim.
	const FString ArbitraryRevision = TEXT("whatever-source-revision-42");
	FProjectileSpawnReservation Reservation;
	if (!TestTrue(TEXT("the provenance volley reserved"),
		M5_027_Reserve(*this, Service, M5_027_MakeShot(Epoch, 9, 81, 2), Definition, 2, Reservation,
			TEXT("the provenance volley"), ArbitraryRevision).bReserved))
	{
		return true;
	}
	TestEqual(TEXT("the reservation carries the revision verbatim"), Reservation.ConfigRevision, ArbitraryRevision);
	if (!TestTrue(TEXT("the provenance volley committed"), Service.CommitSpawn(Reservation, Spawner)))
	{
		return true;
	}
	TestEqual(TEXT("both provenance pellets spawned"), Spawner.Spawned.Num(), 2);
	for (int32 Index = 0; Index < Spawner.Spawned.Num(); ++Index)
	{
		const ACombatProjectile* Actor = Spawner.Spawned[Index];
		TestEqual(FString::Printf(TEXT("pellet %d carries the revision verbatim"), Index), Actor->Context.ConfigRevision, ArbitraryRevision);
		TestTrue(FString::Printf(TEXT("pellet %d carries the weapon instance id"), Index),
			Actor->Context.WeaponInstanceId == Reservation.Shot.WeaponInstanceId);
		TestTrue(FString::Printf(TEXT("pellet %d carries the volley instance id"), Index),
			Actor->Context.ProjectileInstanceId == Reservation.ProjectileInstanceId);
		TestEqual(FString::Printf(TEXT("pellet %d names the projectile id"), Index), Actor->Context.ProjectileId, Definition.ProjectileId);
		TestEqual(FString::Printf(TEXT("pellet %d stamps the launch speed"), Index), Actor->LaunchSpeedCmS, Definition.SpeedCmS);
	}
	return true;
}

#endif
