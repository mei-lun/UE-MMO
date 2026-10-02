// M3-026: the production enemy health bars. The user playtest showed no hit
// feedback on the wave enemies (no bar was ever bound to the spawned
// enemies). The suite locks the card's four behaviors:
//
//   1. A real accepted hit (the full M1-041 real-character attack path inside
//      a fully ticked temp world) establishes exactly one tracked bar entry
//      for the hit enemy, reading the enemy's real HealthComponent truth.
//   2. A repeat hit on the tracked enemy upserts (never duplicates).
//   3. A dead enemy (HP<=0) drops its entry.
//   4. A destroyed enemy's weak-reference entry drops safely (no crash).
//   5. The tracked set caps at the 3-enemy room maximum.
//
// Tests 1-4 reuse the GameCombatWiringTests scaffold precedent (a real
// BeginPlay-initialized temp game world with a real APrototypeCharacter and a
// real ATrainingEnemy, real TickTestWorld frames, the attack driven through
// the production intent entry SubmitCombatInput); all helpers are renamed to
// the M3_026 namespace (per-file compilation, bUseUnity=false). Test 5 drives
// the real ApplyDamage plus the exact OnHitConfirmed event the real damage
// application raises (the M1-035 DamageNumberTests broadcast precedent) to
// exercise the cap semantics without physics timing.
//
// The capture companion stages real strikes in the real game world and
// requests screenshots of the drawn bars (headless runs skip gracefully).
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_026
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_026_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-041 placement convention). X horizontal, Y
	// depth, Z height; the floor top sits exactly at the base Z.
	const FVector M3_026_SceneBase(52000.0, 47000.0, 600.0);

	// All three ground attacks share the hit box offset X=95 and half extent
	// X=85 (M1-009 assets), so the box covers base.X+10..base.X+180 for facing
	// +1: an enemy feet anchor at +95 stays reachable.
	const float M3_026_EnemyFeetOffsetX = 95.0f;

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_026_FloorHalfThickness = 100.0f;
	const float M3_026_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: the characters drop a short distance
	// through real no-controller physics and settle onto the floor.
	const float M3_026_PlayerSpawnHeight = 120.0f;
	const float M3_026_EnemySpawnHeight = 90.0f;

	// Frame caps: the settle phase waits for both ground contacts; the hit
	// loop waits for one accepted hit plus the attack instance's retirement.
	constexpr int32 M3_026_MaxSettleFrames = 300;
	constexpr int32 M3_026_MaxHitFrames = 600;

	// The HUD's tracked-entry cap (mirrors the HUD constant; a room never
	// spawns more than 3 enemies).
	constexpr int32 M3_026_ExpectedBarCap = 3;

	// Recorded combat events (the assertions read this struct only).
	struct FM3_026_Events
	{
		int32 Hits = 0;
		TArray<FName> Finished;
	};

	// World-static blocking floor box the characters stand on (temp worlds
	// ship no geometry; the M1-041 scaffold precedent).
	static AActor* M3_026_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M3_026_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M3_026_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M3_026_FloorHalfExtentXY, M3_026_FloorHalfExtentXY, M3_026_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-041 pattern).
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M3_026_FloorHalfThickness));
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top and enables
	// no-controller physics (the M1-041 pattern: the movement component
	// initializes, integrates and settles the pawn onto the floor).
	static APrototypeCharacter* M3_026_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M3_026_SceneBase + FVector(0.0, 0.0, M3_026_PlayerSpawnHeight),
			FRotator::ZeroRotator, Params);
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

	// Spawns the real training enemy above the floor top at the requested
	// enemy feet anchor (the M1-041 pattern).
	static ATrainingEnemy* M3_026_SpawnEnemy(UWorld& World, float FeetOffsetX)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(),
			M3_026_SceneBase + FVector(FeetOffsetX, 0.0, M3_026_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Enemy == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
		}
		return Enemy;
	}

	// One full scene: the wrapper owns the manually ticked temp world; the
	// HUD under test is spawned like the game mode spawns it (the M2-012
	// HUD-in-world precedent) and its OnHitConfirmed feed is bound through
	// the exact per-frame entries DrawHUD calls (the public M1-035 seams).
	struct FM3_026_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		APrototypeHUD* Hud = nullptr;
		FM3_026_Events Events;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the manually ticked test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			AActor* Floor = M3_026_SpawnFloor(*World, M3_026_SceneBase);
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load pass that builds the
			// scene collision tree; build it once before the first tick (the
			// M1-041 precedent).
			World->EnsureCollisionTreeIsBuilt();
			Player = M3_026_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerCombat = Player->GetCombat();
			if (!Test.TestTrue(TEXT("the player carries a combat component"), PlayerCombat != nullptr))
			{
				return false;
			}
			Enemy = M3_026_SpawnEnemy(*World, M3_026_EnemyFeetOffsetX);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}

			// The HUD under test: spawned like the game mode spawns it (the
			// M2-012 precedent); its BeginPlay binds the session, the feed
			// binds through the per-frame entries DrawHUD calls.
			FActorSpawnParameters HudParams;
			Hud = World->SpawnActor<APrototypeHUD>(APrototypeHUD::StaticClass(),
				FVector::ZeroVector, FRotator::ZeroRotator, HudParams);
			if (!Test.TestNotNull(TEXT("the prototype HUD spawns in the test world"), Hud))
			{
				return false;
			}
			Hud->RefreshDebugReferences();
			Hud->RefreshDamageFeedBinding();

			PlayerCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& /*Hit*/)
			{
				++Events.Hits;
			});
			PlayerCombat->OnFinished.AddLambda([this](FName AttackId, uint64 /*InstanceId*/)
			{
				Events.Finished.Add(AttackId);
			});
			return true;
		}

		// Ticks the world until both characters settled onto the floor (real
		// walking physics; the M1-041 pattern).
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
			for (int32 Frame = 0; Frame < M3_026_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_026_FrameSeconds)))
				{
					return false;
				}
				TickEnemyCombatStandIn();
				const bool bPlayerGrounded = PlayerMovement != nullptr
					&& PlayerMovement->MovementMode == MOVE_Walking
					&& PlayerMovement->Velocity.Size() < 1.0f;
				const bool bEnemyGrounded = EnemyMovement != nullptr
					&& EnemyMovement->MovementMode == MOVE_Walking
					&& EnemyMovement->Velocity.Size() < 1.0f;
				if (bPlayerGrounded && bEnemyGrounded)
				{
					return true;
				}
			}
			Test.AddError(FString::Printf(TEXT(
				"the characters never settled onto the floor within the settle cap (playerMode=%d enemyMode=%d)"),
				PlayerMovement ? static_cast<int32>(PlayerMovement->MovementMode.GetValue()) : -1,
				EnemyMovement ? static_cast<int32>(EnemyMovement->MovementMode.GetValue()) : -1));
			return false;
		}

		// Presses X (Light) once through the production intent entry and ticks
		// until one accepted hit landed and its attack instance retired.
		bool PressLightUntilHit(FAutomationTestBase& Test)
		{
			const double FireAt = World->GetTimeSeconds() + 0.10;
			bool bPressed = false;
			for (int32 Frame = 0; Frame < M3_026_MaxHitFrames; ++Frame)
			{
				if (!bPressed && World->GetTimeSeconds() + 1e-6 >= FireAt)
				{
					Player->SubmitCombatInput(ECombatInput::Light);
					bPressed = true;
				}
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_026_FrameSeconds)))
				{
					return false;
				}
				TickEnemyCombatStandIn();
				if (Events.Hits >= 1 && Events.Finished.Num() >= 1)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the light attack never landed within the frame cap"));
			return false;
		}

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy->GetHealthComponent();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		// M1-041 known-gap stand-in, enemy side only (the game's enemy-side
		// combat driver gap; see GameCombatWiringTests for the full rationale).
		void TickEnemyCombatStandIn()
		{
			UCombatComponent* EnemyCombat = Enemy != nullptr ? Enemy->GetCombatComponent() : nullptr;
			if (EnemyCombat != nullptr && World != nullptr)
			{
				EnemyCombat->SetInputClockSeconds(World->GetTimeSeconds());
				EnemyCombat->TickCombat(M3_026_FrameSeconds);
			}
		}
	};

	// Broadcasts one accepted-hit event shaped exactly like the real damage
	// application's event (the M1-035 DamageNumberTests precedent): Target is
	// the damaged actor and Damage is the ApplyDamage return value. Returns
	// the real applied damage for the caller's assertions.
	static float M3_026_ApplyAndBroadcastHit(UCombatComponent& AttackerCombat, AActor& Target, float Damage)
	{
		UHealthComponent* Health = Target.FindComponentByClass<UHealthComponent>();
		const float Applied = (Health != nullptr) ? Health->ApplyDamage(Damage) : 0.0f;
		FCombatHit Hit;
		Hit.Damage = Applied;
		Hit.Target = &Target;
		Hit.WorldHitLocation = Target.GetActorLocation();
		Hit.AttackId = FName(TEXT("light_01"));
		AttackerCombat.OnHitConfirmed.Broadcast(Hit);
		return Applied;
	}

	static bool M3_026_CanCaptureScreenshot()
	{
		return GEngine != nullptr && GEngine->GameViewport != nullptr && FApp::CanEverRender();
	}

	// Latent step: one real light_01 strike from the player toward the staged
	// enemy (the M1-035 mode-3 choreography: the enemy is snapped exactly one
	// strike's reach in front so the hit lands despite the previous knockback).
	struct FM3_026_StrikeLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		TWeakObjectPtr<ATrainingEnemy> EnemyPtr;

		FM3_026_StrikeLatentCommand(APrototypeCharacter* InPlayer, ATrainingEnemy* InEnemy)
			: PlayerPtr(InPlayer), EnemyPtr(InEnemy)
		{
		}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			ATrainingEnemy* Enemy = EnemyPtr.Get();
			if (Player == nullptr || Enemy == nullptr)
			{
				return true;
			}
			Enemy->SetActorLocation(Player->GetActorLocation() + FVector(150.0, 0.0, 0.0));
			if (UCombatComponent* Combat = Player->GetCombat())
			{
				const int32 Facing = (Enemy->GetActorLocation().X >= Player->GetActorLocation().X) ? 1 : -1;
				Combat->TryStartAttack(FName(TEXT("light_01")), Facing);
			}
			return true;
		}
	};

	// Latent step: logs the live tracked-bar count as render evidence.
	struct FM3_026_BarLogLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeHUD> HudPtr;

		explicit FM3_026_BarLogLatentCommand(APrototypeHUD* InHud)
			: HudPtr(InHud)
		{
		}

		virtual bool Update() override
		{
			if (APrototypeHUD* Hud = HudPtr.Get())
			{
				UE_LOG(LogTemp, Display, TEXT("M3_026 capture: tracked enemy bars=%d"), Hud->GetTrackedEnemyCount());
			}
			return true;
		}
	};

	// Latent step: requests one screenshot (the M2-012 capture precedent).
	struct FM3_026_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM3_026_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			if (!M3_026_CanCaptureScreenshot())
			{
				UE_LOG(LogTemp, Display, TEXT("M3_026 screenshot skipped (no rendering): %s"), *AbsolutePath);
				return true;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
			// bShowUI=true: the HUD canvas drawing rides on the viewport UI
			// pass (the M1-035/M2-012 capture precedent).
			FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
			UE_LOG(LogTemp, Display, TEXT("M3_026 screenshot requested: %s"), *AbsolutePath);
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_026;

// 1. Acceptance: a REAL accepted hit (the full real-character attack path
//    inside the ticked temp world) establishes exactly one tracked enemy bar
//    entry reading the enemy's real HealthComponent truth; a repeat hit on
//    the same enemy upserts instead of duplicating.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_026RealHitEstablishesTrackedEnemyBar,
	"UEMMO.Tasks.M3_026.RealHitEstablishesTrackedEnemyBar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_026RealHitEstablishesTrackedEnemyBar::RunTest(const FString& Parameters)
{
	FM3_026_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	TestEqual(TEXT("the bar tracking starts with no entries"), Scene.Hud->GetTrackedEnemyCount(), 0);

	if (!Scene.PressLightUntilHit(*this))
	{
		return true;
	}
	TestEqual(TEXT("the real attack landed exactly one accepted hit"), Scene.Events.Hits, 1);
	if (!TestEqual(TEXT("the accepted hit established exactly one tracked bar entry"),
		Scene.Hud->GetTrackedEnemyCount(), 1))
	{
		return true;
	}
	TestTrue(TEXT("the tracked entry is the hit enemy"), Scene.Hud->PeekTrackedEnemy(0) == Scene.Enemy);

	const UHealthComponent* Tracked = Scene.Hud->PeekTrackedHealth(0);
	if (Tracked == nullptr)
	{
		AddError(TEXT("the tracked health pool reads null for the only tracked entry"));
		return true;
	}
	TestEqual(TEXT("the tracked bar reads the real damaged health (100 - 10 light_01)"),
		Tracked->GetHealth(), 90.0f, 0.01f);
	TestEqual(TEXT("the tracked bar reads the real max health"), Tracked->GetMaxHealth(), 100.0f, 0.01f);
	TestTrue(TEXT("the tracked pool reports alive"), Tracked->IsAlive());

	// A repeat accepted hit on the SAME enemy (the real event shape via the
	// M1-035 broadcast seam) must upsert, never duplicate.
	const float Applied = M3_026_ApplyAndBroadcastHit(*Scene.PlayerCombat, *Scene.Enemy, 5.0f);
	TestEqual(TEXT("the repeat hit really removed health"), Applied, 5.0f, 0.01f);
	TestEqual(TEXT("the repeat hit on the tracked enemy keeps exactly one entry"),
		Scene.Hud->GetTrackedEnemyCount(), 1);
	TestTrue(TEXT("the upserted entry still names the same enemy"),
		Scene.Hud->PeekTrackedEnemy(0) == Scene.Enemy);
	return true;
}

// 2. Acceptance: a dead enemy (lethal ApplyDamage, HP<=0) drops its bar entry
//    - the death path of the card's remove rule.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_026DeadEnemyRemovesTrackedEntry,
	"UEMMO.Tasks.M3_026.DeadEnemyRemovesTrackedEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_026DeadEnemyRemovesTrackedEntry::RunTest(const FString& Parameters)
{
	FM3_026_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	if (!Scene.PressLightUntilHit(*this))
	{
		return true;
	}
	if (!TestEqual(TEXT("the real hit tracked the enemy"), Scene.Hud->GetTrackedEnemyCount(), 1))
	{
		return true;
	}

	// The real lethal entry point (interface contract 7: damage goes through
	// UHealthComponent::ApplyDamage only).
	UHealthComponent* Health = Scene.Enemy->GetHealthComponent();
	if (!TestNotNull(TEXT("the enemy carries a health component"), Health))
	{
		return true;
	}
	TestTrue(TEXT("the lethal damage lands"), Health->ApplyDamage(9999.0f) > 0.0f);
	TestTrue(TEXT("the enemy pool reads dead after the lethal hit"), !Health->IsAlive());
	TestEqual(TEXT("the dead enemy's bar entry is removed"), Scene.Hud->GetTrackedEnemyCount(), 0);
	return true;
}

// 3. Acceptance: a destroyed enemy's weak-reference entry drops safely - the
//    prune pass reads the stale weak references as null, removes the entry
//    and never crashes (the M1-035 weak-reference contract).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_026DestroyedEnemyEntryDropsSafely,
	"UEMMO.Tasks.M3_026.DestroyedEnemyEntryDropsSafely",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_026DestroyedEnemyEntryDropsSafely::RunTest(const FString& Parameters)
{
	FM3_026_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	if (!Scene.PressLightUntilHit(*this))
	{
		return true;
	}
	if (!TestEqual(TEXT("the real hit tracked the enemy"), Scene.Hud->GetTrackedEnemyCount(), 1))
	{
		return true;
	}

	Scene.Enemy->Destroy();
	Scene.Wrapper.TickTestWorld(M3_026_FrameSeconds);
	TestTrue(TEXT("the destroyed actor's weak reference reads invalid"),
		Scene.Hud->PeekTrackedEnemy(0) == nullptr);
	TestEqual(TEXT("the destroyed enemy's entry dropped without crashing"),
		Scene.Hud->GetTrackedEnemyCount(), 0);
	return true;
}

// 4. Acceptance: the tracked set caps at the 3-enemy room maximum - a fourth
//    distinct hit enemy is refused, the first three stay tracked.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_026TrackedEnemyEntriesCapAtThree,
	"UEMMO.Tasks.M3_026.TrackedEnemyEntriesCapAtThree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_026TrackedEnemyEntriesCapAtThree::RunTest(const FString& Parameters)
{
	FM3_026_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	// Three more enemies (the cap needs four distinct hit targets; physics
	// positions are irrelevant to the cap semantics).
	ATrainingEnemy* Second = M3_026_SpawnEnemy(*Scene.World, M3_026_EnemyFeetOffsetX + 500.0f);
	ATrainingEnemy* Third = M3_026_SpawnEnemy(*Scene.World, M3_026_EnemyFeetOffsetX + 1000.0f);
	ATrainingEnemy* Fourth = M3_026_SpawnEnemy(*Scene.World, M3_026_EnemyFeetOffsetX + 1500.0f);
	if (!TestNotNull(TEXT("the second enemy spawns"), Second)
		|| !TestNotNull(TEXT("the third enemy spawns"), Third)
		|| !TestNotNull(TEXT("the fourth enemy spawns"), Fourth))
	{
		return true;
	}

	// Each enemy takes real damage (ApplyDamage) and the exact accepted-hit
	// event the real damage application raises reaches the HUD feed.
	const float Applied1 = M3_026_ApplyAndBroadcastHit(*Scene.PlayerCombat, *Scene.Enemy, 10.0f);
	const float Applied2 = M3_026_ApplyAndBroadcastHit(*Scene.PlayerCombat, *Second, 10.0f);
	const float Applied3 = M3_026_ApplyAndBroadcastHit(*Scene.PlayerCombat, *Third, 10.0f);
	if (!TestTrue(TEXT("the first enemy accepted the damage"), Applied1 > 0.0f)
		|| !TestTrue(TEXT("the second enemy accepted the damage"), Applied2 > 0.0f)
		|| !TestTrue(TEXT("the third enemy accepted the damage"), Applied3 > 0.0f))
	{
		return true;
	}
	if (!TestEqual(TEXT("three distinct hit enemies are tracked"), Scene.Hud->GetTrackedEnemyCount(), 3))
	{
		return true;
	}

	// The fourth distinct hit enemy is refused by the cap.
	const float Applied4 = M3_026_ApplyAndBroadcastHit(*Scene.PlayerCombat, *Fourth, 10.0f);
	TestTrue(TEXT("the fourth enemy accepted the damage (the refusal is the HUD's, not the pool's)"),
		Applied4 > 0.0f);
	TestEqual(TEXT("the tracked set stays at the 3-entry cap"), Scene.Hud->GetTrackedEnemyCount(), 3);
	TestTrue(TEXT("cap entry 1 is the first enemy"), Scene.Hud->PeekTrackedEnemy(0) == Scene.Enemy);
	TestTrue(TEXT("cap entry 2 is the second enemy"), Scene.Hud->PeekTrackedEnemy(1) == Second);
	TestTrue(TEXT("cap entry 3 is the third enemy"), Scene.Hud->PeekTrackedEnemy(2) == Third);
	return true;
}

// 5. Capture companion (the M2-012/M3-011 precedent): in the real game world
//    with a viewport, stage a training enemy, drive REAL strikes through the
//    real combat path and request screenshots of the drawn bars above the
//    hit enemy. Headless -nullrhi runs skip gracefully.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_026EnemyBarScreenshots,
	"UEMMO.Tasks.M3_026.EnemyBarScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_026EnemyBarScreenshots::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game)
	{
		AddInfo(TEXT("enemy bar capture skipped: no running game world/viewport"));
		return true;
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
	APrototypeHUD* Hud = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
	if (PC == nullptr || Player == nullptr || Hud == nullptr)
	{
		AddInfo(TEXT("enemy bar capture skipped: no player controller/pawn/HUD"));
		return true;
	}

	// Stage one training enemy near the player (debug-staging style, the
	// M1-035 mode-3 precedent; the wave enemies are spawned by the room flow
	// and are out of a capture run's control).
	FActorSpawnParameters Params;
	ATrainingEnemy* Enemy = World->SpawnActor<ATrainingEnemy>(ATrainingEnemy::StaticClass(),
		Player->GetActorLocation() + FVector(400.0, 0.0, 0.0), FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("the staged training enemy spawns"), Enemy))
	{
		return true;
	}

	// The exact per-frame binding entries DrawHUD calls (production already
	// binds them every drawn frame; the explicit call only documents it).
	Hud->RefreshDebugReferences();
	Hud->RefreshDamageFeedBinding();

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M3-026"));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_026_StrikeLatentCommand(Player, Enemy));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_026_BarLogLatentCommand(Hud));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_026_ScreenshotLatentCommand(Directory / TEXT("enemy-bar-hit.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
	// Second strike: the bar must read lower (real accumulated damage).
	ADD_LATENT_AUTOMATION_COMMAND(FM3_026_StrikeLatentCommand(Player, Enemy));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_026_BarLogLatentCommand(Hud));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_026_ScreenshotLatentCommand(Directory / TEXT("enemy-bar-hit-again.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
	return true;
}

#endif
