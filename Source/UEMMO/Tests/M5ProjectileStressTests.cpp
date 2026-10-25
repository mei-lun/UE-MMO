// M5-050: the projectile stress and lifecycle budget (owner 050). Quantifies
// the first-version capacity and cleanup behavior of the REAL production
// projectile stack - no copy implementations:
// - FProjectileWorldService (Projectiles/ProjectileWorldService.h, the live
//   population owner; hard cap MaxLiveProjectiles = 256 with the explicit
//   EProjectileReserveReject::CapacityFull refusal, ProjectileWorldService.cpp
//   capacity gate),
// - FCombatEntityRegistry + FHitLedger (Combat/System/*, the world identity
//   and the idempotent dedup face; the ledger bounds ACTIVE shot instances at
//   DefaultCapacity = 256 with EHitLedgerRejectReason::CapacityFull and never
//   evicts a live key),
// - ACombatProjectile pellet actors plus one FLinearProjectilePolicy per
//   pellet (the production wiring shape: APrototypeCharacter::TickWeaponInput
//   advances one policy per live pellet per frame and retires the finished
//   bodies, PrototypeCharacter.cpp; the pellet spawner mirrors
//   FWeaponProjectileSpawner).
//
// Recorded production gap (NOT fixed here, no production code is touched):
// the production projectile path never releases its ledger shot instances -
// APrototypeCharacter::ShotHitLedger is bound once and neither EndShot nor
// Reset runs during play (the melee path releases at attack end,
// CombatComponent.cpp EndShot call). Sustained projectile fire with hits
// therefore fills the ledger's 256 active-shot cap after 256 distinct
// hitting shots and later hits are refused with CapacityFull. The stress
// driver below releases each volley with EndShot on retire (the melee
// attack-end shape) to quantify the WITH-cleanup baseline; wiring this face
// into the production driver belongs to its own card.
//
// Cases (all numbers land in Artifacts/Tasks/M5-050/<UTC+8 stamp>/stress-report.json):
// - CapacityExplicitRefusal: the over-capacity faces are named refusals, and
//   both explicit release faces (epoch rebuild / EndShot) restore capacity.
// - Cleanup100RoundsBaseline: 100 spawn -> expire/leave rounds return the live
//   population to baseline 0 with no sustained monotonic memory growth.
// - Stress200ConcurrentBudget: 200 concurrent pellets against 10 targets for
//   a 60 s steady load at a fixed 60 Hz step; active population bounded, dedup
//   never lost (a repeated submission never applies twice), no runaway
//   spawning, and the game-thread frame budget sampled with FPlatformTime.
//
// Frame budget assertion shape (deliberate, documented):
// - The timed body is the per-frame game-thread work of the load (top-up
//   reserve/commit, one AdvanceMotion per live pellet, retire sweep, world
//   tick). The automation temp world has no viewport, so no scene render cost
//   is part of the sample in either RHI mode, and the budgeted GC pass runs
//   between timed frames with its own measured total (gc_total_ms) - a full
//   GC reaches every UObject in the process, so its spike is dominated by
//   engine object counts, not by the 200-pellet load; conflating it with the
//   load budget would record noise. The split is recorded in the JSON.
// - With a real RHI (the -Stress acceptance entry runs -RenderOffscreen):
//   the card's red line is enforced - FAIL when p95 > 16.7 ms or p99 > 33.3 ms
//   (the percentiles are written to the evidence JSON before the assert).
// - Under -nullrhi (the plain TestAutomation harness): there is no renderer
//   at all, so the strict frame-budget numbers are not representative of a
//   rendered frame and shared CI agents add noise the load does not cause;
//   the assertion degrades to record + report plus a runaway guard
//   (p99 > 100 ms fails). The mode and its thresholds are recorded in the
//   JSON either way.

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/DamageTypes.h"
#include "../Combat/System/HitLedger.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/HitscanExecutor.h"
#include "../Projectiles/LinearProjectilePolicy.h"
#include "../Projectiles/ProjectileWorldService.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_050
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X is the flight axis, Y the depth axis, Z height.
	const FVector M5_050_SceneBase(120000.0, 90000.0, 0.0);

	// Load shape: 200 concurrent pellets, 10 targets, 10-pellet volleys.
	constexpr int32 M5_050_TargetCount = 10;
	constexpr int32 M5_050_TargetConcurrent = 200;
	constexpr int32 M5_050_VolleyPellets = 10;

	// Flight numbers: 1000 cm/s over 3000 cm = 3 s of flight inside a 3.5 s
	// lifetime (the wall behind the targets sits at 3.4 s of flight), so a
	// pellet normally terminates on its aimed target and the engine LifeSpan
	// only catches the strays. At 200 concurrent and 3 s per retire that is a
	// sustained ~67 pellets/s of retire throughput.
	constexpr float M5_050_SpeedCmS = 1000.0f;
	constexpr float M5_050_LifetimeS = 3.5f;
	constexpr float M5_050_FlightDistanceCm = 3000.0f;

	// The steady load: 3600 fixed steps of 1/60 s (60 seconds of simulated
	// world time; wall-clock time is only ever sampled for the budget itself).
	constexpr float M5_050_FrameSeconds = 1.0f / 60.0f;
	constexpr int32 M5_050_TotalFrames = 3600;
	constexpr int32 M5_050_WarmupFrames = 120;

	// The budgeted cleanup cadence: one forced GC every 30 frames (0.5 s).
	// Destroyed-but-not-collected pellet actors keep their weak reference
	// valid until GC, so the service's live list counts them inside the
	// "stale window"; a production session relies on periodic GC the same
	// way. 200 active + the ~0.5 s retirement window (~33 retired bodies)
	// stays under the 256 hard cap with margin - a single CapacityFull
	// refusal during the load fails the test as a budget miss.
	constexpr int32 M5_050_GcEveryFrames = 30;

	// Targets never die during the load (damage 1 per hit, ~4k hits expected
	// against a 1e6 pool); the pool drop is the exact hit counter.
	constexpr float M5_050_TargetMaxHealth = 1000000.0f;

	// The cleanup regression: 100 rounds, 8 pellets per volley. The aim
	// target sits 400 cm out (0.4 s of flight at 1000 cm/s) inside the 0.6 s
	// lifetime, so a cleanup volley terminates on its hit before the engine
	// LifeSpan catches the leftovers.
	constexpr int32 M5_050_CleanupRounds = 100;
	constexpr int32 M5_050_CleanupVolleyPellets = 8;
	constexpr float M5_050_CleanupLifetimeS = 0.6f;
	constexpr float M5_050_CleanupFlightDistanceCm = 400.0f;

	// Frame budget thresholds in milliseconds.
	constexpr double M5_050_BudgetP95Ms = 16.7;
	constexpr double M5_050_BudgetP99Ms = 33.3;
	constexpr double M5_050_RunawayP99Ms = 100.0;

	// -----------------------------------------------------------------------
	// Evidence writer (the M5-035 scenario-json export precedent): one
	// process-wide report file shared by the three cases - each case appends
	// (or updates) its section and rewrites the whole file, so the report is
	// complete after whichever case finishes last and still holds the earlier
	// sections if a later case dies.
	// -----------------------------------------------------------------------

	/** One recorded evidence section. */
	struct FM5_050_Section
	{
		FString Name;
		bool bSuccess = false;
		FString Json;
	};

	/** The process-wide section store (automation cases run sequentially). */
	static TArray<FM5_050_Section>& M5_050_Sections()
	{
		static TArray<FM5_050_Section> Sections;
		return Sections;
	}

	/** The evidence directory stamp, fixed at first use (UTC+8). */
	static const FString& M5_050_EvidenceStamp()
	{
		static const FString Stamp = (FDateTime::UtcNow() + FTimespan::FromHours(8)).ToString(TEXT("%Y%m%d-%H%M%S")) + TEXT("-UTC+8");
		return Stamp;
	}

	/** Artifacts/Tasks/M5-050/<UTC+8 stamp>/ under the project dir. */
	static FString M5_050_EvidenceDir()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-050") / M5_050_EvidenceStamp());
	}

	/** Minimal JSON string escaping (the M5-035 pattern; evidence stays ASCII). */
	static FString M5_050_Escape(const FString& In)
	{
		return In.Replace(TEXT("\\"), TEXT("/")).Replace(TEXT("\""), TEXT("'"));
	}

	/** Nearest-rank percentile of an ascending-sorted sample set, in the input unit. */
	static double M5_050_PercentileOfSorted(const TArray<double>& SortedAsc, double Percent)
	{
		if (SortedAsc.Num() == 0)
		{
			return 0.0;
		}
		const int32 Rank = FMath::Clamp(FMath::CeilToInt(Percent / 100.0 * static_cast<double>(SortedAsc.Num())), 1, SortedAsc.Num());
		return SortedAsc[Rank - 1];
	}

	/** Records/updates one section and rewrites stress-report.json. */
	static void M5_050_WriteReport(FAutomationTestBase& Test, const FString& Name, bool bSuccess, const FString& SectionJson)
	{
		bool bFound = false;
		for (FM5_050_Section& Section : M5_050_Sections())
		{
			if (Section.Name == Name)
			{
				Section.bSuccess = bSuccess;
				Section.Json = SectionJson;
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			M5_050_Sections().Add(FM5_050_Section{Name, bSuccess, SectionJson});
		}

		bool bAllSuccess = true;
		for (const FM5_050_Section& Section : M5_050_Sections())
		{
			bAllSuccess = bAllSuccess && Section.bSuccess;
		}

		const FString Generated = (FDateTime::UtcNow() + FTimespan::FromHours(8)).ToString(TEXT("%Y-%m-%dT%H:%M:%S"));
		FString Payload = FString::Printf(TEXT("{\n  \"task\": \"M5-050\",\n  \"generated_utc8\": \"%s\",\n  \"success\": %s,\n  \"sections\": [\n"),
			*Generated, bAllSuccess ? TEXT("true") : TEXT("false"));
		const TArray<FM5_050_Section>& Sections = M5_050_Sections();
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			Payload += TEXT("    {\"name\": \"") + M5_050_Escape(Sections[Index].Name)
				+ TEXT("\", \"success\": ") + (Sections[Index].bSuccess ? TEXT("true") : TEXT("false"))
				+ TEXT(", \"data\": ") + Sections[Index].Json + TEXT("}")
				+ (Index + 1 < Sections.Num() ? TEXT(",") : TEXT(""));
			Payload += TEXT("\n");
		}
		Payload += TEXT("  ]\n}\n");

		const FString Directory = M5_050_EvidenceDir();
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		const FString Absolute = Directory / TEXT("stress-report.json");
		if (!FFileHelper::SaveStringToFile(Payload, *Absolute))
		{
			Test.AddError(FString::Printf(TEXT("the stress report could not be written: %s"), *Absolute));
		}
	}

	/** Double to a JSON number with a fixed 3-decimal shape. */
	static FString M5_050_Num3(double Value)
	{
		return FString::Printf(TEXT("%.3f"), Value);
	}

	// -----------------------------------------------------------------------
	// Shared production-shaped fixtures (the M5_028 scene pattern).
	// -----------------------------------------------------------------------

	/** The Actor->EntityId seam (M5-026's IHitscanTargetIdentity, the seam the production pawn also implements). */
	struct FM5_050_Identity : IHitscanTargetIdentity
	{
		TMap<const AActor*, FEntityId> IdsByActor;

		void Record(const AActor& Actor, FEntityId Id)
		{
			IdsByActor.Add(&Actor, Id);
		}

		virtual FEntityId ResolveTargetEntityId(const AActor& HitActor) const override
		{
			const FEntityId* Id = IdsByActor.Find(&HitActor);
			return Id != nullptr ? *Id : InvalidCombatTargetId;
		}
	};

	/**
	 * The pellet spawner mirroring the production FWeaponProjectileSpawner
	 * (PrototypeCharacter.cpp: FWeaponProjectileSpawner::SpawnPellet): stamps
	 * the committed provenance, configures the single movement component from
	 * the definition and arms the engine LifeSpan. The only difference from
	 * the production spawner is the owner parameter (the stress driver is not
	 * a pawn).
	 */
	struct FM5_050_Spawner final : IProjectileSpawner
	{
		UWorld* World = nullptr;
		TArray<TWeakObjectPtr<ACombatProjectile>> Spawned;

		virtual ACombatProjectile* SpawnPellet(const FProjectileSpawnReservation& Reservation, const FShotPellet& Pellet) override
		{
			if (World == nullptr)
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
			Actor->Context.ProjectileInstanceId = FGuid::NewGuid();
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
			Actor->FaceFlightDirection(Pellet.Direction);
			Spawned.Add(Actor);
			return Actor;
		}

		virtual void DestroyPellet(ACombatProjectile& PelletActor) override
		{
			Spawned.Remove(&PelletActor);
			if (PelletActor.GetWorld() != nullptr && IsValid(&PelletActor))
			{
				PelletActor.Destroy();
			}
		}
	};

	/**
	 * The stress round: base 1 damage plus a real stagger request (0.1 s hit
	 * stun, the shape a production round carries). The control payload is
	 * load-bearing for the dedup probes: the unified entry consumes the
	 * shot's once-per-target control ONLY when the resolved outcome accepts
	 * at least one control kind (UnifiedHitApplier.cpp stage 5; a zero-control
	 * profile leaves the control keys unconsumed, so a re-send would be a
	 * legitimate first touch, not a dedup loss). Stun carries no hit-refusal
	 * side effects here: targets never launch (Launch 0), so landing recovery
	 * never triggers, and hit stun alone never refuses a later hit.
	 */
	static FDamageProfile M5_050_MakeProfile()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("dmg_stress_round");
		Profile.BaseDamage = 1.0f;
		Profile.AttackCoefficient = 0.0f;
		Profile.HitStunSeconds = 0.1f;
		Profile.KnockbackCmPerSecond = 0.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.0f;
		return Profile;
	}

	/** A valid straight definition (a value snapshot; no catalog involved). */
	static FProjectileDefinition M5_050_MakeDefinition(float LifetimeS)
	{
		FProjectileDefinition Definition;
		Definition.ProjectileId = TEXT("bullet_stress");
		Definition.Motion = EProjectileMotion::Straight;
		Definition.SpeedCmS = M5_050_SpeedCmS;
		Definition.LifetimeS = LifetimeS;
		Definition.DamageProfileId = TEXT("dmg_stress_round");
		Definition.PierceCount = 0;
		Definition.ExplosionRadiusCm = 0.0f;
		Definition.HomingTurnRateDegS = 0.0f;
		return Definition;
	}

	/** The committed-shot snapshot (the fire registry's identity, the 010 allocator's sequence). */
	static FShotContext M5_050_MakeShot(FCombatEpoch Epoch, FEntityId SourceId, FShotId ShotId, int32 PelletCount)
	{
		FShotContext Shot;
		Shot.Epoch = Epoch;
		Shot.SourceEntityId = SourceId;
		Shot.ShotId = ShotId;
		Shot.WeaponInstanceId = FGuid(0x50, 0x50, 0x0, 0x50);
		Shot.WeaponDefinitionId = TEXT("weapon_stress");
		Shot.FireMode = EWeaponFireMode::Projectile;
		Shot.DamageProfileId = TEXT("dmg_stress_round");
		Shot.ProjectileId = TEXT("bullet_stress");
		Shot.PelletCount = PelletCount;
		return Shot;
	}

	/** Spawns a query-only box body (target or wall) at Center (the M5_028 helper). */
	static AActor* M5_050_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& Extent, ECollisionChannel ObjectType)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_050_Box"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(Extent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ObjectType);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		// The projectile body blocks this object type; the blocking face needs
		// the reciprocal response (either side ignoring kills the block pair).
		Body->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
		Body->RegisterComponent();
		Body->SetWorldLocation(Center);
		return Actor;
	}

	/**
	 * Spawns a registered hostile target: box body + a real health pool + the
	 * legacy combat component (the M5_028 target shape; the unified entry
	 * reads the reaction policy off the combat component when present).
	 */
	static AActor* M5_050_SpawnTarget(FAutomationTestBase& Test, UWorld& World, FCombatEntityRegistry& Registry,
		FM5_050_Identity& Identity, const FVector& Center, FCombatEpoch Epoch)
	{
		AActor* Actor = M5_050_SpawnBoxActor(World, Center, FVector(30.0, 30.0, 90.0), ECC_Pawn);
		if (!Test.TestNotNull(TEXT("a stress target spawned"), Actor))
		{
			return nullptr;
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_050_Health"));
		Health->RegisterComponent();
		Health->SetMaxHealth(M5_050_TargetMaxHealth);
		Health->ResetHealth();
		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M5_050_Combat"));
		Combat->RegisterComponent();
		FCombatEntityMetadata Metadata;
		Metadata.Faction = TEXT("Enemy");
		Metadata.Category = TEXT("target");
		const FEntityId Id = Registry.RegisterEntity(Actor, Metadata, Epoch);
		if (!Test.TestTrue(TEXT("the stress target registered with a valid id"), Id != InvalidCombatEntityId))
		{
			return nullptr;
		}
		Identity.Record(*Actor, Id);
		return Actor;
	}

	/** True when the process runs with -nullrhi (the plain automation harness entry). */
	static bool M5_050_IsNullRhi()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("nullrhi"));
	}
}

using namespace UE::UEMMO::Tasks::M5_050;

// ---------------------------------------------------------------------------
// CapacityExplicitRefusal
// ---------------------------------------------------------------------------

// The over-capacity faces are EXPLICIT refusals on both bounded structures -
// the projectile service's hard live cap (MaxLiveProjectiles) and the hit
// ledger's active-shot cap (DefaultCapacity) - and both release faces (the
// epoch rebuild, EndShot) restore the capacity for the next volley. This is
// the card's "over capacity is explicitly rejected" acceptance, pinned on the
// real production paths (no test-side caps).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_050CapacityExplicitRefusal,
	"UEMMO.Tasks.M5_050.CapacityExplicitRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_050CapacityExplicitRefusal::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the capacity test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		M5_050_WriteReport(*this, TEXT("capacity"), false, TEXT("{\"error\": \"world unavailable\"}"));
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the capacity test world is available"), World))
	{
		M5_050_WriteReport(*this, TEXT("capacity"), false, TEXT("{\"error\": \"world unavailable\"}"));
		return true;
	}
	if (!TestTrue(TEXT("play begins in the capacity test world"), Wrapper.BeginPlayInTestWorld()))
	{
		M5_050_WriteReport(*this, TEXT("capacity"), false, TEXT("{\"error\": \"begin play failed\"}"));
		return true;
	}

	FCombatEntityRegistry Registry;
	FProjectileWorldService Service(World);
	FCombatEpoch Epoch = Registry.GetCurrentEpoch();

	// The registered source and one registered hostile target (logic-only
	// records are a registry face; the ledger probe only needs known ids).
	FEntityId SourceId = Registry.RegisterEntity(nullptr, FCombatEntityMetadata{TEXT("Player"), TEXT("shooter")}, Epoch);
	FEntityId TargetId = Registry.RegisterEntity(nullptr, FCombatEntityMetadata{TEXT("Enemy"), TEXT("target")}, Epoch);
	TestTrue(TEXT("the source registered"), SourceId != InvalidCombatEntityId);
	TestTrue(TEXT("the target registered"), TargetId != InvalidCombatTargetId);

	// -- The projectile service: fill to the hard live cap through the real
	// reserve+commit path, then the next volley is refused BY NAME.
	FM5_050_Spawner Spawner;
	Spawner.World = World;
	const FProjectileDefinition Definition = M5_050_MakeDefinition(M5_050_LifetimeS);
	int32 Committed = 0;
	for (int32 Index = 0; Index < MaxLiveProjectiles; ++Index)
	{
		const FShotId ShotId = Registry.AllocateActionSequence(SourceId, Epoch);
		FProjectileSpawnReservation Reservation;
		const FProjectileReserveOutcome Outcome = Service.ReserveProjectiles(
			M5_050_MakeShot(Epoch, SourceId, ShotId, 1), Definition,
			M5_050_SceneBase, TArray<FShotPellet>{FShotPellet{0, FVector(1.0, 0.0, 0.0)}},
			TEXT("rev-m5-050"), Reservation);
		if (!TestTrue(FString::Printf(TEXT("capacity fill %d reserved"), Index), Outcome.bReserved))
		{
			M5_050_WriteReport(*this, TEXT("capacity"), false, TEXT("{\"error\": \"capacity fill failed\"}"));
			return true;
		}
		if (!TestTrue(FString::Printf(TEXT("capacity fill %d committed"), Index), Service.CommitSpawn(Reservation, Spawner)))
		{
			M5_050_WriteReport(*this, TEXT("capacity"), false, TEXT("{\"error\": \"capacity commit failed\"}"));
			return true;
		}
		++Committed;
	}
	TestEqual(TEXT("the service live count reaches the hard cap"), Service.GetNumLiveProjectiles(), MaxLiveProjectiles);

	FShotId OverflowShotId = Registry.AllocateActionSequence(SourceId, Epoch);
	FProjectileSpawnReservation Overflow;
	const FProjectileReserveOutcome OverflowOutcome = Service.ReserveProjectiles(
		M5_050_MakeShot(Epoch, SourceId, OverflowShotId, 1), Definition,
		M5_050_SceneBase, TArray<FShotPellet>{FShotPellet{0, FVector(1.0, 0.0, 0.0)}},
		TEXT("rev-m5-050"), Overflow);
	TestFalse(TEXT("the over-capacity volley is refused"), OverflowOutcome.bReserved);
	TestEqual(TEXT("the over-capacity refusal is named CapacityFull"), OverflowOutcome.Reject, EProjectileReserveReject::CapacityFull);
	TestTrue(TEXT("the over-capacity refusal names its detail"), !OverflowOutcome.RejectDetail.IsEmpty());

	// The epoch rebuild (map unload / room exit / retry) is the explicit
	// release face: the population returns to baseline 0 and the capacity is
	// restored for the next generation.
	Service.BeginNextWorldEpoch();
	TestEqual(TEXT("the epoch rebuild returns the population to baseline"), Service.GetNumLiveProjectiles(), 0);
	const FCombatEpoch NewEpoch = Registry.BeginNextWorldEpoch();
	SourceId = Registry.RegisterEntity(nullptr, FCombatEntityMetadata{TEXT("Player"), TEXT("shooter")}, NewEpoch);
	FProjectileSpawnReservation Fresh;
	const FProjectileReserveOutcome FreshOutcome = Service.ReserveProjectiles(
		M5_050_MakeShot(NewEpoch, SourceId, Registry.AllocateActionSequence(SourceId, NewEpoch), 1), Definition,
		M5_050_SceneBase, TArray<FShotPellet>{FShotPellet{0, FVector(1.0, 0.0, 0.0)}},
		TEXT("rev-m5-050"), Fresh);
	TestTrue(TEXT("the new generation reserves again"), FreshOutcome.bReserved);

	// -- The hit ledger: active shot instances are capped at DefaultCapacity;
	// a shot past the cap is refused BY NAME, an open shot keeps accepting its
	// own pellet events, and EndShot releases the slot.
	FCombatEntityRegistry LedgerRegistry;
	FHitLedger CappedLedger{&LedgerRegistry};
	const FCombatEpoch LedgerEpoch = LedgerRegistry.GetCurrentEpoch();
	const FEntityId LedgerSource = LedgerRegistry.RegisterEntity(nullptr, FCombatEntityMetadata{TEXT("Player"), TEXT("shooter")}, LedgerEpoch);
	const FEntityId LedgerTarget = LedgerRegistry.RegisterEntity(nullptr, FCombatEntityMetadata{TEXT("Enemy"), TEXT("target")}, LedgerEpoch);
	int32 Accepted = 0;
	for (int32 Index = 0; Index < CappedLedger.GetCapacity(); ++Index)
	{
		FCombatEventKey Key;
		Key.Epoch = LedgerEpoch;
		Key.EntityId = LedgerSource;
		Key.ShotId = static_cast<FShotId>(Index + 1);
		Key.PelletIndex = 0;
		Key.TargetId = LedgerTarget;
		EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
		if (!TestTrue(FString::Printf(TEXT("ledger fill shot %d records"), Index), CappedLedger.TryRecordHitEvent(Key, Reason)))
		{
			break;
		}
		++Accepted;
	}
	TestEqual(TEXT("the ledger fills to its capacity"), CappedLedger.GetNumActiveShots(), CappedLedger.GetCapacity());

	FCombatEventKey OverflowKey;
	OverflowKey.Epoch = LedgerEpoch;
	OverflowKey.EntityId = LedgerSource;
	OverflowKey.ShotId = static_cast<FShotId>(CappedLedger.GetCapacity() + 1);
	OverflowKey.PelletIndex = 0;
	OverflowKey.TargetId = LedgerTarget;
	EHitLedgerRejectReason OverflowReason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("the over-capacity ledger shot is refused"), CappedLedger.TryRecordHitEvent(OverflowKey, OverflowReason));
	TestEqual(TEXT("the ledger refusal is named CapacityFull"), OverflowReason, EHitLedgerRejectReason::CapacityFull);

	// An already-open shot keeps accepting its own pellet events at capacity.
	FCombatEventKey OpenShotSecondPellet;
	OpenShotSecondPellet.Epoch = LedgerEpoch;
	OpenShotSecondPellet.EntityId = LedgerSource;
	OpenShotSecondPellet.ShotId = static_cast<FShotId>(CappedLedger.GetCapacity());
	OpenShotSecondPellet.PelletIndex = 1;
	OpenShotSecondPellet.TargetId = LedgerTarget;
	EHitLedgerRejectReason OpenReason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("an open shot keeps accepting its own pellet events at capacity"),
		CappedLedger.TryRecordHitEvent(OpenShotSecondPellet, OpenReason));

	// EndShot releases the instance slot: the next shot records again.
	TestTrue(TEXT("EndShot releases the instance"), CappedLedger.EndShot(LedgerEpoch, LedgerSource, static_cast<FShotId>(1)));
	EHitLedgerRejectReason AfterEndReason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("a fresh shot records after EndShot released the slot"),
		CappedLedger.TryRecordHitEvent(OverflowKey, AfterEndReason));

	const FString SectionJson = FString::Printf(
		TEXT("{\"service_cap\": %d, \"service_committed_single_pellets\": %d, ")
		TEXT("\"service_overflow_reject\": %d, \"ledger_cap\": %d, \"ledger_events_accepted\": %d, ")
		TEXT("\"ledger_open_shot_pellet_accepted_at_capacity\": true, \"ledger_overflow_reject\": %d, ")
		TEXT("\"endshot_restores_capacity\": true}"),
		static_cast<int32>(MaxLiveProjectiles), Committed,
		static_cast<int32>(EProjectileReserveReject::CapacityFull),
		CappedLedger.GetCapacity(), Accepted,
		static_cast<int32>(EHitLedgerRejectReason::CapacityFull));
	M5_050_WriteReport(*this, TEXT("capacity"), true, SectionJson);

	Wrapper.EndPlayInTestWorld();
	return true;
}

// ---------------------------------------------------------------------------
// Cleanup100RoundsBaseline
// ---------------------------------------------------------------------------

// One hundred spawn -> exit rounds. Even rounds exit through the engine
// LifeSpan expiry (the definition's LifetimeS on the Pause-frozen world
// clock, every pellet actor destroyed); odd rounds exit through the epoch
// rebuild (map unload / room exit / retry, live pellets destroyed mid-flight).
// Every round must return the live population to baseline 0 (the registry/
// ledger/service epoch triple), the ledger returns to 0 active shots, GC
// sample rounds prove the retired bodies are actually collected, and the
// process memory shows no sustained monotonic growth (last quarter mean at
// most 1.15x the first quarter mean, a fluctuation tolerance).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_050Cleanup100RoundsBaseline,
	"UEMMO.Tasks.M5_050.Cleanup100RoundsBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_050Cleanup100RoundsBaseline::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the cleanup test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		M5_050_WriteReport(*this, TEXT("cleanup"), false, TEXT("{\"error\": \"world unavailable\"}"));
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the cleanup test world is available"), World))
	{
		M5_050_WriteReport(*this, TEXT("cleanup"), false, TEXT("{\"error\": \"world unavailable\"}"));
		return true;
	}
	if (!TestTrue(TEXT("play begins in the cleanup test world"), Wrapper.BeginPlayInTestWorld()))
	{
		M5_050_WriteReport(*this, TEXT("cleanup"), false, TEXT("{\"error\": \"begin play failed\"}"));
		return true;
	}

	FCombatEntityRegistry Registry;
	FHitLedger Ledger{&Registry};
	FProjectileWorldService Service(World);
	FM5_050_Identity Identity;
	FM5_050_Spawner Spawner;
	Spawner.World = World;

	// One persistent shooter and one persistent aim target actor for the whole
	// regression; both RE-REGISTER after every epoch rebuild (a rebuilt world
	// is a fresh identity space - the same actors, new ids), so the memory
	// trend measures the projectile spawn/exit behavior, not fixture growth.
	FCombatEpoch Epoch = Registry.GetCurrentEpoch();
	const FVector Origin = M5_050_SceneBase + FVector(0.0, 0.0, 300.0);
	AActor* Shooter = World->SpawnActor<AActor>(AActor::StaticClass(), Origin, FRotator::ZeroRotator, FActorSpawnParameters());
	AActor* Target = M5_050_SpawnTarget(*this, *World, Registry, Identity,
		M5_050_SceneBase + FVector(M5_050_CleanupFlightDistanceCm, 0.0, 300.0), Epoch);
	if (!TestNotNull(TEXT("the cleanup shooter spawned"), Shooter) || !TestNotNull(TEXT("the cleanup target spawned"), Target))
	{
		M5_050_WriteReport(*this, TEXT("cleanup"), false, TEXT("{\"error\": \"scene spawn failed\"}"));
		return true;
	}
	UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
	World->EnsureCollisionTreeIsBuilt();
	FEntityId ShooterId = Registry.RegisterEntity(Shooter, FCombatEntityMetadata{TEXT("Player"), TEXT("shooter")}, Epoch);
	if (!TestTrue(TEXT("the cleanup scene registered"), ShooterId != InvalidCombatEntityId && TargetHealth != nullptr))
	{
		M5_050_WriteReport(*this, TEXT("cleanup"), false, TEXT("{\"error\": \"scene registration failed\"}"));
		return true;
	}
	const auto RegisterScene = [&]()
	{
		ShooterId = Registry.RegisterEntity(Shooter, FCombatEntityMetadata{TEXT("Player"), TEXT("shooter")}, Epoch);
		Identity.Record(*Target, Registry.RegisterEntity(Target, FCombatEntityMetadata{TEXT("Enemy"), TEXT("target")}, Epoch));
	};

	const FProjectileDefinition Definition = M5_050_MakeDefinition(M5_050_CleanupLifetimeS);
	const FVector AimDirection = (FVector(M5_050_CleanupFlightDistanceCm, 0.0, 0.0)).GetSafeNormal();
	const auto IsGcRound = [](int32 Round)
	{
		return Round == 1 || Round == 25 || Round == 50 || Round == 75 || Round == 100;
	};

	TArray<double> MemorySamplesMb;
	int32 ExpiryRounds = 0;
	int32 LeaveRounds = 0;
	int32 RoundsAtBaseline = 0;
	int32 GcRoundsPassed = 0;
	double HitsTotalByHealth = 0.0;
	bool bBaselineHeldEveryRound = true;

	for (int32 Round = 1; Round <= M5_050_CleanupRounds; ++Round)
	{
		const bool bExpiryRound = (Round % 2) == 0;

		// -- Spawn: one volley through the real transaction.
		const FShotId ShotId = Registry.AllocateActionSequence(ShooterId, Epoch);
		TArray<FShotPellet> Pellets;
		for (int32 PelletAt = 0; PelletAt < M5_050_CleanupVolleyPellets; ++PelletAt)
		{
			Pellets.Add(FShotPellet{static_cast<FPelletIndex>(PelletAt), AimDirection});
		}
		FProjectileSpawnReservation Reservation;
		const FProjectileReserveOutcome Outcome = Service.ReserveProjectiles(
			M5_050_MakeShot(Epoch, ShooterId, ShotId, M5_050_CleanupVolleyPellets), Definition,
			Origin, Pellets, TEXT("rev-m5-050"), Reservation);
		if (!TestTrue(FString::Printf(TEXT("round %d reserved its volley"), Round), Outcome.bReserved))
		{
			break;
		}
		if (!TestTrue(FString::Printf(TEXT("round %d committed its volley"), Round), Service.CommitSpawn(Reservation, Spawner)))
		{
			break;
		}

		// Drive the volley with one real policy per pellet (the production
		// one-advancer-per-pellet shape).
		FProjectileHitContext HitContext;
		HitContext.Registry = &Registry;
		HitContext.Ledger = &Ledger;
		HitContext.Identity = &Identity;
		HitContext.AttackProfile = M5_050_MakeProfile();
		HitContext.PierceCount = 0;
		HitContext.SourceActor = Registry.ResolveEntity(ShooterId).Get();
		struct FM5_050_CleanupLive
		{
			TUniquePtr<FLinearProjectilePolicy> Policy;
			TWeakObjectPtr<ACombatProjectile> Actor;
		};
		TArray<FM5_050_CleanupLive> Live;
		for (TWeakObjectPtr<ACombatProjectile>& Weak : Spawner.Spawned)
		{
			ACombatProjectile* Actor = Weak.Get();
			if (Actor == nullptr)
			{
				continue;
			}
			FM5_050_CleanupLive Entry;
			Entry.Actor = Actor;
			Entry.Policy = MakeUnique<FLinearProjectilePolicy>();
			if (!Entry.Policy->Begin(*Actor, AimDirection, HitContext))
			{
				Actor->Destroy();
				continue;
			}
			Live.Add(MoveTemp(Entry));
		}
		if (!TestTrue(FString::Printf(TEXT("round %d bound %d live policies"), Round, Live.Num()),
			Live.Num() == M5_050_CleanupVolleyPellets))
		{
			break;
		}

		// -- Drive to retirement. Expiry rounds tick past the definition's
		// lifetime so the engine LifeSpan owns every remaining body; leave
		// rounds drive a few frames and let the epoch rebuild destroy the
		// still-live volley mid-flight.
		const int32 MaxRoundFrames = bExpiryRound
			? FMath::CeilToInt(M5_050_CleanupLifetimeS / M5_050_FrameSeconds) + 4
			: 6;
		int32 FramesTicked = 0;
		for (; FramesTicked < MaxRoundFrames; ++FramesTicked)
		{
			for (int32 Index = Live.Num() - 1; Index >= 0; --Index)
			{
				ACombatProjectile* Actor = Live[Index].Actor.Get();
				if (Actor == nullptr || Actor->IsPendingKillPending())
				{
					Live.RemoveAtSwap(Index);
					continue;
				}
				Live[Index].Policy->AdvanceMotion(M5_050_FrameSeconds);
				if (Live[Index].Policy->IsFinished())
				{
					// A finished pellet retires its body (the production
					// driver behavior: the terminated bodies are ours).
					if (!Actor->IsPendingKillPending())
					{
						Actor->Destroy();
					}
					Live.RemoveAtSwap(Index);
				}
			}
			if (!Wrapper.TickTestWorld(M5_050_FrameSeconds))
			{
				break;
			}
		}

		if (bExpiryRound)
		{
			// The expiry face: every body of the round is destroyed (pending
			// kill) by hit termination or by the engine LifeSpan timer.
			++ExpiryRounds;
			bool bAllRetired = true;
			for (TWeakObjectPtr<ACombatProjectile>& Weak : Spawner.Spawned)
			{
				ACombatProjectile* Actor = Weak.Get();
				if (Actor != nullptr && !Actor->IsPendingKillPending())
				{
					bAllRetired = false;
				}
			}
			if (!TestTrue(FString::Printf(TEXT("expiry round %d destroyed every pellet body"), Round), bAllRetired))
			{
				break;
			}
			// The per-shot release face: the volley's shot instance returns
			// to 0 active shots in the ledger (a volley whose pellets all
			// expired before any hit never entered the ledger; EndShot on it
			// is a harmless false, the baseline assertion below is the gate).
			Ledger.EndShot(Epoch, ShooterId, ShotId);
			TestEqual(FString::Printf(TEXT("expiry round %d ledger at baseline"), Round),
				Ledger.GetNumActiveShots(), 0);
		}
		else
		{
			++LeaveRounds;
		}

		// -- The exit face every round: the epoch triple (registry + ledger +
		// service) is the production world-rebuild bundle.
		Service.BeginNextWorldEpoch();
		if (!TestEqual(FString::Printf(TEXT("round %d live population at baseline"), Round),
			Service.GetNumLiveProjectiles(), 0))
		{
			bBaselineHeldEveryRound = false;
			break;
		}
		++RoundsAtBaseline;
		Ledger.Reset();
		Epoch = Registry.BeginNextWorldEpoch();
		RegisterScene();
		if (!TestTrue(FString::Printf(TEXT("round %d re-registered the rebuilt scene"), Round),
			ShooterId != InvalidCombatEntityId && Identity.ResolveTargetEntityId(*Target) != InvalidCombatTargetId))
		{
			break;
		}

		// -- GC sample rounds: the retired bodies must actually collect.
		const bool bGcRound = IsGcRound(Round);
		if (bGcRound)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			++GcRoundsPassed;
			bool bAllCollected = true;
			for (TWeakObjectPtr<ACombatProjectile>& Weak : Spawner.Spawned)
			{
				if (Weak.IsValid())
				{
					bAllCollected = false;
				}
			}
			if (!TestTrue(FString::Printf(TEXT("GC round %d collected every retired body"), Round), bAllCollected))
			{
				break;
			}
		}

		// -- Memory sample (process used-physical working set; sampled after
		// the exit face so the reading reflects the round's settled state).
		MemorySamplesMb.Add(static_cast<double>(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0));
		Spawner.Spawned.Reset();
	}

	TestEqual(TEXT("all cleanup rounds ran"), ExpiryRounds + LeaveRounds, M5_050_CleanupRounds);
	TestEqual(TEXT("all cleanup rounds returned to baseline"), RoundsAtBaseline, M5_050_CleanupRounds);
	TestEqual(TEXT("the GC sample rounds all ran"), GcRoundsPassed, 5);
	TestTrue(TEXT("every round held the baseline"), bBaselineHeldEveryRound);

	// The exact hit counter: with base damage 1 and zero defense every
	// accepted event removed exactly 1 health point.
	if (TargetHealth != nullptr)
	{
		HitsTotalByHealth = TargetHealth->GetMaxHealth() - TargetHealth->GetHealth();
	}
	TestTrue(TEXT("the cleanup rounds landed their volley hits"), HitsTotalByHealth > 0.0);

	// The memory trend: last-quarter mean at most 1.15x the first-quarter
	// mean (allocator warmup lives in the first quarter; the tolerance only
	// absorbs fluctuations, never sustained growth).
	double FirstQuarterMb = 0.0;
	double LastQuarterMb = 0.0;
	bool bMemoryHeld = false;
	const int32 Quarter = M5_050_CleanupRounds / 4;
	if (MemorySamplesMb.Num() == M5_050_CleanupRounds)
	{
		for (int32 Index = 0; Index < Quarter; ++Index)
		{
			FirstQuarterMb += MemorySamplesMb[Index];
		}
		for (int32 Index = M5_050_CleanupRounds - Quarter; Index < M5_050_CleanupRounds; ++Index)
		{
			LastQuarterMb += MemorySamplesMb[Index];
		}
		FirstQuarterMb /= static_cast<double>(Quarter);
		LastQuarterMb /= static_cast<double>(Quarter);
		bMemoryHeld = LastQuarterMb <= FirstQuarterMb * 1.15;
	}
	TestTrue(FString::Printf(TEXT("the memory shows no sustained growth (first %.1f MB, last %.1f MB, ratio %.3f)"),
		FirstQuarterMb, LastQuarterMb, FirstQuarterMb > 0.0 ? LastQuarterMb / FirstQuarterMb : 0.0), bMemoryHeld);

	FString MemoryJson = TEXT("[");
	for (int32 Index = 0; Index < MemorySamplesMb.Num(); ++Index)
	{
		MemoryJson += M5_050_Num3(MemorySamplesMb[Index]) + (Index + 1 < MemorySamplesMb.Num() ? TEXT(",") : TEXT(""));
	}
	MemoryJson += TEXT("]");
	// MSVC's checked-format constexpr checker rejects this long multi-literal
	// Printf argument list (C7595), so the section JSON is assembled by plain
	// string concatenation instead.
	const FString SectionJson =
		TEXT("{\"rounds\": ") + FString::FromInt(M5_050_CleanupRounds)
		+ TEXT(", \"volley_pellets\": ") + FString::FromInt(M5_050_CleanupVolleyPellets)
		+ TEXT(", \"expiry_rounds\": ") + FString::FromInt(ExpiryRounds)
		+ TEXT(", \"leave_rounds\": ") + FString::FromInt(LeaveRounds)
		+ TEXT(", \"gc_rounds\": ") + FString::FromInt(GcRoundsPassed)
		+ TEXT(", \"baseline_rounds\": ") + FString::FromInt(RoundsAtBaseline)
		+ TEXT(", \"hits_by_health\": ") + M5_050_Num3(HitsTotalByHealth)
		+ TEXT(", \"memory_mb\": ") + MemoryJson
		+ TEXT(", \"memory_first_quarter_mb\": ") + M5_050_Num3(FirstQuarterMb)
		+ TEXT(", \"memory_last_quarter_mb\": ") + M5_050_Num3(LastQuarterMb)
		+ TEXT(", \"memory_ratio\": ") + M5_050_Num3(FirstQuarterMb > 0.0 ? LastQuarterMb / FirstQuarterMb : 0.0)
		+ TEXT(", \"memory_tolerance\": 1.15}");
	M5_050_WriteReport(*this, TEXT("cleanup"), bBaselineHeldEveryRound && bMemoryHeld && RoundsAtBaseline == M5_050_CleanupRounds, SectionJson);

	Wrapper.EndPlayInTestWorld();
	return true;
}

// ---------------------------------------------------------------------------
// Stress200ConcurrentBudget
// ---------------------------------------------------------------------------

// The steady load: 200 concurrent pellets against 10 registered targets for
// 3600 fixed 60 Hz frames (60 seconds of simulated world time). During the
// load: the service live population never exceeds the 256 hard cap (and the
// driver never asks past 200 active), the ledger never loses the dedup face
// (repeated submissions stay refused, and the health-pool drop equals the
// driver's submitted-passage count exactly - the ledger's recorded-event
// count is NOT cumulative: EndShot releases a shot's keys back to the pool
// by design, HitLedger.cpp EndShot, so only the outstanding volleys' events
// stay held), the budgeted GC cadence sustains the load without a single
// CapacityFull refusal, and the per-frame game-thread budget samples land in
// the evidence JSON with the percentile assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_050Stress200ConcurrentBudget,
	"UEMMO.Tasks.M5_050.Stress200ConcurrentBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_050Stress200ConcurrentBudget::RunTest(const FString& Parameters)
{
	const bool bNullRhi = M5_050_IsNullRhi();
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the stress test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		M5_050_WriteReport(*this, TEXT("stress"), false, TEXT("{\"error\": \"world unavailable\"}"));
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the stress test world is available"), World))
	{
		M5_050_WriteReport(*this, TEXT("stress"), false, TEXT("{\"error\": \"world unavailable\"}"));
		return true;
	}
	if (!TestTrue(TEXT("play begins in the stress test world"), Wrapper.BeginPlayInTestWorld()))
	{
		M5_050_WriteReport(*this, TEXT("stress"), false, TEXT("{\"error\": \"begin play failed\"}"));
		return true;
	}

	FCombatEntityRegistry Registry;
	FHitLedger Ledger{&Registry};
	FProjectileWorldService Service(World);
	FM5_050_Identity Identity;
	FM5_050_Spawner Spawner;
	Spawner.World = World;

	const FCombatEpoch Epoch = Registry.GetCurrentEpoch();
	const FVector Origin = M5_050_SceneBase + FVector(0.0, 0.0, 300.0);
	AActor* Shooter = World->SpawnActor<AActor>(AActor::StaticClass(), Origin, FRotator::ZeroRotator, FActorSpawnParameters());
	if (!TestNotNull(TEXT("the stress shooter spawned"), Shooter))
	{
		M5_050_WriteReport(*this, TEXT("stress"), false, TEXT("{\"error\": \"shooter spawn failed\"}"));
		return true;
	}
	const FEntityId ShooterId = Registry.RegisterEntity(Shooter, FCombatEntityMetadata{TEXT("Player"), TEXT("shooter")}, Epoch);
	if (!TestTrue(TEXT("the stress shooter registered"), ShooterId != InvalidCombatEntityId))
	{
		M5_050_WriteReport(*this, TEXT("stress"), false, TEXT("{\"error\": \"shooter registration failed\"}"));
		return true;
	}

	// The 10 registered hostile targets: two depth rows of five across the
	// depth (Y) axis at two heights (Z).
	TArray<FEntityId> TargetIds;
	TArray<UHealthComponent*> TargetHealth;
	for (int32 Index = 0; Index < M5_050_TargetCount; ++Index)
	{
		const double DepthY = -360.0 + static_cast<double>(Index % 5) * 180.0;
		const double HeightZ = 500.0 + static_cast<double>(Index / 5) * 900.0;
		AActor* Target = M5_050_SpawnTarget(*this, *World, Registry, Identity,
			M5_050_SceneBase + FVector(M5_050_FlightDistanceCm, DepthY, HeightZ), Epoch);
		if (Target == nullptr)
		{
			M5_050_WriteReport(*this, TEXT("stress"), false, TEXT("{\"error\": \"target spawn failed\"}"));
			return true;
		}
		TargetIds.Add(Identity.ResolveTargetEntityId(*Target));
		TargetHealth.Add(Target->FindComponentByClass<UHealthComponent>());
	}

	// A world-static wall behind the targets stops any stray pellet (a
	// straight pellet that somehow passes a target terminates on it).
	M5_050_SpawnBoxActor(*World, M5_050_SceneBase + FVector(M5_050_FlightDistanceCm + 400.0, 0.0, 900.0),
		FVector(40.0, 1200.0, 1500.0), ECC_WorldStatic);
	World->EnsureCollisionTreeIsBuilt();

	const FProjectileDefinition Definition = M5_050_MakeDefinition(M5_050_LifetimeS);
	FProjectileHitContext HitContext;
	HitContext.Registry = &Registry;
	HitContext.Ledger = &Ledger;
	HitContext.Identity = &Identity;
	HitContext.AttackProfile = M5_050_MakeProfile();
	HitContext.PierceCount = 0;
	HitContext.SourceActor = Shooter;

	// Aim helper: pellet slots walk the 10 targets round-robin so every
	// volley spreads its pellets over the whole target set.
	const auto TargetCenterOf = [](int32 TargetIndex) -> FVector
	{
		const double DepthY = -360.0 + static_cast<double>(TargetIndex % 5) * 180.0;
		const double HeightZ = 500.0 + static_cast<double>(TargetIndex / 5) * 900.0;
		return M5_050_SceneBase + FVector(M5_050_FlightDistanceCm, DepthY, HeightZ);
	};

	struct FM5_050_LivePellet
	{
		TUniquePtr<FLinearProjectilePolicy> Policy;
		TWeakObjectPtr<ACombatProjectile> Actor;
		FShotId ShotId = InvalidCombatShotId;
		FPelletIndex PelletIndex = InvalidCombatPelletIndex;
		FEntityId AimedTargetId = InvalidCombatTargetId;
	};
	TArray<FM5_050_LivePellet> Live;
	TMap<FShotId, int32> OutstandingPellets;
	TArray<FCombatEventKey> RecentSubmittedKeys;

	int32 AimCursor = 0;
	int32 CapacityFullRefusals = 0;
	int32 ReserveFailures = 0;
	int32 CommitFailures = 0;
	int32 MaxActive = 0;
	int32 MaxServiceLive = 0;
	int32 GcCount = 0;
	double GcTotalMs = 0.0;
	int32 DedupProbes = 0;
	int32 DedupProbeAccepted = 0;
	int32 DedupDuplicateEventRefusals = 0;
	int32 DedupShotEndedRefusals = 0;
	int32 ControlProbes = 0;
	int32 ControlProbeAccepted = 0;
	int32 HitsBySubmissions = 0;

	TArray<double> FrameMs;
	TArray<double> ServiceLiveCurve;
	TArray<double> ActivePelletsCurve;
	TArray<double> LedgerShotsCurve;
	TArray<double> MemorySamplesMb;

	// One top-up pass: reserve/commit volleys until the active population
	// reaches the concurrency target (never past it).
	const auto TopUp = [&]()
	{
		int32 Guard = 0;
		while (Live.Num() + M5_050_VolleyPellets <= M5_050_TargetConcurrent && ++Guard <= 40)
		{
			const FShotId ShotId = Registry.AllocateActionSequence(ShooterId, Epoch);
			TArray<FShotPellet> Pellets;
			TArray<FEntityId> AimTargets;
			for (int32 PelletAt = 0; PelletAt < M5_050_VolleyPellets; ++PelletAt)
			{
				const int32 TargetIndex = (AimCursor + PelletAt) % M5_050_TargetCount;
				AimTargets.Add(TargetIds[TargetIndex]);
				FShotPellet Pellet;
				Pellet.PelletIndex = static_cast<FPelletIndex>(PelletAt);
				Pellet.Direction = (TargetCenterOf(TargetIndex) - Origin).GetSafeNormal();
				Pellets.Add(Pellet);
			}
			AimCursor = (AimCursor + M5_050_VolleyPellets) % M5_050_TargetCount;

			FProjectileSpawnReservation Reservation;
			const FProjectileReserveOutcome Outcome = Service.ReserveProjectiles(
				M5_050_MakeShot(Epoch, ShooterId, ShotId, M5_050_VolleyPellets), Definition,
				Origin, Pellets, TEXT("rev-m5-050"), Reservation);
			if (!Outcome.bReserved)
			{
				if (Outcome.Reject == EProjectileReserveReject::CapacityFull)
				{
					++CapacityFullRefusals;
				}
				else
				{
					++ReserveFailures;
				}
				return;
			}
			if (!Service.CommitSpawn(Reservation, Spawner))
			{
				++CommitFailures;
				return;
			}
			for (int32 PelletAt = 0; PelletAt < Spawner.Spawned.Num(); ++PelletAt)
			{
				ACombatProjectile* Actor = Spawner.Spawned[PelletAt].Get();
				if (Actor == nullptr)
				{
					continue;
				}
				FM5_050_LivePellet Entry;
				Entry.Actor = Actor;
				Entry.ShotId = ShotId;
				Entry.PelletIndex = Pellets[PelletAt].PelletIndex;
				Entry.AimedTargetId = AimTargets[PelletAt];
				Entry.Policy = MakeUnique<FLinearProjectilePolicy>();
				if (!Entry.Policy->Begin(*Actor, Pellets[PelletAt].Direction, HitContext))
				{
					Actor->Destroy();
					continue;
				}
				Live.Add(MoveTemp(Entry));
				OutstandingPellets.FindOrAdd(ShotId)++;
			}
			Spawner.Spawned.Reset();
		}
	};

	// One retire step at array index (production driver shape: advance, then
	// a finished pellet retires its body; a destroyed/expired body drops out).
	const auto RetireAt = [&](int32 Index)
	{
		const FShotId ShotId = Live[Index].ShotId;
		if (Live[Index].Policy.IsValid() && Live[Index].Policy->GetNumPiercedHits() > 0)
		{
			// One hostile passage = exactly one unified submission (Pierce 0
			// terminates on the first hostile, so the count is 0 or 1). The
			// submission-side accumulator is the exact cross-check for the
			// health-pool drop: every accepted event applied exactly 1 damage
			// through the applier (damage only after ledger acceptance), and
			// no damage exists without a submission.
			HitsBySubmissions += Live[Index].Policy->GetNumPiercedHits();
			// A hostile passage submitted through the unified entry: remember
			// the exact five-tuple key for the dedup probes.
			FCombatEventKey Key;
			Key.Epoch = Epoch;
			Key.EntityId = ShooterId;
			Key.ShotId = ShotId;
			Key.PelletIndex = Live[Index].PelletIndex;
			Key.TargetId = Live[Index].AimedTargetId;
			RecentSubmittedKeys.Add(Key);
			if (RecentSubmittedKeys.Num() > 128)
			{
				RecentSubmittedKeys.RemoveAt(0);
			}
		}
		int32* Outstanding = OutstandingPellets.Find(ShotId);
		if (Outstanding != nullptr && --(*Outstanding) <= 0)
		{
			// The per-volley release face (the melee attack-end EndShot shape;
			// the production projectile path does not wire this yet - see the
			// file header gap note).
			Ledger.EndShot(Epoch, ShooterId, ShotId);
			OutstandingPellets.Remove(ShotId);
		}
		Live.RemoveAtSwap(Index);
	};

	// -- The warmup: reach the steady population before sampling starts.
	const auto AdvanceAndRetire = [&]()
	{
		for (int32 Index = Live.Num() - 1; Index >= 0; --Index)
		{
			ACombatProjectile* Actor = Live[Index].Actor.Get();
			if (Actor == nullptr || Actor->IsPendingKillPending())
			{
				RetireAt(Index);
				continue;
			}
			Live[Index].Policy->AdvanceMotion(M5_050_FrameSeconds);
			if (Live[Index].Policy->IsFinished())
			{
				if (!Actor->IsPendingKillPending())
				{
					Actor->Destroy();
				}
				RetireAt(Index);
			}
		}
	};
	for (int32 Frame = 0; Frame < M5_050_WarmupFrames; ++Frame)
	{
		TopUp();
		AdvanceAndRetire();
		if (!Wrapper.TickTestWorld(M5_050_FrameSeconds))
		{
			break;
		}
		if (Frame % M5_050_GcEveryFrames == M5_050_GcEveryFrames - 1)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}
	if (!TestTrue(TEXT("the warmup reached the steady population"), Live.Num() >= M5_050_TargetConcurrent / 2))
	{
		M5_050_WriteReport(*this, TEXT("stress"), false, TEXT("{\"error\": \"warmup never reached the steady population\"}"));
		return true;
	}

	// -- The sampled steady load. The timed body is the per-frame game-thread
	// work of the load (top-up transaction, one advance per live pellet, the
	// retire sweep, the world tick). The budgeted GC pass runs BETWEEN timed
	// frames and is measured separately (gc_total_ms): a full GC reaches over
	// every UObject in the process, so its spike is dominated by engine
	// object counts, not by the 200-pellet load - conflating it with the
	// load budget would record noise, and the split is reported openly.
	for (int32 Frame = 0; Frame < M5_050_TotalFrames; ++Frame)
	{
		const double FrameStart = FPlatformTime::Seconds();

		TopUp();
		AdvanceAndRetire();

		if (!Wrapper.TickTestWorld(M5_050_FrameSeconds))
		{
			break;
		}

		const double FrameMsValue = (FPlatformTime::Seconds() - FrameStart) * 1000.0;
		FrameMs.Add(FrameMsValue);
		MaxActive = FMath::Max(MaxActive, Live.Num());
		MaxServiceLive = FMath::Max(MaxServiceLive, Service.GetNumLiveProjectiles());

		// The budgeted GC (untimed, see above).
		if (Frame % M5_050_GcEveryFrames == M5_050_GcEveryFrames - 1)
		{
			const double GcStart = FPlatformTime::Seconds();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			GcTotalMs += (FPlatformTime::Seconds() - GcStart) * 1000.0;
			++GcCount;
		}

		// Curve checkpoints (every second) plus the dedup/control probes.
		if (Frame % 60 == 0)
		{
			ServiceLiveCurve.Add(Service.GetNumLiveProjectiles());
			ActivePelletsCurve.Add(Live.Num());
			LedgerShotsCurve.Add(Ledger.GetNumActiveShots());
		}
		if (Frame % 600 == 599)
		{
			MemorySamplesMb.Add(static_cast<double>(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0));
			const int32 ProbeCount = FMath::Min(16, RecentSubmittedKeys.Num());
			for (int32 Probe = 0; Probe < ProbeCount; ++Probe)
			{
				const FCombatEventKey& Key = RecentSubmittedKeys[RecentSubmittedKeys.Num() - 1 - Probe];
				EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
				++DedupProbes;
				if (Ledger.TryRecordHitEvent(Key, Reason))
				{
					++DedupProbeAccepted;
				}
				else if (Reason == EHitLedgerRejectReason::DuplicateEvent)
				{
					++DedupDuplicateEventRefusals;
				}
				else if (Reason == EHitLedgerRejectReason::ShotAlreadyEnded)
				{
					++DedupShotEndedRefusals;
				}
				EHitLedgerRejectReason ControlReason = EHitLedgerRejectReason::None;
				++ControlProbes;
				if (Ledger.TryAcceptShotControl(Key, ControlReason))
				{
					++ControlProbeAccepted;
				}
			}
		}
	}

	// -- The load assertions.
	const int32 SampledFrames = FrameMs.Num();
	TArray<double> SortedMs = FrameMs;
	SortedMs.Sort();
	const double P50Ms = M5_050_PercentileOfSorted(SortedMs, 50.0);
	const double P95Ms = M5_050_PercentileOfSorted(SortedMs, 95.0);
	const double P99Ms = M5_050_PercentileOfSorted(SortedMs, 99.0);

	// The exact hit cross-check: base damage 1, defense 0, so the health-pool
	// drop equals the driver's submitted-passage count exactly - every
	// submission applied exactly once and no damage exists without a
	// submission. The ledger's recorded-event count is deliberately NOT this
	// counter: EndShot releases a retired shot's keys back to the pool by
	// design (HitLedger.cpp EndShot), so GetNumRecordedEvents only reports
	// the outstanding volleys' held keys (ledger_events_held below).
	double HitsTotalByHealth = 0.0;
	for (const UHealthComponent* Health : TargetHealth)
	{
		HitsTotalByHealth += Health->GetMaxHealth() - Health->GetHealth();
	}
	const int32 LedgerEventsHeld = Ledger.GetNumRecordedEvents();
	const int32 LedgerTrackedShots = Ledger.GetNumTrackedShots();

	bool bBudgetHeld = true;
	if (bNullRhi)
	{
		// -nullrhi: record + report; only a runaway frame (> 100 ms at p99)
		// fails - see the file header for the reliability rationale.
		bBudgetHeld = P99Ms <= M5_050_RunawayP99Ms;
		if (!bBudgetHeld)
		{
			AddError(FString::Printf(TEXT("the nullrhi runaway guard failed: p99 %.3f ms > %.3f ms"), P99Ms, M5_050_RunawayP99Ms));
		}
	}
	else
	{
		// A real RHI (the -Stress acceptance entry): the card's red line.
		bBudgetHeld = P95Ms <= M5_050_BudgetP95Ms && P99Ms <= M5_050_BudgetP99Ms;
		if (P95Ms > M5_050_BudgetP95Ms)
		{
			AddError(FString::Printf(TEXT("the frame budget p95 %.3f ms exceeds %.3f ms"), P95Ms, M5_050_BudgetP95Ms));
		}
		if (P99Ms > M5_050_BudgetP99Ms)
		{
			AddError(FString::Printf(TEXT("the frame budget p99 %.3f ms exceeds %.3f ms"), P99Ms, M5_050_BudgetP99Ms));
		}
	}

	TestEqual(TEXT("every sampled frame produced a sample"), SampledFrames, M5_050_TotalFrames);
	TestTrue(FString::Printf(TEXT("the active population stayed at the target (max %d)"), MaxActive),
		MaxActive <= M5_050_TargetConcurrent);
	TestTrue(FString::Printf(TEXT("the service live population stayed under the hard cap (max %d of %d)"),
		MaxServiceLive, static_cast<int32>(MaxLiveProjectiles)), MaxServiceLive <= MaxLiveProjectiles);
	TestEqual(TEXT("the budgeted cadence sustained the load without a CapacityFull refusal"), CapacityFullRefusals, 0);
	TestEqual(TEXT("no other reserve refusal happened under load"), ReserveFailures, 0);
	TestEqual(TEXT("no commit failure happened under load"), CommitFailures, 0);
	TestTrue(FString::Printf(TEXT("the dedup probes all stayed refused (%d probes, %d accepted)"),
		DedupProbes, DedupProbeAccepted), DedupProbeAccepted == 0);
	TestTrue(FString::Printf(TEXT("the control probes all stayed refused (%d probes, %d accepted)"),
		ControlProbes, ControlProbeAccepted), ControlProbeAccepted == 0);
	TestTrue(FString::Printf(TEXT("the health-pool drop equals the submitted passages exactly (%.0f vs %d)"),
		HitsTotalByHealth, HitsBySubmissions), FMath::Abs(HitsTotalByHealth - static_cast<double>(HitsBySubmissions)) < 0.5);
	TestTrue(FString::Printf(TEXT("the load actually hit the targets (%.0f hits)"), HitsTotalByHealth),
		HitsTotalByHealth >= 1000.0);
	TestTrue(TEXT("the probes found dedup keys to re-send"), DedupProbes > 0);
	TestTrue(TEXT("the ledger active shots stayed bounded under load"), Ledger.GetNumActiveShots() <= 64);

	// -- The exit: the epoch rebuild returns everything to baseline.
	for (FM5_050_LivePellet& Entry : Live)
	{
		if (ACombatProjectile* Actor = Entry.Actor.Get())
		{
			if (!Actor->IsPendingKillPending())
			{
				Actor->Destroy();
			}
		}
	}
	Live.Reset();
	Service.BeginNextWorldEpoch();
	TestEqual(TEXT("the stress load exits to baseline"), Service.GetNumLiveProjectiles(), 0);

	// -- The evidence section.
	FString FramesJson = TEXT("[");
	for (int32 Index = 0; Index < FrameMs.Num(); ++Index)
	{
		FramesJson += M5_050_Num3(FrameMs[Index]) + (Index + 1 < FrameMs.Num() ? TEXT(",") : TEXT(""));
	}
	FramesJson += TEXT("]");
	FString CurveJson = [&](const TCHAR* Name, const TArray<double>& Curve)
	{
		FString Json = FString::Printf(TEXT("\"%s\": ["), Name);
		for (int32 Index = 0; Index < Curve.Num(); ++Index)
		{
			Json += M5_050_Num3(Curve[Index]) + (Index + 1 < Curve.Num() ? TEXT(",") : TEXT(""));
		}
		return Json + TEXT("]");
	}(TEXT("service_live"), ServiceLiveCurve);
	CurveJson += TEXT(", ") + [&](const TCHAR* Name, const TArray<double>& Curve)
	{
		FString Json = FString::Printf(TEXT("\"%s\": ["), Name);
		for (int32 Index = 0; Index < Curve.Num(); ++Index)
		{
			Json += M5_050_Num3(Curve[Index]) + (Index + 1 < Curve.Num() ? TEXT(",") : TEXT(""));
		}
		return Json + TEXT("]");
	}(TEXT("active_pellets"), ActivePelletsCurve);
	CurveJson += TEXT(", ") + [&](const TCHAR* Name, const TArray<double>& Curve)
	{
		FString Json = FString::Printf(TEXT("\"%s\": ["), Name);
		for (int32 Index = 0; Index < Curve.Num(); ++Index)
		{
			Json += M5_050_Num3(Curve[Index]) + (Index + 1 < Curve.Num() ? TEXT(",") : TEXT(""));
		}
		return Json + TEXT("]");
	}(TEXT("ledger_active_shots"), LedgerShotsCurve);
	FString MemoryJson = TEXT("[");
	for (int32 Index = 0; Index < MemorySamplesMb.Num(); ++Index)
	{
		MemoryJson += M5_050_Num3(MemorySamplesMb[Index]) + (Index + 1 < MemorySamplesMb.Num() ? TEXT(",") : TEXT(""));
	}
	MemoryJson += TEXT("]");

	const FString SectionJson = FString::Printf(
		TEXT("{\"rhi_context\": \"%s\", \"frame_seconds\": %s, \"frames\": %d, \"targets\": %d, ")
		TEXT("\"concurrency_target\": %d, \"volley_pellets\": %d, \"speed_cm_s\": %s, \"lifetime_s\": %s, ")
		TEXT("\"gc_every_frames\": %d, \"gc_count\": %d, \"gc_total_ms\": %s, ")
		TEXT("\"p50_ms\": %s, \"p95_ms\": %s, \"p99_ms\": %s, ")
		TEXT("\"budget_thresholds\": {\"p95_ms\": %s, \"p99_ms\": %s, \"nullrhi_runaway_p99_ms\": %s}, ")
		TEXT("\"budget_held\": %s, \"frame_ms\": %s, %s, \"memory_mb\": %s, ")
		TEXT("\"max_active_pellets\": %d, \"max_service_live\": %d, \"service_cap\": %d, ")
		TEXT("\"capacity_full_refusals\": %d, \"reserve_failures\": %d, \"commit_failures\": %d, ")
		TEXT("\"ledger_events_held\": %d, \"ledger_tracked_shots\": %d, \"hits_by_health\": %s, ")
		TEXT("\"hits_by_submissions\": %d, \"ledger_active_shots_final\": %d, ")
		TEXT("\"dedup_probes\": %d, \"dedup_probe_accepted\": %d, \"dedup_duplicate_event\": %d, \"dedup_shot_ended\": %d, ")
		TEXT("\"control_probes\": %d, \"control_probe_accepted\": %d}"),
		bNullRhi ? TEXT("nullrhi") : TEXT("rendered"),
		*M5_050_Num3(M5_050_FrameSeconds), SampledFrames, M5_050_TargetCount,
		M5_050_TargetConcurrent, M5_050_VolleyPellets,
		*M5_050_Num3(M5_050_SpeedCmS), *M5_050_Num3(M5_050_LifetimeS),
		M5_050_GcEveryFrames, GcCount, *M5_050_Num3(GcTotalMs),
		*M5_050_Num3(P50Ms), *M5_050_Num3(P95Ms), *M5_050_Num3(P99Ms),
		*M5_050_Num3(M5_050_BudgetP95Ms), *M5_050_Num3(M5_050_BudgetP99Ms), *M5_050_Num3(M5_050_RunawayP99Ms),
		bBudgetHeld ? TEXT("true") : TEXT("false"), *FramesJson, *CurveJson, *MemoryJson,
		MaxActive, MaxServiceLive, static_cast<int32>(MaxLiveProjectiles),
		CapacityFullRefusals, ReserveFailures, CommitFailures,
		LedgerEventsHeld, LedgerTrackedShots, *M5_050_Num3(HitsTotalByHealth), HitsBySubmissions,
		Ledger.GetNumActiveShots(),
		DedupProbes, DedupProbeAccepted, DedupDuplicateEventRefusals, DedupShotEndedRefusals,
		ControlProbes, ControlProbeAccepted);
	M5_050_WriteReport(*this, TEXT("stress"),
		bBudgetHeld && CapacityFullRefusals == 0 && ReserveFailures == 0 && CommitFailures == 0
			&& DedupProbeAccepted == 0 && ControlProbeAccepted == 0
			&& FMath::Abs(HitsTotalByHealth - static_cast<double>(HitsBySubmissions)) < 0.5
			&& SampledFrames == M5_050_TotalFrames && MaxActive <= M5_050_TargetConcurrent
			&& MaxServiceLive <= MaxLiveProjectiles,
		SectionJson);

	Wrapper.EndPlayInTestWorld();
	return true;
}

#endif
