// M5-042: the vehicle ground drive and the driver-seat camera bridge tests.
// The suite drives the REAL ACombatVehicle hull (the M5-039 query box) and the
// REAL APrototypeCharacter side-view rig inside a real test world and pins:
//   1. Configure loads the hull definition's speed/acceleration (brake defaults
//      to the acceleration) and refuses an unloaded hull; the brake override
//      rejects non-finite/non-positive values and a re-Configure resets it;
//   2. the drive authority gates: no driver / suspended controls / a dead hull
//      refuse SetDriveInput AND clear any held throttle (a driver lost
//      mid-drive coasts down, never drives on stale input); the component's
//      own tick forwards into the same TickMovement entry the tests step;
//   3. straight X and depth Y displacement match the configured
//      acceleration/top speed (Y is a first-class axis - no plane lock), the
//      released input brakes to an exact stop and never keeps moving;
//   4. diagonal input is normalized (the M1-029 precedent): one-frame speed
//      equals the straight one and the whole-vector clamp keeps the diagonal
//      top speed at the straight top speed (no per-axis boost);
//   5. a blocking wall stops the hull at the face with zero penetration and
//      zero creep while the throttle is held;
//   6. the camera bridge re-parents the rig onto the hull ground anchor with
//      the definition's follow-camera values, the rig follows the hull through
//      the engine's transform propagation, and the restore hands the rig back
//      to the character anchor (the rig's own M1-030 pure math) with the saved
//      framing and tick state;
//   7. the bridge's command refusals never touch the rig (no rig, no
//      definition), the duplicated bridge is the accepted no-op and a hull
//      switch restores the previous anchor first;
//   8. the hull's one-shot destroy broadcast and the bridge's own EndPlay
//      self-restore the camera even without a wiring call.
//
// World shape (the M5-039/041 lesson, reused): a raw UWorld::CreateWorld world
// never dispatches BeginPlay and never routes EndPlay on Destroy, so every
// scene uses the engine FTestWorldWrapper (GameInstance + world context +
// SetGameMode + InitializeActorsForPlay + BeginPlay, manually ticked).
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealClient.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

#include <limits>

#include "../Character/SideViewCameraComponent.h"
#include "../Combat/HealthComponent.h"
#include "../PrototypeCharacter.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/VehicleCameraBridge.h"
#include "../Vehicles/VehicleMovementComponent.h"
#include "../Vehicles/VehicleTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_042
{
	// Remote scene base so no other test's world content can collide (the
	// M5-039/041 placement convention). X lateral, Y depth, Z height; the
	// floor top sits exactly at the base Z.
	const FVector M5_042_SceneBase(86000.0, 76000.0, 0.0);

	// The shipped M5-037 sample (vehicles.json): the real production kind, not
	// test slang (the 041 suite precedent). The drive tuning is this card's
	// subject: max 700 cm/s, acceleration 900 cm/s^2 (the brake defaults to it).
	static FVehicleDefinition M5_042_MakeShippedSampleVehicle()
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

	// World-static blocking floor box (the M3-031 pattern verbatim): half
	// thickness 100 cm, top face exactly at the base.
	static AActor* M5_042_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_042_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(4000.0f, 4000.0f, 100.0f));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-022 pattern).
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, 100.0));
		return Actor;
	}

	// A BlockAll box wall (the drive boundary): a tall slab so the 80cm-tall
	// hull cannot sweep over or slide around it inside the test corridor.
	static AActor* M5_042_SpawnWall(UWorld& World, const FVector& Center, const FVector& HalfExtent)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_042_Wall"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(HalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(Center);
		return Actor;
	}

	// Spawns + configures the hull (no registry: the M5-042 subject is the
	// drive and the camera, not the hittable identity the M5-039 suite pins).
	static ACombatVehicle* M5_042_SpawnVehicle(UWorld& World, const FVector& Location,
		const FVehicleDefinition* Definition)
	{
		FActorSpawnParameters Params;
		ACombatVehicle* Vehicle = World.SpawnActor<ACombatVehicle>(
			ACombatVehicle::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Vehicle == nullptr)
		{
			return nullptr;
		}
		if (Definition != nullptr)
		{
			FString Errors;
			if (!Vehicle->SetVehicleDefinition(*Definition, &Errors))
			{
				UE_LOG(LogTemp, Error, TEXT("M5_042: the vehicle definition was refused: %s"), *Errors);
				return nullptr;
			}
		}
		return Vehicle;
	}

	// Spawns the real prototype pawn above the floor with no-controller
	// physics enabled (the M3-031 pattern verbatim).
	static APrototypeCharacter* M5_042_SpawnPlayer(UWorld& World, const FVector& Location)
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
	 * Attaches the drive component onto the hull and pins it to manual
	 * stepping: the component's own tick exists (and is pinned forwarding in
	 * the authority test), but every displacement test steps TickMovement
	 * itself, so the wrapper's world ticks must not double-step the drive.
	 */
	static UVehicleMovementComponent* M5_042_AttachMovement(ACombatVehicle& Vehicle)
	{
		UVehicleMovementComponent* Movement = NewObject<UVehicleMovementComponent>(&Vehicle, TEXT("M5_042_Movement"));
		Movement->RegisterComponent();
		Movement->SetComponentTickEnabled(false);
		return Movement;
	}

	// ------------------------------------------------------------------
	// Drive scene: the wrapped world + one hull + its attached drive.
	// DECLARATION ORDER MATTERS (the M5-039/041 scene lesson): the wrapper is
	// declared before the hull pointer so the reverse destruction tears the
	// world down while the test's raw pointers are still plain values.
	// ------------------------------------------------------------------
	struct FM5_042_DriveScene
	{
		FTestWorldWrapper Wrapper;
		FVehicleDefinition VehicleDefinition;
		ACombatVehicle* Vehicle = nullptr;
		UVehicleMovementComponent* Movement = nullptr;

		// bLoadDefinition=false spawns the bare hull (the Configure refusal).
		bool Build(FAutomationTestBase& Test, bool bLoadDefinition = true)
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

			M5_042_SpawnFloor(*World, M5_042_SceneBase);
			VehicleDefinition = M5_042_MakeShippedSampleVehicle();
			Vehicle = M5_042_SpawnVehicle(*World,
				M5_042_SceneBase + FVector(400.0, 0.0, 96.0), bLoadDefinition ? &VehicleDefinition : nullptr);
			if (!Test.TestNotNull(TEXT("the vehicle hull spawns"), Vehicle))
			{
				return false;
			}
			Movement = M5_042_AttachMovement(*Vehicle);
			if (!Test.TestNotNull(TEXT("the movement component attaches"), Movement))
			{
				return false;
			}
			// The drive tuning loads from the hull's definition through
			// Configure (the wiring form); the bare-hull scene skips it (the
			// Configure test drives that refusal itself).
			if (bLoadDefinition)
			{
				FString ConfigureError;
				if (!Test.TestTrue(TEXT("the drive configures from the hull's loaded definition"),
						Movement->Configure(&ConfigureError)))
				{
					Test.AddError(FString::Printf(TEXT("the drive configuration refused: %s"), *ConfigureError));
					return false;
				}
			}
			return true;
		}

		/** Deterministic manual stepping of the drive (1/60 per step). */
		void StepMovement(int32 Count)
		{
			for (int32 Step = 0; Step < Count; ++Step)
			{
				Movement->TickMovement(1.0f / 60.0f);
			}
		}

		/** Drops the hull onto the floor and asserts the hover hold. */
		bool Settle(FAutomationTestBase& Test)
		{
			StepMovement(30);
			if (!Test.TestTrue(TEXT("the hull grounds on the swept floor contact"), Movement->IsGrounded()))
			{
				return false;
			}
			// The hull box half extent is 80 cm and the hover clearance is the
			// implementation's 1 cm: the settled origin rests at base Z + 81.
			const float OriginZ = Vehicle->GetActorLocation().Z - M5_042_SceneBase.Z;
			return Test.TestTrue(TEXT("the hull rests at the floor contact plus the working hover gap"),
				FMath::IsNearlyEqual(OriginZ, 81.0f, 0.5f));
		}
	};

	// ------------------------------------------------------------------
	// Camera scene: the wrapped world + the real pawn (the rig owner) + two
	// defined hulls and one bare hull for the refusal faces.
	// ------------------------------------------------------------------
	struct FM5_042_CameraScene
	{
		FTestWorldWrapper Wrapper;
		FVehicleDefinition VehicleDefinition;
		APrototypeCharacter* Player = nullptr;
		ACombatVehicle* Vehicle = nullptr;
		ACombatVehicle* VehicleB = nullptr;
		ACombatVehicle* BareVehicle = nullptr;
		USideViewCameraComponent* Rig = nullptr;
		UVehicleCameraBridge* Bridge = nullptr;

		bool Build(FAutomationTestBase& Test)
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

			M5_042_SpawnFloor(*World, M5_042_SceneBase);
			VehicleDefinition = M5_042_MakeShippedSampleVehicle();

			Player = M5_042_SpawnPlayer(*World, M5_042_SceneBase + FVector(250.0, 0.0, 160.0));
			if (!Test.TestNotNull(TEXT("the prototype pawn spawns"), Player))
			{
				return false;
			}
			Vehicle = M5_042_SpawnVehicle(*World, M5_042_SceneBase + FVector(400.0, 0.0, 96.0), &VehicleDefinition);
			VehicleB = M5_042_SpawnVehicle(*World, M5_042_SceneBase + FVector(1400.0, 600.0, 96.0), &VehicleDefinition);
			BareVehicle = M5_042_SpawnVehicle(*World, M5_042_SceneBase + FVector(-1200.0, 0.0, 96.0), nullptr);
			if (!Test.TestNotNull(TEXT("the vehicle hull spawns"), Vehicle) ||
				!Test.TestNotNull(TEXT("the second vehicle hull spawns"), VehicleB) ||
				!Test.TestNotNull(TEXT("the bare hull spawns"), BareVehicle))
			{
				return false;
			}

			// The settle phase: the pawn drops the 64 cm to the floor top and
			// rests with the capsule bottom on it (capsule center at Z=96).
			for (int32 Frame = 0; Frame < 90; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}

			Rig = Player->FindComponentByClass<USideViewCameraComponent>();
			if (!Test.TestNotNull(TEXT("the pawn carries the side-view rig (the M1-030 component)"), Rig))
			{
				return false;
			}
			Bridge = NewObject<UVehicleCameraBridge>(Player, TEXT("M5_042_CameraBridge"));
			Bridge->RegisterComponent();
			if (!Test.TestNotNull(TEXT("the camera bridge attaches"), Bridge))
			{
				return false;
			}
			return true;
		}
	};

	// ------------------------------------------------------------------
	// 1. Configure: definition-sourced tuning + brake override faces
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042ConfigureAndBrakeTuningFromDefinition,
		"UEMMO.Tasks.M5_042.ConfigureAndBrakeTuningFromDefinition",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042ConfigureAndBrakeTuningFromDefinition::RunTest(const FString& Parameters)
	{
		FM5_042_DriveScene Scene;
		if (!Scene.Build(*this, /*bLoadDefinition*/ false))
		{
			return false;
		}

		// The bare hull: no definition, no tuning - the drive refuses to fake
		// configuration and reports the reason.
		TestFalse(TEXT("the unconfigured hull reports unconfigured"), Scene.Movement->IsConfigured());
		FString ConfigureError;
		TestFalse(TEXT("the Configure without a definition refuses"), Scene.Movement->Configure(&ConfigureError));
		TestFalse(TEXT("the refusal names the missing definition"), ConfigureError.IsEmpty());
		TestEqual(TEXT("no top speed was invented"), Scene.Movement->GetConfiguredMaxSpeedCmS(), 0.0f);
		TestEqual(TEXT("no acceleration was invented"), Scene.Movement->GetConfiguredAccelerationCmS2(), 0.0f);

		// Loading the shipped definition opens the drive with its exact tuning.
		FString LoadError;
		TestTrue(TEXT("the hull accepts the shipped definition"),
			Scene.Vehicle->SetVehicleDefinition(Scene.VehicleDefinition, &LoadError));
		FString SecondError;
		TestTrue(TEXT("the Configure with a loaded definition accepts"), Scene.Movement->Configure(&SecondError));
		TestTrue(TEXT("the drive reports configured"), Scene.Movement->IsConfigured());
		TestTrue(TEXT("the top speed is the definition's MaxSpeedCmS"),
			FMath::IsNearlyEqual(Scene.Movement->GetConfiguredMaxSpeedCmS(), 700.0f));
		TestTrue(TEXT("the acceleration is the definition's AccelerationCmS2"),
			FMath::IsNearlyEqual(Scene.Movement->GetConfiguredAccelerationCmS2(), 900.0f));
		// The definition carries no brake field: the default brake is the
		// acceleration (symmetric, config-derived).
		TestTrue(TEXT("the default brake equals the acceleration"),
			FMath::IsNearlyEqual(Scene.Movement->GetBrakingDecelerationCmS2(), 900.0f));

		// The brake override rejects values that would freeze or poison the
		// coast-down, keeps the previous value, and a re-Configure resets it.
		TestFalse(TEXT("a zero brake refuses"), Scene.Movement->SetBrakingDecelerationCmS2(0.0f));
		TestFalse(TEXT("a negative brake refuses"), Scene.Movement->SetBrakingDecelerationCmS2(-5.0f));
		TestFalse(TEXT("a non-finite brake refuses"),
			Scene.Movement->SetBrakingDecelerationCmS2(std::numeric_limits<float>::quiet_NaN()));
		TestTrue(TEXT("the refused overrides kept the default"),
			FMath::IsNearlyEqual(Scene.Movement->GetBrakingDecelerationCmS2(), 900.0f));
		TestTrue(TEXT("a finite positive override accepts"), Scene.Movement->SetBrakingDecelerationCmS2(450.0f));
		TestTrue(TEXT("the override is live"), FMath::IsNearlyEqual(Scene.Movement->GetBrakingDecelerationCmS2(), 450.0f));
		TestTrue(TEXT("the re-Configure accepts"), Scene.Movement->Configure(nullptr));
		TestTrue(TEXT("the re-Configure reset the override"),
			FMath::IsNearlyEqual(Scene.Movement->GetBrakingDecelerationCmS2(), 900.0f));
		return true;
	}

	// ------------------------------------------------------------------
	// 2. Drive authority gates refuse and clear the held throttle
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042DriveAuthorityGatesRefuseAndClearThrottle,
		"UEMMO.Tasks.M5_042.DriveAuthorityGatesRefuseAndClearThrottle",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042DriveAuthorityGatesRefuseAndClearThrottle::RunTest(const FString& Parameters)
	{
		FM5_042_DriveScene Scene;
		if (!Scene.Build(*this) || !Scene.Settle(*this))
		{
			return false;
		}
		const FVector SettledLocation = Scene.Vehicle->GetActorLocation();

		// No driver: the input is refused, nothing is held and the hull never
		// moves (the vertical pass alone keeps the hover hold).
		TestFalse(TEXT("the drive gate without a driver is closed"), Scene.Movement->IsDriveInputAccepted());
		TestFalse(TEXT("the SetDriveInput without a driver refuses"), Scene.Movement->SetDriveInput(1.0f, 0.0f));
		TestTrue(TEXT("the refused input holds nothing"), Scene.Movement->GetDriveInput().IsNearlyZero());
		Scene.StepMovement(30);
		TestTrue(TEXT("the driverless hull never moves"),
			Scene.Vehicle->GetActorLocation().Equals(SettledLocation, 0.05f));

		// A present driver opens the gate; the manual suspension flag closes it
		// again and the refusal drops the previously held throttle.
		Scene.Movement->SetDriverPresent(true);
		TestTrue(TEXT("the drive gate with a driver opens"), Scene.Movement->IsDriveInputAccepted());
		TestTrue(TEXT("the driver input is accepted"), Scene.Movement->SetDriveInput(1.0f, 0.0f));
		TestTrue(TEXT("the accepted input is held"), Scene.Movement->GetDriveInput().X > 0.5f);
		Scene.Movement->SetDriveControlsSuspended(true);
		TestFalse(TEXT("the suspended drive gate is closed"), Scene.Movement->IsDriveInputAccepted());
		TestFalse(TEXT("the suspended SetDriveInput refuses"), Scene.Movement->SetDriveInput(1.0f, 1.0f));
		TestTrue(TEXT("the suspension refusal cleared the held throttle"), Scene.Movement->GetDriveInput().IsNearlyZero());

		// The injected providers override the manual flags (the production
		// wiring reads the FSeatOccupancyModel through them).
		Scene.Movement->SetControlSuspensionProvider([]()
		{
			return true;
		});
		Scene.Movement->SetDriveControlsSuspended(false);
		TestFalse(TEXT("the suspension provider keeps the gate closed"), Scene.Movement->IsDriveInputAccepted());
		Scene.Movement->SetControlSuspensionProvider(TFunction<bool()>());
		TestTrue(TEXT("an absent suspension provider returns to the manual flag"), Scene.Movement->IsDriveInputAccepted());
		Scene.Movement->SetDriverPresenceProvider([]()
		{
			return false;
		});
		TestFalse(TEXT("the presence provider outranks the manual flag"), Scene.Movement->IsDriveInputAccepted());
		Scene.Movement->SetDriverPresenceProvider(TFunction<bool()>());
		TestTrue(TEXT("the manual driver flag returns"), Scene.Movement->IsDriveInputAccepted());

		// The component's own tick forwards into the same TickMovement entry:
		// ten world ticks with the held input advance the planar velocity by
		// ten acceleration steps (and nothing else invents motion).
		Scene.Movement->SetDriveInput(1.0f, 0.0f);
		Scene.Movement->SetComponentTickEnabled(true);
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			Scene.Wrapper.TickTestWorld(1.0f / 60.0f);
		}
		Scene.Movement->SetComponentTickEnabled(false);
		TestTrue(TEXT("the component tick forwarded into the drive (10 world ticks accelerated once each)"),
			FMath::IsNearlyEqual(Scene.Movement->GetPlanarVelocity().X, 150.0f, 5.0f));

		// A dead hull never drives: the tick re-read drops the held throttle
		// and the coast-down bleeds the velocity to an exact stop.
		Scene.Vehicle->GetHealthComponent()->ApplyDamage(99999.0f);
		TestFalse(TEXT("the destroyed hull's drive gate is closed"), Scene.Movement->IsDriveInputAccepted());
		TestFalse(TEXT("the dead hull refuses new input"), Scene.Movement->SetDriveInput(1.0f, 0.0f));
		TestTrue(TEXT("the death cleared the held throttle"), Scene.Movement->GetDriveInput().IsNearlyZero());
		Scene.StepMovement(40);
		TestTrue(TEXT("the dead hull's velocity coasted down to an exact stop"),
			Scene.Movement->GetPlanarVelocity().IsNearlyZero());
		return true;
	}

	// ------------------------------------------------------------------
	// 3. Straight X, depth Y and the released-stop match the configuration
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042StraightDepthAndStopMatchConfiguration,
		"UEMMO.Tasks.M5_042.StraightDepthAndStopMatchConfiguration",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042StraightDepthAndStopMatchConfiguration::RunTest(const FString& Parameters)
	{
		FM5_042_DriveScene Scene;
		if (!Scene.Build(*this) || !Scene.Settle(*this))
		{
			return false;
		}
		Scene.Movement->SetDriverPresent(true);
		const float BaseZ = M5_042_SceneBase.Z;

		// Straight X: 60 frames from rest travel the clamped acceleration
		// integral (frames 1..46 on the 15 cm/s-per-frame ramp cover
		// sum(0.25*i) = 270.25 cm, then 14 frames at the 700 cap cover
		// 163.33 cm -> ~433.6 cm), with no depth drift and the hover height
		// untouched (Z is collision's alone).
		const FVector Start = Scene.Vehicle->GetActorLocation();
		Scene.Movement->SetDriveInput(1.0f, 0.0f);
		Scene.StepMovement(60);
		const FVector After60 = Scene.Vehicle->GetActorLocation();
		TestTrue(TEXT("the straight X displacement matches the configured acceleration"),
			After60.X - Start.X >= 432.0f && After60.X - Start.X <= 435.0f);
		TestTrue(TEXT("the straight drive produces no depth drift"), FMath::Abs(After60.Y - Start.Y) < 0.5f);
		TestTrue(TEXT("the drive never changes the hover height"), FMath::IsNearlyEqual(After60.Z - BaseZ, 81.0f, 0.5f));
		TestTrue(TEXT("60 frames of throttle reached the configured top speed"),
			FMath::IsNearlyEqual(Scene.Movement->GetPlanarVelocity().X, 700.0f, 1.0f));

		// At the top speed the displacement is exactly speed * time (the sweep
		// travels unobstructed): 700 cm over 60 frames.
		const FVector AtTopSpeed = Scene.Vehicle->GetActorLocation();
		Scene.StepMovement(60);
		const FVector AfterWindow = Scene.Vehicle->GetActorLocation();
		TestTrue(TEXT("the top-speed window travels max speed times time"),
			AfterWindow.X - AtTopSpeed.X >= 698.0f && AfterWindow.X - AtTopSpeed.X <= 702.0f);

		// The released input brakes to an exact stop and then never moves again
		// (the "stop means stop" acceptance).
		Scene.Movement->SetDriveInput(0.0f, 0.0f);
		Scene.StepMovement(90);
		TestTrue(TEXT("the released input braked to an exact stop"),
			Scene.Movement->GetPlanarVelocity().IsNearlyZero());
		const FVector Stopped = Scene.Vehicle->GetActorLocation();
		Scene.StepMovement(30);
		TestTrue(TEXT("the stopped hull never creeps on"),
			Scene.Vehicle->GetActorLocation().Equals(Stopped, 0.1f));

		// Depth Y is a first-class drive axis (no plane lock): the same
		// clamped-ramp integral holds on Y, with no lateral drift.
		Scene.Movement->SetDriveInput(0.0f, 1.0f);
		Scene.StepMovement(60);
		const FVector AfterY = Scene.Vehicle->GetActorLocation();
		TestTrue(TEXT("the depth Y displacement matches the configuration (no plane lock)"),
			AfterY.Y - Stopped.Y >= 432.0f && AfterY.Y - Stopped.Y <= 435.0f);
		TestTrue(TEXT("the depth drive produces no lateral drift"), FMath::Abs(AfterY.X - Stopped.X) < 0.5f);
		TestTrue(TEXT("the depth drive never changes the hover height"), FMath::IsNearlyEqual(AfterY.Z - BaseZ, 81.0f, 0.5f));
		return true;
	}

	// ------------------------------------------------------------------
	// 4. Diagonal input is normalized to the single top speed
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042DiagonalDriveIsNormalizedToSingleTopSpeed,
		"UEMMO.Tasks.M5_042.DiagonalDriveIsNormalizedToSingleTopSpeed",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042DiagonalDriveIsNormalizedToSingleTopSpeed::RunTest(const FString& Parameters)
	{
		FM5_042_DriveScene Scene;
		if (!Scene.Build(*this) || !Scene.Settle(*this))
		{
			return false;
		}
		Scene.Movement->SetDriverPresent(true);

		// One frame of (1,1): the acceleration is a unit direction times the
		// scalar - the diagonal speed gain equals the straight one (no
		// sqrt(2) boost).
		Scene.Movement->SetDriveInput(1.0f, 1.0f);
		Scene.StepMovement(1);
		const FVector2D FirstStep = Scene.Movement->GetPlanarVelocity();
		TestTrue(TEXT("one diagonal frame gains exactly the straight speed"),
			FMath::IsNearlyEqual(FirstStep.Size(), 15.0f, 0.05f));
		TestTrue(TEXT("the diagonal direction is even between the axes"),
			FMath::IsNearlyEqual(FirstStep.X, FirstStep.Y, 0.01f));

		// The whole-vector clamp keeps the DIAGONAL top speed at the straight
		// one (700, never 700*sqrt(2)).
		Scene.StepMovement(119);
		TestTrue(TEXT("the diagonal top speed equals the straight top speed"),
			FMath::IsNearlyEqual(Scene.Movement->GetPlanarVelocity().Size(), 700.0f, 0.5f));

		// At the clamped diagonal speed each axis carries speed/sqrt(2): the
		// 60-frame window travels 494.97 cm per axis, equally.
		const FVector WindowStart = Scene.Vehicle->GetActorLocation();
		Scene.StepMovement(60);
		const FVector WindowEnd = Scene.Vehicle->GetActorLocation();
		const float Dx = WindowEnd.X - WindowStart.X;
		const float Dy = WindowEnd.Y - WindowStart.Y;
		TestTrue(TEXT("the diagonal window travels max speed over sqrt(2) per axis"),
			Dx >= 493.0f && Dx <= 497.0f && Dy >= 493.0f && Dy <= 497.0f);
		TestTrue(TEXT("the diagonal axes stay even through the clamp"), FMath::Abs(Dx - Dy) < 0.5f);
		return true;
	}

	// ------------------------------------------------------------------
	// 5. A wall stops the hull without penetration or creep
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042WallStopsHullWithoutPenetration,
		"UEMMO.Tasks.M5_042.WallStopsHullWithoutPenetration",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042WallStopsHullWithoutPenetration::RunTest(const FString& Parameters)
	{
		FM5_042_DriveScene Scene;
		if (!Scene.Build(*this) || !Scene.Settle(*this))
		{
			return false;
		}

		// A tall wall slab ahead: the blocking face at base X + 1200 (the hull
		// starts at +400 with a 90 cm half extent, front face at +490).
		const float WallFaceX = M5_042_SceneBase.X + 1200.0f;
		if (!TestNotNull(TEXT("the boundary wall spawns"),
				M5_042_SpawnWall(*Scene.Wrapper.GetTestWorld(),
					M5_042_SceneBase + FVector(1250.0, 0.0, 200.0), FVector(50.0f, 1500.0f, 200.0f))))
		{
			return false;
		}

		Scene.Movement->SetDriverPresent(true);
		Scene.Movement->SetDriveInput(1.0f, 0.0f);

		// Full throttle into the wall: every checkpoint keeps the hull front on
		// the near side (the swept move makes penetration need a bug).
		bool bPenetrated = false;
		FVector FinalLocation = Scene.Vehicle->GetActorLocation();
		for (int32 Frame = 0; Frame < 300; ++Frame)
		{
			Scene.StepMovement(1);
			FinalLocation = Scene.Vehicle->GetActorLocation();
			if (FinalLocation.X + 90.0f > WallFaceX + 0.6f)
			{
				bPenetrated = true;
				break;
			}
		}
		TestFalse(TEXT("the wall never penetrates under held throttle"), bPenetrated);
		const float HullFrontX = FinalLocation.X + 90.0f;
		TestTrue(TEXT("the hull reached the wall contact"),
			HullFrontX >= WallFaceX - 1.5f && HullFrontX <= WallFaceX + 0.6f);

		// Held throttle against the face: no creep (the into-wall velocity
		// share is removed on every contact).
		Scene.StepMovement(60);
		TestTrue(TEXT("the held throttle never creeps into the wall"),
			FMath::Abs(Scene.Vehicle->GetActorLocation().X - FinalLocation.X) < 0.5f);
		TestTrue(TEXT("the head-on wall stop produces no depth slide"),
			FMath::Abs(Scene.Vehicle->GetActorLocation().Y - M5_042_SceneBase.Y) < 0.5f);
		TestTrue(TEXT("the wall stop keeps the hover height"),
			FMath::IsNearlyEqual(Scene.Vehicle->GetActorLocation().Z - M5_042_SceneBase.Z, 81.0f, 0.5f));

		// Releasing the input at the wall: brake to a stop, then never move.
		Scene.Movement->SetDriveInput(0.0f, 0.0f);
		Scene.StepMovement(90);
		const FVector Released = Scene.Vehicle->GetActorLocation();
		Scene.StepMovement(30);
		TestTrue(TEXT("the released hull at the wall never moves on"),
			Scene.Vehicle->GetActorLocation().Equals(Released, 0.1f));
		return true;
	}

	// ------------------------------------------------------------------
	// 6. The bridge follows the hull and restores the character anchor
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042CameraBridgeFollowsVehicleAndRestoresAnchor,
		"UEMMO.Tasks.M5_042.CameraBridgeFollowsVehicleAndRestoresAnchor",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042CameraBridgeFollowsVehicleAndRestoresAnchor::RunTest(const FString& Parameters)
	{
		FM5_042_CameraScene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}
		USideViewCameraComponent* Rig = Scene.Rig;

		// Baseline: the character-side framing (the M0 values on the pawn).
		TestTrue(TEXT("the baseline rig hangs on the pawn root"),
			Rig->GetAttachParent() == Scene.Player->GetRootComponent());
		TestTrue(TEXT("the baseline rig keeps the M0 boom"),
			FMath::IsNearlyEqual(Rig->TargetArmLength, 1700.0f));
		TestTrue(TEXT("the baseline rig tick is enabled"), Rig->IsComponentTickEnabled());

		// The bridge: re-parent onto the hull with the definition's follow
		// values (pivot = origin + CameraHeightCm, boom = CameraDistanceCm,
		// pitch = CameraPitchDeg; the M0 side-view yaw stays).
		FString BridgeError;
		TestTrue(TEXT("the bridge accepts"), Scene.Bridge->BridgeToVehicle(*Scene.Vehicle, &BridgeError));
		TestTrue(TEXT("the bridge reports bridged"), Scene.Bridge->IsBridged());
		TestEqual(TEXT("the bridge names its hull"), Scene.Bridge->GetBridgedVehicle(), Scene.Vehicle);
		TestTrue(TEXT("the bridged rig hangs on the hull root"),
			Rig->GetAttachParent() == Scene.Vehicle->GetRootComponent());
		TestTrue(TEXT("the bridged boom is the definition's camera distance"),
			FMath::IsNearlyEqual(Rig->TargetArmLength, 650.0f, 0.01f));
		const FRotator BridgedRotation = Rig->GetComponentRotation();
		TestTrue(TEXT("the bridged pitch is the definition's camera pitch"),
			FMath::IsNearlyEqual(BridgedRotation.Pitch, -18.0f, 0.01f));
		TestTrue(TEXT("the bridged yaw keeps the fixed side view"),
			FMath::IsNearlyEqual(BridgedRotation.Yaw, UE::UEMMO::Tasks::M1_030::SideViewCameraYaw, 0.01f));
		TestTrue(TEXT("the bridged pivot sits at the vehicle ground anchor"),
			Rig->GetComponentLocation().Equals(Scene.Vehicle->GetActorLocation() + FVector(0.0f, 0.0f, 260.0f), 0.1f));
		TestFalse(TEXT("the bridged rig's own anchor tick is suspended"), Rig->IsComponentTickEnabled());

		// The follow: the rig is attached, so the engine's transform
		// propagation carries it with the hull - including depth (Y), the
		// project's core axis.
		const FVector MovedOrigin = M5_042_SceneBase + FVector(700.0, 200.0, 96.0);
		Scene.Vehicle->SetActorLocation(MovedOrigin);
		TestTrue(TEXT("the bridged rig follows the hull laterally"),
			FMath::IsNearlyEqual(Rig->GetComponentLocation().X, MovedOrigin.X, 0.1f));
		TestTrue(TEXT("the bridged rig follows the hull in depth"),
			FMath::IsNearlyEqual(Rig->GetComponentLocation().Y, MovedOrigin.Y, 0.1f));
		TestTrue(TEXT("the bridged pivot keeps the definition height"),
			FMath::IsNearlyEqual(Rig->GetComponentLocation().Z, MovedOrigin.Z + 260.0f, 0.1f));

		// The restore: the saved character-side framing back, the anchor snap
		// onto the rig's own pure math (the same-source construction), the tick
		// resumed.
		FString RestoreUnused;
		TestTrue(TEXT("the restore accepts"), Scene.Bridge->RestoreCharacterAnchor());
		TestFalse(TEXT("the bridge reports unbridged"), Scene.Bridge->IsBridged());
		TestTrue(TEXT("the restored rig hangs on the pawn root again"),
			Rig->GetAttachParent() == Scene.Player->GetRootComponent());
		TestTrue(TEXT("the restored boom is the saved M0 value"),
			FMath::IsNearlyEqual(Rig->TargetArmLength, 1700.0f, 0.01f));
		TestTrue(TEXT("the restored rig tick resumed"), Rig->IsComponentTickEnabled());
		const FVector PawnLocation = Scene.Player->GetActorLocation();
		const FVector ExpectedAnchor = UE::UEMMO::Tasks::M1_030::ComputeAnchorLocation(
			PawnLocation, /*GroundZ*/ M5_042_SceneBase.Z, PawnLocation.Y, Rig->Config);
		TestTrue(TEXT("the restored rig snapped onto the character anchor (the rig's own M1-030 math)"),
			Rig->GetComponentLocation().Equals(ExpectedAnchor, 1.0f));

		// The duplicated restore is the not-bridged no-op with zero side
		// effects (the rig stays exactly where the first restore left it).
		TestFalse(TEXT("the second restore is the no-op"), Scene.Bridge->RestoreCharacterAnchor());
		TestTrue(TEXT("the no-op never moved the rig"),
			Rig->GetComponentLocation().Equals(ExpectedAnchor, 0.01f));
		return true;
	}

	// ------------------------------------------------------------------
	// 7. Bridge refusals, the duplicated no-op and the hull switch
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042CameraBridgeRefusalDuplicateAndSwitchFaces,
		"UEMMO.Tasks.M5_042.CameraBridgeRefusalDuplicateAndSwitchFaces",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042CameraBridgeRefusalDuplicateAndSwitchFaces::RunTest(const FString& Parameters)
	{
		FM5_042_CameraScene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}
		USideViewCameraComponent* Rig = Scene.Rig;

		// A never-bridged restore is the false no-op.
		TestFalse(TEXT("the restore without a bridge is the no-op"), Scene.Bridge->RestoreCharacterAnchor());

		// The duplicate bridge command is the accepted no-op: the framing is
		// never re-captured from the already-bridged state.
		TestTrue(TEXT("the first bridge accepts"), Scene.Bridge->BridgeToVehicle(*Scene.Vehicle, nullptr));
		TestTrue(TEXT("the duplicated bridge of the same hull accepts"), Scene.Bridge->BridgeToVehicle(*Scene.Vehicle, nullptr));
		TestTrue(TEXT("the duplicated bridge stayed bridged"), Scene.Bridge->IsBridged());
		TestTrue(TEXT("the duplicated bridge never re-applied a poisoned framing"),
			FMath::IsNearlyEqual(Rig->TargetArmLength, 650.0f, 0.01f));

		// The definition-less hull is refused (the follow-camera values are the
		// framing source) and the refusal never touches the live bridge.
		FString Refusal;
		TestFalse(TEXT("the definition-less hull refuses the bridge"),
			Scene.Bridge->BridgeToVehicle(*Scene.BareVehicle, &Refusal));
		TestFalse(TEXT("the refusal names the missing definition"), Refusal.IsEmpty());
		TestTrue(TEXT("the refusal kept the original bridge"), Scene.Bridge->GetBridgedVehicle() == Scene.Vehicle);
		TestTrue(TEXT("the refusal never re-parented the rig"),
			Rig->GetAttachParent() == Scene.Vehicle->GetRootComponent());

		// The hull switch restores the previous anchor first and re-bridges
		// onto the new hull.
		TestTrue(TEXT("the switch to the second hull accepts"),
			Scene.Bridge->BridgeToVehicle(*Scene.VehicleB, nullptr));
		TestEqual(TEXT("the bridge names the new hull"), Scene.Bridge->GetBridgedVehicle(), Scene.VehicleB);
		TestTrue(TEXT("the rig hangs on the new hull root"),
			Rig->GetAttachParent() == Scene.VehicleB->GetRootComponent());
		TestTrue(TEXT("the new hull's follow framing applied"),
			FMath::IsNearlyEqual(Rig->TargetArmLength, 650.0f, 0.01f));
		return true;
	}

	// ------------------------------------------------------------------
	// 8. The destroy broadcast and the bridge EndPlay self-restore
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042CameraBridgeDestroyAndEndPlaySelfRestore,
		"UEMMO.Tasks.M5_042.CameraBridgeDestroyAndEndPlaySelfRestore",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042CameraBridgeDestroyAndEndPlaySelfRestore::RunTest(const FString& Parameters)
	{
		FM5_042_CameraScene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}
		USideViewCameraComponent* Rig = Scene.Rig;

		// Bridge onto the second hull, then kill it: the one-shot destroy
		// broadcast self-restores the surviving driver's camera even though
		// the wiring never calls RestoreCharacterAnchor (contract section 8).
		TestTrue(TEXT("the bridge onto the second hull accepts"),
			Scene.Bridge->BridgeToVehicle(*Scene.VehicleB, nullptr));
		Scene.VehicleB->GetHealthComponent()->ApplyDamage(99999.0f);
		TestTrue(TEXT("the hull finalized its destruction"), Scene.VehicleB->IsDestroyFinalized());
		TestFalse(TEXT("the destroy self-restored the bridge"), Scene.Bridge->IsBridged());
		TestTrue(TEXT("the self-restored rig hangs on the pawn root"),
			Rig->GetAttachParent() == Scene.Player->GetRootComponent());
		TestTrue(TEXT("the self-restored boom is the saved M0 value"),
			FMath::IsNearlyEqual(Rig->TargetArmLength, 1700.0f, 0.01f));
		TestTrue(TEXT("the self-restored rig tick resumed"), Rig->IsComponentTickEnabled());

		// The bridge's own EndPlay face: a bridge torn down with its pawn hands
		// the camera back first (the component Destroy routes EndPlay).
		TestTrue(TEXT("the re-bridge accepts"), Scene.Bridge->BridgeToVehicle(*Scene.Vehicle, nullptr));
		Scene.Bridge->DestroyComponent();
		TestFalse(TEXT("the bridge EndPlay restored the bridge face"), Scene.Bridge->IsBridged());
		TestTrue(TEXT("the EndPlay-restored rig hangs on the pawn root"),
			Rig->GetAttachParent() == Scene.Player->GetRootComponent());
		TestTrue(TEXT("the EndPlay-restored boom is the saved M0 value"),
			FMath::IsNearlyEqual(Rig->TargetArmLength, 1700.0f, 0.01f));

		// The destroyed bridge never answers again (the weak references died
		// with it): the hull's later destroy broadcast reaches nobody.
		Scene.Vehicle->GetHealthComponent()->ApplyDamage(99999.0f);
		TestTrue(TEXT("the hull finalized its destruction"), Scene.Vehicle->IsDestroyFinalized());
		TestFalse(TEXT("the destroyed bridge stays unbridged"), Scene.Bridge->IsBridged());
		TestTrue(TEXT("the rig stayed on the pawn root through the hull death"),
			Rig->GetAttachParent() == Scene.Player->GetRootComponent());
		return true;
	}

	// ------------------------------------------------------------------
	// VehicleSceneCapture (the card's dedicated rendered capture companion;
	// ClientContext only). In a real rendered game world the hull is staged
	// beside the game pawn with a second prototype character as the proportion
	// reference, then three screenshots land in the task evidence folder:
	// the character-side framing with the vehicle in view, the bridged driver
	// framing (the rig on the hull ground anchor), and the restored character
	// framing. Headless runs skip silently; the assertions live in the suites
	// above.
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_042VehicleSceneCapture,
		"UEMMO.Tasks.M5_042.VehicleSceneCapture",
		EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_042VehicleSceneCapture::RunTest(const FString& Parameters)
	{
		UWorld* World = GWorld;
		APrototypeCharacter* GamePawn = World != nullptr
			? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0))
			: nullptr;
		if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
			|| World->WorldType != EWorldType::Game || GamePawn == nullptr)
		{
			AddInfo(TEXT("vehicle capture skipped: no rendered game world with the prototype pawn"));
			return true;
		}

		// Stage the hull in front of the game camera (the fixed side view looks
		// from +X toward -X, so the in-view side of the pawn is -X) with a
		// second prototype character beside it as the proportion reference.
		const FVector Base = GamePawn->GetActorLocation();
		const FVehicleDefinition Definition = M5_042_MakeShippedSampleVehicle();
		ACombatVehicle* Hull = M5_042_SpawnVehicle(*World, Base + FVector(-700.0, 150.0, Base.Z - 16.0), &Definition);
		APrototypeCharacter* Reference = M5_042_SpawnPlayer(*World, Base + FVector(-300.0, 150.0, Base.Z));
		if (!TestNotNull(TEXT("the staged hull spawns"), Hull) ||
			!TestNotNull(TEXT("the proportion reference spawns"), Reference))
		{
			return true;
		}
		UVehicleCameraBridge* Bridge = NewObject<UVehicleCameraBridge>(GamePawn, TEXT("M5_042_CaptureBridge"));
		Bridge->RegisterComponent();

		const FString Directory = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-042") / TEXT("render"));
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);

		// Screenshot 1: the character-side framing with the vehicle and the
		// reference character in view (the proportions face).
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, GamePawn]()
		{
			if (GamePawn != nullptr)
			{
				USideViewCameraComponent* CaptureRig = GamePawn->FindComponentByClass<USideViewCameraComponent>();
				if (CaptureRig != nullptr)
				{
					AddInfo(FString::Printf(TEXT("M5_042 capture step1: pawn=%s rig=%s boom=%f parent=%s"),
						*GamePawn->GetActorLocation().ToCompactString(),
						*CaptureRig->GetComponentLocation().ToCompactString(),
						CaptureRig->TargetArmLength,
						CaptureRig->GetAttachParent() != nullptr ? *CaptureRig->GetAttachParent()->GetName() : TEXT("null")));
				}
			}
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("vehicle-proportions.png"), true, false);
			return true;
		}));

		// Screenshot 2: the bridged driver framing - the rig re-parented onto
		// the hull ground anchor with the definition's follow values.
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Bridge, Hull]()
		{
			const bool bBridged = Bridge != nullptr && Hull != nullptr && Bridge->BridgeToVehicle(*Hull, nullptr);
			AddInfo(FString::Printf(TEXT("M5_042 capture bridge accepted=%d hull=%s"), bBridged,
				Hull != nullptr ? *Hull->GetActorLocation().ToCompactString() : TEXT("null")));
			return bBridged;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("vehicle-bridged-camera.png"), true, false);
			return true;
		}));

		// Screenshot 3: the restored character framing after the exit-side
		// restore face.
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Bridge, GamePawn]()
		{
			const bool bRestored = Bridge != nullptr && Bridge->RestoreCharacterAnchor();
			if (GamePawn != nullptr)
			{
				USideViewCameraComponent* CaptureRig = GamePawn->FindComponentByClass<USideViewCameraComponent>();
				if (CaptureRig != nullptr)
				{
					AddInfo(FString::Printf(TEXT("M5_042 capture restore accepted=%d rig=%s boom=%f parent=%s pawn=%s"),
						bRestored,
						*CaptureRig->GetComponentLocation().ToCompactString(),
						CaptureRig->TargetArmLength,
						CaptureRig->GetAttachParent() != nullptr ? *CaptureRig->GetAttachParent()->GetName() : TEXT("null"),
						*GamePawn->GetActorLocation().ToCompactString()));
				}
			}
			return bRestored;
		}));
		// A longer settle before the third shot: the spring arm's socket
		// updates lag the rig transform restore; this also separates a real
		// restore failure from a capture-timing artifact in the PNG.
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("vehicle-restored-camera.png"), true, false);
			return true;
		}));

		// Cleanup: retire the staged actors and the bridge (the shared game
		// world stays clean).
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Bridge, Hull, Reference]()
		{
			if (Bridge != nullptr && IsValid(Bridge))
			{
				Bridge->DestroyComponent();
			}
			if (Hull != nullptr && IsValid(Hull))
			{
				Hull->Destroy();
			}
			if (Reference != nullptr && IsValid(Reference))
			{
				Reference->Destroy();
			}
			return true;
		}));
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
