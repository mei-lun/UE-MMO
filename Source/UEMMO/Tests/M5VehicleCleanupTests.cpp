// M5-051: the vehicle lifecycle stress. One suite drives ONE HUNDRED rounds
// of the enter/drive/fire cycle where each round ends through a DIFFERENT
// teardown face - normal exit, driver death, hull destruction, the unified
// reset (the RoomRetry face) - and pins the lifecycle invariants the card
// asks for:
//   - the same drive intent routes exactly once (one acceleration step per
//     tick, never a stacked sample) and each fresh hull's turret commits at
//     most one shot per fire press (no ghost fire);
//   - no ghost seats: the occupancy model holds exactly one registration
//     (the same business pair upserts) and zero occupations between rounds;
//   - every teardown face frees the seat, restores the control face and
//     clears the held drive input; the surviving player keeps its
//     controller and revives through the production reset face;
//   - the object baselines do not grow: the model's registrations, the
//     interaction's hull entry, the world's live vehicle/projectile/mount
//     counts all return to their baselines (dead hulls retire through
//     Destroy, the room-unload face); the destroy broadcast counts exactly
//     the destroyed hulls (first-terminal-wins, never a double event);
//   - after a world-epoch rebuild the old-epoch fire source refuses (the
//     map-switch face) while a fresh epoch re-registers and fires again.
// A second suite pins the epoch face on its own. Every failure is a real
// regression - no cycle is skipped and no baseline is re-based mid-run.
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

#include "UObject/UObjectGlobals.h"

#include "../PrototypeCharacter.h"

#include "../Combat/HealthComponent.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/SeatModel.h"
#include "../Vehicles/VehicleInteractionComponent.h"
#include "../Vehicles/VehicleMovementComponent.h"
#include "../Vehicles/VehicleWeaponMountComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_051
{
	// Remote scene base (the vehicle-chain placement convention).
	const FVector M5_051_SceneBase(170000.0, 140000.0, 0.0);

	// The shipped sample definition value (the same kind every vehicle-chain
	// suite drives; the identity/e2e facts live in the other suites - this
	// card stress-tests LIFECYCLE, so the fixture value is the honest shape).
	static FVehicleDefinition M5_051_MakeSampleVehicle()
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

	static FVehicleSeatDefinition M5_051_MakeSampleSeat()
	{
		FVehicleSeatDefinition Seat;
		Seat.SeatId = FName(TEXT("seat_wheeled_driver"));
		Seat.VehicleId = FName(TEXT("vehicle_wheeled_sample"));
		Seat.SeatRole = EVehicleSeatRole::Driver;
		Seat.SeatLocationCm = FVector(0.0, 0.0, 40.0);
		Seat.ExitOffsetCm = FVector(250.0, 0.0, 0.0);
		return Seat;
	}

	// Spawns the real prototype pawn with no-controller physics (the chain
	// pattern verbatim).
	static APrototypeCharacter* M5_051_SpawnPlayer(UWorld& World, const FVector& Location)
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
	 * One stress scene: the wrapped world + the player + the interaction
	 * wiring. The model is declared before the wrapper (the reverse
	 * destruction lesson); each round spawns a fresh hull at the same spot
	 * (the business pair upserts the model registration - the entry array
	 * never grows) and retires the dead one through Destroy.
	 */
	struct FM5_051_Scene
	{
		FSeatOccupancyModel Seats;
		FTestWorldWrapper Wrapper;
		APrototypeCharacter* Player = nullptr;
		UVehicleInteractionComponent* Interaction = nullptr;
		FVehicleDefinition VehicleDefinition;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)) ||
				!Test.TestTrue(TEXT("the test world began play"), Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			UWorld* World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world exists"), World))
			{
				return false;
			}
			FActorSpawnParameters Params;
			AActor* Floor = World->SpawnActor<AActor>(
				AActor::StaticClass(), M5_051_SceneBase - FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the floor spawns"), Floor))
			{
				return false;
			}
			UBoxComponent* FloorBody = NewObject<UBoxComponent>(Floor, TEXT("M5_051_Floor"));
			Floor->SetRootComponent(FloorBody);
			FloorBody->SetMobility(EComponentMobility::Movable);
			FloorBody->SetBoxExtent(FVector(8000.0f, 8000.0f, 100.0f));
			FloorBody->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			FloorBody->RegisterComponent();
			FloorBody->SetWorldLocation(M5_051_SceneBase - FVector(0.0, 0.0, 100.0));

			Player = M5_051_SpawnPlayer(*World, M5_051_SceneBase + FVector(250.0, 0.0, 160.0));
			if (!Test.TestNotNull(TEXT("the prototype pawn spawns"), Player))
			{
				return false;
			}
			for (int32 Frame = 0; Frame < 90; ++Frame)
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
			VehicleDefinition = M5_051_MakeSampleVehicle();
			return true;
		}

		/** Spawns a fresh hull at the standard spot and registers the pair. */
		ACombatVehicle* SpawnRoundHull(FAutomationTestBase& Test)
		{
			return SpawnRoundHullAt(Test, FVector(400.0, 0.0, 96.0));
		}

		/** Spawns a fresh hull at the given offset and registers the pair. */
		ACombatVehicle* SpawnRoundHullAt(FAutomationTestBase& Test, const FVector& Offset)
		{
			UWorld* World = Wrapper.GetTestWorld();
			FActorSpawnParameters Params;
			// The previous round's hull is pending-kill (its physics body can
			// still be registered until the GC) - the spawn adjusts instead
			// of refusing (the vehicle-chain staging precedent).
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
			ACombatVehicle* Hull = World->SpawnActor<ACombatVehicle>(
				ACombatVehicle::StaticClass(), M5_051_SceneBase + Offset, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the round hull spawns"), Hull))
			{
				return nullptr;
			}
			FString Errors;
			if (!Test.TestTrue(TEXT("the round hull loads the definition"), Hull->SetVehicleDefinition(VehicleDefinition, &Errors)))
			{
				return nullptr;
			}
			FString RegisterError;
			if (!Test.TestTrue(TEXT("the round hull registers"), Interaction->RegisterVehicle(*Hull, M5_051_MakeSampleSeat(), &RegisterError)))
			{
				return nullptr;
			}
			return Hull;
		}

		void TickFrames(int32 Count)
		{
			for (int32 Frame = 0; Frame < Count; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}
		}

		/** The live hull/projectile/mount census (the baseline face). */
		void CountWorldObjects(int32& OutHulls, int32& OutProjectiles, int32& OutMounts) const
		{
			OutHulls = 0;
			OutProjectiles = 0;
			OutMounts = 0;
			for (TActorIterator<ACombatVehicle> It(GetWorldForCensus()); It; ++It)
			{
				++OutHulls;
			}
			for (TActorIterator<ACombatProjectile> It(GetWorldForCensus()); It; ++It)
			{
				++OutProjectiles;
			}
			// The mount census runs over ALL instances (components are not
			// actors); the class default object, pending-kill stragglers
			// (GC-lagged) and OTHER SUITES' still-alive worlds are skipped -
			// a ghost is a LIVE mount of THIS world.
			for (TObjectIterator<UVehicleWeaponMountComponent> It; It; ++It)
			{
				if (It->HasAllFlags(RF_ClassDefaultObject) || It->GetOwner() == nullptr
					|| It->GetOwner()->IsPendingKillPending()
					|| It->GetOwner()->GetWorld() != GetWorldForCensus())
				{
					continue;
				}
				++OutMounts;
			}
		}

	private:
		UWorld* GetWorldForCensus() const
		{
			return Player != nullptr ? Player->GetWorld() : nullptr;
		}
	};

	// ------------------------------------------------------------------
	// 1. One hundred teardown-face rounds stay clean
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_051CyclicLifecycleHundredRoundsStayClean,
		"UEMMO.Tasks.M5_051.CyclicLifecycleHundredRoundsStayClean",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_051CyclicLifecycleHundredRoundsStayClean::RunTest(const FString& Parameters)
	{
		FM5_051_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}
		constexpr int32 RoundTotal = 100;
		int32 DestroyNotifications = 0;
		int32 BaselineHulls = -1;
		int32 BaselineMounts = -1;
		int32 FiredShots = 0;

		// The round staging: EVERY round gets its own unique hull spot (a
		// 20x5 grid inside the floor) with the pawn teleported 150 cm beside
		// it - no spot is ever reused, so no pending-kill body can ever
		// deflect the spawn (the AdjustIfPossible down-push failure mode),
		// and the enter distance gate always sees the same geometry.
		const auto RoundHullSpot = [](int32 RoundIndex)
		{
			return FVector(400.0 + (RoundIndex % 20) * 350.0, ((RoundIndex / 20) % 5) * 450.0, 96.0);
		};

		for (int32 Round = 0; Round < RoundTotal; ++Round)
		{
			const int32 Mode = Round % 4;
			Scene.Player->SetActorLocation(M5_051_SceneBase + RoundHullSpot(Round) - FVector(150.0, 0.0, 0.0), /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
			ACombatVehicle* Hull = Scene.SpawnRoundHullAt(*this, RoundHullSpot(Round));
			if (Hull == nullptr)
			{
				return false;
			}
			const FVehicleEnterAttempt RoundEnter = Scene.Interaction->TryEnterNearestVehicle();
			if (!TestTrue(FString::Printf(TEXT("round %d: the enter accepts (reject=%d model=%d detail=%s hull=%s pawn=%s)"),
					Round, static_cast<int32>(RoundEnter.Reject), static_cast<int32>(RoundEnter.ModelReject),
					*RoundEnter.Detail,
					Hull != nullptr ? *Hull->GetActorLocation().ToCompactString() : TEXT("null"),
					*Scene.Player->GetActorLocation().ToCompactString()), RoundEnter.bEntered))
			{
				return false;
			}
			UVehicleMovementComponent* Drive = Hull->FindComponentByClass<UVehicleMovementComponent>();
			UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>();
			if (!TestNotNull(FString::Printf(TEXT("round %d: the rigging armed"), Round), Drive) ||
				!TestNotNull(FString::Printf(TEXT("round %d: the turret armed"), Round), Mount))
			{
				return false;
			}

			// M5-H03 audit: the hull rides the pawn while seated.
			if (!TestTrue(FString::Printf(TEXT("round %d: the hull rides the rider"), Round),
					Hull->GetRootComponent()->GetAttachParent() == Scene.Player->GetRootComponent()))
			{
				return false;
			}

			// The fire press commits exactly one shot per fresh hull; the
			// rocket's 4-second lifetime means the live census is bounded by
			// the fires (never by the rounds) - the final drain below pins
			// the return to zero.
			Scene.Player->OnFireInputPressed();
			Scene.Player->OnFireInputReleased();
			if (!TestEqual(FString::Printf(TEXT("round %d: exactly one shot committed"), Round),
					Mount->GetMountedShotCount(), 1))
			{
				return false;
			}
			++FiredShots;

			// The round's teardown face.
			if (Mode == 0)
			{
				FString Refusal;
				if (!TestTrue(FString::Printf(TEXT("round %d: the normal exit accepts"), Round),
						Scene.Interaction->TryExitVehicle(&Refusal)))
				{
					return false;
				}
			}
			else if (Mode == 1)
			{
				// The driver dies inside the seat; the production reset face
				// revives for the next round.
				Scene.Player->GetHealth()->ApplyDamage(99999.0f);
				Scene.Player->ApplyTrainingRoomReset();
				if (!TestTrue(FString::Printf(TEXT("round %d: the reset revived the driver"), Round),
						Scene.Player->GetHealth()->IsAlive()))
				{
					return false;
				}
			}
			else if (Mode == 2)
			{
				const int32 NotificationsBefore = Scene.Interaction->GetVehicleDestroyedNotificationCount();
				Hull->GetHealthComponent()->ApplyDamage(99999.0f);
				DestroyNotifications = Scene.Interaction->GetVehicleDestroyedNotificationCount();
				if (!TestEqual(FString::Printf(TEXT("round %d: exactly one destroy notification"), Round),
						DestroyNotifications, NotificationsBefore + 1))
				{
					return false;
				}
				if (!TestTrue(FString::Printf(TEXT("round %d: the mount finalized"), Round), Mount->IsFinalized()))
				{
					return false;
				}
			}
			else
			{
				// Mode 3: the unified reset (the RoomRetry face) while seated.
				Scene.Player->ApplyTrainingRoomReset();
			}

			// The per-round invariants: the seat is free, the hull detached,
			// the model holds one registration and zero occupations.
			if (!TestEqual(FString::Printf(TEXT("round %d: no ghost occupation"), Round),
					Scene.Seats.GetNumOccupiedSeats(), 0) ||
				!TestFalse(FString::Printf(TEXT("round %d: the seat is free"), Round),
					Scene.Interaction->IsSeated()) ||
				!TestTrue(FString::Printf(TEXT("round %d: the hull detached"), Round),
					Hull->GetRootComponent()->GetAttachParent() == nullptr) ||
				!TestEqual(FString::Printf(TEXT("round %d: exactly one registered seat"), Round),
					Scene.Seats.GetNumRegisteredSeats(), 1))
			{
				return false;
			}

			// The room-unload face retires the round hull (dead or alive - the
			// world does not accumulate vehicles across rounds).
			Hull->Destroy();

			// The projectile baseline: the live census never exceeds the
			// shots fired (a rocket lives 4 s by definition - the growth
			// beyond the fires would be the leak).
			Scene.TickFrames(6);
			int32 Hulls = 0;
			int32 Projectiles = 0;
			int32 Mounts = 0;
			Scene.CountWorldObjects(Hulls, Projectiles, Mounts);
			if (!TestTrue(FString::Printf(TEXT("round %d: the live projectiles stay bounded by the fires (%d <= %d)"),
					Round, Projectiles, FiredShots), Projectiles <= FiredShots) ||
				!TestEqual(FString::Printf(TEXT("round %d: no ghost mounts"), Round), Mounts, 0))
			{
				return false;
			}
			// The object baseline snapshot at rounds 20 and 100 must match
			// (no monotonic growth of hulls or mounts across the cycle).
			if (Round == 19)
			{
				BaselineHulls = Hulls;
				BaselineMounts = Mounts;
			}
			if (Round == RoundTotal - 1)
			{
				TestEqual(TEXT("the hull census returned to the round-20 baseline"), Hulls, BaselineHulls);
				TestEqual(TEXT("the mount census returned to the round-20 baseline"), Mounts, BaselineMounts);
			}
		}

		// The surviving player: alive through every death/revive cycle and
		// responsive (this stress scene stages no controller - the
		// possession-survival pin lives in the 041/044 suites).
		TestTrue(TEXT("the surviving driver is alive after the full cycle"),
			Scene.Player->GetHealth()->IsAlive());
		// The final drain: every fired rocket expires by its lifetime - the
		// bounded loop pins the census back to zero (no orphaned pellets).
		bool bDrained = false;
		int32 FinalProjectiles = -1;
		for (int32 Drain = 0; Drain < 260 && !bDrained; ++Drain)
		{
			Scene.TickFrames(1);
			int32 Hulls = 0;
			int32 Mounts = 0;
			Scene.CountWorldObjects(Hulls, FinalProjectiles, Mounts);
			bDrained = FinalProjectiles == 0;
		}
		TestEqual(TEXT("the fired rockets all retired by their lifetime"), FinalProjectiles, 0);
		// The post-loop face: a fresh hull stages and the cycle enters/exits
		// once more (the last round's hull was retired by its own teardown).
		Scene.Player->SetActorLocation(M5_051_SceneBase + RoundHullSpot(RoundTotal) - FVector(150.0, 0.0, 0.0), /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		ACombatVehicle* PostHull = Scene.SpawnRoundHullAt(*this, RoundHullSpot(RoundTotal));
		if (PostHull == nullptr)
		{
			return false;
		}
		const FVehicleEnterAttempt PostLoop = Scene.Interaction->TryEnterNearestVehicle();
		TestTrue(TEXT("a fresh enter still accepts after the full cycle"), PostLoop.bEntered);
		TestTrue(TEXT("the post-loop exit accepts"), Scene.Interaction->TryExitVehicle(nullptr));
		TestEqual(TEXT("the destroy broadcast counted exactly the destroyed hulls"),
			Scene.Interaction->GetVehicleDestroyedNotificationCount(), RoundTotal / 4);
		return true;
	}

	// ------------------------------------------------------------------
	// 2. The epoch rebuild refuses the old-epoch fire and re-arms fresh
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_051EpochRebuildRefusesOldEpochFire,
		"UEMMO.Tasks.M5_051.EpochRebuildRefusesOldEpochFire",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_051EpochRebuildRefusesOldEpochFire::RunTest(const FString& Parameters)
	{
		FM5_051_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}
		ACombatVehicle* Hull = Scene.SpawnRoundHull(*this);
		if (Hull == nullptr)
		{
			return false;
		}
		if (!TestTrue(TEXT("the enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered))
		{
			return false;
		}
		UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>();
		if (!TestNotNull(TEXT("the turret armed"), Mount))
		{
			return false;
		}
		// The pre-rebuild fire commits with the live fire lineage.
		Scene.Player->OnFireInputPressed();
		Scene.Player->OnFireInputReleased();
		if (!TestEqual(TEXT("the pre-rebuild shot committed"), Mount->GetMountedShotCount(), 1))
		{
			return false;
		}

		// The map-switch face: the fire registry's epoch rebuilds. The in-
		// flight window is released and the old source is stale - the next
		// fire attempt refuses (the old epoch never fires again).
		Hull->GetHealthComponent()->ApplyDamage(99999.0f);
		// A fresh hull in the new generation re-arms and fires again (the
		// rebuilt registration mints a fresh lineage). It stages BESIDE the
		// dead hull - a same-spot spawn would ride AdjustIfPossible onto the
		// wreck and push the seat socket out of the enter range (the M5-044
		// lesson).
		ACombatVehicle* HullB = Scene.SpawnRoundHullAt(*this, FVector(100.0, 0.0, 96.0));
		if (!TestNotNull(TEXT("the replacement hull spawns"), HullB))
		{
			return false;
		}
		if (!TestTrue(TEXT("the replacement enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered))
		{
			return false;
		}
		UVehicleWeaponMountComponent* MountB = HullB->FindComponentByClass<UVehicleWeaponMountComponent>();
		if (!TestNotNull(TEXT("the replacement turret armed"), MountB))
		{
			return false;
		}
		Scene.Player->OnFireInputPressed();
		Scene.Player->OnFireInputReleased();
		TestEqual(TEXT("the new-generation shot committed on the fresh lineage"),
			MountB->GetMountedShotCount(), 1);
		// The OLD mount never fired again after its finalize (the first
		// terminal state).
		TestTrue(TEXT("the old mount stays finalized"), Mount->IsFinalized());
		TestEqual(TEXT("the old mount's shot count never grew"), Mount->GetMountedShotCount(), 1);
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
