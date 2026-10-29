// M5-049: the table-driven second ground vehicle (vehicle_armored_test) -
// slower (400/600 vs 700/900), tougher (HP 500 / defense 25 vs 250/15) and
// mounting a different turret weapon (weapon_projectile_sample, the 12-round
// linear bullet, vs the 3-round rocket) - proven through the REAL production
// faces with ZERO production-code changes (the card's hash-frozen C++ rule):
//
//   1. CatalogMirrorsArmoredTables: the shared table loader (the same
//      LoadCombatDataDirectory the rigging runs per hull) resolves every new
//      row and its cross-table references verbatim.
//   2. ArmoredToughnessThroughUnifiedEntry: two live hulls built from the
//      REAL rows take the SAME unified-entry rounds - the shared damage
//      formula splits them exactly per table (round(29000/125)=232 leaves
//      the armored alive at 268 where the sample's 250 pool dies at 252),
//      and each hull owns its own health pool (no shared instance).
//   3. ArmoredMountArmsProjectileTurret: the production rigging (the enter
//      face's EnsureProductionRigging, armed per hull from the real tables)
//      mounts the armored row's own turret - the mounted row mirrors the
//      table (id/weapon/pitch clamp) and the fire book is per hull (one
//      round consumes 1 of the 12-round magazine, the sample's 3-round
//      rocket book untouched on its own hull).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitQuery.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Combat/System/UnifiedHitApplier.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/SeatModel.h"
#include "../Vehicles/VehicleInteractionComponent.h"
#include "../Vehicles/VehicleTypes.h"
#include "../Vehicles/VehicleWeaponMountComponent.h"
#include "../PrototypeCharacter.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_049
{
	const FVector M5_049_SceneBase = FVector(0.0, 0.0, 0.0);
	const FName M5_049_ArmoredId = FName(TEXT("vehicle_armored_test"));
	const FName M5_049_ArmoredSeatId = FName(TEXT("seat_armored_driver"));
	const FName M5_049_ArmoredMountId = FName(TEXT("mount_armored_turret"));
	const FName M5_049_SampleId = FName(TEXT("vehicle_wheeled_sample"));

	/**
	 * Loads the real production tables (the exact loader the production
	 * rigging runs per hull). Returns false with the loader problems reported
	 * when the directory refuses - the differentiation claims need the real
	 * source, never a fixture double.
	 */
	bool LoadRealTables(FAutomationTestBase& Test, FCombatDataTableSet& OutTables)
	{
		TArray<FString> Problems;
		if (!Test.TestTrue(TEXT("the real combat source tables load"),
			LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), OutTables, Problems)))
		{
			for (const FString& Problem : Problems)
			{
				Test.AddError(FString::Printf(TEXT("table loader: %s"), *Problem));
			}
			return false;
		}
		return true;
	}

	/** The floor plate under the scene (the M5-044 scene precedent). */
	static AActor* M5_049_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Floor = World.SpawnActor<AActor>(AActor::StaticClass(), Base + FVector(0.0, 0.0, -50.0), FRotator::ZeroRotator, Params);
		if (Floor != nullptr)
		{
			UBoxComponent* Slab = NewObject<UBoxComponent>(Floor, TEXT("M5_049_Floor"));
			Floor->SetRootComponent(Slab);
			Slab->SetMobility(EComponentMobility::Movable);
			Slab->SetBoxExtent(FVector(2000.0f, 2000.0f, 50.0f));
			Slab->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			Slab->RegisterComponent();
		}
		return Floor;
	}

	/**
	 * The wrapper-world scene for the hull suites: a real game world, the
	 * floor, the prototype pawn and the interaction component - the same
	 * shape the M5-044 scene builds (the hulls stage through the production
	 * definition/register faces fed from the REAL loaded tables).
	 */
	struct FM5_049_Scene
	{
		FSeatOccupancyModel Seats;
		FTestWorldWrapper Wrapper;
		APrototypeCharacter* Player = nullptr;
		UVehicleInteractionComponent* Interaction = nullptr;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the test world is created"),
					Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("the test world began play"), Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			UWorld* World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world exists"), World))
			{
				return false;
			}
			M5_049_SpawnFloor(*World, M5_049_SceneBase);
			{
				FActorSpawnParameters Params;
				Player = World->SpawnActor<APrototypeCharacter>(
					APrototypeCharacter::StaticClass(), M5_049_SceneBase + FVector(250.0, 0.0, 160.0), FRotator::ZeroRotator, Params);
			}
			if (!Test.TestNotNull(TEXT("the prototype pawn spawns"), Player))
			{
				return false;
			}
			for (int32 Frame = 0; Frame < 30; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}
			Interaction = Player->FindComponentByClass<UVehicleInteractionComponent>();
			if (!Test.TestNotNull(TEXT("the interaction component exists"), Interaction))
			{
				return false;
			}
			Interaction->BindSeatModel(Seats);
			Interaction->SetOperatorFaction(FName(TEXT("player")));
			return true;
		}

		void TickFrames(int32 Count)
		{
			for (int32 Frame = 0; Frame < Count; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}
		}

		/**
		 * Stages one hull from the REAL loaded rows (no fixture doubles): the
		 * definition and the seat registration come from the loaded tables.
		 */
		ACombatVehicle* StageHullFromTables(FAutomationTestBase& Test, UWorld& World,
			const FVehicleDefinition& VehicleRow, const FVehicleSeatDefinition& SeatRow, float OffsetX)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
			ACombatVehicle* Hull = World.SpawnActor<ACombatVehicle>(
				ACombatVehicle::StaticClass(), M5_049_SceneBase + FVector(OffsetX, 0.0, 96.0), FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the hull spawns"), Hull))
			{
				return nullptr;
			}
			FString DefinitionErrors;
			if (!Test.TestTrue(TEXT("the hull accepts the real table row"),
					Hull->SetVehicleDefinition(VehicleRow, &DefinitionErrors)))
			{
				Test.AddError(FString::Printf(TEXT("the table row refused: %s"), *DefinitionErrors));
				return nullptr;
			}
			FString RegisterError;
			if (!Test.TestTrue(TEXT("the hull registers into the interaction"),
					Interaction->RegisterVehicle(*Hull, SeatRow, &RegisterError)))
			{
				Test.AddError(FString::Printf(TEXT("the registration refused: %s"), *RegisterError));
				return nullptr;
			}
			return Hull;
		}
	};

	/**
	 * The bare damage world (the M5-039 acquisition pattern): a plain game
	 * world the damage scene drives through SetGameMode ->
	 * InitializeActorsForPlay -> BeginPlay exactly once.
	 */
	struct FM5_049_TestIdentity : IHitscanTargetIdentity
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

	static UWorld* M5_049_AcquireDamageWorld()
	{
		UGameInstance* TestGameInstance = NewObject<UGameInstance>(GEngine);
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_049_TestWorld")));
		if (TempWorld != nullptr)
		{
			TempWorld->SetShouldTick(false);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.OwningGameInstance = TestGameInstance;
			WorldContext.SetCurrentWorld(TempWorld);
			TempWorld->SetGameInstance(TestGameInstance);
			return TempWorld;
		}
		return GWorld;
	}

	/** Destroys the temp world again; the test owns exactly one. */
	struct FM5_049_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_049_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}
	};

	/**
	 * The lean damage scene (the M5-039 shape): a bare game world with the
	 * game mode initialized, the entity registry, the shot ledger and the
	 * target-identity recorder the unified entry needs.
	 */
	struct FM5_049_DamageScene
	{
		FCombatEntityRegistry Registry;
		FHitLedger Ledger{ &Registry };
		FM5_049_TestIdentity Identity;
		AActor* Attacker = nullptr;
		FEntityId AttackerId;

		bool Build(FAutomationTestBase& Test, UWorld& World)
		{
			FURL Url;
			World.SetGameMode(Url);
			if (!Test.TestTrue(TEXT("the damage world's game mode initialized"),
					World.GetAuthGameMode() != nullptr && World.GetWorldSettings() != nullptr))
			{
				return false;
			}
			World.InitializeActorsForPlay(Url);
			World.BeginPlay();

			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), M5_049_SceneBase + FVector(-100.0, 0.0, 0.0), FRotator::ZeroRotator);
			if (!Test.TestNotNull(TEXT("the attacker actor spawns"), Attacker))
			{
				return false;
			}
			FCombatEntityMetadata Metadata;
			Metadata.Faction = NAME_None;
			Metadata.Category = FName(TEXT("attacker"));
			AttackerId = Registry.RegisterEntity(Attacker, Metadata, Registry.GetCurrentEpoch());
			return Test.TestTrue(TEXT("the attacker entity registers"), IsValidCombatEntityId(AttackerId));
		}

		FShotId AllocateShot()
		{
			return Registry.AllocateActionSequence(AttackerId, Registry.GetCurrentEpoch());
		}

		/** Spawns + configures + registers one hull from the given REAL row. */
		ACombatVehicle* SpawnHull(FAutomationTestBase& Test, UWorld& World,
			const FVehicleDefinition& VehicleRow, float OffsetX)
		{
			ACombatVehicle* Hull = World.SpawnActor<ACombatVehicle>(
				ACombatVehicle::StaticClass(), M5_049_SceneBase + FVector(OffsetX, 0.0, 0.0), FRotator::ZeroRotator);
			if (!Test.TestNotNull(TEXT("the damage hull spawns"), Hull))
			{
				return nullptr;
			}
			FString Errors;
			if (!Test.TestTrue(TEXT("the damage hull accepts its real table row"),
					Hull->SetVehicleDefinition(VehicleRow, &Errors)))
			{
				Test.AddError(FString::Printf(TEXT("the table row refused: %s"), *Errors));
				return nullptr;
			}
			if (!IsValidCombatEntityId(Hull->RegisterCombatEntity(Registry, NAME_None)))
			{
				Test.AddError(TEXT("the damage hull entity could not register"));
				return nullptr;
			}
			Identity.Record(*Hull, Hull->GetEntityId());
			return Hull;
		}

		/** The unified request shape the M5-026 executor builds. */
		FUnifiedHitRequest MakeHitRequest(ACombatVehicle& Vehicle, const FDamageProfile& Attack, FShotId ShotId) const
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
	};

	/** One damage profile row (the bullet round face with a set base). */
	static FDamageProfile M5_049_MakeRound(float BaseDamage)
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = FName(*FString::Printf(TEXT("m5_049_round_%d"), static_cast<int32>(BaseDamage)));
		Profile.BaseDamage = BaseDamage;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.0f;
		Profile.KnockbackCmPerSecond = 0.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.0f;
		return Profile;
	}
}

// Suite 1: the real loader resolves every new row and its cross-table
// references verbatim (the data the rigging and the mounts consume).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_049CatalogMirrorsArmoredTables,
	"UEMMO.Tasks.M5_049.CatalogMirrorsArmoredTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_049CatalogMirrorsArmoredTables::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M5_049;
	FCombatDataTableSet Tables;
	if (!LoadRealTables(*this, Tables))
	{
		return true;
	}

	// The vehicle row mirrors the table values (slower + tougher than the
	// shipped sample).
	const FVehicleDefinition* Armored = Tables.Vehicles.Find(M5_049_ArmoredId);
	if (!TestNotNull(TEXT("the armored vehicle row resolves"), Armored))
	{
		return true;
	}
	TestTrue(TEXT("the armored row is a ground vehicle"), Armored->DriveType == EVehicleDriveType::Ground);
	TestEqual(TEXT("the armored max health is the table 500"), Armored->MaxHealth, 500.0f);
	TestEqual(TEXT("the armored defense is the table 25"), Armored->Defense, 25.0f);
	TestEqual(TEXT("the armored top speed is the table 400"), Armored->MaxSpeedCmS, 400.0f);
	TestEqual(TEXT("the armored acceleration is the table 600"), Armored->AccelerationCmS2, 600.0f);
	const FVehicleDefinition* Sample = Tables.Vehicles.Find(M5_049_SampleId);
	if (TestNotNull(TEXT("the shipped sample row still resolves"), Sample))
	{
		TestTrue(TEXT("the armored is slower than the sample"),
			Armored->MaxSpeedCmS < Sample->MaxSpeedCmS && Armored->AccelerationCmS2 < Sample->AccelerationCmS2);
		TestTrue(TEXT("the armored is tougher than the sample"),
			Armored->MaxHealth > Sample->MaxHealth && Armored->Defense > Sample->Defense);
	}

	// The seat row: one driver seat for the armored (the cross-table gate
	// refuses a vehicle without one).
	const FVehicleSeatDefinition* Seat = nullptr;
	for (const TPair<FName, FVehicleSeatDefinition>& Row : Tables.VehicleSeats)
	{
		if (Row.Value.VehicleId == M5_049_ArmoredId)
		{
			Seat = &Row.Value;
			break;
		}
	}
	if (!TestNotNull(TEXT("the armored seat row resolves"), Seat))
	{
		return true;
	}
	TestEqual(TEXT("the armored seat is the table seat id"), Seat->SeatId, M5_049_ArmoredSeatId);

	// The mount row: the armored carries ITS OWN turret weapon (the linear
	// 12-round bullet, not the sample's 3-round rocket) and its own pitch
	// clamp.
	const FVehicleMountDefinition* Mount = nullptr;
	for (const TPair<FName, FVehicleMountDefinition>& Row : Tables.VehicleMounts)
	{
		if (Row.Value.VehicleId == M5_049_ArmoredId)
		{
			Mount = &Row.Value;
			break;
		}
	}
	if (!TestNotNull(TEXT("the armored mount row resolves"), Mount))
	{
		return true;
	}
	TestEqual(TEXT("the armored mount is the table mount id"), Mount->MountId, M5_049_ArmoredMountId);
	TestEqual(TEXT("the armored mount carries the projectile weapon"),
		Mount->TurretWeaponId, FName(TEXT("weapon_projectile_sample")));
	TestEqual(TEXT("the armored pitch clamp is the table -10 degrees"), Mount->MinPitchDegrees, -10.0f);
	TestEqual(TEXT("the armored pitch clamp is the table 45 degrees"), Mount->MaxPitchDegrees, 45.0f);

	// The cross-table weapon references: both mount weapons resolve with
	// their own magazine/ammo identities (the differentiation the fire books
	// later show).
	const FWeaponDefinition* ProjectileWeapon = Tables.Weapons.Find(Mount->TurretWeaponId);
	if (TestNotNull(TEXT("the armored turret weapon resolves"), ProjectileWeapon))
	{
		TestEqual(TEXT("the projectile weapon holds the 12-round magazine"),
			ProjectileWeapon->MagazineSize, 12);
	}
	const FVehicleMountDefinition* SampleMount = nullptr;
	for (const TPair<FName, FVehicleMountDefinition>& Row : Tables.VehicleMounts)
	{
		if (Row.Value.VehicleId == M5_049_SampleId)
		{
			SampleMount = &Row.Value;
			break;
		}
	}
	if (TestNotNull(TEXT("the sample mount row still resolves"), SampleMount))
	{
		const FWeaponDefinition* ExplosiveWeapon = Tables.Weapons.Find(SampleMount->TurretWeaponId);
		if (TestNotNull(TEXT("the sample turret weapon resolves"), ExplosiveWeapon))
		{
			TestTrue(TEXT("the two mounts carry different weapons"),
				SampleMount->TurretWeaponId != Mount->TurretWeaponId
				&& ExplosiveWeapon->MagazineSize != ProjectileWeapon->MagazineSize);
		}
	}
	return true;
}

// Suite 2: two live hulls from the REAL rows take the same unified-entry
// rounds - the shared formula splits them per table and the pools never
// cross.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_049ArmoredToughnessThroughUnifiedEntry,
	"UEMMO.Tasks.M5_049.ArmoredToughnessThroughUnifiedEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_049ArmoredToughnessThroughUnifiedEntry::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M5_049;
	FCombatDataTableSet Tables;
	if (!LoadRealTables(*this, Tables))
	{
		return true;
	}
	const FVehicleDefinition* ArmoredRow = Tables.Vehicles.Find(M5_049_ArmoredId);
	const FVehicleDefinition* SampleRow = Tables.Vehicles.Find(M5_049_SampleId);
	if (!TestNotNull(TEXT("the armored row resolves"), ArmoredRow)
		|| !TestNotNull(TEXT("the sample row resolves"), SampleRow))
	{
		return true;
	}

	FM5_049_DamageScene Scene;
	UWorld* DamageWorld = M5_049_AcquireDamageWorld();
	FM5_049_WorldScope Scope{ DamageWorld };
	if (!TestNotNull(TEXT("the damage world exists"), DamageWorld))
	{
		return true;
	}
	M5_049_SpawnFloor(*DamageWorld, M5_049_SceneBase);
	if (!Scene.Build(*this, *DamageWorld))
	{
		return true;
	}

	ACombatVehicle* SampleHull = Scene.SpawnHull(*this, *DamageWorld, *SampleRow, 400.0f);
	ACombatVehicle* ArmoredHull = Scene.SpawnHull(*this, *DamageWorld, *ArmoredRow, 900.0f);
	if (SampleHull == nullptr || ArmoredHull == nullptr)
	{
		return true;
	}

	// Each hull owns its own pool at its own configured max (no shared
	// instance, no borrowed seat/HP state).
	TestEqual(TEXT("the sample hull starts at the table 250"),
		SampleHull->GetHealthComponent()->GetHealth(), 250.0f);
	TestEqual(TEXT("the armored hull starts at the table 500"),
		ArmoredHull->GetHealthComponent()->GetHealth(), 500.0f);
	TestEqual(TEXT("the sample hull carries the table defense 15"),
		SampleHull->GetCombatComponent()->GetDefense(), 15.0f);
	TestEqual(TEXT("the armored hull carries the table defense 25"),
		ArmoredHull->GetCombatComponent()->GetDefense(), 25.0f);

	// The SAME lethal-to-sample round through the unified entry: the sample
	// pool dies (round(29000/115)=252 >= 250) while the armored survives
	// (round(29000/125)=232 < 500) at exactly 500-232=268.
	const FDamageProfile Lethal = M5_049_MakeRound(290.0f);
	const FUnifiedHitOutcome SampleOutcome = ApplyUnifiedHit(
		Scene.MakeHitRequest(*SampleHull, Lethal, Scene.AllocateShot()), Scene.Ledger);
	AddInfo(FString::Printf(TEXT("M5_049 sample hit diagnose: decision=%d ledger=%d damageBlock=%d blocked=%d immune=%d"),
		static_cast<int32>(SampleOutcome.Decision), static_cast<int32>(SampleOutcome.LedgerReason),
		static_cast<int32>(SampleOutcome.DamageBlockReason), SampleOutcome.bWasBlocked ? 1 : 0, SampleOutcome.bWasImmune ? 1 : 0));
	TestTrue(TEXT("the lethal round applied to the sample hull"), SampleOutcome.WasApplied());
	TestEqual(TEXT("the sample hull died to the lethal round"),
		SampleHull->GetHealthComponent()->GetHealth(), 0.0f);
	const FUnifiedHitOutcome ArmoredOutcome = ApplyUnifiedHit(
		Scene.MakeHitRequest(*ArmoredHull, Lethal, Scene.AllocateShot()), Scene.Ledger);
	TestTrue(TEXT("the lethal round applied to the armored hull"), ArmoredOutcome.WasApplied());
	TestEqual(TEXT("the armored hull survives at the formula 268"),
		ArmoredHull->GetHealthComponent()->GetHealth(), 268.0f);

	// A smaller round splits the two live pools per the defense column:
	// round(15000/115)=130 vs round(15000/125)=120 (on a fresh pair).
	ACombatVehicle* SampleTwo = Scene.SpawnHull(*this, *DamageWorld, *SampleRow, 1400.0f);
	ACombatVehicle* ArmoredTwo = Scene.SpawnHull(*this, *DamageWorld, *ArmoredRow, 1900.0f);
	if (SampleTwo != nullptr && ArmoredTwo != nullptr)
	{
		const FDamageProfile Medium = M5_049_MakeRound(150.0f);
		ApplyUnifiedHit(Scene.MakeHitRequest(*SampleTwo, Medium, Scene.AllocateShot()), Scene.Ledger);
		ApplyUnifiedHit(Scene.MakeHitRequest(*ArmoredTwo, Medium, Scene.AllocateShot()), Scene.Ledger);
		TestEqual(TEXT("the sample hull loses the formula 130"),
			SampleTwo->GetHealthComponent()->GetHealth(), 120.0f);
		TestEqual(TEXT("the armored hull loses the formula 120"),
			ArmoredTwo->GetHealthComponent()->GetHealth(), 380.0f);
	}
	return true;
}

// Suite 3: the production rigging arms the armored hull's OWN mount from the
// real tables - the mounted row mirrors the table and the fire book is per
// hull.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_049ArmoredMountArmsProjectileTurret,
	"UEMMO.Tasks.M5_049.ArmoredMountArmsProjectileTurret",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_049ArmoredMountArmsProjectileTurret::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M5_049;
	FCombatDataTableSet Tables;
	if (!LoadRealTables(*this, Tables))
	{
		return true;
	}
	const FVehicleDefinition* ArmoredRow = Tables.Vehicles.Find(M5_049_ArmoredId);
	const FVehicleSeatDefinition* ArmoredSeat = nullptr;
	for (const TPair<FName, FVehicleSeatDefinition>& Row : Tables.VehicleSeats)
	{
		if (Row.Value.VehicleId == M5_049_ArmoredId)
		{
			ArmoredSeat = &Row.Value;
			break;
		}
	}
	const FVehicleDefinition* SampleRow = Tables.Vehicles.Find(M5_049_SampleId);
	const FVehicleSeatDefinition* SampleSeat = nullptr;
	for (const TPair<FName, FVehicleSeatDefinition>& Row : Tables.VehicleSeats)
	{
		if (Row.Value.VehicleId == M5_049_SampleId)
		{
			SampleSeat = &Row.Value;
			break;
		}
	}
	if (!TestNotNull(TEXT("the armored rows resolve"), ArmoredRow) || !TestNotNull(TEXT("the armored seat resolves"), ArmoredSeat)
		|| !TestNotNull(TEXT("the sample rows resolve"), SampleRow) || !TestNotNull(TEXT("the sample seat resolves"), SampleSeat))
	{
		return true;
	}

	FM5_049_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	UWorld* World = Scene.Wrapper.GetTestWorld();
	// The sample stages FAR (outside the 300cm enter range) so the nearest
	// candidate resolution reaches only the armored hull; its un-rigged state
	// is the no-shared-book control.
	ACombatVehicle* SampleHull = Scene.StageHullFromTables(*this, *World, *SampleRow, *SampleSeat, 1400.0f);
	ACombatVehicle* ArmoredHull = Scene.StageHullFromTables(*this, *World, *ArmoredRow, *ArmoredSeat, 400.0f);
	if (SampleHull == nullptr || ArmoredHull == nullptr)
	{
		return true;
	}
	// Before any enter the hulls carry no armed mount (the rigging runs once
	// per hull on the production enter face).
	TestNull(TEXT("the un-rigged sample hull holds no mount"),
		SampleHull->FindComponentByClass<UVehicleWeaponMountComponent>());

	// THE PRODUCTION ENTER: the riding-model enter accepts the nearest
	// candidate (the armored, 150cm ahead) and arms its production rigging
	// from the real tables.
	const FVehicleEnterAttempt Enter = Scene.Interaction->TryEnterNearestVehicle();
	TestTrue(TEXT("the production enter accepts the armored hull"), Enter.bEntered);
	TestEqual(TEXT("the enter locked onto the armored vehicle"), Enter.VehicleId, M5_049_ArmoredId);
	UVehicleWeaponMountComponent* ArmoredMount = ArmoredHull->FindComponentByClass<UVehicleWeaponMountComponent>();
	if (!TestNotNull(TEXT("the armored mount armed on the enter"), ArmoredMount))
	{
		return true;
	}
	TestEqual(TEXT("the armored mount mirrors its row id"),
		ArmoredMount->GetMountedDefinition().MountId, M5_049_ArmoredMountId);
	TestEqual(TEXT("the armored mount carries the projectile weapon"),
		ArmoredMount->GetMountedDefinition().TurretWeaponId, FName(TEXT("weapon_projectile_sample")));
	TestEqual(TEXT("the armored pitch clamp mirrors the table -10"),
		ArmoredMount->GetMountedDefinition().MinPitchDegrees, -10.0f);
	TestEqual(TEXT("the armored pitch clamp mirrors the table 45"),
		ArmoredMount->GetMountedDefinition().MaxPitchDegrees, 45.0f);
	UVehicleWeaponMountComponent* SampleMount = SampleHull->FindComponentByClass<UVehicleWeaponMountComponent>();
	TestNull(TEXT("the far sample hull stays un-rigged (no shared book)"), SampleMount);

	// The fire book is the armored hull's own: one routed round consumes
	// exactly one round of the 12-round projectile magazine.
	UWeaponComponent* ArmoredTurret = ArmoredMount->GetTurretWeapon();
	if (!TestNotNull(TEXT("the armored turret weapon exists"), ArmoredTurret)
		|| !TestNotNull(TEXT("the armored turret holds an active binding"), ArmoredTurret->GetActiveBinding()))
	{
		return true;
	}
	TestEqual(TEXT("the projectile magazine capacity is the table 12"),
		ArmoredTurret->GetActiveBinding()->MagazineCapacity, 12);
	const int32 RoundsBefore = ArmoredTurret->GetActiveBinding()->LoadedRounds;
	FString FireRefusal;
	TestTrue(TEXT("the seated rider routes one fire round"),
		Scene.Interaction->RouteVehicleFirePressed(&FireRefusal));
	Scene.TickFrames(5);
	const int32 RoundsAfter = ArmoredTurret->GetActiveBinding()->LoadedRounds;
	TestEqual(TEXT("one routed round consumed exactly one projectile round"),
		RoundsBefore - RoundsAfter, 1);

	// The exit detaches cleanly (the riding-model exit face).
	TestTrue(TEXT("the production exit accepts"), Scene.Interaction->TryExitVehicle());
	return true;
}

#endif
