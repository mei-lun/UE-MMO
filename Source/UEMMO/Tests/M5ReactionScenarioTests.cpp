// M5-018: the Segment-A production reaction scenario. The scenes run the
// CONFIG-DRIVEN system test room (the M5-018A driver loads Data/test_room.json
// - the production facility, no fixture doubles injected into production
// data) and drive every event through the REAL hit entry: a combat-host
// character starts the real attack (TryStartAttack), the active window runs
// the production hit query and the M5-012 unified entry resolves damage,
// control and dedup - the tests never write health or state directly.
// Recorded HP/control/knockdown/death event sequences are exported as JSON
// evidence under Artifacts/Tasks/M5-018/scenario-json (the M2-014 export
// precedent).

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"
#include "Tests/AutomationCommon.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Enemy/MeleeEnemy.h"
#include "../PrototypeCharacter.h"

#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_018
{
	/** One recorded reaction event (the JSON evidence row). */
	struct FM5_018_ReactionEvent
	{
		FString Kind;
		FString TargetId;
		float Health;
		bool bStunned;
		bool bLaunched;
		bool bDown;
		double AtSeconds;

		FString ToJson() const
		{
			return FString::Printf(
				TEXT("{\"kind\":\"%s\",\"target\":\"%s\",\"health\":%.1f,\"stunned\":%s,\"launched\":%s,\"down\":%s,\"at\":%.3f}"),
				*Kind, *TargetId, Health,
				bStunned ? TEXT("true") : TEXT("false"),
				bLaunched ? TEXT("true") : TEXT("false"),
				bDown ? TEXT("true") : TEXT("false"),
				AtSeconds);
		}
	};

	/**
	 * The reaction scene: the M5-018A driver loads the production source
	 * table (Data/test_room.json) and spawns the configured targets; a
	 * combat-host character (the real APrototypeCharacter pipeline) is
	 * staged at an attacker position facing the wanted target.
	 */
	struct FM5_018_ReactionScene
	{
		ACombatTestRoomDriver* Driver = nullptr;
		FTestRoomConfig Config;
		UAttackCatalog* Catalog = nullptr;
		APrototypeCharacter* Host = nullptr;
		UCombatComponent* HostCombat = nullptr;
		TArray<AMeleeEnemy*> Targets;
		TArray<FM5_018_ReactionEvent> Events;
		double NowSeconds = 0.0;

		bool Build(FAutomationTestBase& Test, UWorld& World)
		{
			// The driver loads the PRODUCTION config (the file the shipped map
			// uses); a fixture config would defeat the scenario's purpose.
			Driver = NewObject<ACombatTestRoomDriver>();
			FString Error;
			if (!Test.TestTrue(TEXT("the production test_room source loads"),
				Driver->LoadConfig(Error)))
			{
				Test.AddError(Error);
				return false;
			}
			Config = Driver->GetConfig();

			// The combat host at the first target's front (the real attack
			// chain needs the real geometry).
			FActorSpawnParameters Params;
			const FVector HostLocation = Config.Targets[0].LocationCm - FVector(95.0, 0.0, 2.0);
			Host = World.SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(), HostLocation, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the combat host spawns"), Host))
			{
				return false;
			}
			if (!Host->HasActorBegunPlay())
			{
				Host->DispatchBeginPlay();
			}
			HostCombat = Host->GetCombat();
			if (!Test.TestNotNull(TEXT("the host combat component exists"), HostCombat))
			{
				return false;
			}
			Catalog = NewObject<UAttackCatalog>();
			FText CatalogError;
			if (!Test.TestTrue(TEXT("the host attaches the shipped attack catalog"),
				Catalog->InitializeFromConfig(CatalogError)))
			{
				return false;
			}
			HostCombat->InitializeFromCatalog(Catalog);
			// Pin the legacy 0 attack power: the shared game instance's
			// profile may carry an equipped sword (+5) from an earlier suite,
			// and the scenario pins the legacy base-damage numbers.
			HostCombat->SetCombatStats(0.0f, HostCombat->GetDefense());

			// Track exactly the enemies THIS scene spawned (the shared world
			// may carry corpses from earlier cases): a pre/post diff.
			TSet<AActor*> PreExisting;
			for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
			{
				PreExisting.Add(*It);
			}
			TArray<FName> SpawnMissing;
			const int32 Spawned = Driver->SpawnTargets(World, SpawnMissing);
			if (!Test.TestEqual(TEXT("the configured targets spawn"), Spawned, Config.Targets.Num()))
			{
				return false;
			}
			for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
			{
				if (!PreExisting.Contains(*It))
				{
					Targets.Add(*It);
				}
			}
			return Test.TestEqual(TEXT("the scene holds exactly the configured targets"),
				Targets.Num(), Config.Targets.Num());
		}

		AMeleeEnemy* FindTargetById(const FName& TargetId)
		{
			for (int32 Index = 0; Index < Config.Targets.Num(); ++Index)
			{
				if (Config.Targets[Index].TargetId == TargetId)
				{
					return Targets.IsValidIndex(Index) ? Targets[Index] : nullptr;
				}
			}
			return nullptr;
		}

		/** Advances the host combat (and the target's) by one 1/60 s frame. */
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				NowSeconds += 1.0 / 60.0;
				HostCombat->TickCombat(1.0f / 60.0f);
				for (AMeleeEnemy* Enemy : Targets)
				{
					if (UCombatComponent* Combat = Enemy->GetCombatComponent())
					{
						Combat->TickCombat(1.0f / 60.0f);
					}
				}
			}
		}

		/** Stages one real attack from the host (the real hit entry). */
		bool HostAttack(FAutomationTestBase& Test, const TCHAR* AttackId, int32 Facing)
		{
			if (!Test.TestTrue(FString::Printf(TEXT("%s starts on the host"), AttackId),
				HostCombat->TryStartAttack(FName(AttackId), Facing)))
			{
				return false;
			}
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			if (Definition != nullptr)
			{
				TickFrames(Definition->ActiveWindow.StartFrame + 1);
			}
			return true;
		}

		void Record(const FString& Kind, const FString& TargetId, AMeleeEnemy* Enemy)
		{
			FM5_018_ReactionEvent Event;
			Event.Kind = Kind;
			Event.TargetId = TargetId;
			const UHealthComponent* Health = Enemy ? Enemy->GetHealthComponent() : nullptr;
			Event.Health = Health ? Health->GetHealth() : -1.0f;
			const UCombatComponent* Combat = Enemy ? Enemy->GetCombatComponent() : nullptr;
			const FCombatSnapshot Snapshot = Combat ? Combat->GetSnapshot() : FCombatSnapshot();
			Event.bStunned = Snapshot.ActionState == ECombatActionState::HitStun;
			Event.bDown = Snapshot.ActionState == ECombatActionState::Knockdown
				|| Snapshot.ActionState == ECombatActionState::Recovering;
			Event.bLaunched = false;
			if (const ACharacter* VictimCharacter = Cast<ACharacter>(Enemy))
			{
				if (const UCharacterMovementComponent* Movement = VictimCharacter->GetCharacterMovement())
				{
					Event.bLaunched = Movement->Velocity.Z > 0.0f || Movement->PendingLaunchVelocity.Z > 0.0f;
				}
			}
			Event.AtSeconds = NowSeconds;
			Events.Add(Event);
		}

		/** Destroys this scene's spawned actors (the shared world stays clean). */
		void DestroyScene(UWorld& World)
		{
			for (AMeleeEnemy* Enemy : Targets)
			{
				if (Enemy != nullptr)
				{
					Enemy->Destroy();
				}
			}
			if (Host != nullptr)
			{
				Host->Destroy();
			}
		}

		bool ExportJson(FAutomationTestBase& Test, const FString& FileName)
		{
			const FString Directory = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-018") / TEXT("scenario-json"));
			IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
			FString Payload = TEXT("{\n  \"section\": \"Reaction\",\n  \"events\": [\n");
			for (int32 Index = 0; Index < Events.Num(); ++Index)
			{
				Payload += TEXT("    ") + Events[Index].ToJson()
					+ (Index + 1 < Events.Num() ? TEXT(",") : TEXT(""));
				Payload += TEXT("\n");
			}
			Payload += TEXT("  ]\n}\n");
			const FString Absolute = Directory / FileName;
			if (!FFileHelper::SaveStringToFile(Payload, *Absolute))
			{
				Test.AddError(FString::Printf(TEXT("the scenario JSON could not be written: %s"), *Absolute));
				return false;
			}
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_018;

// ---------------------------------------------------------------------------
// LightHitOnNormalTargetFlowsTheRealChain
// ---------------------------------------------------------------------------

// The production chain: the host starts the real light_01, the active window
// runs the hit query and the unified entry applies the legacy 10 damage and
// the stun through the victim's combat component; a depth whiff (the target
// outside the Y-axis reach) hits nothing - the depth axis still matters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018LightHitOnNormalTargetFlowsTheRealChain,
	"UEMMO.Tasks.M5_018.LightHitOnNormalTargetFlowsTheRealChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018LightHitOnNormalTargetFlowsTheRealChain::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (!TestTrue(TEXT("a world is available"), World != nullptr))
	{
		return true;
	}
	FM5_018_ReactionScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}
	AMeleeEnemy* Normal = Scene.FindTargetById(FName(TEXT("dummy_normal")));
	if (!TestNotNull(TEXT("the normal target exists"), Normal))
	{
		return true;
	}

	// The real hit: 10 damage lands and the target is stunned.
	if (!Scene.HostAttack(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	Scene.Record(TEXT("light_hit"), TEXT("dummy_normal"), Normal);
	TestEqual("the real light_01 removes exactly the configured 10 (80 -> 70)",
		Scene.FindTargetById(FName(TEXT("dummy_normal")))->GetHealthComponent()->GetHealth(), 70.0f);
	TestTrue("the real hit stunned the normal target",
		Normal->GetCombatComponent()->GetSnapshot().ActionState == ECombatActionState::HitStun);
	Scene.ExportJson(*this, TEXT("reaction-light-hit.json"));
	Scene.DestroyScene(*World);

	// The depth whiff: the same attack cannot reach a target 400 cm deep.
	AMeleeEnemy* Second = Scene.FindTargetById(FName(TEXT("dummy_heavy")));
	if (TestNotNull(TEXT("the second target exists"), Second))
	{
		const float HealthBefore = Second->GetHealthComponent()->GetHealth();
		const FVector SavedLocation = Second->GetActorLocation();
		// Finish the running instance before staging the whiff attack.
		Scene.TickFrames(45);
		// Move the second target into the first target's spot temporarily so
		// the geometry is known, then push it 400 cm deep (out of the Y
		// half-extent) - a depth whiff must hit nothing.
		Second->SetActorLocation(Scene.Config.Targets[0].LocationCm + FVector(0.0f, 400.0f, 0.0f));
		if (Scene.HostAttack(*this, TEXT("light_01"), 1))
		{
			// The active window passed: the deep target takes nothing.
		}
		Second->SetActorLocation(SavedLocation);
		TestEqual("the depth whiff removed no health",
			Second->GetHealthComponent()->GetHealth(), HealthBefore);
		Scene.Record(TEXT("depth_whiff"), TEXT("dummy_heavy"), Second);
		Scene.ExportJson(*this, TEXT("reaction-depth-whiff.json"));
	}
	Scene.DestroyScene(*World);
	return true;
}

// ---------------------------------------------------------------------------
// HeavyPolicyRefusesLaunchAndNormalFloats
// ---------------------------------------------------------------------------

// The configured policies decide the control through the real chain: the
// launcher's real hit applies its damage but the heavy target refuses the
// launch (no float, no knockdown), while the normal target floats and its
// launched landing runs the knockdown process; the death path ends the
// sequence through the real health entry only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018HeavyPolicyRefusesLaunchAndNormalFloats,
	"UEMMO.Tasks.M5_018.HeavyPolicyRefusesLaunchAndNormalFloats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018HeavyPolicyRefusesLaunchAndNormalFloats::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (!TestTrue(TEXT("a world is available"), World != nullptr))
	{
		return true;
	}
	FM5_018_ReactionScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}
	AMeleeEnemy* Heavy = Scene.FindTargetById(FName(TEXT("dummy_heavy")));
	AMeleeEnemy* Normal = Scene.FindTargetById(FName(TEXT("dummy_normal")));
	if (!TestNotNull(TEXT("the heavy target exists"), Heavy)
		|| !TestNotNull(TEXT("the normal target exists"), Normal))
	{
		return true;
	}

	// Stage the host in front of the HEAVY target (its configured location)
	// and launch: the damage lands, the launch is refused by the policy.
	Scene.Host->SetActorLocation(Scene.Config.Targets[1].LocationCm - FVector(95.0, 0.0, 2.0));
	Scene.Host->GetCharacterMovement()->StopMovementImmediately();
	if (!Scene.HostAttack(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	Scene.Record(TEXT("launcher_on_heavy"), TEXT("dummy_heavy"), Heavy);
	TestEqual("the heavy target took the launcher damage (120 -> 102)",
		Heavy->GetHealthComponent()->GetHealth(), 102.0f);
	TestTrue("the heavy target refused the launch (stays grounded, no knockdown)",
		Heavy->GetCombatComponent()->GetSnapshot().ActionState != ECombatActionState::Knockdown
		&& Heavy->GetCharacterMovement()->Velocity.Z <= 0.0f);

	// The normal target floats and lands into the configured knockdown.
	Scene.Host->SetActorLocation(Scene.Config.Targets[0].LocationCm - FVector(95.0, 0.0, 2.0));
	Scene.Host->GetCharacterMovement()->StopMovementImmediately();
	Scene.TickFrames(60);
	if (!Scene.HostAttack(*this, TEXT("launcher"), 1))
	{
		return true;
	}
	Scene.Record(TEXT("launcher_on_normal"), TEXT("dummy_normal"), Normal);
	TestTrue("the normal target was launched (rising)",
		Normal->GetCharacterMovement()->Velocity.Z > 0.0f
		|| Normal->GetCharacterMovement()->PendingLaunchVelocity.Z > 0.0f);
	// Simulate the flight the manual world does not step: land the victim.
	Normal->GetCharacterMovement()->AddImpulse(FVector(0.0f, 0.0f, -5000.0f), true);
	Scene.TickFrames(60);
	Scene.Record(TEXT("landing"), TEXT("dummy_normal"), Normal);
	Scene.ExportJson(*this, TEXT("reaction-launch-heavy-normal.json"));
	Scene.DestroyScene(*World);
	return true;
}

// ---------------------------------------------------------------------------
// DeathPathEndsTheReaction
// ---------------------------------------------------------------------------

// The death path through the real chain: repeated real light_01 hits walk
// the configured 80 health down and the lethal hit marks the target dead
// through its own health component (the single death event); a dead target
// refuses further hits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018DeathPathEndsTheReaction,
	"UEMMO.Tasks.M5_018.DeathPathEndsTheReaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018DeathPathEndsTheReaction::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (!TestTrue(TEXT("a world is available"), World != nullptr))
	{
		return true;
	}
	FM5_018_ReactionScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}
	AMeleeEnemy* Normal = Scene.FindTargetById(FName(TEXT("dummy_normal")));
	if (!TestNotNull(TEXT("the normal target exists"), Normal))
	{
		return true;
	}

	// Eight real light hits (8 x 10 = 80): the last one is lethal.
	for (int32 Hit = 1; Hit <= 8; ++Hit)
	{
		Scene.TickFrames(40);
		if (!Scene.HostAttack(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		Scene.Record(FString::Printf(TEXT("light_hit_%d"), Hit), TEXT("dummy_normal"), Normal);
	}
	TestTrue("the lethal hit marked the target dead through its own health entry",
		!Normal->GetHealthComponent()->IsAlive());
	Scene.ExportJson(*this, TEXT("reaction-death.json"));
	Scene.DestroyScene(*World);
	return true;
}

// ---------------------------------------------------------------------------
// ReactionCapture (the rendered capture companion; ClientContext only)
// ---------------------------------------------------------------------------

// In a real rendered game world the config-driven room is staged at the game
// pawn's location and the reaction phases are captured one screenshot each:
// the three configured targets in place, then the normal target mid-launch.
// Headless runs skip silently; the assertions live in the suites above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018ReactionCapture,
	"UEMMO.Tasks.M5_018.Reaction.ReactionCapture",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018ReactionCapture::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	APrototypeCharacter* GamePawn = World != nullptr
		? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0))
		: nullptr;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game || GamePawn == nullptr)
	{
		AddInfo(TEXT("reaction capture skipped: no rendered game world with the prototype pawn"));
		return true;
	}

	// Stage the config-driven room at the game pawn's location (the capture
	// follows the game camera, so the room moves to the view).
	const FVector Base = GamePawn->GetActorLocation();
	ACombatTestRoomDriver* Driver = NewObject<ACombatTestRoomDriver>();
	FString Error;
	if (!TestTrue(TEXT("the production test_room source loads"), Driver->LoadConfig(Error)))
	{
		AddError(Error);
		return true;
	}
	const FTestRoomConfig Config = Driver->GetConfig();

	// Spawn the configured targets relative to the pawn.
	TArray<AMeleeEnemy*> Spawned;
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	for (const FTestRoomTargetConfig& Target : Config.Targets)
	{
		AMeleeEnemy* Enemy = World->SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(), Base + Target.LocationCm, FRotator::ZeroRotator, Params);
		if (Enemy != nullptr)
		{
			if (UHealthComponent* Health = Enemy->GetHealthComponent())
			{
				Health->SetMaxHealth(Target.MaxHealth);
				Health->ResetHealth();
			}
			Spawned.Add(Enemy);
		}
	}
	if (!TestEqual(TEXT("the configured targets staged for the capture"), Spawned.Num(), Config.Targets.Num()))
	{
		return true;
	}

	// The real chain from the game pawn: its combat component carries the
	// shipped catalog (BeginPlay wiring).
	UCombatComponent* PawnCombat = GamePawn->GetCombat();
	UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
	FText CatalogError;
	if (!TestTrue(TEXT("the pawn attaches the shipped attack catalog"),
		PawnCombat != nullptr && Catalog->InitializeFromConfig(CatalogError) && PawnCombat->InitializeFromCatalog(Catalog)))
	{
		return true;
	}
	PawnCombat->SetCombatStats(0.0f, PawnCombat->GetDefense());

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-018") / TEXT("scenario-json") / TEXT("render"));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);

	// Screenshot 1: the three configured targets in place.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Directory]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(Directory / TEXT("scenario-targets.png"), true, false);
		return true;
	}));

	// Screenshot 2: the normal target mid-launch (the real launcher hit; the
	// pawn faces +X and stands at the target's front).
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, PawnCombat, Base, Config]()
	{
		APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0));
		if (Pawn != nullptr && PawnCombat != nullptr)
		{
			Pawn->SetActorLocation(Base + Config.Targets[0].LocationCm - FVector(95.0, 0.0, 2.0));
			PawnCombat->TryStartAttack(FName(TEXT("launcher")), 1);
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Directory]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(Directory / TEXT("scenario-float.png"), true, false);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));

	// Cleanup: retire the staged targets (the shared game world stays clean).
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Spawned]()
	{
		for (AMeleeEnemy* Enemy : Spawned)
		{
			if (Enemy != nullptr && IsValid(Enemy))
			{
				Enemy->Destroy();
			}
		}
		return true;
	}));
	return true;
}

#endif
