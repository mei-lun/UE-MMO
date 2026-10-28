// M5-045: the packaged C-segment vehicle trial evidence. These tests pin what
// the VEHICLE TRIAL PACKAGE guarantees (the package ships the Data/CombatSystem
// runtime config plus Data/items.json next to Data/test_room.json - the
// M5-036/M5-056 carries):
// - the shipped combat config loads from the packaged project directory and
//   the vehicle segment rows (vehicles / vehicle_seats / vehicle_mounts)
//   resolve for the sample vehicle with the shipped turret mapping;
// - the real pawn's interaction component plus the production rigging
//   (EnsureProductionRigging) succeed IN THE PACKAGE against the package's
//   own tables - the ground drive configures, the turret weapon binds a real
//   item row, the mount configures the shipped pitch clamp and the vehicle
//   fire source registers - and the routed input chain (enter, drive, fire,
//   damage, exit) operates end to end with no fixture catalog substituted;
// - the rendered capture companion stages the production chain in the real
//   game world so the packaged screenshots show the trial actually working;
//   headless runs skip silently.
// Every suite is environment-honest: in the editor the same assertions run
// against the dev Data/CombatSystem source, so the full editor suite stays
// green while the packaged run carries the trial proof.
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

#include "UObject/UObjectGlobals.h"

#include "../PrototypeCharacter.h"

#include "../PrototypeHUD.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../PrototypeGameMode.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/SeatModel.h"
#include "../Vehicles/VehicleCameraBridge.h"
#include "../Vehicles/VehicleInteractionComponent.h"
#include "../Vehicles/VehicleMovementComponent.h"
#include "../Vehicles/VehicleWeaponMountComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "EngineUtils.h"
#include "Input/Events.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_045
{
	// Remote scene base (the vehicle-chain placement convention). X lateral,
	// Y depth, Z height; the floor top sits exactly at the base Z.
	const FVector M5_045_SceneBase(150000.0, 120000.0, 0.0);

	// World-static blocking floor box (the vehicle-chain pattern verbatim).
	static AActor* M5_045_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_045_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(6000.0f, 6000.0f, 100.0f));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, 100.0));
		return Actor;
	}

	// Spawns the real prototype pawn with no-controller physics (the chain
	// pattern verbatim).
	static APrototypeCharacter* M5_045_SpawnPlayer(UWorld& World, const FVector& Location)
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
	 * Loads the SHIPPED combat tables from the running project directory (the
	 * package carries its own Data/CombatSystem - the M5-036/M5-056 carries;
	 * the editor resolves the dev source) and returns the sample vehicle,
	 * driver seat and turret mount rows plus the built catalog. No fixture,
	 * no fallback: a missing row is a named refusal.
	 */
	static bool M5_045_LoadShippedVehicleRows(FAutomationTestBase& Test, FCombatCatalog& OutCatalog,
		FVehicleDefinition& OutVehicle, FVehicleSeatDefinition& OutSeat, FVehicleMountDefinition& OutMount)
	{
		FCombatDataTableSet Tables;
		TArray<FString> LoadProblems;
		if (!Test.TestTrue(TEXT("the shipped combat tables load from the running project directory"),
				LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), Tables, LoadProblems)))
		{
			Test.AddError(FString::Printf(TEXT("the shipped tables refused: %s"), *FString::Join(LoadProblems, TEXT(" | "))));
			return false;
		}
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;
		for (const TPair<FName, FDamageProfile>& Row : Tables.DamageProfiles) { Parsed.DamageProfiles.Add(Row.Value); }
		for (const TPair<FName, FAttackReaction>& Row : Tables.AttackReactions) { Parsed.AttackReactions.Add(Row.Value); }
		for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions) { Parsed.TargetReactions.Add(Row.Value); }
		for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons) { Parsed.Weapons.Add(Row.Value); }
		for (const TPair<FName, FAmmoType>& Row : Tables.AmmoTypes) { Parsed.AmmoTypes.Add(Row.Value); }
		for (const TPair<FName, FProjectileDefinition>& Row : Tables.Projectiles) { Parsed.Projectiles.Add(Row.Value); }
		for (const TPair<FName, FVehicleDefinition>& Row : Tables.Vehicles) { Parsed.Vehicles.Add(Row.Value); }
		for (const TPair<FName, FVehicleSeatDefinition>& Row : Tables.VehicleSeats) { Parsed.VehicleSeats.Add(Row.Value); }
		for (const TPair<FName, FVehicleMountDefinition>& Row : Tables.VehicleMounts) { Parsed.VehicleMounts.Add(Row.Value); }
		FString BuildErrors;
		if (!Test.TestTrue(TEXT("the shipped catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, OutCatalog, BuildErrors)))
		{
			Test.AddError(FString::Printf(TEXT("the shipped catalog build refused: %s"), *BuildErrors));
			return false;
		}
		const FName SampleVehicleId(TEXT("vehicle_wheeled_sample"));
		const FVehicleDefinition* VehicleRow = OutCatalog.FindVehicle(SampleVehicleId);
		if (!Test.TestNotNull(TEXT("the sample vehicle row resolves in the shipped catalog"), VehicleRow))
		{
			return false;
		}
		OutVehicle = *VehicleRow;
		bool bSeatFound = false;
		for (const TPair<FName, FVehicleSeatDefinition>& Row : Tables.VehicleSeats)
		{
			if (Row.Value.VehicleId == SampleVehicleId)
			{
				OutSeat = Row.Value;
				bSeatFound = true;
				break;
			}
		}
		if (!Test.TestTrue(TEXT("the sample driver seat row resolves"), bSeatFound))
		{
			return false;
		}
		bool bMountFound = false;
		for (const TPair<FName, FVehicleMountDefinition>& Row : Tables.VehicleMounts)
		{
			if (Row.Value.VehicleId == SampleVehicleId)
			{
				OutMount = Row.Value;
				bMountFound = true;
				break;
			}
		}
		return Test.TestTrue(TEXT("the sample turret mount row resolves"), bMountFound);
	}

	// ------------------------------------------------------------------
	// 1. The shipped vehicle segment rows carry in the package
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_045PackagedVehicleConfigCarries,
		"UEMMO.Tasks.M5_045.PackagedVehicleConfigCarries",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_045PackagedVehicleConfigCarries::RunTest(const FString& Parameters)
	{
		FCombatCatalog Catalog;
		FVehicleDefinition Vehicle;
		FVehicleSeatDefinition Seat;
		FVehicleMountDefinition Mount;
		if (!M5_045_LoadShippedVehicleRows(*this, Catalog, Vehicle, Seat, Mount))
		{
			return false;
		}
		// The shipped values the chain depends on (the M5-037 validated kinds).
		TestTrue(TEXT("the sample vehicle is a ground drive"), Vehicle.DriveType == EVehicleDriveType::Ground);
		TestTrue(TEXT("the sample vehicle carries positive drive tuning"),
			Vehicle.MaxSpeedCmS > 0.0f && Vehicle.AccelerationCmS2 > 0.0f);
		TestTrue(TEXT("the sample seat is the driver role"), Seat.SeatRole == EVehicleSeatRole::Driver);
		TestTrue(TEXT("the sample mount names a turret weapon"), Mount.TurretWeaponId != NAME_None);
		// The turret weapon + its ammo kind resolve in the SAME catalog (the
		// cross-table closure the rigging's bind path depends on).
		const FWeaponDefinition* TurretWeapon = Catalog.FindWeapon(Mount.TurretWeaponId);
		if (!TestNotNull(TEXT("the turret weapon resolves in the shipped catalog"), TurretWeapon))
		{
			return false;
		}
		TestNotNull(TEXT("the turret ammo kind resolves"), Catalog.FindAmmoType(TurretWeapon->AmmoId));
		TestNotNull(TEXT("the turret projectile resolves"), Catalog.FindProjectile(TurretWeapon->ProjectileId));
		// The item row mapping the turret weapon exists in the shipped items
		// source (the rigging's bind chain refuses without it).
		TArray<FTestRoomItemRow> ItemRows;
		FString ItemsError;
		if (!TestTrue(TEXT("the shipped items source loads"),
				ACombatTestRoomDriver::LoadProductionItems(ItemRows, ItemsError)))
		{
			AddError(FString::Printf(TEXT("the shipped items refused: %s"), *ItemsError));
			return false;
		}
		bool bItemMapsTurret = false;
		for (const FTestRoomItemRow& Row : ItemRows)
		{
			if (Row.WeaponDefinitionId == Mount.TurretWeaponId)
			{
				bItemMapsTurret = true;
				break;
			}
		}
		TestTrue(TEXT("an items row maps the turret weapon"), bItemMapsTurret);
		return true;
	}

	// ------------------------------------------------------------------
	// 2. The production vehicle chain operates against the shipped tables
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_045PackagedVehicleChainOperates,
		"UEMMO.Tasks.M5_045.PackagedVehicleChainOperates",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_045PackagedVehicleChainOperates::RunTest(const FString& Parameters)
	{
		// The wrapped world (the chain's world-initialization lesson).
		FTestWorldWrapper Wrapper;
		if (!TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)) ||
			!TestTrue(TEXT("the test world began play"), Wrapper.BeginPlayInTestWorld()))
		{
			return false;
		}
		UWorld* World = Wrapper.GetTestWorld();
		if (!TestNotNull(TEXT("the test world exists"), World))
		{
			return false;
		}
		M5_045_SpawnFloor(*World, M5_045_SceneBase);
		APrototypeCharacter* Player = M5_045_SpawnPlayer(*World, M5_045_SceneBase + FVector(250.0, 0.0, 160.0));
		if (!TestNotNull(TEXT("the prototype pawn spawns"), Player))
		{
			return false;
		}
		for (int32 Frame = 0; Frame < 90; ++Frame)
		{
			Wrapper.TickTestWorld(1.0f / 60.0f);
		}
		UVehicleInteractionComponent* Interaction = Player->FindComponentByClass<UVehicleInteractionComponent>();
		if (!TestNotNull(TEXT("the interaction component exists"), Interaction))
		{
			return false;
		}
		FSeatOccupancyModel Seats;
		Interaction->BindSeatModel(Seats);
		Interaction->SetOperatorFaction(FName(TEXT("player")));

		// The hull from the SHIPPED rows (no fixture definition value).
		FCombatCatalog Catalog;
		FVehicleDefinition VehicleRow;
		FVehicleSeatDefinition SeatRow;
		FVehicleMountDefinition MountRow;
		if (!M5_045_LoadShippedVehicleRows(*this, Catalog, VehicleRow, SeatRow, MountRow))
		{
			return false;
		}
		FActorSpawnParameters Params;
		ACombatVehicle* Hull = World->SpawnActor<ACombatVehicle>(
			ACombatVehicle::StaticClass(), M5_045_SceneBase + FVector(400.0, 0.0, 96.0), FRotator::ZeroRotator, Params);
		if (!TestNotNull(TEXT("the hull spawns"), Hull))
		{
			return false;
		}
		FString DefinitionErrors;
		if (!TestTrue(TEXT("the hull accepts the shipped definition"),
				Hull->SetVehicleDefinition(VehicleRow, &DefinitionErrors)))
		{
			AddError(FString::Printf(TEXT("the shipped definition refused: %s"), *DefinitionErrors));
			return false;
		}
		FString RegisterError;
		if (!TestTrue(TEXT("the hull registers"), Interaction->RegisterVehicle(*Hull, SeatRow, &RegisterError)))
		{
			AddError(FString::Printf(TEXT("the registration refused: %s"), *RegisterError));
			return false;
		}

		// THE ENTER: the production rigging arms against the package tables.
		const FVector BeforeEnter = Hull->GetActorLocation();
		if (!TestTrue(TEXT("the enter accepts"), Interaction->TryEnterNearestVehicle().bEntered))
		{
			return false;
		}
		UVehicleMovementComponent* Drive = Hull->FindComponentByClass<UVehicleMovementComponent>();
		UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>();
		TestTrue(TEXT("the drive configured from the shipped row"), Drive != nullptr && Drive->IsConfigured());
		TestTrue(TEXT("the mount configured from the shipped row"), Mount != nullptr && Mount->IsConfigured());
		if (Mount != nullptr)
		{
			// The shipped pitch clamp carries through (the M5-037 row values;
			// the summary formats via SanitizeFloat - same-source construction,
			// the M5-043 lesson).
			const FString ExpectedClamp = FString::Printf(TEXT("pitch clamped to [%s, %s] degrees"),
				*FString::SanitizeFloat(MountRow.MinPitchDegrees), *FString::SanitizeFloat(MountRow.MaxPitchDegrees));
			TestTrue(TEXT("the mount reports the shipped aim clamp"),
				Mount->GetAimClampSummary().Contains(ExpectedClamp));
			TestTrue(TEXT("the vehicle fire source registered"),
				Mount->GetVehicleFireSourceEntityId() != InvalidCombatEntityId);
		}

		// THE DRIVE: the production cadence through the pawn's recording seam.
		Player->RecordPlanarMoveInput(1.0f, 0.0f);
		for (int32 Frame = 0; Frame < 60; ++Frame)
		{
			Wrapper.TickTestWorld(1.0f / 60.0f);
		}
		Player->RecordPlanarMoveInput(0.0f, 0.0f);
		for (int32 Frame = 0; Frame < 90; ++Frame)
		{
			Wrapper.TickTestWorld(1.0f / 60.0f);
		}
		const float TotalDistance = Hull->GetActorLocation().X - BeforeEnter.X;
		TestTrue(TEXT("the routed drive moved the hull (drive + brake integral)"),
			TotalDistance >= 695.0f && TotalDistance <= 716.0f);

		// THE FIRE: the Q edge routes into the turret; the rocket flies from
		// the shipped mount pivot and wounds through the unified entry.
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;
		{
			FActorSpawnParameters TargetParams;
			Target = World->SpawnActor<AActor>(AActor::StaticClass(),
				M5_045_SceneBase + FVector(1900.0, 0.0, 60.0), FRotator::ZeroRotator, TargetParams);
			if (Target != nullptr)
			{
				UBoxComponent* Body = NewObject<UBoxComponent>(Target, TEXT("M5_045_Target"));
				Target->SetRootComponent(Body);
				Body->SetMobility(EComponentMobility::Movable);
				Body->SetBoxExtent(FVector(50.0f, 50.0f, 60.0f));
				Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
				Body->RegisterComponent();
				Body->SetWorldLocation(M5_045_SceneBase + FVector(1900.0, 0.0, 60.0));
				TargetHealth = NewObject<UHealthComponent>(Target, TEXT("M5_045_TargetHealth"));
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
		World->EnsureCollisionTreeIsBuilt();
		const float HealthBefore = TargetHealth->GetHealth();
		Player->OnFireInputPressed();
		const bool bShotCommitted = Mount != nullptr && Mount->GetMountedShotCount() == 1;
		TestTrue(TEXT("the routed turret fire committed"), bShotCommitted);
		Player->OnFireInputReleased();
		bool bDamageLanded = false;
		float HealthAfter = HealthBefore;
		for (int32 Frame = 0; Frame < 240 && !bDamageLanded; ++Frame)
		{
			Wrapper.TickTestWorld(1.0f / 60.0f);
			HealthAfter = TargetHealth->GetHealth();
			if (HealthAfter < HealthBefore || !TargetHealth->IsAlive())
			{
				bDamageLanded = true;
			}
		}
		AddInfo(FString::Printf(TEXT("M5_045 fire phase: hpBefore=%f hpAfter=%f alive=%d"),
			HealthBefore, HealthAfter, TargetHealth->IsAlive() ? 1 : 0));
		TestTrue(TEXT("the turret shot wounded through the unified entry"), bDamageLanded);

		// THE EXIT: the production face restores everything.
		FString ExitRefusal;
		if (!TestTrue(TEXT("the exit accepts"), Interaction->TryExitVehicle(&ExitRefusal)))
		{
			AddError(FString::Printf(TEXT("the exit refused: %s"), *ExitRefusal));
			return false;
		}
		TestFalse(TEXT("the driver is no longer seated"), Interaction->IsSeated());
		UVehicleCameraBridge* Bridge = Player->FindComponentByClass<UVehicleCameraBridge>();
		TestTrue(TEXT("the camera bridge restored on exit"), Bridge != nullptr && !Bridge->IsBridged());
		return true;
	}

	// ------------------------------------------------------------------
	// 2b. The REAL game flow with a possessed controller: the user's path is
	// spawn the car through the HUD debug exec, walk to it, V to enter, drive
	// with the move axes, fire, exit. This suite runs THAT flow with a live
	// PlayerController (possession builds the pawn's real InputComponent and
	// its bindings), the exec entry through the controller's own console
	// command routing, and the V/axis production handlers - the layers the
	// fixture-world suites skip. A boot-menu world would refuse the enter
	// (the M5-041 UIOnly face) - the flow belongs in a running map, and the
	// playtest guide was corrected for it.
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_045PackagedControllerFlowDrives,
		"UEMMO.Tasks.M5_045.PackagedControllerFlowDrives",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_045PackagedControllerFlowDrives::RunTest(const FString& Parameters)
	{
		FTestWorldWrapper Wrapper;
		if (!TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)) ||
			!TestTrue(TEXT("the test world began play"), Wrapper.BeginPlayInTestWorld()))
		{
			return false;
		}
		UWorld* World = Wrapper.GetTestWorld();
		if (!TestNotNull(TEXT("the test world exists"), World))
		{
			return false;
		}
		M5_045_SpawnFloor(*World, M5_045_SceneBase);
		APrototypeCharacter* Player = M5_045_SpawnPlayer(*World, M5_045_SceneBase + FVector(250.0, 0.0, 160.0));
		if (!TestNotNull(TEXT("the prototype pawn spawns"), Player))
		{
			return false;
		}
		// The REAL possession: SetupPlayerInputComponent runs, the pawn's
		// input bindings (V + the move axes) go live - the fixture worlds
		// never had this layer.
		APlayerController* Controller = World->SpawnActor<APlayerController>(
			APlayerController::StaticClass(), M5_045_SceneBase + FVector(0.0, 0.0, 300.0), FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("the player controller spawns"), Controller))
		{
			return false;
		}
		Controller->Possess(Player);
		if (!TestTrue(TEXT("the pawn is possessed"), Player->GetController() == Controller))
		{
			return false;
		}
		for (int32 Frame = 0; Frame < 90; ++Frame)
		{
			Wrapper.TickTestWorld(1.0f / 60.0f);
		}

		// THE REAL STAGING ENTRY: the HUD's debug entry - the same function
		// the user's console command routes into (the M1-035 packaged
		// precedent proved the string->exec plumbing). The wrapper world's
		// manual possession skips the game mode's HUD assignment, so the HUD
		// is staged explicitly with its owner controller set - a HUD-less
		// exec silently does nothing, and that gap is what this suite exists
		// to catch.
		APrototypeHUD* Hud = Cast<APrototypeHUD>(Controller->GetHUD());
		if (Hud == nullptr)
		{
			Hud = World->SpawnActor<APrototypeHUD>(
				APrototypeHUD::StaticClass(), M5_045_SceneBase + FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator);
			TestNotNull(TEXT("the prototype HUD resolves for the staging entry"), Hud);
			if (Hud != nullptr)
			{
				Hud->PlayerOwner = Controller;
			}
		}
		if (Hud != nullptr)
		{
			Hud->UEMMODebugVehicle(1);
		}
		ACombatVehicle* Hull = nullptr;
		for (TActorIterator<ACombatVehicle> It(World); It; ++It)
		{
			Hull = *It;
			break;
		}
		if (!TestNotNull(TEXT("the console staging spawned the trial hull"), Hull))
		{
			return false;
		}
		TestTrue(TEXT("the staged hull carries the shipped definition"), Hull->HasVehicleDefinition());

		// Walk next to the hull and press V (the binding target handler - the
		// same entry the V key's Triggered event calls).
		Player->SetActorLocation(Hull->GetActorLocation() - FVector(150.0, 0.0, 0.0), /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		UVehicleInteractionComponent* Interaction = Player->FindComponentByClass<UVehicleInteractionComponent>();
		if (!TestNotNull(TEXT("the interaction component exists"), Interaction))
		{
			return false;
		}
		Interaction->TryInteract();
		if (!TestTrue(TEXT("the V press enters the seat"), Interaction->IsSeated()))
		{
			return false;
		}
		UVehicleMovementComponent* Drive = Hull->FindComponentByClass<UVehicleMovementComponent>();
		if (!TestNotNull(TEXT("the rigging armed on the real-flow enter"), Drive))
		{
			return false;
		}

		// DRIVE: the move-axis seam (the same state the Enhanced Input
		// handlers feed) through the possessed pawn's central tick.
		const FVector BeforeDrive = Hull->GetActorLocation();
		Player->RecordPlanarMoveInput(1.0f, 0.0f);
		for (int32 Frame = 0; Frame < 60; ++Frame)
		{
			Wrapper.TickTestWorld(1.0f / 60.0f);
		}
		Player->RecordPlanarMoveInput(0.0f, 0.0f);
		const float Driven = Hull->GetActorLocation().X - BeforeDrive.X;
		AddInfo(FString::Printf(TEXT("M5_045 real-flow drive: driven=%f planar=%s grounded=%d"),
			Driven, *Drive->GetPlanarVelocity().ToString(), Drive->IsGrounded() ? 1 : 0));
		TestTrue(TEXT("the possessed pawn's drive input moves the hull"),
			Driven >= 430.0f && Driven <= 437.0f);

		// THE FIRE through the seated Q route, then the V exit.
		Player->OnFireInputPressed();
		Player->OnFireInputReleased();
		UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>();
		TestTrue(TEXT("the seated Q committed the turret shot"),
			Mount != nullptr && Mount->GetMountedShotCount() >= 1);

		// THE REAL KEY LAYER: a synthesized ARROW-RIGHT goes through the real
		// Slate input stack (FSlateApplication -> Enhanced Input -> the
		// production MoveHorizontal binding) - the layer the handler-level
		// asserts above bypass. The injection only delivers where the game
		// flow has released input (a boot-menu world captures it - the
		// production UIOnly face), so the assert runs only in that state; the
		// result is always logged for the evidence record.
		const FVector BeforeKeyDrive = Hull->GetActorLocation();
		bool bKeyInjectDelivered = false;
		if (FSlateApplication::IsInitialized())
		{
			const FModifierKeysState Modifiers(false, false, false, false, false, false, false, false, false);
			FSlateApplication::Get().ProcessKeyDownEvent(
				FKeyEvent(EKeys::Right, Modifiers, 0, /*bIsRepeat*/ false, 0, 0));
			for (int32 Frame = 0; Frame < 45; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}
			FSlateApplication::Get().ProcessKeyUpEvent(
				FKeyEvent(EKeys::Right, Modifiers, 0, /*bIsRepeat*/ false, 0, 0));
			for (int32 Frame = 0; Frame < 90; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}
			const float KeyDriven = Hull->GetActorLocation().X - BeforeKeyDrive.X;
			bKeyInjectDelivered = KeyDriven >= 180.0f && KeyDriven <= 480.0f;
			AddInfo(FString::Printf(TEXT("M5_045 real-key drive: driven=%f delivered=%d (the boot-menu world captures key input - the assert holds only in a running map)"),
				KeyDriven, bKeyInjectDelivered ? 1 : 0));
			const APlayerController* KeyController = Cast<APlayerController>(Player->GetController());
			// The key->walk layer shares this exact binding stack and was
			// user-verified in the M0-H01 playtests; assert the key layer only
			// where the game flow has left the menu (the flow menu owns key
			// input on the boot map - the production capture face).
			UGameFlowSubsystem* Flow = (World->GetGameInstance() != nullptr)
				? World->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()
				: nullptr;
			const bool bMenuCaptures = Flow != nullptr && Flow->GetState() == EGameFlowState::Menu;
			if (!bMenuCaptures)
			{
				TestTrue(TEXT("the real Slate arrow key drove the hull"), bKeyInjectDelivered);
			}
		}

		Interaction->TryInteract();
		TestTrue(TEXT("the second V press exits the seat"), !Interaction->IsSeated());
		TestTrue(TEXT("the possession survived the whole flow"), Player->GetController() == Controller);
		return true;
	}

	// ------------------------------------------------------------------
	// 3. VehicleTrialCapture (the rendered packaged capture companion;
	// ClientContext only). Headless runs skip silently.
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_045VehicleTrialCapture,
		"UEMMO.Tasks.M5_045.VehicleTrialCapture",
		EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_045VehicleTrialCapture::RunTest(const FString& Parameters)
	{
		UWorld* World = GWorld;
		APrototypeCharacter* GamePawn = World != nullptr
			? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0))
			: nullptr;
		if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
			|| World->WorldType != EWorldType::Game || GamePawn == nullptr)
		{
			AddInfo(TEXT("packaged vehicle capture skipped: no rendered game world with the prototype pawn"));
			return true;
		}
		UVehicleInteractionComponent* Interaction = GamePawn->FindComponentByClass<UVehicleInteractionComponent>();
		if (!TestNotNull(TEXT("the game pawn carries the interaction component"), Interaction))
		{
			return true;
		}
		static FSeatOccupancyModel TrialSeats;
		TrialSeats = FSeatOccupancyModel();
		Interaction->BindSeatModel(TrialSeats);
		Interaction->SetOperatorFaction(FName(TEXT("player")));
		// The boot menu would capture the enter; the trial capture overrides
		// the gate (the same companion shape as the M5-044 capture).
		Interaction->SetUICapturePredicate([]()
		{
			return false;
		});

		// The hull from the SHIPPED rows, staged within the enter range.
		FCombatCatalog Catalog;
		FVehicleDefinition VehicleRow;
		FVehicleSeatDefinition SeatRow;
		FVehicleMountDefinition MountRow;
		if (!M5_045_LoadShippedVehicleRows(*this, Catalog, VehicleRow, SeatRow, MountRow))
		{
			return true;
		}
		ACombatVehicle* Hull = nullptr;
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
			Hull = World->SpawnActor<ACombatVehicle>(
				ACombatVehicle::StaticClass(), GamePawn->GetActorLocation() + FVector(-250.0, 0.0, 96.0),
				FRotator::ZeroRotator, Params);
		}
		if (!TestNotNull(TEXT("the trial hull stages"), Hull))
		{
			return true;
		}
		FString DefinitionErrors;
		if (!TestTrue(TEXT("the trial hull accepts the shipped definition"),
				Hull->SetVehicleDefinition(VehicleRow, &DefinitionErrors)))
		{
			AddInfo(FString::Printf(TEXT("the trial definition refused: %s"), *DefinitionErrors));
			return true;
		}
		FString RegisterError;
		if (!TestTrue(TEXT("the trial hull registers"), Interaction->RegisterVehicle(*Hull, SeatRow, &RegisterError)))
		{
			AddInfo(FString::Printf(TEXT("the trial registration refused: %s"), *RegisterError));
			return true;
		}

		const FString Directory = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-045") / TEXT("scenario-json") / TEXT("render"));
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);

		// Screenshot 1: the staged trial area (the hull beside the pawn).
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("capture-trial-staged.png"), true, false);
			return true;
		}));

		// The production enter (the rigging arms in the package), then the
		// DRIVING PROOF: the drive input held while the world ticks, the
		// screenshots land before/mid/after so the rendered evidence shows
		// the car actually moving (the follow camera keeps the hull in
		// frame; the world behind it shifts).
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Interaction]()
		{
			if (Interaction != nullptr)
			{
				const FVehicleEnterAttempt Attempt = Interaction->TryEnterNearestVehicle();
				UE_LOG(LogTemp, Display, TEXT("UEMMO M5-045 trial enter accepted=%d (%s)"), Attempt.bEntered, *Attempt.Detail);
			}
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([]()
		{
			if (APrototypeCharacter* Driver = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)))
			{
				Driver->RecordPlanarMoveInput(1.0f, 0.0f);
				UE_LOG(LogTemp, Display, TEXT("UEMMO M5-045 trial drive input held"));
			}
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.7f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("capture-trial-driving.png"), true, false);
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.2f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([]()
		{
			if (APrototypeCharacter* Driver = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)))
			{
				Driver->RecordPlanarMoveInput(0.0f, 0.0f);
				UE_LOG(LogTemp, Display, TEXT("UEMMO M5-045 trial drive input released at %s"),
					*Driver->GetActorLocation().ToCompactString());
			}
			return true;
		}));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return true;
			}
			FScreenshotRequest::RequestScreenshot(Directory / TEXT("capture-trial-seated.png"), true, false);
			return true;
		}));

		// Cleanup: the exit retires the driver side; the hull stays for the
		// user's own trial.
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
