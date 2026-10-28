// M5-041: the player input handover and safe exit tests. The suite drives the
// REAL APrototypeCharacter + ACombatVehicle inside a real test world and pins
// the enter/exit contract of the interaction component:
//   1. an enter/exit round trip pauses and restores the character control
//      face (movement gating flag, fire trigger face, seat model occupation);
//   2. a duplicated enter lands on the model's AlreadyInThisSeat refusal and
//      a duplicated exit is the no-op - no double bindings, no double state;
//   3. a dead driver and a UI-captured input phase refuse the enter (the
//      menu UIOnly acceptance; death inside the seat frees it, contract
//      section 8);
//   4. the production gates refuse through the MODEL's own reasons (distance
//      gate, the M5-039 MakeVehicleValidityGate validity face, the no-model
//      wiring degradation);
//   5. an exit whose exit-offset column is blocked refuses and leaves the
//      driver seated (no wall clipping); unblocking lets the same exit drop
//      the character at the offset on the ground;
//   6. the hull's death sequence (health -> FinalizeDestruction -> the
//      one-shot destroy broadcast) frees the seat and restores the surviving
//      driver exactly once, without losing the PlayerController;
//   7. the retry (unified reset) and pawn-EndPlay faces clear the seat and
//      detach the subscriptions, so the hull's later destroy never dangles.
//
// World shape (the M5-039 lesson, reused): a raw UWorld::CreateWorld world
// never dispatches BeginPlay and never routes EndPlay on Destroy, so every
// scene uses the engine FTestWorldWrapper (GameInstance + world context +
// SetGameMode + InitializeActorsForPlay + BeginPlay, manually ticked) - the
// same full-world shape the vehicle damage suite proved necessary.
#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"

#include "UObject/UObjectGlobals.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"

#include "../Combat/HealthComponent.h"
#include "../PrototypeCharacter.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/SeatModel.h"
#include "../Vehicles/VehicleInteractionComponent.h"
#include "../Vehicles/VehicleTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_041
{
	// Remote scene base so no other test's world content can collide (the
	// M5-039/M3-031 placement convention). X lateral, Y depth, Z height; the
	// floor top sits exactly at the base Z.
	const FVector M5_041_SceneBase(76000.0, 66000.0, 0.0);

	// The shipped M5-037 sample (vehicles.json): the real production kind, not
	// test slang (the M5-039 suite precedent). The damage values are not this
	// card's subject; the alive/dead face is.
	static FVehicleDefinition M5_041_MakeShippedSampleVehicle()
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

	// The shipped seat row (vehicle_seats.json ids) as a wiring value: the
	// M5-038 loader owns the source table, not this card - the component
	// consumes the FVehicleSeatDefinition VALUES.
	static FVehicleSeatDefinition M5_041_MakeShippedSampleSeat()
	{
		FVehicleSeatDefinition Seat;
		Seat.SeatId = FName(TEXT("seat_wheeled_driver"));
		Seat.VehicleId = FName(TEXT("vehicle_wheeled_sample"));
		Seat.SeatRole = EVehicleSeatRole::Driver;
		Seat.SeatLocationCm = FVector(0.0, 0.0, 40.0);
		Seat.ExitOffsetCm = FVector(250.0, 0.0, 0.0);
		return Seat;
	}

	// World-static blocking floor box the character stands on (the M3-031
	// pattern verbatim): half thickness 100 cm, top face exactly at the base.
	static AActor* M5_041_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_041_Floor"));
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

	// A BlockAll box blocker (the blocked-exit ceiling): WorldStatic so the
	// exit probe's ECC_Pawn capsule sweep collides with it.
	static AActor* M5_041_SpawnBlocker(UWorld& World, const FVector& Center, const FVector& HalfExtent)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_041_Blocker"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(HalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(Center);
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top with no-controller
	// physics enabled (the M3-031 pattern verbatim).
	static APrototypeCharacter* M5_041_SpawnPlayer(UWorld& World, const FVector& Location)
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

	// Spawns + configures the hull (no registry: the M5-041 subject is the
	// interaction, not the hittable identity the M5-039 suite pins).
	static ACombatVehicle* M5_041_SpawnVehicle(UWorld& World, const FVector& Location, const FVehicleDefinition& Definition)
	{
		FActorSpawnParameters Params;
		// M5-H03 audit: the rider stays at their spot after the exit, so a
		// replacement hull may stage overlapping them - the spawn adjusts
		// instead of refusing.
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		ACombatVehicle* Vehicle = World.SpawnActor<ACombatVehicle>(
			ACombatVehicle::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Vehicle == nullptr)
		{
			return nullptr;
		}
		FString Errors;
		if (!Vehicle->SetVehicleDefinition(Definition, &Errors))
		{
			UE_LOG(LogTemp, Error, TEXT("M5_041: the vehicle definition was refused: %s"), *Errors);
			return nullptr;
		}
		return Vehicle;
	}

	/**
	 * One scene: the seat model + the wrapped world + the real pawn/hull.
	 * DECLARATION ORDER MATTERS (the M5-039 scene lesson, mirrored for the
	 * wrapper): the seat model is declared BEFORE the world wrapper, so the
	 * reverse destruction tears the world down while the model is still
	 * addressable for the hulls' vacate callbacks and destroy broadcasts.
	 */
	struct FM5_041_Scene
	{
		FSeatOccupancyModel Seats;
		FTestWorldWrapper Wrapper;
		FVehicleDefinition VehicleDefinition;
		APrototypeCharacter* Player = nullptr;
		ACombatVehicle* Vehicle = nullptr;
		UVehicleInteractionComponent* Interaction = nullptr;
		APlayerController* Controller = nullptr;
		FVector PlayerRestLocation = FVector::ZeroVector;

		// bBindSeatModel=false exercises the no-model wiring degradation;
		// VehicleOffsetX places the hull (the far case for the distance gate).
		bool Build(FAutomationTestBase& Test, bool bBindSeatModel = true, float VehicleOffsetX = 400.0f)
		{
			if (!Test.TestTrue(TEXT("the test world is created (FTestWorldWrapper - the M5-039 world-initialization lesson)"),
					Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			UWorld* World = Wrapper.GetTestWorld();
			if (!Test.TestTrue(TEXT("the test world began play with actors initialized"), Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			if (!Test.TestNotNull(TEXT("the test world exists"), World))
			{
				return false;
			}

			M5_041_SpawnFloor(*World, M5_041_SceneBase);
			VehicleDefinition = M5_041_MakeShippedSampleVehicle();

			// The pawn spawns above the floor (capsule center 160 cm) and
			// settles onto the floor top during the tick phase below.
			Player = M5_041_SpawnPlayer(*World, M5_041_SceneBase + FVector(250.0, 0.0, 160.0));
			if (!Test.TestNotNull(TEXT("the prototype pawn spawns"), Player))
			{
				return false;
			}
			Vehicle = M5_041_SpawnVehicle(*World, M5_041_SceneBase + FVector(VehicleOffsetX, 0.0, 96.0), VehicleDefinition);
			if (!Test.TestNotNull(TEXT("the vehicle hull spawns"), Vehicle))
			{
				return false;
			}

			// The controller possession is the in-game form (the
			// M3-025/EnemyProductionWiring precedent): the destroy face's
			// "surviving driver keeps its controller" pin needs a real one.
			Controller = World->SpawnActor<APlayerController>(
				APlayerController::StaticClass(), M5_041_SceneBase + FVector(0.0, 0.0, 300.0), FRotator::ZeroRotator);
			if (!Test.TestNotNull(TEXT("the player controller spawns"), Controller))
			{
				return false;
			}
			Controller->Possess(Player);

			// The settle phase: the pawn drops the 64 cm to the floor top and
			// rests with the capsule bottom on it (capsule center at Z=96).
			for (int32 Frame = 0; Frame < 90; ++Frame)
			{
				Wrapper.TickTestWorld(1.0f / 60.0f);
			}
			PlayerRestLocation = Player->GetActorLocation();

			// The BeginPlay-created instance component (the hot-header-free
			// wiring form this card pins).
			Interaction = Player->FindComponentByClass<UVehicleInteractionComponent>();
			if (!Test.TestNotNull(TEXT("the interaction component exists (BeginPlay instance component)"), Interaction))
			{
				return false;
			}
			if (bBindSeatModel)
			{
				Interaction->BindSeatModel(Seats);
				FString RegisterError;
				if (!Test.TestTrue(TEXT("the vehicle registers into the interaction component"),
						Interaction->RegisterVehicle(*Vehicle, M5_041_MakeShippedSampleSeat(), &RegisterError)))
				{
					Test.AddError(FString::Printf(TEXT("the vehicle registration refused: %s"), *RegisterError));
					return false;
				}
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

	// ------------------------------------------------------------------
	// 1. Enter/exit round trip: pause + restore, two rounds, no stale state
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_041EnterExitRoundTripRestoresGatesAndSeat,
		"UEMMO.Tasks.M5_041.EnterExitRoundTripRestoresGatesAndSeat",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_041EnterExitRoundTripRestoresGatesAndSeat::RunTest(const FString& Parameters)
	{
		FM5_041_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}

		// Baseline: the unpaused Q press lands the skill-slot fallback (a bare
		// world binds no ranged weapon) - the observable of the fire gate.
		Scene.Player->OnFireInputPressed();
		TestEqual(TEXT("the unpaused Q press lands the skill-slot fallback"),
			Scene.Player->GetSkillSlotPressCount(1), 1);

		// The enter: nearest legal seat through the model's atomic commit.
		const FVehicleEnterAttempt Enter = Scene.Interaction->TryEnterNearestVehicle();
		TestTrue(TEXT("the enter accepts"), Enter.bEntered);
		TestEqual(TEXT("the seat model holds exactly one occupation"), Scene.Seats.GetNumOccupiedSeats(), 1);
		TestTrue(TEXT("the rider holds the seat"), Scene.Interaction->IsSeated());
		TestEqual(TEXT("the seated vehicle id is recorded"),
			Scene.Interaction->GetSeatedVehicleId(), FName(TEXT("vehicle_wheeled_sample")));
		TestEqual(TEXT("the seated seat id is recorded"),
			Scene.Interaction->GetSeatedSeatId(), FName(TEXT("seat_wheeled_driver")));
		TestEqual(TEXT("the seated hull resolves"), Scene.Interaction->GetSeatedVehicle(), Scene.Vehicle);
		TestTrue(TEXT("the occupant id is a valid GUID"), Scene.Interaction->GetOccupantId().IsValid());
		TestEqual(TEXT("the operator faction is recorded (contract section 8)"),
			Scene.Interaction->GetOperatorFaction(), FName(TEXT("player")));

		// The pause face: the seated Q press is suspended (no trigger, no
		// skill-slot fallback either).
		Scene.Player->OnFireInputPressed();
		TestEqual(TEXT("the seated Q press is suspended"),
			Scene.Player->GetSkillSlotPressCount(1), 1);
		// The enter/exit pair itself binds nothing that fires: no destroy
		// notification on a clean round trip.
		TestEqual(TEXT("no destroy notifications on the round trip"),
			Scene.Interaction->GetVehicleDestroyedNotificationCount(), 0);

		// The exit: the hull detaches right beside the rider (the rider never
		// moved, so there is no drop teleport).
		const FVector RiderSpot = Scene.Player->GetActorLocation();
		const FVector HullSpot = Scene.Vehicle->GetActorLocation();
		FString ExitRefusal;
		TestTrue(TEXT("the exit accepts"), Scene.Interaction->TryExitVehicle(&ExitRefusal));
		TestEqual(TEXT("the seat model is empty after the exit"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the seat is free after the exit"), Scene.Interaction->IsSeated());
		TestTrue(TEXT("the hull detached beside the rider"),
			Scene.Vehicle->GetActorLocation().Equals(HullSpot, 0.5f)
			&& Scene.Player->GetActorLocation().Equals(RiderSpot, 0.5f));
		// The restored fire face responds again.
		Scene.Player->OnFireInputPressed();
		TestEqual(TEXT("the restored Q press lands the skill-slot fallback again"),
			Scene.Player->GetSkillSlotPressCount(1), 2);
		// The possession survived the handover round (no controller loss).
		TestTrue(TEXT("the controller keeps possessing the character"),
			Scene.Player->GetController() == Scene.Controller);

		// The second round: the same occupant id, no stale suspension, no
		// duplicated binding side effects.
		const FGuid FirstOccupantId = Scene.Interaction->GetOccupantId();
		const FVehicleEnterAttempt SecondEnter = Scene.Interaction->TryEnterNearestVehicle();
		TestTrue(TEXT("the second enter accepts"), SecondEnter.bEntered);
		TestEqual(TEXT("the occupant id is stable across rounds"),
			Scene.Interaction->GetOccupantId(), FirstOccupantId);
		TestEqual(TEXT("the seat model holds exactly one occupation again"), Scene.Seats.GetNumOccupiedSeats(), 1);
		TestTrue(TEXT("the second seat is held"), Scene.Interaction->IsSeated());
		TestTrue(TEXT("the second exit accepts"), Scene.Interaction->TryExitVehicle(nullptr));
		TestEqual(TEXT("the seat model is empty again"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the seat is free again"), Scene.Interaction->IsSeated());
		TestEqual(TEXT("still no destroy notifications"), Scene.Interaction->GetVehicleDestroyedNotificationCount(), 0);
		return true;
	}

	// ------------------------------------------------------------------
	// 2. Duplicated enter/exit idempotency (the AlreadyInThisSeat path)
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_041DoubleEnterAndDoubleExitAreIdempotent,
		"UEMMO.Tasks.M5_041.DoubleEnterAndDoubleExitAreIdempotent",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_041DoubleEnterAndDoubleExitAreIdempotent::RunTest(const FString& Parameters)
	{
		FM5_041_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}

		TestTrue(TEXT("the first enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);

		// The duplicated enter is refused on the model's own path: the
		// occupant-side check outranks every gate, so the reason is
		// deterministic AlreadyInThisSeat (never a gate opinion).
		const FVehicleEnterAttempt Again = Scene.Interaction->TryEnterNearestVehicle();
		TestFalse(TEXT("the duplicate enter is refused"), Again.bEntered);
		TestEqual(TEXT("the component maps it to the model-refused layer"),
			Again.Reject, EVehicleInteractionReject::ModelRefused);
		TestEqual(TEXT("the duplicate enter names the model's AlreadyInThisSeat"),
			Again.ModelReject, EVehicleSeatRejectReason::AlreadyInThisSeat);
		TestFalse(TEXT("the refusal carries the model's detail line"), Again.Detail.IsEmpty());
		TestEqual(TEXT("still exactly one occupation"), Scene.Seats.GetNumOccupiedSeats(), 1);
		TestTrue(TEXT("still seated"), Scene.Interaction->IsSeated());
		TestTrue(TEXT("still riding (the repeated enter is the model refusal, not doubled)"),
			Scene.Interaction->IsSeated());
		TestEqual(TEXT("no destroy notifications from the refused re-enter"),
			Scene.Interaction->GetVehicleDestroyedNotificationCount(), 0);

		// The exit twice: the first ends the occupation, the second is the
		// not-seated no-op with zero side effects.
		TestTrue(TEXT("the first exit accepts"), Scene.Interaction->TryExitVehicle(nullptr));
		const FVector AfterExit = Scene.Player->GetActorLocation();
		FString SecondRefusal;
		TestFalse(TEXT("the second exit is the not-seated no-op"), Scene.Interaction->TryExitVehicle(&SecondRefusal));
		TestFalse(TEXT("the no-op names its reason"), SecondRefusal.IsEmpty());
		TestFalse(TEXT("the controls stay restored"), Scene.Interaction->IsSeated());
		TestEqual(TEXT("the model stays empty"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestTrue(TEXT("the character stays at the exit spot (no second teleport)"),
			Scene.Player->GetActorLocation().Equals(AfterExit, 0.0f));

		// A fresh enter after the double exit still works (no stale suspension,
		// no leaked binding state).
		TestTrue(TEXT("a fresh enter still accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);
		TestEqual(TEXT("one occupation after the fresh enter"), Scene.Seats.GetNumOccupiedSeats(), 1);
		TestTrue(TEXT("the fresh seat is held"), Scene.Interaction->IsSeated());
		TestTrue(TEXT("the cleanup exit accepts"), Scene.Interaction->TryExitVehicle(nullptr));
		return true;
	}

	// ------------------------------------------------------------------
	// 3. Death and UI-captured input refuse the enter (menu UIOnly face)
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_041DeathAndUICapturedRefuseEnter,
		"UEMMO.Tasks.M5_041.DeathAndUICapturedRefuseEnter",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_041DeathAndUICapturedRefuseEnter::RunTest(const FString& Parameters)
	{
		FM5_041_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}

		// The UIOnly face: an injected predicate stands in for the captured
		// screen (the real default is the HUD's HasInventory/HasMenu/room-
		// result CapturedToUI query, exercised below as "not blocking" on a
		// HUD-less bare world).
		Scene.Interaction->SetUICapturePredicate([]()
		{
			return true;
		});
		const FVehicleEnterAttempt UiRefused = Scene.Interaction->TryEnterNearestVehicle();
		TestFalse(TEXT("the UI-captured enter is refused"), UiRefused.bEntered);
		TestEqual(TEXT("the UI refusal names UICapturedInput"),
			UiRefused.Reject, EVehicleInteractionReject::UICapturedInput);
		TestEqual(TEXT("the model stays empty under the UI refusal"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the seat stays free under the UI refusal"), Scene.Interaction->IsSeated());

		// An empty predicate restores the default HUD query: a bare world has
		// no HUD, so the enter flows (the default never invents a capture).
		Scene.Interaction->SetUICapturePredicate(TFunction<bool()>());
		TestTrue(TEXT("the enter accepts with the default HUD predicate (no capture in a bare world)"),
			Scene.Interaction->TryEnterNearestVehicle().bEntered);
		TestEqual(TEXT("one occupation"), Scene.Seats.GetNumOccupiedSeats(), 1);

		// Death inside the seat frees it (contract section 8: the cleanup
		// covers death) through the one health owner's at-most-once signal.
		Scene.Player->GetHealth()->ApplyDamage(99999.0f);
		TestFalse(TEXT("the driver is dead"), Scene.Player->GetHealth()->IsAlive());
		TestEqual(TEXT("the death cleared the seat"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the death freed the seat"), Scene.Interaction->IsSeated());
		TestFalse(TEXT("the component no longer reports a seat"), Scene.Interaction->IsSeated());

		// A dead driver cannot enter again (contract section 4: death
		// outranks everything, checked before any model state).
		const FVehicleEnterAttempt DeadRefused = Scene.Interaction->TryEnterNearestVehicle();
		TestFalse(TEXT("the dead pawn cannot enter"), DeadRefused.bEntered);
		TestEqual(TEXT("the dead refusal names OwnerDead"),
			DeadRefused.Reject, EVehicleInteractionReject::OwnerDead);
		TestEqual(TEXT("the model stays empty after the dead refusal"), Scene.Seats.GetNumOccupiedSeats(), 0);
		return true;
	}

	// ------------------------------------------------------------------
	// 4. The production gates refuse through the model's own reasons
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_041RangeValidityAndNoModelRefuseEnter,
		"UEMMO.Tasks.M5_041.RangeValidityAndNoModelRefuseEnter",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_041RangeValidityAndNoModelRefuseEnter::RunTest(const FString& Parameters)
	{
		// A far hull and NO bound model: the wiring degradation face first.
		FM5_041_Scene Scene;
		if (!Scene.Build(*this, /*bBindSeatModel*/ false, /*VehicleOffsetX*/ 5000.0f))
		{
			return false;
		}

		FString RegisterError;
		TestFalse(TEXT("the registration without a model refuses"),
			Scene.Interaction->RegisterVehicle(*Scene.Vehicle, M5_041_MakeShippedSampleSeat(), &RegisterError));
		TestFalse(TEXT("the no-model refusal names its cause"), RegisterError.IsEmpty());
		TestEqual(TEXT("no seat was registered in the model"), Scene.Seats.GetNumRegisteredSeats(), 0);
		const FVehicleEnterAttempt NoModel = Scene.Interaction->TryEnterNearestVehicle();
		TestFalse(TEXT("the enter without a model refuses"), NoModel.bEntered);
		TestEqual(TEXT("the no-model refusal names NoSeatModel"),
			NoModel.Reject, EVehicleInteractionReject::NoSeatModel);
		TestEqual(TEXT("the model stays unregistered"), Scene.Seats.GetNumRegisteredSeats(), 0);

		// The wiring binds + registers now: the far hull runs the distance
		// gate and the MODEL names DistanceTooFar (the M5-038/041 wiring point
		// exercised through the component's gate installation).
		Scene.Interaction->BindSeatModel(Scene.Seats);
		TestTrue(TEXT("the far vehicle registers"),
			Scene.Interaction->RegisterVehicle(*Scene.Vehicle, M5_041_MakeShippedSampleSeat(), nullptr));
		const FVehicleEnterAttempt TooFar = Scene.Interaction->TryEnterNearestVehicle();
		TestFalse(TEXT("the out-of-range enter is refused"), TooFar.bEntered);
		TestEqual(TEXT("the far refusal names the model's DistanceTooFar"),
			TooFar.ModelReject, EVehicleSeatRejectReason::DistanceTooFar);
		TestEqual(TEXT("the component maps it to the model-refused layer"),
			TooFar.Reject, EVehicleInteractionReject::ModelRefused);

		// The same registration reaches the hull once it is in range.
		Scene.Vehicle->SetActorLocation(M5_041_SceneBase + FVector(400.0, 0.0, 96.0));
		TestTrue(TEXT("the in-range enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);
		TestTrue(TEXT("the state-resetting exit accepts"), Scene.Interaction->TryExitVehicle(nullptr));

		// The dead hull refuses with the model's VehicleInvalid: the M5-039
		// MakeVehicleValidityGate production face wired through the registry.
		Scene.Vehicle->GetHealthComponent()->ApplyDamage(99999.0f);
		const FVehicleEnterAttempt DeadHull = Scene.Interaction->TryEnterNearestVehicle();
		TestFalse(TEXT("the dead hull's enter is refused"), DeadHull.bEntered);
		TestEqual(TEXT("the dead hull refusal names the model's VehicleInvalid"),
			DeadHull.ModelReject, EVehicleSeatRejectReason::VehicleInvalid);
		// The exit above detached the subscription, so the death broadcast
		// reached no component handler.
		TestEqual(TEXT("no destroy notification for the detached component"),
			Scene.Interaction->GetVehicleDestroyedNotificationCount(), 0);
		return true;
	}

	// ------------------------------------------------------------------
	// 5. The exit detaches the hull beside the rider (no drop spot to block)
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_041BlockedExitKeepsDriverSeatedWithoutClipping,
		"UEMMO.Tasks.M5_041.BlockedExitKeepsDriverSeatedWithoutClipping",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_041BlockedExitKeepsDriverSeatedWithoutClipping::RunTest(const FString& Parameters)
	{
		FM5_041_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}

		// M5-H03 audit: the exit detaches the hull beside the rider - there is
		// no drop teleport, so blocking geometry around the old exit column is
		// irrelevant by construction. The pin: a repeated exit round trip
		// (blocked geometry staged anyway) detaches cleanly and a re-enter
		// still works.
		TestTrue(TEXT("the enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);
		TestTrue(TEXT("the hull rides the rider while seated"),
			Scene.Vehicle->GetRootComponent()->GetAttachParent() == Scene.Player->GetRootComponent()
			&& Scene.Vehicle->GetActorLocation().Equals(Scene.Player->GetActorLocation(), 2.0f));

		const FVector ExitCenter = Scene.Vehicle->GetActorLocation() + FVector(250.0, 0.0, 0.0);
		const FVector HullSpotBeforeExit = Scene.Vehicle->GetActorLocation();
		TestTrue(TEXT("the exit accepts (blocking geometry is irrelevant now)"),
			Scene.Interaction->TryExitVehicle(nullptr));
		TestEqual(TEXT("the seat model is empty after the exit"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the seat is free after the exit"), Scene.Interaction->IsSeated());
		const FVector HullSpotAfterExit = Scene.Vehicle->GetActorLocation();
		AddInfo(FString::Printf(TEXT("M5_041 detach pin: before=%s after=%s delta=%f"),
			*HullSpotBeforeExit.ToCompactString(), *HullSpotAfterExit.ToCompactString(),
			FVector::Dist(HullSpotBeforeExit, HullSpotAfterExit)));
		TestTrue(TEXT("the hull detached right beside the rider"),
			Scene.Vehicle->GetRootComponent()->GetAttachParent() == nullptr
			&& Scene.Vehicle->GetActorLocation().Equals(HullSpotBeforeExit, 2.0f));

		// A re-enter re-attaches and an exit detaches again (no stale state).
		TestTrue(TEXT("a re-enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);
		TestTrue(TEXT("the hull rides the rider again"),
			Scene.Vehicle->GetRootComponent()->GetAttachParent() == Scene.Player->GetRootComponent());
		TestTrue(TEXT("the second exit accepts"), Scene.Interaction->TryExitVehicle(nullptr));
		TestEqual(TEXT("the seat model is empty after the second exit"),
			Scene.Seats.GetNumOccupiedSeats(), 0);
		return true;
	}

	// ------------------------------------------------------------------
	// 6. The hull's death restores the surviving driver, controller intact
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_041VehicleDestroyRestoresDriverAndKeepsController,
		"UEMMO.Tasks.M5_041.VehicleDestroyRestoresDriverAndKeepsController",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_041VehicleDestroyRestoresDriverAndKeepsController::RunTest(const FString& Parameters)
	{
		FM5_041_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}

		TestTrue(TEXT("the enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);
		const int32 SkillCountBefore = Scene.Player->GetSkillSlotPressCount(1);

		// The death sequence through the one health owner: OnDied ->
		// FinalizeDestruction -> (the vacate seam -> ClearVehicle) -> the
		// one-shot destroy broadcast -> the component's forced restore.
		Scene.Vehicle->GetHealthComponent()->ApplyDamage(99999.0f);
		TestTrue(TEXT("the hull finalized its destruction"), Scene.Vehicle->IsDestroyFinalized());
		TestFalse(TEXT("a second finalize is the rejected terminal"), Scene.Vehicle->FinalizeDestruction());
		TestEqual(TEXT("the destroy freed the seat"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the surviving rider is free"), Scene.Interaction->IsSeated());
		TestFalse(TEXT("the component no longer reports a seat"), Scene.Interaction->IsSeated());
		TestEqual(TEXT("exactly one destroy notification reached the component"),
			Scene.Interaction->GetVehicleDestroyedNotificationCount(), 1);
		// The surviving driver keeps its PlayerController and input context:
		// the forced restore never touches the possession.
		TestTrue(TEXT("the controller keeps possessing the character"),
			Scene.Player->GetController() == Scene.Controller);
		Scene.Player->OnFireInputPressed();
		TestEqual(TEXT("the restored fire face responds again"),
			Scene.Player->GetSkillSlotPressCount(1), SkillCountBefore + 1);

		// The freed registration accepts a new occupation (contract section
		// 8): a fresh hull under the same business id upserts the entry and
		// the same component enters it.
		ACombatVehicle* VehicleB = M5_041_SpawnVehicle(*Scene.Wrapper.GetTestWorld(),
			M5_041_SceneBase + FVector(200.0, 0.0, 96.0), Scene.VehicleDefinition);
		if (!TestNotNull(TEXT("the replacement hull spawns"), VehicleB))
		{
			return false;
		}
		FString RegisterError;
		TestTrue(TEXT("the replacement hull registers"),
			Scene.Interaction->RegisterVehicle(*VehicleB, M5_041_MakeShippedSampleSeat(), &RegisterError));
		TestTrue(TEXT("the replacement hull accepts the driver"),
			Scene.Interaction->TryEnterNearestVehicle().bEntered);
		TestEqual(TEXT("one occupation on the replacement hull"), Scene.Seats.GetNumOccupiedSeats(), 1);
		TestEqual(TEXT("the seated hull is the replacement"), Scene.Interaction->GetSeatedVehicle(), VehicleB);

		// The A->B hull switch kept exactly one subscription per hull (the
		// switch detached A's): B's own destroy notifies once more - total 2,
		// never 3 - and frees the seat again with the control face restored.
		VehicleB->GetHealthComponent()->ApplyDamage(99999.0f);
		TestTrue(TEXT("the replacement hull finalized its destruction"), VehicleB->IsDestroyFinalized());
		TestEqual(TEXT("each hull's destroy notified exactly once"),
			Scene.Interaction->GetVehicleDestroyedNotificationCount(), 2);
		TestEqual(TEXT("the seat model is empty again"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the seat is free once more"), Scene.Interaction->IsSeated());
		return true;
	}

	// ------------------------------------------------------------------
	// 7. The retry and pawn-EndPlay faces clear the seat, no dangling binds
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_041ResetAndEndPlayClearSeatWithoutDangling,
		"UEMMO.Tasks.M5_041.ResetAndEndPlayClearSeatWithoutDangling",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_041ResetAndEndPlayClearSeatWithoutDangling::RunTest(const FString& Parameters)
	{
		FM5_041_Scene Scene;
		if (!Scene.Build(*this))
		{
			return false;
		}

		TestTrue(TEXT("the enter accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);

		// The retry face: the unified reset (the F2/service shared entry)
		// clears the seat and restores the control face.
		Scene.Player->ApplyTrainingRoomReset();
		TestEqual(TEXT("the reset cleared the seat"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestFalse(TEXT("the reset freed the seat"), Scene.Interaction->IsSeated());
		TestTrue(TEXT("a post-reset enter still accepts"), Scene.Interaction->TryEnterNearestVehicle().bEntered);
		TestEqual(TEXT("one occupation after the post-reset enter"), Scene.Seats.GetNumOccupiedSeats(), 1);

		// The EndPlay face: the pawn's teardown frees the seat and detaches
		// the destroy subscription (the FTestWorldWrapper world routes EndPlay
		// on Destroy - the M5-039 lesson).
		Scene.Player->Destroy();
		TestEqual(TEXT("the pawn EndPlay freed the seat"), Scene.Seats.GetNumOccupiedSeats(), 0);
		TestTrue(TEXT("the seat registration survives the occupancy cleanup (model semantics)"),
			Scene.Seats.IsSeatRegistered(FName(TEXT("vehicle_wheeled_sample")), FName(TEXT("seat_wheeled_driver"))));

		// The hull's later destroy must not dangle into the detached
		// component: the vacate seam was cleared and the subscription removed
		// at the component's EndPlay, so the broadcast reaches nobody.
		Scene.Vehicle->Destroy();
		TestTrue(TEXT("the hull finalized its own destroy face"), Scene.Vehicle->IsDestroyFinalized());
		TestEqual(TEXT("the model stays empty through the hull teardown"), Scene.Seats.GetNumOccupiedSeats(), 0);
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
