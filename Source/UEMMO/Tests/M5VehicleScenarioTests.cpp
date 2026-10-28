// M5-044: the vehicle production scenario. The suites run the WHOLE
// production chain in a real ticked world - the HUD-less wiring owns a real
// FSeatOccupancyModel (the contract's World-wiring owner), the hull registers
// through the interaction component, the FIRST ENTER arms the production
// rigging (ground drive + turret weapon + mount from the real
// Data/CombatSystem and items.json sources), and every input after that flows
// through the production ROUTING faces (RouteDriveInput / RouteVehicleFire* /
// RouteVehicleReloadPressed) - never a component fixture call - while the
// damage rides the unified entry through the real projectile delivery. The
// observed event sequence exports as JSON evidence (the M5-018/035 export
// precedent) with the driver/vehicle/Shot/Damage/deregistration counts the
// card requires. The rendered capture companion stages the same chain in the
// real game world; headless runs skip silently.
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

#include "UObject/UObjectGlobals.h"

#include "../PrototypeCharacter.h"
#include "../Enemy/MeleeEnemy.h"

#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/HealthComponent.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/SeatModel.h"
#include "../Vehicles/VehicleCameraBridge.h"
#include "../Vehicles/VehicleInteractionComponent.h"
#include "../Vehicles/VehicleMovementComponent.h"
#include "../Vehicles/VehicleTypes.h"
#include "../Vehicles/VehicleWeaponMountComponent.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_044
{
	// Remote scene base so no other test's world content can collide (the
	// M5-039/041/042 placement convention). X lateral, Y depth, Z height; the
	// floor top sits exactly at the base Z.
	const FVector M5_044_SceneBase(120000.0, 96000.0, 0.0);

	// The shipped M5-037 sample rows as values (the M5-039/041 suite
	// precedent): the hull definition the spawner loads and the driver seat
	// the wiring registers. The production rigging loads the REAL tables at
	// the enter, so the hull id must match vehicles.json.
	static FVehicleDefinition M5_044_MakeShippedSampleVehicle()
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

	static FVehicleSeatDefinition M5_044_MakeShippedSampleSeat()
	{
		FVehicleSeatDefinition Seat;
		Seat.SeatId = FName(TEXT("seat_wheeled_driver"));
		Seat.VehicleId = FName(TEXT("vehicle_wheeled_sample"));
		Seat.SeatRole = EVehicleSeatRole::Driver;
		Seat.SeatLocationCm = FVector(0.0, 0.0, 40.0);
		Seat.ExitOffsetCm = FVector(250.0, 0.0, 0.0);
		return Seat;
	}

	// World-static blocking floor box (the M3-031 pattern verbatim).
	static AActor* M5_044_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_044_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(6000.0f, 6000.0f, 100.0f));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, 100.0));
		return Actor;
	}

	// Spawns the real prototype pawn above the floor with no-controller
	// physics enabled (the M3-031 pattern verbatim).
	static APrototypeCharacter* M5_044_SpawnPlayer(UWorld& World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Player == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
			if (!Movement->IsActive())
			{
				Movement->Activate(/*bReset*/ true);
			}
			if (Movement->MovementMode == MOVE_None)
			{
				Movement->SetDefaultMovementMode();
			}
		}
		return Player;
	}

	/**
	 * One production scene: the wrapped world + the real pawn + its own
	 * FSeatOccupancyModel (the contract's World-wiring owner) + one shipped
	 * hull registered through the interaction component. DECLARATION ORDER
	 * MATTERS (the M5-039/041 scene lesson): the seat model is declared
	 * BEFORE the world wrapper, so the reverse destruction tears the world
	 * down while the model is still addressable.
	 */
	struct FM5_044_Scene
	{
		FSeatOccupancyModel Seats;
		FTestWorldWrapper Wrapper;
		APrototypeCharacter* Player = nullptr;
		ACombatVehicle* Vehicle = nullptr;
		UVehicleInteractionComponent* Interaction = nullptr;

		bool Build(FAutomationTestBase& Test, float VehicleOffsetX = 400.0f)
		{
			if (!Test.TestTrue(TEXT("the test world is created (FTestWorldWrapper - the world-initialization lesson)"),
					Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("the test world began play with actors initialized"), Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			UWorld* World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world exists"), World))
			{
				return false;
			}

			M5_044_SpawnFloor(*World, M5_044_SceneBase);
			Player = M5_044_SpawnPlayer(*World, M5_044_SceneBase + FVector(250.0, 0.0, 160.0));
			if (!Test.TestNotNull(TEXT("the prototype pawn spawns"), Player))
			{
				return false;
			}
			for (int32 Frame = 0; Frame < 90; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}

			Interaction = Player->FindComponentByClass<UVehicleInteractionComponent>();
			if (!Test.TestNotNull(TEXT("the interaction component exists (BeginPlay instance component)"), Interaction))
			{
				return false;
			}
			// The World-wiring faces: the scene owns the seat model (the same
			// shape the production HUD entry owns) and binds it.
			Interaction->BindSeatModel(Seats);
			Interaction->SetOperatorFaction(FName(TEXT("player")));

			FActorSpawnParameters Params;
			Vehicle = World->SpawnActor<ACombatVehicle>(
				ACombatVehicle::StaticClass(), M5_044_SceneBase + FVector(VehicleOffsetX, 0.0, 96.0), FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the vehicle hull spawns"), Vehicle))
			{
				return false;
			}
			FString DefinitionErrors;
			if (!Test.TestTrue(TEXT("the hull accepts the shipped definition"),
					Vehicle->SetVehicleDefinition(M5_044_MakeShippedSampleVehicle(), &DefinitionErrors)))
			{
				Test.AddError(FString::Printf(TEXT("the shipped definition refused: %s"), *DefinitionErrors));
				return false;
			}
			FString RegisterError;
			if (!Test.TestTrue(TEXT("the hull registers into the interaction component"),
					Interaction->RegisterVehicle(*Vehicle, M5_044_MakeShippedSampleSeat(), &RegisterError)))
			{
				Test.AddError(FString::Printf(TEXT("the vehicle registration refused: %s"), *RegisterError));
				return false;
			}
			return true;
		}

		/** Manual world frames (the wrapper's own tick face). */
		void TickFrames(int32 Count)
		{
			for (int32 Frame = 0; Frame < Count; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}
		}
	};

	// The JSON evidence exporter (the M5-018/035 scenario precedent): one
	// events array with the observed chain and the counts block the card
	// requires (driver, vehicle, Shot, Damage, deregistration numbers).
	struct FM5_044_Exporter
	{
		TArray<FString> Events;
		int32 DriverEnters = 0;
		int32 DriverExits = 0;
		int32 ShotsCommitted = 0;
		int32 DamageEvents = 0;
		int32 VehicleDestroyed = 0;
		int32 Deregistrations = 0;

		void Add(const FString& Line)
		{
			Events.Add(Line);
		}

		void Export(FAutomationTestBase& Test, const TCHAR* FileName)
		{
			const FString Directory = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-044") / TEXT("scenario-json"));
			IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
			FString Json = TEXT("{\n  \"events\": [\n");
			for (int32 Index = 0; Index < Events.Num(); ++Index)
			{
				Json += TEXT("    ") + Events[Index] + (Index + 1 < Events.Num() ? TEXT(",\n") : TEXT("\n"));
			}
			Json += FString::Printf(TEXT("  ],\n  \"counts\": { \"driver_enters\": %d, \"driver_exits\": %d, ")
				TEXT("\"shots_committed\": %d, \"damage_events\": %d, \"vehicle_destroyed\": %d, \"deregistrations\": %d }\n}\n"),
				DriverEnters, DriverExits, ShotsCommitted, DamageEvents, VehicleDestroyed, Deregistrations);
			const FString Path = Directory / FileName;
			Test.TestTrue(TEXT("the scenario evidence writes"),
				FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		}
	};

	// ------------------------------------------------------------------
	// 1. The production chain: enter -> drive -> fire -> hit -> exit
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_044ProductionChainEnterDriveFireHitExit,
		"UEMMO.Tasks.M5_044.ProductionChainEnterDriveFireHitExit",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_044ProductionChainEnterDriveFireHitExit::RunTest(const FString& Parameters)
	{
		FM5_044_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}
		FM5_044_Exporter Exporter;

		// THE ENTER (production face): the first enter arms the rigging from
		// the real source tables.
		const FVehicleEnterAttempt Enter = Scene.Interaction->TryEnterNearestVehicle();
		TestTrue(TEXT("the production enter accepts"), Enter.bEntered);
		++Exporter.DriverEnters;
		Exporter.Add(FString::Printf(TEXT("{ \"type\": \"driver_enter\", \"vehicle\": \"%s\", \"seat\": \"%s\", \"operator\": \"player\" }"),
			*Enter.VehicleId.ToString(), *Enter.SeatId.ToString()));

		// The rigging: the drive exists and is configured from the shipped
		// row (idle in the riding model - movement is the rider's walk); the
		// turret is bound and configured; the fire source is the VEHICLE.
		UVehicleMovementComponent* Drive = Scene.Vehicle->FindComponentByClass<UVehicleMovementComponent>();
		TestNotNull(TEXT("the production drive exists after the enter"), Drive);
		TestTrue(TEXT("the drive is configured"), Drive != nullptr && Drive->IsConfigured());
		UVehicleWeaponMountComponent* Mount = Scene.Vehicle->FindComponentByClass<UVehicleWeaponMountComponent>();
		TestNotNull(TEXT("the production mount exists after the enter"), Mount);
		TestTrue(TEXT("the mount is configured"), Mount != nullptr && Mount->IsConfigured());
		TestTrue(TEXT("the vehicle fire source is registered"),
			Mount != nullptr && Mount->GetVehicleFireSourceEntityId() != InvalidCombatEntityId);
		UWeaponComponent* Turret = Mount != nullptr ? Mount->GetTurretWeapon() : nullptr;
		TestNotNull(TEXT("the turret weapon exists"), Turret);

		// M5-H03 audit: the HULL RIDES THE PAWN - attached as the pawn's
		// child, it follows every walk step while the rider keeps control.
		TestTrue(TEXT("the hull rides the pawn (attached child)"),
			Scene.Vehicle->GetRootComponent()->GetAttachParent() == Scene.Player->GetRootComponent());

		// THE RIDE (the production input chain, real world ticks): the rider
		// walks; the hull follows through the attachment (it snapped onto the
		// pawn at the enter, so at any instant the hull coincides with the
		// rider).
		Scene.Player->RecordPlanarMoveInput(1.0f, 0.0f);
		Scene.TickFrames(60);
		Scene.Player->RecordPlanarMoveInput(0.0f, 0.0f);
		Scene.TickFrames(90);
		const float WalkedDistance = Scene.Player->GetActorLocation().X - M5_044_SceneBase.X - 250.0f;
		AddInfo(FString::Printf(TEXT("M5_044 ride phase: walked=%f rider=%s hull=%s"),
			WalkedDistance,
			*Scene.Player->GetActorLocation().ToCompactString(),
			*Scene.Vehicle->GetActorLocation().ToCompactString()));
		TestTrue(TEXT("the rider walked forward while riding"), WalkedDistance >= 300.0f);
		// The hull rides the rider's X/Y through the attachment while its own
		// drive hover keeps its Z ground-locked (the car sits on the ground
		// around the person - the M5-H03 audit model).
		TestTrue(TEXT("the hull rides the rider on the plane (X/Y coincide)"),
			Scene.Vehicle->GetActorLocation().Equals(
				FVector(Scene.Player->GetActorLocation().X, Scene.Player->GetActorLocation().Y, Scene.Vehicle->GetActorLocation().Z), 1.0f));
		Exporter.Add(FString::Printf(TEXT("{ \"type\": \"ride_sample\", \"dx_cm\": %f }"), WalkedDistance));

		// THE TARGET + THE FIRE (production routing through the mount): the
		// shot attributes to the vehicle, the projectile flies and the damage
		// lands through the unified entry (the pool drop is the proof). The
		// target is a STATIC health box staged on the muzzle line (the rider
		// wears the car, so the muzzle rides at pawn + mount pivot; an AI
		// enemy would walk off the fixed flight line).
		const FVector MuzzleWorld = Scene.Vehicle->GetActorLocation()
			+ FVector(120.0, 40.0, 40.0);
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;
		{
			FActorSpawnParameters Params;
			Target = Scene.Wrapper.GetTestWorld()->SpawnActor<AActor>(AActor::StaticClass(),
				MuzzleWorld + FVector(1000.0, 0.0, 0.0), FRotator::ZeroRotator, Params);
			if (Target != nullptr)
			{
				UBoxComponent* Body = NewObject<UBoxComponent>(Target, TEXT("M5_044_Target"));
				Target->SetRootComponent(Body);
				Body->SetMobility(EComponentMobility::Movable);
				Body->SetBoxExtent(FVector(60.0f, 60.0f, 120.0f));
				Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
				Body->RegisterComponent();
				Body->SetWorldLocation(MuzzleWorld + FVector(1000.0, 0.0, 0.0));
				TargetHealth = NewObject<UHealthComponent>(Target, TEXT("M5_044_TargetHealth"));
				TargetHealth->RegisterComponent();
				TargetHealth->SetMaxHealth(100.0f);
				TargetHealth->ResetHealth();
			}
		}
		if (!TestNotNull(TEXT("the staging target spawns"), Target) ||
			!TestNotNull(TEXT("the staging target carries health"), TargetHealth))
		{
			return false;
		}
		// The collision tree is built before the projectile flight (the
		// M5-035 rig precedent - fresh worlds sweep incomplete trees).
		Scene.Wrapper.GetTestWorld()->EnsureCollisionTreeIsBuilt();
		const float TargetHealthBefore = TargetHealth->GetHealth();
		// THE FIRE: the production Q edge routes through the pawn's seated
		// gate into the turret mount (the same OnFireInputPressed the input
		// binding calls).
		Scene.Player->OnFireInputPressed();
		const bool bFired = Mount != nullptr && Mount->GetMountedShotCount() == 1;
		TestTrue(TEXT("the routed turret fire commits"), bFired);
		if (!bFired)
		{
			AddError(TEXT("the turret fire did not commit through the seated Q route"));
			return false;
		}
		++Exporter.ShotsCommitted;
		TestTrue(TEXT("the mount holds the shot attribution"), Mount->GetMountedShotCount() == 1);
		FShotContext Shot;
		TestTrue(TEXT("the committed shot snapshot resolves"), Turret != nullptr && Turret->GetLastShotContext(Shot));
		TestTrue(TEXT("the shot source is the vehicle fire lineage"),
			Shot.SourceEntityId == Mount->GetVehicleFireSourceEntityId());
		Exporter.Add(FString::Printf(TEXT("{ \"type\": \"shot_committed\", \"source_entity\": %llu, \"shot\": %llu }"),
			Shot.SourceEntityId, Shot.ShotId));
		Scene.Player->OnFireInputReleased();

		// The flight: the rocket flies toward the target (real world ticks);
		// the damage lands through the unified entry.
		bool bDamageLanded = false;
		float TargetHealthAfter = TargetHealthBefore;
		for (int32 Frame = 0; Frame < 240 && !bDamageLanded; ++Frame)
		{
			Scene.TickFrames(1);
			TargetHealthAfter = TargetHealth->GetHealth();
			if (TargetHealthAfter < TargetHealthBefore || !TargetHealth->IsAlive())
			{
				bDamageLanded = true;
			}
			if (Frame == 20 || Frame == 60)
			{
				int32 LiveRockets = 0;
				FVector RocketSpot = FVector::ZeroVector;
				for (TActorIterator<ACombatProjectile> It(Scene.Wrapper.GetTestWorld()); It; ++It)
				{
					++LiveRockets;
					RocketSpot = It->GetActorLocation();
				}
				AddInfo(FString::Printf(TEXT("M5_044 flight tick %d: liveRockets=%d rocket=%s"),
					Frame, LiveRockets, *RocketSpot.ToCompactString()));
			}
		}
		TestTrue(TEXT("the turret shot damaged the target through the unified entry"), bDamageLanded);
		{
			int32 LiveRockets = 0;
			FVector RocketSpot = FVector::ZeroVector;
			for (TActorIterator<ACombatProjectile> It(Scene.Wrapper.GetTestWorld()); It; ++It)
			{
				++LiveRockets;
				RocketSpot = It->GetActorLocation();
			}
			AddInfo(FString::Printf(TEXT("M5_044 fire phase: hpBefore=%f hpAfter=%f alive=%d liveRockets=%d rocket=%s target=%s rider=%s"),
				TargetHealthBefore, TargetHealthAfter,
				(TargetHealth != nullptr && TargetHealth->IsAlive()) ? 1 : 0,
				LiveRockets, *RocketSpot.ToCompactString(),
				*Target->GetActorLocation().ToCompactString(),
				*Scene.Player->GetActorLocation().ToCompactString()));
		}
		if (bDamageLanded)
		{
			++Exporter.DamageEvents;
			Exporter.Add(FString::Printf(TEXT("{ \"type\": \"damage_applied\", \"target\": \"%s\", \"from\": %f, \"to\": %f }"),
				*Target->GetName(), TargetHealthBefore, TargetHealthAfter));
		}

		// THE EXIT (production face): the hull detaches right beside the rider
		// (the rider never moved; there is no drop teleport).
		const FVector RiderSpotAtExit = Scene.Player->GetActorLocation();
		const FVector HullSpotAtExit = Scene.Vehicle->GetActorLocation();
		FString ExitRefusal;
		const bool bExited = Scene.Interaction->TryExitVehicle(&ExitRefusal);
		AddInfo(FString::Printf(TEXT("M5_044 exit: accepted=%d refusal=%s hull=%s pawn=%s"),
			bExited, *ExitRefusal, *HullSpotAtExit.ToCompactString(), *Scene.Player->GetActorLocation().ToCompactString()));
		TestTrue(TEXT("the production exit accepts"), bExited);
		++Exporter.DriverExits;
		Exporter.Add(TEXT("{ \"type\": \"driver_exit\" }"));
		TestTrue(TEXT("the hull detached beside the rider"),
			Scene.Vehicle->GetRootComponent()->GetAttachParent() == nullptr
			&& Scene.Vehicle->GetActorLocation().Equals(HullSpotAtExit, 0.5f)
			&& Scene.Player->GetActorLocation().Equals(RiderSpotAtExit, 0.5f));

		Exporter.Export(*this, TEXT("vehicle-scenario.json"));
		return true;
	}

	// ------------------------------------------------------------------
	// 2. The cleanup faces leave no residue (destroy / death / retry)
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_044CleanupFacesLeaveNoResidue,
		"UEMMO.Tasks.M5_044.CleanupFacesLeaveNoResidue",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_044CleanupFacesLeaveNoResidue::RunTest(const FString& Parameters)
	{
		FM5_044_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}
		FM5_044_Exporter Exporter;

		TestTrue(TEXT("the enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);
		++Exporter.DriverEnters;
		UVehicleWeaponMountComponent* Mount = Scene.Vehicle->FindComponentByClass<UVehicleWeaponMountComponent>();
		TestTrue(TEXT("the rigging exists while seated"),
			Scene.Vehicle->GetRootComponent()->GetAttachParent() == Scene.Player->GetRootComponent());

		// A fired shot at the destroy moment: the cleanup must leave no
		// seat/input/attack residue (contract section 8).
		Scene.Player->OnFireInputPressed();
		Scene.Player->OnFireInputReleased();

		// THE DESTROY: the hull's death sequence frees the rider.
		Scene.Vehicle->GetHealthComponent()->ApplyDamage(99999.0f);
		TestTrue(TEXT("the hull finalized its destruction"), Scene.Vehicle->IsDestroyFinalized());
		++Exporter.VehicleDestroyed;
		++Exporter.Deregistrations;
		Exporter.Add(FString::Printf(TEXT("{ \"type\": \"vehicle_destroyed\", \"vehicle\": \"%s\" }"),
			*Scene.Interaction->GetSeatedVehicleId().ToString()));
		TestEqual(TEXT("the destroy freed the seat"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the rider is free after the destroy"), Scene.Interaction->IsSeated());
		TestTrue(TEXT("the mount finalized (a destroyed vehicle never fires again)"),
			Mount != nullptr && Mount->IsFinalized());
		TestEqual(TEXT("exactly one destroy notification"), Scene.Interaction->GetVehicleDestroyedNotificationCount(), 1);
		TestFalse(TEXT("the pawn is no longer reported seated"), Scene.Interaction->IsSeated());

		// The death face: the driver dies inside the NEXT hull's seat - the
		// seat frees, the control face restores (no input residue).
		ACombatVehicle* HullB = nullptr;
		{
			FActorSpawnParameters Params;
			HullB = Scene.Wrapper.GetTestWorld()->SpawnActor<ACombatVehicle>(
				ACombatVehicle::StaticClass(), M5_044_SceneBase + FVector(100.0, 0.0, 96.0), FRotator::ZeroRotator, Params);
		}
		if (!TestNotNull(TEXT("the replacement hull spawns"), HullB))
		{
			return false;
		}
		FString DefinitionErrors;
		TestTrue(TEXT("the replacement hull accepts the shipped definition"),
			HullB->SetVehicleDefinition(M5_044_MakeShippedSampleVehicle(), &DefinitionErrors));
		FString RegisterError;
		TestTrue(TEXT("the replacement hull registers"),
			Scene.Interaction->RegisterVehicle(*HullB, M5_044_MakeShippedSampleSeat(), &RegisterError));
		const FVehicleEnterAttempt ReplaceEnter = Scene.Interaction->TryEnterNearestVehicle();
		AddInfo(FString::Printf(TEXT("M5_044 replace enter: accepted=%d reject=%d model=%d detail=%s pawn=%s hullB=%s"),
			ReplaceEnter.bEntered, static_cast<int32>(ReplaceEnter.Reject),
			static_cast<int32>(ReplaceEnter.ModelReject), *ReplaceEnter.Detail,
			*Scene.Player->GetActorLocation().ToCompactString(),
			HullB != nullptr ? *HullB->GetActorLocation().ToCompactString() : TEXT("null")));
		TestTrue(TEXT("the replacement hull accepts the driver"), ReplaceEnter.bEntered);
		++Exporter.DriverEnters;
		Scene.Player->GetHealth()->ApplyDamage(99999.0f);
		TestFalse(TEXT("the driver is dead"), Scene.Player->GetHealth()->IsAlive());
		TestEqual(TEXT("the death freed the seat"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the death restored the control face"), Scene.Interaction->IsSeated());
		Exporter.Add(TEXT("{ \"type\": \"driver_died_in_seat\" }"));

		// The retry face: the unified reset with no seat is the idempotent
		// no-op (the pawn cannot re-enter while dead - contract section 4).
		Scene.Interaction->NotifyOwnerReset();
		TestEqual(TEXT("the reset keeps the model empty"), Scene.Seats.GetNumOccupiedSeats(), 0);
		const FVehicleEnterAttempt DeadRefused = Scene.Interaction->TryEnterNearestVehicle();
		TestFalse(TEXT("the dead pawn cannot enter"), DeadRefused.bEntered);

		Exporter.Export(*this, TEXT("vehicle-cleanup.json"));
		return true;
	}

	// ------------------------------------------------------------------
	// 3. VehicleScenarioCapture (the rendered capture companion;
	// ClientContext only). Headless runs skip silently.
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_044VehicleScenarioCapture,
		"UEMMO.Tasks.M5_044.VehicleScenarioCapture",
		EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_044VehicleScenarioCapture::RunTest(const FString& Parameters)
	{
		UWorld* World = GWorld;
		APrototypeCharacter* GamePawn = World != nullptr
			? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0))
			: nullptr;
		if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
			|| World->WorldType != EWorldType::Game || GamePawn == nullptr)
		{
			AddInfo(TEXT("vehicle scenario capture skipped: no rendered game world with the prototype pawn"));
			return true;
		}

		// The production staging in the real game world: the seat model, the
		// shipped hull beside the pawn, the register.
		static FSeatOccupancyModel CaptureSeats;
		CaptureSeats = FSeatOccupancyModel();
		UVehicleInteractionComponent* Interaction = GamePawn->FindComponentByClass<UVehicleInteractionComponent>();
		ACombatVehicle* Hull = nullptr;
		if (Interaction != nullptr)
		{
			Interaction->BindSeatModel(CaptureSeats);
			Interaction->SetOperatorFaction(FName(TEXT("player")));
			// The boot map's menu screen would capture the enter (the M5-041
			// UIOnly face); the capture companion overrides the gate so the
			// bridged framing shows.
			Interaction->SetUICapturePredicate([]()
			{
				return false;
			});
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
			Hull = World->SpawnActor<ACombatVehicle>(
				ACombatVehicle::StaticClass(), GamePawn->GetActorLocation() + FVector(-250.0, 0.0, 96.0),
				FRotator::ZeroRotator, Params);
			if (Hull != nullptr)
			{
				FString DefinitionErrors;
				if (Hull->SetVehicleDefinition(UE::UEMMO::Tasks::M5_044::M5_044_MakeShippedSampleVehicle(), &DefinitionErrors))
				{
					FString RegisterError;
					if (!Interaction->RegisterVehicle(*Hull, UE::UEMMO::Tasks::M5_044::M5_044_MakeShippedSampleSeat(), &RegisterError))
					{
						AddInfo(FString::Printf(TEXT("the capture vehicle registration refused: %s"), *RegisterError));
					}
				}
				else
				{
					AddInfo(FString::Printf(TEXT("the capture vehicle definition refused: %s"), *DefinitionErrors));
				}
			}
		}
		if (!TestNotNull(TEXT("the capture hull stages"), Hull) || Interaction == nullptr)
		{
			return true;
		}

		const FString Directory = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-044") / TEXT("scenario-json") / TEXT("render"));
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);

		// Screenshot 1: the staged hull area from the character framing.
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("capture-vehicle.png"), true, false);
			return true;
		}));

		// The production enter + a short drive, then the bridged shot. The
		// latent steps return TRUE unconditionally - a false return means
		// "keep ticking", which would hang the queue on a refusal.
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Interaction]()
		{
			if (Interaction != nullptr)
			{
				const FVehicleEnterAttempt Attempt = Interaction->TryEnterNearestVehicle();
				UE_LOG(LogTemp, Display, TEXT("UEMMO M5-044 capture enter accepted=%d (%s)"),
					Attempt.bEntered, *Attempt.Detail);
			}
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("capture-driving.png"), true, false);
			return true;
		}));

		// Cleanup: the exit retires the driver side; the hull stays for the
		// user's own trial (the debug entry removes it with mode 2).
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Interaction]()
		{
			if (Interaction != nullptr)
			{
				Interaction->TryExitVehicle(nullptr);
			}
			return true;
		}));
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
