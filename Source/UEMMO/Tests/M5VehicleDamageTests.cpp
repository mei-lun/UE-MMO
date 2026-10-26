// M5-039: the vehicle hull as a hittable world entity (interface contract
// section 8). The suite drives the REAL shared pipeline against an
// ACombatVehicle hull - no parallel combat stack anywhere:
//
//   1. Unified entry damage: the hitscan delivery executor (M5-026, the
//      bullet face) traces the hull body and applies the configured damage
//      through ApplyUnifiedHit; the melee face submits the same way. The
//      numbers come from the real formula over the real definition
//      (HP 250 / defense 15, the shipped M5-037 sample): a base-7 round
//      removes round(700/115) = 6, a base-10 melee removes 9, and a same-key
//      replay is refused by the ledger (exactly one application).
//   2. Melee selection: QueryTargets picks the hull (a live-HealthComponent
//      body) and a dead hull drops out of selection; the hull takes full
//      damage with every control kind refused (blanket control immunity -
//      contract section 5), no stun state, no landing recovery.
//   3. Faction: the hull's registered faction joins the existing filter -
//      a same-faction attacker is refused (SameFaction), a different or
//      unaligned (empty) faction takes the hit.
//   4. The death sequence runs exactly once: seats vacated first (the
//      M5-040 ClearVehicle seam), then the registry unregistered, then the
//      one-shot destroy event. A repeated hit (TargetDead), a second
//      FinalizeDestruction and the actor destroy are rejected second
//      terminals with no side effects.
//   5. Spawn/destroy cycles: registry and occupancy return to their
//      baselines every cycle.
//   6. Old epoch / destroyed target: a stale-epoch request is refused by the
//      ledger (StaleEpoch), the wiped target id is refused (UnknownTarget),
//      and the living hull continues under the new generation after
//      re-registration (the retry/leave-room face).
//   7. Structural Profile ban: the World-transient hull state is not
//      reflected (no save path can reach it) and the destroy event stays a
//      pointer-free value snapshot (contract section 0.5).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/Actor.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatHitQuery.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Combat/System/UnifiedHitApplier.h"
#include "../Projectiles/HitscanExecutor.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/SeatModel.h"
#include "../Vehicles/VehicleTypes.h"
#include "../Weapons/ShotPattern.h"
#include "../Weapons/WeaponComponent.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

// The pins of the header, repeated at the test site (the M5-010 precedent):
// the destroy event is an id value snapshot - ids and one weak reference,
// never a raw actor pointer, never a profile value.
static_assert(std::is_trivially_copyable_v<FVehicleDestroyedEvent>, "FVehicleDestroyedEvent must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FVehicleDestroyedEvent::Vehicle)>, "FVehicleDestroyedEvent::Vehicle must stay a weak reference");

namespace UE::UEMMO::Tasks::M5_039
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X is the horizontal firing axis, Y the depth axis, Z height.
	const FVector M5_039_SceneBase(62000.0, 52000.0, 800.0);

	// The shipped M5-037/M5-038 single-seat ground sample (vehicles.json /
	// vehicle_seats.json ids): the real production configuration, not test
	// slang - the acceptance pins damage against the REAL definition values.
	static FVehicleDefinition MakeShippedSampleVehicle()
	{
		FVehicleDefinition Vehicle;
		Vehicle.VehicleId = FName(TEXT("vehicle_wheeled_sample"));
		Vehicle.DriveType = EVehicleDriveType::Ground;
		Vehicle.MaxHealth = 250.0f;
		Vehicle.Defense = 15.0f;
		Vehicle.MaxSpeedCmS = 700.0f;
		Vehicle.AccelerationCmS2 = 900.0f;
		Vehicle.MeshPath = TEXT("/Game/UEMMO/Vehicles/SM_Vehicle_Wheeled_Sample.SM_Vehicle_Wheeled_Sample");
		Vehicle.CameraDistanceCm = 650.0f;
		Vehicle.CameraHeightCm = 260.0f;
		Vehicle.CameraPitchDeg = -18.0f;
		return Vehicle;
	}

	/** The bullet face: the M5-026 hitscan test round (base 7, control ride-along). */
	static FDamageProfile MakeBulletRound()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("vehicle_test_round");
		Profile.BaseDamage = 7.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 1.0f;
		Profile.KnockbackCmPerSecond = 70.0f;
		Profile.LaunchCmPerSecond = 700.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** The melee face: the legacy light_01 numbers (M5-003 pinned values). */
	static FDamageProfile MakeMeleeLight()
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

	/** The lethal hull-buster: base 290 removes round(29000/115) = 252 >= 250 HP. */
	static FDamageProfile MakeLethalRound()
	{
		FDamageProfile Profile = MakeBulletRound();
		Profile.DamageProfileId = TEXT("vehicle_test_lethal_round");
		Profile.BaseDamage = 290.0f;
		return Profile;
	}

	/** The launcher-class melee face: base 18 with the full control ride-along (M5-003 launcher numbers). */
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

	/** One straight pellet down the firing axis. */
	static FShotPellet MakeStraightPellet()
	{
		FShotPellet Pellet;
		Pellet.PelletIndex = 0;
		Pellet.Direction = FVector(1.0, 0.0, 0.0);
		return Pellet;
	}

	/**
	 * World acquisition, the M5_012 pattern PLUS the engine FTestWorldWrapper's
	 * full world shape (M5-047-lesson): a GameInstance, a registered world
	 * context and SetGameInstance - without them the engine skips
	 * PostInitializeComponents for spawned actors and skips EndPlay routing on
	 * Destroy() (Actor.cpp gates both on AreActorsInitialized /
	 * ActorHasBegunPlay), which would keep the vehicle's death sequence and
	 * the destroy-cleanup face from ever running.
	 */
	static UWorld* M5_039_AcquireWorld()
	{
		UGameInstance* TestGameInstance = NewObject<UGameInstance>(GEngine);
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_039_TestWorld")));
		if (TempWorld != nullptr)
		{
			TempWorld->SetShouldTick(false);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.OwningGameInstance = TestGameInstance;
			WorldContext.SetCurrentWorld(TempWorld);
			TempWorld->SetGameInstance(TestGameInstance);
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_039 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_039_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_039_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	/** Test seam for the hit-to-entity face (the M5_026 pattern): the scene's id map. */
	struct FM5_039_TestIdentity : IHitscanTargetIdentity
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
	 * One scene: registry + bound ledger + seat model + identity seam + one
	 * registered attacker entity. DECLARATION ORDER MATTERS: the scene is a
	 * local declared BEFORE the world scope, so it outlives the world's actor
	 * teardown - the vehicles' EndPlay destroy sequence broadcasts into the
	 * still-live counters and clears through the still-live seat model.
	 */
	struct FM5_039_Scene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{ &Registry };
		FSeatOccupancyModel Seats;
		FM5_039_TestIdentity Identity;
		FVehicleDefinition VehicleDefinition;

		AActor* Attacker = nullptr;
		FEntityId AttackerId = InvalidCombatEntityId;
		int32 BaselineEntities = 0;

		int32 DestroyedEvents = 0;
		FName LastDestroyedVehicleId;
		int32 SeatVacateCalls = 0;
		FName LastVacatedVehicleId;

		bool Build(FAutomationTestBase& Test, UWorld& World, const TCHAR* AttackerFaction = TEXT("player"))
		{
			// M5-047-lesson: the engine dispatches PostInitializeComponents and
			// BeginPlay, and routes EndPlay on Destroy, only for worlds past
			// actor initialization (Actor.cpp's AreActorsInitialized gate). A
			// raw UWorld::CreateWorld world never passes that gate unless the
			// scene initializes it HERE - without this, spawned vehicles skip
			// their death-sequence binding face and Destroy() skips EndPlay.
			// The GameMode step mirrors the engine FTestWorldWrapper (which
			// sets one whenever the world carries a GameInstance).
			FURL Url;
			World.SetGameMode(Url);
			if (!Test.TestTrue(TEXT("the world's game mode initialized"),
					World.GetAuthGameMode() != nullptr && World.GetWorldSettings() != nullptr))
			{
				return false;
			}
			World.InitializeActorsForPlay(Url);
			World.BeginPlay();

			VehicleDefinition = MakeShippedSampleVehicle();

			// The attacker body: no collision (a bare source entity, like the
			// M5_026 shooter - traces start past it, and the melee query
			// ignores it by identity).
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), M5_039_SceneBase + FVector(-100.0, 0.0, 0.0), FRotator::ZeroRotator);
			if (Attacker == nullptr)
			{
				Test.AddError(TEXT("the attacker actor could not spawn"));
				return false;
			}

			FCombatEntityMetadata Metadata;
			Metadata.Faction = AttackerFaction;
			Metadata.Category = FName(TEXT("attacker"));
			AttackerId = Registry.RegisterEntity(Attacker, Metadata, Registry.GetCurrentEpoch());
			if (!IsValidCombatEntityId(AttackerId))
			{
				Test.AddError(TEXT("the attacker entity could not register"));
				return false;
			}
			BaselineEntities = Registry.GetNumRegisteredEntities();
			return true;
		}

		/** The registered attacker fires a new public shot slot (M5-010 allocator). */
		FShotId AllocateShot()
		{
			return Registry.AllocateActionSequence(AttackerId, Registry.GetCurrentEpoch());
		}

		/**
		 * Spawns + configures + registers a vehicle hull and wires the
		 * death-sequence seams into this scene's counters and seat model.
		 * Returns null (test asserts) when any step refuses.
		 */
		ACombatVehicle* SpawnVehicle(UWorld& World, const FVector& Location, FName Faction,
			FAutomationTestBase& Test, bool bWithOccupiedSeat = false, uint32 OccupantSeed = 1)
		{
			ACombatVehicle* Vehicle = World.SpawnActor<ACombatVehicle>(ACombatVehicle::StaticClass(), Location, FRotator::ZeroRotator);
			if (Vehicle == nullptr)
			{
				Test.AddError(TEXT("the vehicle actor could not spawn"));
				return nullptr;
			}

			FString Errors;
			if (!Vehicle->SetVehicleDefinition(VehicleDefinition, &Errors))
			{
				Test.AddError(FString::Printf(TEXT("the vehicle definition was refused: %s"), *Errors));
				return nullptr;
			}
			if (!IsValidCombatEntityId(Vehicle->RegisterCombatEntity(Registry, Faction)))
			{
				Test.AddError(TEXT("the vehicle entity could not register"));
				return nullptr;
			}

			Vehicle->SetSeatVacateCallback([this](FName VehicleIdOfHull)
			{
				++SeatVacateCalls;
				LastVacatedVehicleId = VehicleIdOfHull;
				// The M5-041 wiring face: the hull's own id clears its
				// occupancy; registrations survive (M5-040 semantics).
				Seats.ClearVehicle(VehicleIdOfHull);
			});
			Vehicle->OnVehicleDestroyed.AddRaw(this, &FM5_039_Scene::HandleVehicleDestroyed);
			Identity.Record(*Vehicle, Vehicle->GetEntityId());

			if (bWithOccupiedSeat)
			{
				if (!Seats.IsSeatRegistered(VehicleDefinition.VehicleId, FName(TEXT("seat_wheeled_driver"))))
				{
					EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
					if (!Seats.RegisterSeat(VehicleDefinition.VehicleId, FName(TEXT("seat_wheeled_driver")), Reason))
					{
						Test.AddError(TEXT("the seat could not register in the occupancy model"));
						return nullptr;
					}
				}
				// The M5-040 wiring point this card owns: the validity gate
				// vouches for the living hull (SeatModel.h's named 039 face).
				Seats.SetVehicleValidityGate(Vehicle->MakeVehicleValidityGate());

				FVehicleSeatEnterRequest Enter;
				Enter.VehicleId = VehicleDefinition.VehicleId;
				Enter.SeatId = FName(TEXT("seat_wheeled_driver"));
				Enter.OccupantId = FGuid(OccupantSeed, OccupantSeed + 1, OccupantSeed + 2, OccupantSeed + 3);
				EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
				if (!Seats.TryEnterSeat(Enter, Reason))
				{
					Test.AddError(FString::Printf(TEXT("the occupant could not enter the seat (reason %d)"), static_cast<int32>(Reason)));
					return nullptr;
				}
			}
			return Vehicle;
		}

		/**
		 * The unified request of one hit against the hull - the exact shape
		 * the M5-026 executor builds (HitscanExecutor::MakeUnifiedRequest):
		 * the target reaction is read from the target's own combat component,
		 * AttackPower stays 0 (the resolved profile applies verbatim).
		 */
		FUnifiedHitRequest MakeVehicleHitRequest(ACombatVehicle& Vehicle, const FDamageProfile& Attack, FShotId ShotId) const
		{
			FUnifiedHitRequest Request;
			Request.Epoch = Registry.GetCurrentEpoch();
			Request.AttackerEntityId = AttackerId;
			Request.ShotId = ShotId;
			Request.PelletIndex = 0;
			Request.TargetEntityId = Vehicle.GetEntityId();
			Request.TargetActor = &Vehicle;
			Request.Attack = Attack;
			if (UCombatComponent* TargetCombat = Vehicle.GetCombatComponent())
			{
				Request.TargetReaction = TargetCombat->GetTargetReactionPolicy();
			}
			Request.AttackPower = 0.0f;
			Request.HitLocation = Vehicle.GetActorLocation();
			return Request;
		}

		void HandleVehicleDestroyed(const FVehicleDestroyedEvent& Event)
		{
			++DestroyedEvents;
			LastDestroyedVehicleId = Event.VehicleId;
		}
	};

	// ------------------------------------------------------------------
	// 1. Unified entry applies the configured damage (bullet + melee faces)
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_039UnifiedEntryAppliesConfiguredVehicleDamage,
		"UEMMO.Tasks.M5_039.UnifiedEntryAppliesConfiguredVehicleDamage",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_039UnifiedEntryAppliesConfiguredVehicleDamage::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_039_AcquireWorld();
		FM5_039_WorldScope Scope{ World };
		FM5_039_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}

		ACombatVehicle* Vehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(400.0, 0.0, 0.0), NAME_None, *this);
		if (Vehicle == nullptr)
		{
			return false;
		}

		// The definition drove the shared pool: the one health owner starts at
		// the configured max, the defense rides the shared combat component.
		TestTrue(TEXT("the hull loaded its definition"), Vehicle->HasVehicleDefinition());
		TestEqual(TEXT("the hull starts at the configured max health"), Vehicle->GetHealthComponent()->GetHealth(), 250.0f);
		TestEqual(TEXT("the hull carries the configured defense"), Vehicle->GetCombatComponent()->GetDefense(), 15.0f);
		int32 HealthPoolCount = 0;
		for (UActorComponent* Component : Vehicle->GetComponents())
		{
			if (Component != nullptr && Component->IsA(UHealthComponent::StaticClass()))
			{
				++HealthPoolCount;
			}
		}
		TestEqual(TEXT("the hull owns exactly one health component (no second pool)"), HealthPoolCount, 1);

		// Bullet face end-to-end: the real delivery executor traces the hull
		// body and submits through ApplyUnifiedHit (no direct health path).
		FHitscanRequest Hitscan;
		Hitscan.Shot.Epoch = Scene.Registry.GetCurrentEpoch();
		Hitscan.Shot.SourceEntityId = Scene.AttackerId;
		Hitscan.Shot.ShotId = Scene.AllocateShot();
		Hitscan.Shot.PelletCount = 1;
		Hitscan.Shot.RangeCm = 1000.0f;
		Hitscan.Origin = M5_039_SceneBase + FVector(-100.0, 0.0, 0.0);
		Hitscan.Pellets.Add(MakeStraightPellet());
		Hitscan.AttackProfile = MakeBulletRound();
		const FHitscanExecutionOutcome HitscanOutcome = ExecuteHitscan(World, Hitscan, &Scene.Registry, &Scene.Ledger, &Scene.Identity);
		TestTrue(TEXT("the hitscan request executed"), HitscanOutcome.bExecuted);
		TestEqual(TEXT("the bullet face applied one hit"), HitscanOutcome.HitsApplied, 1);
		TestEqual(TEXT("the bullet submitted exactly one hit record"), HitscanOutcome.Hits.Num(), 1);
		if (HitscanOutcome.Hits.Num() > 0)
		{
			TestEqual(TEXT("the bullet removes the configured damage round(700/115)=6"),
				HitscanOutcome.Hits[0].Outcome.DamageApplied, 6.0f);
			TestFalse(TEXT("the hull does not accept the bullet's control ride-along"),
				HitscanOutcome.Hits[0].Outcome.Control.AnyAccepted());
		}
		TestEqual(TEXT("the hull health dropped by the configured damage"), Vehicle->GetHealthComponent()->GetHealth(), 244.0f);

		// Melee face: the same unified entry over the legacy light_01 profile.
		const FUnifiedHitRequest MeleeRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeMeleeLight(), Scene.AllocateShot());
		const FUnifiedHitOutcome MeleeOutcome = ApplyUnifiedHit(MeleeRequest, Scene.Ledger);
		TestTrue(TEXT("the melee face applied"), MeleeOutcome.WasApplied());
		TestEqual(TEXT("the melee removes the configured damage round(1000/115)=9"), MeleeOutcome.DamageApplied, 9.0f);
		TestEqual(TEXT("the hull health dropped again by the configured damage"), Vehicle->GetHealthComponent()->GetHealth(), 235.0f);
		TestFalse(TEXT("the melee control never applies to the hull"), MeleeOutcome.Control.AnyAccepted());

		// Exactly one application: the same-key replay is ledger-refused.
		const FUnifiedHitOutcome ReplayOutcome = ApplyUnifiedHit(MeleeRequest, Scene.Ledger);
		TestTrue(TEXT("the same-key replay was blocked"), ReplayOutcome.bWasBlocked);
		TestEqual(TEXT("the replay was refused as a duplicate event"), ReplayOutcome.LedgerReason, EHitLedgerRejectReason::DuplicateEvent);
		TestEqual(TEXT("the replay applied zero damage"), ReplayOutcome.DamageApplied, 0.0f);
		TestEqual(TEXT("the hull health is untouched by the replay"), Vehicle->GetHealthComponent()->GetHealth(), 235.0f);
		return true;
	}

	// ------------------------------------------------------------------
	// 2. Melee selection finds the hull; the hull is control-immune
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_039MeleeSelectionHitsVehicleAndHullIsControlImmune,
		"UEMMO.Tasks.M5_039.MeleeSelectionHitsVehicleAndHullIsControlImmune",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_039MeleeSelectionHitsVehicleAndHullIsControlImmune::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_039_AcquireWorld();
		FM5_039_WorldScope Scope{ World };
		FM5_039_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}

		// A real melee candidate body (the attacker's own body, ignored by
		// identity) and the hull in the same box.
		AActor* MeleeBody = World->SpawnActor<AActor>(AActor::StaticClass(), M5_039_SceneBase + FVector(-200.0, 0.0, 0.0), FRotator::ZeroRotator);
		if (MeleeBody == nullptr)
		{
			AddError(TEXT("the melee body could not spawn"));
			return false;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(MeleeBody, TEXT("M5_039_MeleeBody"));
		MeleeBody->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(FVector(30.0, 30.0, 60.0));
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Block);
		Body->RegisterComponent();
		UHealthComponent* BodyHealth = NewObject<UHealthComponent>(MeleeBody, TEXT("M5_039_MeleeBodyHealth"));
		BodyHealth->RegisterComponent();

		ACombatVehicle* Vehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(200.0, 0.0, 0.0), NAME_None, *this);
		if (Vehicle == nullptr)
		{
			return false;
		}

		// The melee selection face: the hull is a live-HealthComponent body in
		// the pawn channel, so the existing overlap query picks it. The box
		// spans both bodies (melee body at -200, hull box at 200..290 X).
		FCombatHitBox Box;
		Box.Center = M5_039_SceneBase;
		Box.Extent = FVector(500.0, 200.0, 120.0);
		const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(World, Box, MeleeBody, 0);
		TestEqual(TEXT("the melee query selects exactly the hull"), Targets.Num(), 1);
		if (Targets.Num() == 1)
		{
			TestEqual(TEXT("the selected target is the vehicle"), Targets[0].Get(), static_cast<AActor*>(Vehicle));
		}

		// The hull's target policy: blanket control immunity, full damage, no
		// death resistance - and it stays a legal policy value.
		const FTargetReaction HullPolicy = Vehicle->GetCombatComponent()->GetTargetReactionPolicy();
		TestTrue(TEXT("the hull overrides the target reaction policy"), Vehicle->GetCombatComponent()->HasOverriddenTargetReactionPolicy());
		FString PolicyErrors;
		TestTrue(TEXT("the hull policy is a legal reaction value"), ValidateTargetReaction(HullPolicy, PolicyErrors));
		TestTrue(TEXT("the hull is blanket control-immune"), HullPolicy.bImmuneControl);
		TestFalse(TEXT("the hull refuses stagger"), HullPolicy.bAllowStagger);
		TestFalse(TEXT("the hull refuses launch"), HullPolicy.bAllowLaunch);
		TestFalse(TEXT("the hull refuses knockdown"), HullPolicy.bAllowKnockdown);
		TestFalse(TEXT("the hull takes damage (no damage immunity)"), HullPolicy.bImmuneDamage);
		TestFalse(TEXT("the hull dies (no death resistance)"), HullPolicy.bDeathResistant);

		// A launcher-class melee hit on the living hull: full configured damage
		// with every control kind refused (control immunity still lets damage
		// through - contract section 5) and no stun state on the shared
		// component.
		const FUnifiedHitRequest LauncherRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeLauncher(), Scene.AllocateShot());
		const FUnifiedHitOutcome LauncherOutcome = ApplyUnifiedHit(LauncherRequest, Scene.Ledger);
		TestTrue(TEXT("the launcher hit applied"), LauncherOutcome.WasApplied());
		TestEqual(TEXT("the hull takes the full configured damage round(1800/115)=16"), LauncherOutcome.DamageApplied, 16.0f);
		TestEqual(TEXT("the hull health dropped by the launcher damage"), Vehicle->GetHealthComponent()->GetHealth(), 234.0f);
		TestFalse(TEXT("no stagger on the hull"), LauncherOutcome.Control.bStagger);
		TestFalse(TEXT("no launch on the hull"), LauncherOutcome.Control.bLaunch);
		TestFalse(TEXT("no knockdown on the hull"), LauncherOutcome.Control.bKnockdown);
		TestTrue(TEXT("the hull's combat component accepts movement (no stun state)"),
			Vehicle->GetCombatComponent()->CanAcceptMovement());
		TestFalse(TEXT("the hull is not in landing recovery"), Vehicle->GetCombatComponent()->IsInLandingRecovery());

		// The hull dies to the lethal hit; the removal caps at the hull's
		// remaining pool (the health owner's clamp), the selection face drops
		// dead bodies.
		const FUnifiedHitRequest LethalRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeLethalRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(LethalRequest, Scene.Ledger);
		TestTrue(TEXT("the lethal hit applied"), Outcome.WasApplied());
		TestEqual(TEXT("the removal caps at the hull's remaining pool"), Outcome.DamageApplied, 234.0f);
		TestTrue(TEXT("the lethal hit reported the target death"), Outcome.bTargetDied);
		TestEqual(TEXT("the hull health is zero"), Vehicle->GetHealthComponent()->GetHealth(), 0.0f);
		TestFalse(TEXT("the hull is no longer alive"), Vehicle->IsHullAlive());
		const TArray<TWeakObjectPtr<AActor>> TargetsAfterDeath = QueryTargets(World, Box, MeleeBody, 0);
		TestEqual(TEXT("a dead hull is never selected again"), TargetsAfterDeath.Num(), 0);
		return true;
	}

	// ------------------------------------------------------------------
	// 3. The registered faction joins the existing friendly-fire filter
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_039FactionFilteringCoversVehicleTargets,
		"UEMMO.Tasks.M5_039.FactionFilteringCoversVehicleTargets",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_039FactionFilteringCoversVehicleTargets::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_039_AcquireWorld();
		FM5_039_WorldScope Scope{ World };
		FM5_039_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}

		// A player-faction hull next to a player-faction attacker: the existing
		// SameFaction filter covers the vehicle without any new rule.
		ACombatVehicle* FriendlyVehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(400.0, 0.0, 0.0), FName(TEXT("player")), *this);
		ACombatVehicle* HostileVehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(400.0, 600.0, 0.0), FName(TEXT("enemy")), *this);
		ACombatVehicle* UnalignedVehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(400.0, 1200.0, 0.0), NAME_None, *this);
		if (FriendlyVehicle == nullptr || HostileVehicle == nullptr || UnalignedVehicle == nullptr)
		{
			return false;
		}

		// Same non-empty faction: refused before anything applies.
		const FUnifiedHitRequest FriendlyRequest = Scene.MakeVehicleHitRequest(*FriendlyVehicle, MakeBulletRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome FriendlyOutcome = ApplyUnifiedHit(FriendlyRequest, Scene.Ledger);
		TestTrue(TEXT("the teammate hull refused the hit"), FriendlyOutcome.bWasBlocked);
		TestEqual(TEXT("the teammate refusal names SameFaction"), FriendlyOutcome.RefuseReason, EUnifiedHitRefuseReason::SameFaction);
		TestEqual(TEXT("the teammate hull took no damage"), FriendlyVehicle->GetHealthComponent()->GetHealth(), 250.0f);

		// A different faction takes the configured damage.
		const FUnifiedHitRequest HostileRequest = Scene.MakeVehicleHitRequest(*HostileVehicle, MakeBulletRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome HostileOutcome = ApplyUnifiedHit(HostileRequest, Scene.Ledger);
		TestTrue(TEXT("the hostile hull applied the hit"), HostileOutcome.WasApplied());
		TestEqual(TEXT("the hostile hull took the configured damage"), HostileVehicle->GetHealthComponent()->GetHealth(), 244.0f);

		// The default (empty = unaligned) faction never trips the filter - the
		// UCombatComponent precedent for a world without a faction source.
		const FUnifiedHitRequest UnalignedRequest = Scene.MakeVehicleHitRequest(*UnalignedVehicle, MakeBulletRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome UnalignedOutcome = ApplyUnifiedHit(UnalignedRequest, Scene.Ledger);
		TestTrue(TEXT("the unaligned hull applied the hit"), UnalignedOutcome.WasApplied());
		TestEqual(TEXT("the unaligned hull took the configured damage"), UnalignedVehicle->GetHealthComponent()->GetHealth(), 244.0f);
		return true;
	}

	// ------------------------------------------------------------------
	// 4. The death sequence runs exactly once (seats -> registry -> event)
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_039LethalSequenceRunsExactlyOnce,
		"UEMMO.Tasks.M5_039.LethalSequenceRunsExactlyOnce",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_039LethalSequenceRunsExactlyOnce::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_039_AcquireWorld();
		FM5_039_WorldScope Scope{ World };
		FM5_039_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}

		// An occupied hull: the seat is registered and held by occupant A.
		ACombatVehicle* Vehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(400.0, 0.0, 0.0), NAME_None, *this,
			/*bWithOccupiedSeat*/ true, /*OccupantSeed*/ 10);
		if (Vehicle == nullptr)
		{
			return false;
		}
		const FEntityId VehicleEntityId = Vehicle->GetEntityId();
		const int32 EntitiesWithHull = Scene.Registry.GetNumRegisteredEntities();
		TestEqual(TEXT("the occupied hull is one more live entity"), EntitiesWithHull, Scene.BaselineEntities + 1);
		TestEqual(TEXT("one seat is occupied before the death"), Scene.Seats.GetNumOccupiedSeats(), 1);

		// The lethal hit drives HealthComponent to 0 -> OnDied -> the sequence.
		const FUnifiedHitRequest LethalRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeLethalRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome LethalOutcome = ApplyUnifiedHit(LethalRequest, Scene.Ledger);
		TestTrue(TEXT("the lethal hit applied"), LethalOutcome.WasApplied());
		TestTrue(TEXT("the lethal hit reported the target death"), LethalOutcome.bTargetDied);
		TestEqual(TEXT("the hull health is zero"), Vehicle->GetHealthComponent()->GetHealth(), 0.0f);

		// (a) seats first: the occupancy is gone, the registration survives
		// (M5-040 ClearVehicle semantics).
		TestEqual(TEXT("the vacate seam ran exactly once"), Scene.SeatVacateCalls, 1);
		TestEqual(TEXT("the vacate named the hull's vehicle id"), Scene.LastVacatedVehicleId, FName(TEXT("vehicle_wheeled_sample")));
		TestEqual(TEXT("no seat is occupied after the death"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestEqual(TEXT("the seat registration survived the death"), Scene.Seats.GetNumRegisteredSeats(), 1);

		// (b) the registry record is gone; the count returned to baseline.
		TestFalse(TEXT("the hull's entity is unregistered"), Scene.Registry.IsKnownEntity(VehicleEntityId));
		TestEqual(TEXT("the registry count returned to baseline"), Scene.Registry.GetNumRegisteredEntities(), Scene.BaselineEntities);

		// (c) the destroy event broadcast exactly once.
		TestEqual(TEXT("the destroy event fired exactly once"), Scene.DestroyedEvents, 1);
		TestEqual(TEXT("the destroy event named the hull's vehicle id"), Scene.LastDestroyedVehicleId, FName(TEXT("vehicle_wheeled_sample")));
		TestTrue(TEXT("the hull reports the finalized terminal state"), Vehicle->IsDestroyFinalized());

		// Second terminal faces are rejected without side effects:
		// a repeated hit is refused by the unified entry (dead target).
		const FUnifiedHitRequest RepeatRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeBulletRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome RepeatOutcome = ApplyUnifiedHit(RepeatRequest, Scene.Ledger);
		TestTrue(TEXT("the hit on the destroyed hull was blocked"), RepeatOutcome.bWasBlocked);
		TestEqual(TEXT("the destroyed hull refuses as TargetDead"), RepeatOutcome.RefuseReason, EUnifiedHitRefuseReason::TargetDead);
		TestEqual(TEXT("the destroyed hull took no damage"), RepeatOutcome.DamageApplied, 0.0f);
		TestEqual(TEXT("the destroy event did not fire again"), Scene.DestroyedEvents, 1);
		TestEqual(TEXT("the vacate seam did not run again"), Scene.SeatVacateCalls, 1);
		TestEqual(TEXT("the registry count stayed at baseline"), Scene.Registry.GetNumRegisteredEntities(), Scene.BaselineEntities);

		// A direct second FinalizeDestruction is the rejected terminal.
		TestFalse(TEXT("the second finalize call is rejected"), Vehicle->FinalizeDestruction());
		TestEqual(TEXT("the rejected finalize changed nothing (events)"), Scene.DestroyedEvents, 1);
		TestEqual(TEXT("the rejected finalize changed nothing (seats)"), Scene.SeatVacateCalls, 1);

		// The actor destroy after death is the same rejected terminal.
		Vehicle->Destroy();
		TestEqual(TEXT("the actor destroy did not re-run the sequence (events)"), Scene.DestroyedEvents, 1);
		TestEqual(TEXT("the actor destroy did not re-run the sequence (seats)"), Scene.SeatVacateCalls, 1);

		// The model-level reuse face (M5-040): the registration survived, so a
		// new occupant can take the same vehicle again once the gate allows -
		// while the gate is bound, the DEAD hull refuses entry (the validity
		// gate this card wired is doing its job).
		FVehicleSeatEnterRequest Reenter;
		Reenter.VehicleId = FName(TEXT("vehicle_wheeled_sample"));
		Reenter.SeatId = FName(TEXT("seat_wheeled_driver"));
		Reenter.OccupantId = FGuid(20, 21, 22, 23);
		EVehicleSeatRejectReason GateReason = EVehicleSeatRejectReason::None;
		TestFalse(TEXT("the dead hull's validity gate refuses re-entry"), Scene.Seats.TryEnterSeat(Reenter, GateReason));
		TestEqual(TEXT("the dead hull refuses with VehicleInvalid"), GateReason, EVehicleSeatRejectReason::VehicleInvalid);
		Scene.Seats.SetVehicleValidityGate(FSeatOccupancyModel::FSeatEnterGate());
		EVehicleSeatRejectReason ReenterReason = EVehicleSeatRejectReason::None;
		TestTrue(TEXT("the same vehicle id can be occupied again (model level)"), Scene.Seats.TryEnterSeat(Reenter, ReenterReason));
		return true;
	}

	// ------------------------------------------------------------------
	// 5. Spawn/destroy cycles return registry and occupancy to baseline
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_039SpawnDestroyCyclesReturnRegistryToBaseline,
		"UEMMO.Tasks.M5_039.SpawnDestroyCyclesReturnRegistryToBaseline",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_039SpawnDestroyCyclesReturnRegistryToBaseline::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_039_AcquireWorld();
		FM5_039_WorldScope Scope{ World };
		FM5_039_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}

		for (int32 Cycle = 0; Cycle < 3; ++Cycle)
		{
			ACombatVehicle* Vehicle = Scene.SpawnVehicle(*World,
				M5_039_SceneBase + FVector(400.0 + Cycle * 800.0, 0.0, 0.0), NAME_None, *this);
			if (Vehicle == nullptr)
			{
				return false;
			}
			TestTrue(TEXT("each cycle mints a valid entity id"), IsValidCombatEntityId(Vehicle->GetEntityId()));
			// M5-050 precedent: the id counter never rewinds after unregister,
			// so the cycle mint continues past the attacker (baseline count 1
			// = the attacker's id 1; the first vehicle mints id 2).
			TestEqual(TEXT("the cycle registration added exactly one entity"),
				Scene.Registry.GetNumRegisteredEntities(), Scene.BaselineEntities + 1);
			TestEqual(TEXT("each cycle mints a fresh entity id"),
				Vehicle->GetEntityId(), static_cast<FEntityId>(Scene.BaselineEntities + 1 + Cycle));

			// A double registration of the same hull is the idempotent cached
			// face (never a second record).
			TestEqual(TEXT("a repeated registration reuses the cached id"),
				Vehicle->RegisterCombatEntity(Scene.Registry, NAME_None), Vehicle->GetEntityId());
			TestEqual(TEXT("the repeated registration minted no second record"),
				Scene.Registry.GetNumRegisteredEntities(), Scene.BaselineEntities + 1);

			// Destroying the hull unregisters it (the destroy cleanup face).
			Vehicle->Destroy();
			TestEqual(TEXT("the destroyed cycle returned the registry to baseline"),
				Scene.Registry.GetNumRegisteredEntities(), Scene.BaselineEntities);
		}

		TestEqual(TEXT("all cycles broadcast their destroy event once"), Scene.DestroyedEvents, 3);
		TestEqual(TEXT("all cycles vacated through the seam once"), Scene.SeatVacateCalls, 3);
		TestEqual(TEXT("the ledger collected no events outside the cycles"), Scene.Ledger.GetNumRecordedEvents(), 0);
		return true;
	}

	// ------------------------------------------------------------------
	// 6. Old-epoch and destroyed targets are refused
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_039StaleEpochAndDestroyedTargetRefused,
		"UEMMO.Tasks.M5_039.StaleEpochAndDestroyedTargetRefused",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_039StaleEpochAndDestroyedTargetRefused::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_039_AcquireWorld();
		FM5_039_WorldScope Scope{ World };
		FM5_039_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}

		ACombatVehicle* Vehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(400.0, 0.0, 0.0), NAME_None, *this);
		if (Vehicle == nullptr)
		{
			return false;
		}
		const FCombatEpoch OldEpoch = Scene.Registry.GetCurrentEpoch();
		const FEntityId OldTargetId = Vehicle->GetEntityId();
		const FShotId OldShot = Scene.AllocateShot();
		TestTrue(TEXT("the first generation starts at a valid epoch"), IsValidCombatEpoch(OldEpoch));

		// World rebuild (leave room / retry): the old generation ends and every
		// record of it is wiped - the vehicle's registered identity included.
		const FCombatEpoch NewEpoch = Scene.Registry.BeginNextWorldEpoch();
		TestEqual(TEXT("the rebuild advanced the generation"), NewEpoch, OldEpoch + 1);
		TestFalse(TEXT("the old-generation target id is wiped"), Scene.Registry.IsKnownEntity(OldTargetId));

		// A request carrying the OLD epoch is refused by the ledger before
		// anything applies (the stale epoch outranks the unknown entities).
		FUnifiedHitRequest StaleRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeBulletRound(), OldShot);
		StaleRequest.Epoch = OldEpoch;
		StaleRequest.AttackerEntityId = Scene.AttackerId;
		StaleRequest.TargetEntityId = OldTargetId;
		StaleRequest.TargetActor = Vehicle;
		const FUnifiedHitOutcome StaleOutcome = ApplyUnifiedHit(StaleRequest, Scene.Ledger);
		TestTrue(TEXT("the stale-epoch request was blocked"), StaleOutcome.bWasBlocked);
		TestEqual(TEXT("the ledger refused the stale epoch"), StaleOutcome.RefuseReason, EUnifiedHitRefuseReason::LedgerRefused);
		TestEqual(TEXT("the ledger reason names StaleEpoch"), StaleOutcome.LedgerReason, EHitLedgerRejectReason::StaleEpoch);
		TestEqual(TEXT("the stale request applied zero damage"), StaleOutcome.DamageApplied, 0.0f);
		TestEqual(TEXT("the hull health is untouched by the stale request"), Vehicle->GetHealthComponent()->GetHealth(), 250.0f);

		// The same wiped target id under the CURRENT epoch (with a re-registered
		// attacker so the source face passes) is refused as UnknownTarget.
		FCombatEntityMetadata AttackerMetadata;
		AttackerMetadata.Faction = FName(TEXT("player"));
		AttackerMetadata.Category = FName(TEXT("attacker"));
		Scene.AttackerId = Scene.Registry.RegisterEntity(Scene.Attacker, AttackerMetadata, NewEpoch);
		TestTrue(TEXT("the attacker re-registered in the new generation"), IsValidCombatEntityId(Scene.AttackerId));
		FUnifiedHitRequest WipedTargetRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeBulletRound(), Scene.AllocateShot());
		WipedTargetRequest.TargetEntityId = OldTargetId;
		const FUnifiedHitOutcome WipedOutcome = ApplyUnifiedHit(WipedTargetRequest, Scene.Ledger);
		TestTrue(TEXT("the wiped-target request was blocked"), WipedOutcome.bWasBlocked);
		TestEqual(TEXT("the ledger refused the wiped target id"), WipedOutcome.LedgerReason, EHitLedgerRejectReason::UnknownTarget);
		TestEqual(TEXT("the wiped-target request applied zero damage"), WipedOutcome.DamageApplied, 0.0f);

		// The retry/leave-room face: the LIVING hull re-registers under the new
		// generation and keeps fighting - the old identity is dead, the hull
		// is not.
		const FEntityId ReMintedId = Vehicle->RegisterCombatEntity(Scene.Registry, NAME_None);
		TestTrue(TEXT("the hull re-minted its identity in the new generation"), IsValidCombatEntityId(ReMintedId));
		TestTrue(TEXT("the new identity is a live record"), Scene.Registry.IsKnownEntity(ReMintedId));
		const FUnifiedHitRequest RenewedRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeBulletRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome RenewedOutcome = ApplyUnifiedHit(RenewedRequest, Scene.Ledger);
		TestTrue(TEXT("the renewed identity took the hit"), RenewedOutcome.WasApplied());
		TestEqual(TEXT("the hull health dropped under the new generation"), Vehicle->GetHealthComponent()->GetHealth(), 244.0f);

		// The destroyed-target face: once the hull dies in the current
		// generation, every later hit is refused by the entry itself.
		const FUnifiedHitRequest LethalRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeLethalRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome LethalOutcome = ApplyUnifiedHit(LethalRequest, Scene.Ledger);
		TestTrue(TEXT("the lethal hit killed the hull"), LethalOutcome.bTargetDied);
		const FUnifiedHitRequest DeadTargetRequest = Scene.MakeVehicleHitRequest(*Vehicle, MakeBulletRound(), Scene.AllocateShot());
		const FUnifiedHitOutcome DeadOutcome = ApplyUnifiedHit(DeadTargetRequest, Scene.Ledger);
		TestTrue(TEXT("the hit on the dead hull was blocked"), DeadOutcome.bWasBlocked);
		TestEqual(TEXT("the dead hull refuses as TargetDead"), DeadOutcome.RefuseReason, EUnifiedHitRefuseReason::TargetDead);
		TestEqual(TEXT("the dead hull took no damage"), DeadOutcome.DamageApplied, 0.0f);
		return true;
	}

	// ------------------------------------------------------------------
	// 7. Structural: the hull's World state never reaches a save path
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_039HullStateStaysOutOfProfile,
		"UEMMO.Tasks.M5_039.HullStateStaysOutOfProfile",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_039HullStateStaysOutOfProfile::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_039_AcquireWorld();
		FM5_039_WorldScope Scope{ World };
		FM5_039_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}

		ACombatVehicle* Vehicle = Scene.SpawnVehicle(*World, M5_039_SceneBase + FVector(400.0, 0.0, 0.0), NAME_None, *this);
		if (Vehicle == nullptr)
		{
			return false;
		}

		// The ownership ladder (contract section 0.5): the hull is a World
		// actor (APawn), not a GameInstance/Profile value - vehicle instances
		// die with the World and nothing about them is persistent business
		// state (the M5-040 SeatModel precedent, now on the actor side).
		TestNotNull(TEXT("the hull is a World-ladder pawn body"), Cast<APawn>(Vehicle));

		// The World-transient state is deliberately NOT reflected: no property
		// of the class carries the definition, the entity identity or the
		// terminal flag, so no serialization/save path (Profile = GameInstance
		// level) can ever reach them.
		const TCHAR* BannedPropertyNames[] = {
			TEXT("Definition"), TEXT("bHasDefinition"), TEXT("EntityId"),
			TEXT("RegisteredEpoch"), TEXT("BoundRegistry"), TEXT("bDestroyFinalized"), TEXT("SeatVacate")
		};
		for (const TCHAR* PropertyName : BannedPropertyNames)
		{
			const FProperty* ReflectedProperty = Vehicle->GetClass()->FindPropertyByName(FName(PropertyName));
			TestFalse(FString::Printf(TEXT("the World state '%s' is not reflected (never serialized)"), PropertyName),
				ReflectedProperty != nullptr);
		}

		// The shared-pool shape: the hull reuses the existing components
		// instead of carrying any second combat stack.
		TestNotNull(TEXT("the hull owns the shared health component"), Vehicle->GetHealthComponent());
		TestNotNull(TEXT("the hull owns the shared combat component"), Vehicle->GetCombatComponent());
		TestNotNull(TEXT("the hull owns a query body"), Vehicle->GetRootComponent());
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
