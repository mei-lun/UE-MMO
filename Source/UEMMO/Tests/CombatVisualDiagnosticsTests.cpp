// M3-027: room combat visual diagnostics. The user's fourth playtest round
// reported four perception gaps in the combat room while the package log
// proved the mechanism layer fully working (two waves spawned, 18 accepted
// hits, 3 kills, wave progression): "X/aerial attack works, Z launcher does
// nothing, hits give no feedback, enemies never move, enemies have no health
// bar". This suite reproduces that session OFFSCREEN in the REAL game loop
// (-game -RenderOffscreen on the production GameDefaultMap L_CombatRoom01,
// driven through the production movement/combat entries) and records the
// evidence the four-item diagnosis needs:
//   1. enemies moving or standing still (per-frame position/velocity/brain
//      telemetry from spawn to the attack band),
//   2. the Z launcher lifting an enemy (per-frame Z telemetry after one real
//      launcher press),
//   3. hit feedback visibility (HUD tracked-bar count + damage-number pool
//      around real hits),
//   4. a serial screenshot timeline (0.4-0.5 s cadence) covering spawn,
//      approach, attack, launcher and post-launcher landing.
// Before the fight the suite runs the MATERIAL PROBE (screenshots of the
// idle player mesh under candidate flash overlays) that settled why the
// first flash attempt stayed invisible: the engine BasicShapeMaterial
// renders nothing as a skeletal-mesh overlay (static-mesh usage only,
// probe pixel-identical to the unmodified mesh) while the mesh's own
// slot-0 material as the overlay base with every inherited vector
// parameter written red renders the whole body red.
// The headless regression run (nullrhi) skips the capture phases gracefully
// (the M3-024 AttackPoseScreenshots pattern) - the hard in-loop assertions
// live with the fix-locking tests of this suite.
#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/MeleeEnemyController.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameters.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_027
{
	// Evidence output directory (M3-024 pattern: direct project artifacts path).
	const TCHAR* const M3_027_EvidenceDir = TEXT("Artifacts/Tasks/M3-027");

	// Screenshot cadence while a phase runs (seconds, the task card's 0.3-0.5 s
	// band) and the telemetry row cap (the timeline stays bounded even if a
	// phase runs to its timeout).
	constexpr double M3_027_ScreenshotCadenceSeconds = 0.45;
	constexpr int32 M3_027_TimelineRowCap = 5000;

	// Phase timeouts (game seconds). The walk phase ends as soon as the room
	// session reports a running run; approach+attack and the launcher phases
	// end on their own success conditions earlier.
	constexpr double M3_027_WalkPhaseTimeoutSeconds = 8.0;
	constexpr double M3_027_CombatPhaseTimeoutSeconds = 9.0;
	constexpr double M3_027_LauncherPhaseSeconds = 2.2;

	// Attack start band: the shared ground-attack geometry reaches ~180 cm in
	// front of the player (offset 95 + half extent 85, M1-041), so the driver
	// stops closing inside 175 cm and presses X there. The depth slack is
	// generous on purpose (the real room's enemies hold depth offsets).
	constexpr float M3_027_AttackBandX = 175.0f;
	constexpr float M3_027_AttackBandY = 60.0f;

	// Light-attack cadence while in the band (the M3-024 staged-mash cadence;
	// a real 60 fps press is consumed next frame, the automation clock can
	// jump, so the driver presses on an interval instead of once).
	constexpr double M3_027_LightPressIntervalSeconds = 0.4;
	constexpr int32 M3_027_LightPressTarget = 5;

	// One telemetry row about every other frame is plenty (the JSON keeps the
	// file small; the UE log carries every row).
	constexpr int32 M3_027_TelemetryStride = 2;

	// The shared evidence recorder. Held as TSharedRef by every latent command
	// (the commands outlive RunTest, so the state must be heap-shared).
	struct FM3_027_Recorder
	{
		bool bRunStarted = false;
		bool bPlayerDied = false;
		int32 MaxEnemyCount = 0;
		int32 KillCount = 0;
		int32 TrackedBarsMax = 0;
		int32 DamageNumbersMax = 0;
		int32 ScreenshotCount = 0;
		int32 TelemetryStrideCounter = 0;

		// Enemy movement evidence (whole session, per telemetry sample).
		float MaxEnemyPlanarSpeed = 0.0f;
		int32 FirstEnemySpawnFrame = -1;

		// Session freshness: the shared real game world can carry a session a
		// PREVIOUS suite's run left active (the full UEMMO.Tasks filter runs
		// several real-world suites before this one). A fresh Idle -> active
		// transition observed INSIDE this test is the only state the
		// environmental "enemies spawned" assertion may demand; a pre-existing
		// active session is logged and the capture phases become no-ops
		// (nothing to diagnose without this test's own run).
		bool bSawIdleSession = false;
		bool bFreshRun = false;

		// Launcher evidence (relative to the launcher press).
		bool bLauncherPressed = false;
		float LauncherPressEnemyZ = 0.0f;
		float LauncherMaxEnemyZ = -BIG_NUMBER;
		float LauncherMaxRiseZ = 0.0f;
		float LauncherMaxUpSpeed = 0.0f;

		// Spawn bookkeeping: first-seen location per enemy name.
		TMap<FString, FVector> SpawnLocations;

		TArray<FString> Timeline;
		double NextScreenshotAt = 0.0;
		int32 ScreenshotIndex = 0;

		void Log(const FString& Row)
		{
			UE_LOG(LogTemp, Display, TEXT("M3_027 %s"), *Row);
			if (Timeline.Num() < M3_027_TimelineRowCap)
			{
				Timeline.Add(Row);
			}
		}

		FString EvidencePath() const
		{
			return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / M3_027_EvidenceDir);
		}

		void MaybeScreenshot()
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				return;
			}
			const double Now = FPlatformTime::Seconds();
			if (Now < NextScreenshotAt)
			{
				return;
			}
			NextScreenshotAt = Now + M3_027_ScreenshotCadenceSeconds;
			IFileManager::Get().MakeDirectory(*EvidencePath(), /*Tree*/ true);
			const FString Path = EvidencePath() / FString::Printf(TEXT("m3-027-%03d.png"), ++ScreenshotIndex);
			FScreenshotRequest::RequestScreenshot(Path, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
			++ScreenshotCount;
			Log(FString::Printf(TEXT("screenshot requested: %s"), *Path));
		}

		void WriteEvidence(bool bCompleted)
		{
			IFileManager::Get().MakeDirectory(*EvidencePath(), /*Tree*/ true);
			FString Json;
			Json += TEXT("{\n");
			Json += FString::Printf(TEXT("  \"completed\": %s,\n"), bCompleted ? TEXT("true") : TEXT("false"));
			Json += FString::Printf(TEXT("  \"run_started\": %s,\n"), bRunStarted ? TEXT("true") : TEXT("false"));
			Json += FString::Printf(TEXT("  \"player_died\": %s,\n"), bPlayerDied ? TEXT("true") : TEXT("false"));
			Json += FString::Printf(TEXT("  \"max_enemy_count\": %d,\n"), MaxEnemyCount);
			Json += FString::Printf(TEXT("  \"kill_count\": %d,\n"), KillCount);
			Json += FString::Printf(TEXT("  \"tracked_bars_max\": %d,\n"), TrackedBarsMax);
			Json += FString::Printf(TEXT("  \"damage_numbers_max\": %d,\n"), DamageNumbersMax);
			Json += FString::Printf(TEXT("  \"max_enemy_planar_speed_cm_s\": %.2f,\n"), MaxEnemyPlanarSpeed);
			Json += FString::Printf(TEXT("  \"launcher_pressed\": %s,\n"), bLauncherPressed ? TEXT("true") : TEXT("false"));
			Json += FString::Printf(TEXT("  \"launcher_press_enemy_z\": %.2f,\n"), LauncherPressEnemyZ);
			Json += FString::Printf(TEXT("  \"launcher_max_enemy_z\": %.2f,\n"), LauncherMaxEnemyZ);
			Json += FString::Printf(TEXT("  \"launcher_max_rise_cm\": %.2f,\n"), LauncherMaxRiseZ);
			Json += FString::Printf(TEXT("  \"launcher_max_up_speed_cm_s\": %.2f,\n"), LauncherMaxUpSpeed);
			Json += FString::Printf(TEXT("  \"screenshot_count\": %d,\n"), ScreenshotCount);
			Json += TEXT("  \"spawn_locations_cm\": {\n");
			bool bFirst = true;
			for (const TPair<FString, FVector>& Pair : SpawnLocations)
			{
				if (!bFirst)
				{
					Json += TEXT(",\n");
				}
				bFirst = false;
				Json += FString::Printf(TEXT("    \"%s\": \"%.1f %.1f %.1f\""),
					*Pair.Key, Pair.Value.X, Pair.Value.Y, Pair.Value.Z);
			}
			Json += TEXT("\n  },\n");
			Json += TEXT("  \"timeline\": [\n");
			for (int32 Index = 0; Index < Timeline.Num(); ++Index)
			{
				Json += FString::Printf(TEXT("    \"%s\"%s\n"),
					*Timeline[Index].Replace(TEXT("\""), TEXT("'"), ESearchCase::IgnoreCase), Index + 1 < Timeline.Num() ? TEXT(",") : TEXT(""));
			}
			Json += TEXT("  ]\n}\n");
			const FString JsonPath = EvidencePath() / TEXT("m3-027-diagnostics.json");
			FFileHelper::SaveStringToFile(Json, *JsonPath);
			UE_LOG(LogTemp, Display, TEXT("M3_027 evidence written: %s"), *JsonPath);
		}
	};

	// One telemetry sample of the whole room: player, session, HUD feed state
	// and every melee enemy (position, speed, Z, brain state, hit stun). This
	// is the four-question evidence base, sampled every stride.
	void M3_027_SampleRoom(APrototypeCharacter* Player, FM3_027_Recorder& Recorder, const TCHAR* Phase)
	{
		UWorld* World = Player->GetWorld();
		if (World == nullptr)
		{
			return;
		}
		if (++Recorder.TelemetryStrideCounter < M3_027_TelemetryStride)
		{
			return;
		}
		Recorder.TelemetryStrideCounter = 0;

		if (Player->GetHealth() != nullptr && !Player->GetHealth()->IsAlive())
		{
			Recorder.bPlayerDied = true;
		}
		if (URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>())
		{
			const ERoomSessionState State = Session->GetState();
			Recorder.bSawIdleSession = Recorder.bSawIdleSession || State == ERoomSessionState::Idle;
			if (State != ERoomSessionState::Idle && Recorder.bSawIdleSession)
			{
				Recorder.bFreshRun = true;
			}
			Recorder.bRunStarted = Recorder.bRunStarted || State != ERoomSessionState::Idle;
			Recorder.KillCount = FMath::Max(Recorder.KillCount, Session->GetKilledCount());
		}
		if (APlayerController* Controller = Cast<APlayerController>(Player->GetController()))
		{
			if (APrototypeHUD* Hud = Cast<APrototypeHUD>(Controller->GetHUD()))
			{
				Recorder.TrackedBarsMax = FMath::Max(Recorder.TrackedBarsMax, Hud->GetTrackedEnemyCount());
				Recorder.DamageNumbersMax = FMath::Max(Recorder.DamageNumbersMax, Hud->PeekDamageNumberCount());
			}
		}

		const FVector PlayerLocation = Player->GetActorLocation();
		int32 EnemyCount = 0;
		for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
		{
			AMeleeEnemy* Enemy = *It;
			if (Enemy == nullptr)
			{
				continue;
			}
			++EnemyCount;
			const FVector Location = Enemy->GetActorLocation();
			const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
			const float Speed2D = Movement != nullptr ? Movement->Velocity.Size2D() : 0.0f;
			Recorder.MaxEnemyPlanarSpeed = FMath::Max(Recorder.MaxEnemyPlanarSpeed, Speed2D);
			Recorder.MaxEnemyCount = FMath::Max(Recorder.MaxEnemyCount, EnemyCount);
			const FString EnemyName = Enemy->GetName();
			if (!Recorder.SpawnLocations.Contains(EnemyName))
			{
				Recorder.SpawnLocations.Add(EnemyName, Location);
			}
			FString BrainState(TEXT("none"));
			float PlanarDistance = FVector::Dist2D(Location, PlayerLocation);
			if (const AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>(Enemy->GetController()))
			{
				static const TCHAR* StateNames[] = { TEXT("Idle"), TEXT("Approach"), TEXT("Telegraph"), TEXT("Attack"), TEXT("Recover") };
				const int32 StateIndex = static_cast<int32>(Brain->GetState());
				BrainState = StateNames[StateIndex];
			}
			const UCombatComponent* Combat = Enemy->GetCombatComponent();
			Recorder.Log(FString::Printf(TEXT("%s t=%.2f enemy=%s pos=%.0f,%.0f,%.0f vz=%.0f speed2d=%.0f dist2d=%.0f brain=%s hp=%.0f action=%d"),
				Phase, World->GetTimeSeconds(), *EnemyName,
				Location.X, Location.Y, Location.Z,
				Movement != nullptr ? Movement->Velocity.Z : 0.0f, Speed2D, PlanarDistance,
				*BrainState,
				Enemy->GetHealthComponent() != nullptr ? Enemy->GetHealthComponent()->GetHealth() : -1.0f,
				Combat != nullptr ? static_cast<int32>(Combat->GetSnapshot().ActionState) : -1));
		}
	}

	// Requests one screenshot immediately (used for the named phase markers).
	struct FM3_027_MarkerScreenshot : public IAutomationLatentCommand
	{
		FString AbsolutePath;
		explicit FM3_027_MarkerScreenshot(const FString& InAbsolutePath) : AbsolutePath(InAbsolutePath) {}
		virtual bool Update() override
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				UE_LOG(LogTemp, Display, TEXT("M3_027 marker screenshot skipped (no rendering): %s"), *AbsolutePath);
				return true;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
			FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
			UE_LOG(LogTemp, Display, TEXT("M3_027 marker screenshot requested: %s"), *AbsolutePath);
			return true;
		}
	};

	// ---- M3-027 material probe: WHY does the hit-flash overlay not show? ----
	// The 4th-round fix capture proved every other feedback works (bars from
	// spawn, damage numbers, combo, jog clip, 250 cm launcher lift, knockdown
	// landing) while the BasicShapeMaterial red overlay set on the stunned
	// victims stayed invisible (telemetry showed action=HitStun through the
	// flight, the MID existed and held the red Color, yet apex frames read
	// mannequin gray). This probe runs BEFORE the fight and answers the three
	// candidate root causes with screenshots of the idle player mesh:
	//   1. overlays broken on skinned meshes entirely -> the WorldGridMaterial
	//      probe (the universal fallback) shows nothing too,
	//   2. the static-mesh-only BasicShapeMaterial lacks the skeletal usage
	//      flag and falls back silently -> shape-red shows nothing while the
	//      grid probe renders,
	//   3. the mannequin body material ignores vector parameters -> the
	//      params-red probe (slot-0 material with every vector parameter set
	//      red, rendered as an overlay whose base carries the skeletal usage)
	//      shows no change either.
	// The probe logs the slot-0 material, its base and every inherited vector
	// parameter name (the per-slot tint candidates) into the evidence log.

	// Probe overlay states (one command per state; each screenshot marker sits
	// between waits so the async capture catches exactly one state).
	enum class EM3_027ProbeOverlay : int32
	{
		Clear = 0,
		BasicShapeRed = 1,
		WorldGrid = 2,
		SlotParamsRed = 3
	};

	const TCHAR* M3_027_WorldGridMaterialPath =
		TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

	// Dumps the player mesh's slot-0 material chain and its vector parameters.
	struct FM3_027_MaterialInfoProbe : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		explicit FM3_027_MaterialInfoProbe(const TWeakObjectPtr<APrototypeCharacter>& InPlayer)
			: PlayerPtr(InPlayer) {}
		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			USkeletalMeshComponent* Mesh = Player ? Player->GetMesh() : nullptr;
			UMaterialInterface* Slot0 = Mesh ? Mesh->GetMaterial(0) : nullptr;
			UE_LOG(LogTemp, Display, TEXT("M3_027 probe slot0=%s"),
				Slot0 ? *Slot0->GetFullName() : TEXT("null"));
			UMaterial* Base = Slot0 ? Slot0->GetBaseMaterial() : nullptr;
			UE_LOG(LogTemp, Display, TEXT("M3_027 probe slot0 base=%s"),
				Base ? *Base->GetFullName() : TEXT("null"));
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			if (Slot0 != nullptr)
			{
				Slot0->GetAllParameterInfoOfType(EMaterialParameterType::Vector, Infos, Ids);
				for (const FMaterialParameterInfo& Info : Infos)
				{
					UE_LOG(LogTemp, Display, TEXT("M3_027 probe slot0 vector param: %s"),
						*Info.Name.ToString());
				}
				Infos.Reset();
				Ids.Reset();
			}
			if (Base != nullptr)
			{
				Base->GetAllParameterInfoOfType(EMaterialParameterType::Vector, Infos, Ids);
				for (const FMaterialParameterInfo& Info : Infos)
				{
					UE_LOG(LogTemp, Display, TEXT("M3_027 probe base vector param: %s"),
						*Info.Name.ToString());
				}
			}
			return true;
		}
	};

	// Applies one probe overlay state to the player mesh (presentation only;
	// the Clear step restores the pristine mesh). The overlay MIDs are created
	// with the transient package as outer and kept alive by the mesh's overlay
	// material property until cleared.
	struct FM3_027_ApplyProbeOverlay : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		EM3_027ProbeOverlay State;
		FM3_027_ApplyProbeOverlay(const TWeakObjectPtr<APrototypeCharacter>& InPlayer, EM3_027ProbeOverlay InState)
			: PlayerPtr(InPlayer), State(InState) {}
		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			USkeletalMeshComponent* Mesh = Player ? Player->GetMesh() : nullptr;
			if (Mesh == nullptr)
			{
				return true;
			}
			switch (State)
			{
			case EM3_027ProbeOverlay::BasicShapeRed:
				if (UMaterialInterface* ShapeBase = LoadObject<UMaterialInterface>(
					nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")))
				{
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(
						ShapeBase, GetTransientPackage());
					Mid->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.12f, 0.10f, 1.0f));
					Mesh->SetOverlayMaterial(Mid);
					UE_LOG(LogTemp, Display, TEXT("M3_027 probe overlay applied: BasicShapeMaterial red"));
				}
				break;
			case EM3_027ProbeOverlay::WorldGrid:
				if (UMaterialInterface* Grid = LoadObject<UMaterialInterface>(
					nullptr, M3_027_WorldGridMaterialPath))
				{
					Mesh->SetOverlayMaterial(Grid);
					UE_LOG(LogTemp, Display, TEXT("M3_027 probe overlay applied: WorldGridMaterial"));
				}
				break;
			case EM3_027ProbeOverlay::SlotParamsRed:
				if (UMaterialInterface* Slot0 = Mesh->GetMaterial(0))
				{
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(
						Slot0, GetTransientPackage());
					TArray<FMaterialParameterInfo> Infos;
					TArray<FGuid> Ids;
					Mid->GetAllParameterInfoOfType(EMaterialParameterType::Vector, Infos, Ids);
					int32 Written = 0;
					for (const FMaterialParameterInfo& Info : Infos)
					{
						Mid->SetVectorParameterValue(Info.Name, FLinearColor(1.0f, 0.02f, 0.02f, 1.0f));
						++Written;
					}
					Mesh->SetOverlayMaterial(Mid);
					UE_LOG(LogTemp, Display, TEXT("M3_027 probe overlay applied: slot0 params red (%d params)"), Written);
				}
				break;
			case EM3_027ProbeOverlay::Clear:
			default:
				Mesh->SetOverlayMaterial(nullptr);
				UE_LOG(LogTemp, Display, TEXT("M3_027 probe overlay cleared"));
				break;
			}
			return true;
		}
	};

	// Phase 1: walk +X until the room session leaves Idle (the production
	// trigger overlap starts the run) or the timeout expires.
	struct FM3_027_WalkToTrigger : public IAutomationLatentCommand
	{
		TSharedRef<FM3_027_Recorder> Recorder;
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		double StartedSeconds = 0.0;
		bool bStarted = false;

		explicit FM3_027_WalkToTrigger(const TSharedRef<FM3_027_Recorder>& InRecorder, APrototypeCharacter* InPlayer)
			: Recorder(InRecorder), PlayerPtr(InPlayer) {}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			if (Player == nullptr || Player->GetWorld() == nullptr)
			{
				return true;
			}
			const double Now = FPlatformTime::Seconds();
			if (!bStarted)
			{
				bStarted = true;
				StartedSeconds = Now;
			}
			M3_027_SampleRoom(Player, *Recorder, TEXT("walk"));
			Recorder->MaybeScreenshot();

			URoomSessionSubsystem* Session = Player->GetWorld()->GetSubsystem<URoomSessionSubsystem>();
			if (Session != nullptr && Session->GetState() != ERoomSessionState::Idle)
			{
				Recorder->bRunStarted = true;
				if (Recorder->bSawIdleSession)
				{
					Recorder->Log(FString::Printf(TEXT("walk phase: fresh run started (state %d) after %.2fs"),
						static_cast<int32>(Session->GetState()), Now - StartedSeconds));
				}
				else
				{
					// A previous suite's run is still active in the shared game
					// world: nothing to diagnose in this leftover state (the
					// finalize step skips the spawn assertion for it).
					Recorder->Log(FString::Printf(TEXT("walk phase: session already active (state %d) - no fresh run in this test"),
						static_cast<int32>(Session->GetState())));
				}
				return true;
			}
			Player->AddMovementInput(FVector::ForwardVector, 1.0f);
			if (Now - StartedSeconds >= M3_027_WalkPhaseTimeoutSeconds)
			{
				Recorder->Log(FString::Printf(TEXT("walk phase: TIMEOUT after %.1fs (run never started)"),
					M3_027_WalkPhaseTimeoutSeconds));
				return true;
			}
			return false;
		}
	};

	// Phase 2: approach the nearest living enemy and press Light on cadence
	// once inside the attack band. Ends after the target press count, on the
	// death of every enemy, or on timeout.
	struct FM3_027_ApproachAndAttack : public IAutomationLatentCommand
	{
		TSharedRef<FM3_027_Recorder> Recorder;
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		double StartedSeconds = 0.0;
		double LastPressSeconds = -1.0;
		bool bStarted = false;
		int32 PressCount = 0;

		explicit FM3_027_ApproachAndAttack(const TSharedRef<FM3_027_Recorder>& InRecorder, APrototypeCharacter* InPlayer)
			: Recorder(InRecorder), PlayerPtr(InPlayer) {}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			if (Player == nullptr || Player->GetWorld() == nullptr)
			{
				return true;
			}
			const double Now = FPlatformTime::Seconds();
			if (!bStarted)
			{
				bStarted = true;
				StartedSeconds = Now;
			}
			M3_027_SampleRoom(Player, *Recorder, TEXT("attack"));
			Recorder->MaybeScreenshot();

			// Nearest living enemy (the run's first priority target).
			AMeleeEnemy* Nearest = nullptr;
			float NearestDistance = BIG_NUMBER;
			for (TActorIterator<AMeleeEnemy> It(Player->GetWorld()); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				if (Enemy == nullptr || Enemy->GetHealthComponent() == nullptr
					|| !Enemy->GetHealthComponent()->IsAlive())
				{
					continue;
				}
				const float Distance = FVector::Dist2D(Enemy->GetActorLocation(), Player->GetActorLocation());
				if (Distance < NearestDistance)
				{
					NearestDistance = Distance;
					Nearest = Enemy;
				}
			}
			if (Nearest == nullptr)
			{
				Recorder->Log(FString::Printf(TEXT("attack phase: no living enemy left after %.2fs (%d presses)"),
					Now - StartedSeconds, PressCount));
				return true;
			}

			const FVector Delta = Nearest->GetActorLocation() - Player->GetActorLocation();
			const bool bInBand = FMath::Abs(Delta.X) <= M3_027_AttackBandX && FMath::Abs(Delta.Y) <= M3_027_AttackBandY;
			if (!bInBand)
			{
				// Depth-first-then-X walk toward the enemy (the same rule the
				// enemy brain follows; keeps the driver honest about range).
				if (FMath::Abs(Delta.Y) > M3_027_AttackBandY)
				{
					Player->AddMovementInput(FVector::RightVector, Delta.Y > 0.0f ? 1.0f : -1.0f);
				}
				else
				{
					Player->AddMovementInput(FVector::ForwardVector, Delta.X > 0.0f ? 1.0f : -1.0f);
				}
				return false;
			}
			if (Now - LastPressSeconds >= M3_027_LightPressIntervalSeconds)
			{
				LastPressSeconds = Now;
				++PressCount;
				Player->SubmitCombatInput(ECombatInput::Light);
				Recorder->Log(FString::Printf(TEXT("attack phase: press #%d (dist2d=%.0f)"), PressCount, FVector::Dist2D(Delta, FVector::ZeroVector)));
			}
			if (PressCount >= M3_027_LightPressTarget)
			{
				Recorder->Log(FString::Printf(TEXT("attack phase: done after %d presses in %.2fs"), PressCount, Now - StartedSeconds));
				return true;
			}
			if (Now - StartedSeconds >= M3_027_CombatPhaseTimeoutSeconds)
			{
				Recorder->Log(FString::Printf(TEXT("attack phase: TIMEOUT after %.1fs (%d presses)"),
					M3_027_CombatPhaseTimeoutSeconds, PressCount));
				return true;
			}
			return false;
		}
	};

	// Phase 3: clean launcher presses followed by per-frame Z telemetry of
	// the nearest living enemy (the lift evidence) plus screenshots. A press
	// only fires while the player combat is Free (a press buffered behind a
	// running attack expires within 150 ms - the M1-021 lifetime - and would
	// stage a silent no-op instead of the launch evidence this phase hunts);
	// a press that lands no hit (no HP drop) is retried a few times.
	struct FM3_027_LauncherLift : public IAutomationLatentCommand
	{
		TSharedRef<FM3_027_Recorder> Recorder;
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		double StartedSeconds = 0.0;
		double PressedSeconds = 0.0;
		float TargetHealthAtPress = -1.0f;
		bool bStarted = false;
		bool bPressed = false;
		bool bFlashCaptureDone = false;
		int32 PressCount = 0;

		explicit FM3_027_LauncherLift(const TSharedRef<FM3_027_Recorder>& InRecorder, APrototypeCharacter* InPlayer)
			: Recorder(InRecorder), PlayerPtr(InPlayer) {}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			if (Player == nullptr || Player->GetWorld() == nullptr)
			{
				return true;
			}
			const double Now = FPlatformTime::Seconds();
			if (!bStarted)
			{
				bStarted = true;
				StartedSeconds = Now;
			}

			// Nearest living enemy.
			AMeleeEnemy* Nearest = nullptr;
			float NearestDistance = BIG_NUMBER;
			for (TActorIterator<AMeleeEnemy> It(Player->GetWorld()); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				if (Enemy == nullptr || Enemy->GetHealthComponent() == nullptr
					|| !Enemy->GetHealthComponent()->IsAlive())
				{
					continue;
				}
				const float Distance = FVector::Dist2D(Enemy->GetActorLocation(), Player->GetActorLocation());
				if (Distance < NearestDistance)
				{
					NearestDistance = Distance;
					Nearest = Enemy;
				}
			}
			if (Nearest == nullptr)
			{
				Recorder->Log(TEXT("launcher phase: no living enemy - skipped"));
				return true;
			}

			const FVector Delta = Nearest->GetActorLocation() - Player->GetActorLocation();
			const bool bInBand = FMath::Abs(Delta.X) <= M3_027_AttackBandX && FMath::Abs(Delta.Y) <= M3_027_AttackBandY;
			if (!bPressed)
			{
				// Close in first; press only from a clean combat state inside
				// the band: Free AND an empty input buffer. A buffered Light
				// from the attack phase would be consumed first and the
				// launcher press would expire behind it (the M1-021 150 ms
				// lifetime) - the exact swallow the user reports as "Z does
				// nothing".
				const UCombatComponent* Combat = Player->GetCombat();
				FBufferedCombatInput Pending;
				const bool bClean = Combat != nullptr
					&& Combat->GetSnapshot().ActionState == ECombatActionState::Free
					&& !Combat->PeekInputBuffer(Pending, 0);
				if (!bInBand || !bClean)
				{
					M3_027_SampleRoom(Player, *Recorder, TEXT("launcher-walk"));
					if (!bInBand)
					{
						if (FMath::Abs(Delta.Y) > M3_027_AttackBandY)
						{
							Player->AddMovementInput(FVector::RightVector, Delta.Y > 0.0f ? 1.0f : -1.0f);
						}
						else
						{
							Player->AddMovementInput(FVector::ForwardVector, Delta.X > 0.0f ? 1.0f : -1.0f);
						}
					}
					if (Now - StartedSeconds >= M3_027_CombatPhaseTimeoutSeconds)
					{
						Recorder->Log(TEXT("launcher phase: approach TIMEOUT"));
						return true;
					}
					return false;
				}
				bPressed = true;
				PressedSeconds = Now;
				TargetHealthAtPress = Nearest->GetHealthComponent()->GetHealth();
				Recorder->bLauncherPressed = true;
				Recorder->LauncherPressEnemyZ = Nearest->GetActorLocation().Z;
				Recorder->LauncherMaxEnemyZ = Recorder->LauncherPressEnemyZ;
				Player->SubmitCombatInput(ECombatInput::Launcher);
				Recorder->Log(FString::Printf(TEXT("launcher phase: press #%d (enemy z=%.1f dist2d=%.0f hp=%.0f)"),
					++PressCount, Recorder->LauncherPressEnemyZ,
					FVector::Dist2D(Delta, FVector::ZeroVector), TargetHealthAtPress));
				return false;
			}

			// Post-press telemetry every frame (no stride: the flight is ~1.5 s).
			const FVector Location = Nearest->GetActorLocation();
			const UCharacterMovementComponent* Movement = Nearest->GetCharacterMovement();
			const float Health = Nearest->GetHealthComponent()->GetHealth();
			// The hit-stun flash capture: the first frame with a confirmed HP
			// drop requests one immediate screenshot - the launcher stun holds
			// 1.0 s, so this frame sits inside the flash window and decides
			// visually whether the overlay flash renders red on the victim.
			if (!bFlashCaptureDone && Health < TargetHealthAtPress - 0.5f)
			{
				bFlashCaptureDone = true;
				if (GEngine != nullptr && GEngine->GameViewport != nullptr && FApp::CanEverRender())
				{
					IFileManager::Get().MakeDirectory(*Recorder->EvidencePath(), /*Tree*/ true);
					const FString FlashPath = Recorder->EvidencePath() / TEXT("m3-027-flash-launcher.png");
					FScreenshotRequest::RequestScreenshot(FlashPath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
					Recorder->Log(FString::Printf(TEXT("launcher phase: flash capture requested at hp=%.0f -> %s"), Health, *FlashPath));
				}
			}
			Recorder->LauncherMaxEnemyZ = FMath::Max(Recorder->LauncherMaxEnemyZ, Location.Z);
			Recorder->LauncherMaxRiseZ = FMath::Max(Recorder->LauncherMaxRiseZ, Location.Z - Recorder->LauncherPressEnemyZ);
			if (Movement != nullptr)
			{
				Recorder->LauncherMaxUpSpeed = FMath::Max(Recorder->LauncherMaxUpSpeed, Movement->Velocity.Z);
			}
			Recorder->Log(FString::Printf(TEXT("launcher phase t+%.2f enemy z=%.1f vz=%.0f mode=%d hp=%.0f"),
				Now - PressedSeconds, Location.Z,
				Movement != nullptr ? Movement->Velocity.Z : 0.0f,
				Movement != nullptr ? static_cast<int32>(Movement->MovementMode) : -1,
				Health));
			M3_027_SampleRoom(Player, *Recorder, TEXT("launcher"));
			Recorder->MaybeScreenshot();

			// Retry: the press landed no hit within the launch window (no HP
			// drop 0.7 s after the press) and tries remain - re-press.
			const bool bHitLanded = Health < TargetHealthAtPress - 0.5f;
			if (!bHitLanded && Now - PressedSeconds >= 0.7f && PressCount < 4)
			{
				const UCombatComponent* Combat = Player->GetCombat();
				FBufferedCombatInput Pending;
				const bool bClean = Combat != nullptr
					&& Combat->GetSnapshot().ActionState == ECombatActionState::Free
					&& !Combat->PeekInputBuffer(Pending, 0);
				if (bClean && bInBand)
				{
					bPressed = false; // re-press on the next update
					Recorder->Log(TEXT("launcher phase: press landed no hit - retrying"));
					return false;
				}
			}
			if (Now - PressedSeconds >= M3_027_LauncherPhaseSeconds)
			{
				Recorder->Log(FString::Printf(TEXT("launcher phase: done (presses %d, max z %.1f, rise %.1f, max up speed %.0f, hit %d)"),
					PressCount, Recorder->LauncherMaxEnemyZ, Recorder->LauncherMaxRiseZ,
					Recorder->LauncherMaxUpSpeed, bHitLanded ? 1 : 0));
				return true;
			}
			return false;
		}
	};

	// Final command: writes the evidence bundle and runs the environmental
	// assertions on the still-in-progress test object (latent commands keep
	// the test instance alive until the queue drains).
	struct FM3_027_FinalizeEvidence : public IAutomationLatentCommand
	{
		TSharedRef<FM3_027_Recorder> Recorder;
		FAutomationTestBase& Test;

		FM3_027_FinalizeEvidence(const TSharedRef<FM3_027_Recorder>& InRecorder, FAutomationTestBase& InTest)
			: Recorder(InRecorder), Test(InTest) {}

		virtual bool Update() override
		{
			Recorder->WriteEvidence(true);
			Test.TestTrue(TEXT("the room run started in the real game loop"), Recorder->bRunStarted);
			if (Recorder->bFreshRun)
			{
				// Only a run started from Idle inside this test owes enemies:
				// a session a previous suite left active cannot spawn for this
				// test and is not a product defect.
				Test.TestTrue(TEXT("at least one melee enemy spawned in the run"), Recorder->MaxEnemyCount > 0);
			}
			else
			{
				Test.AddInfo(TEXT("capture skipped: the session was already active from a previous suite (no fresh run to assert enemies for)"));
			}
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_027;

// The offscreen reproduction run. Runs the four diagnosis phases in the real
// game loop and writes the evidence bundle; the assertions cover only the
// environmental preconditions (a booted combat room with a run and enemies) -
// the four perception findings themselves are judged from the evidence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_027RoomCombatVisualDiagnostics,
	"UEMMO.Tasks.M3_027.RoomCombatVisualDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_027RoomCombatVisualDiagnostics::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game)
	{
		AddInfo(TEXT("capture skipped: no running game world/viewport (headless regression)"));
		return true;
	}
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
	if (Player == nullptr)
	{
		AddInfo(TEXT("capture skipped: no prototype player in the running world"));
		return true;
	}

	const TSharedRef<FM3_027_Recorder> Recorder = MakeShared<FM3_027_Recorder>();
	const FString Dir = Recorder->EvidencePath();

	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_MarkerScreenshot(Dir / TEXT("m3-027-000-boot.png")));
	// The material probe: screenshot -> wait -> mutate -> wait, so each async
	// capture catches exactly one overlay state (idle, BasicShape red, grid,
	// params red, restored).
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_MaterialInfoProbe(Player));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_MarkerScreenshot(Dir / TEXT("m3-027-probe-base.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_ApplyProbeOverlay(Player, EM3_027ProbeOverlay::BasicShapeRed));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_MarkerScreenshot(Dir / TEXT("m3-027-probe-shape-red.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_ApplyProbeOverlay(Player, EM3_027ProbeOverlay::WorldGrid));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_MarkerScreenshot(Dir / TEXT("m3-027-probe-grid.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_ApplyProbeOverlay(Player, EM3_027ProbeOverlay::SlotParamsRed));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_MarkerScreenshot(Dir / TEXT("m3-027-probe-params-red.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_ApplyProbeOverlay(Player, EM3_027ProbeOverlay::Clear));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_WalkToTrigger(Recorder, Player));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_MarkerScreenshot(Dir / TEXT("m3-027-001-runstarted.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_ApproachAndAttack(Recorder, Player));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_LauncherLift(Recorder, Player));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_027_FinalizeEvidence(Recorder, *this));
	return true;
}

// ---------------------------------------------------------------------------
// M3-027 fix-locking tests (headless, the FTestWorldWrapper temp-game-world
// precedent): each one locks one presentation fix the diagnosis grounded.
// ---------------------------------------------------------------------------

namespace UE::UEMMO::Tasks::M3_027_Fixes
{
	constexpr float M3_027F_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base (well away from the other suites' arenas).
	const FVector M3_027F_SceneBase(72000.0, 55000.0, 600.0);

	// Floor geometry (top face exactly at the scene base Z).
	const float M3_027F_FloorHalfThickness = 100.0f;
	const float M3_027F_FloorHalfExtentXY = 4000.0f;

	// The shared ground-attack reach (offset 95 + half extent 85): an enemy
	// feet anchor at +95 takes every ground hit through the real pipeline.
	const float M3_027F_EnemyFeetOffsetX = 95.0f;

	const float M3_027F_PlayerSpawnHeight = 120.0f;
	const float M3_027F_EnemySpawnHeight = 90.0f;

	// Chase spawn offset for the locomotion-clip test (depth-first-then-X).
	const FVector M3_027F_ChaseSpawnOffset(500.0, 400.0, 0.0);

	constexpr int32 M3_027F_MaxSettleFrames = 300;
	constexpr int32 M3_027F_MaxWaitFrames = 600;

	// The launcher definition's contract launch speed (the M1-022 value).
	const float M3_027F_LauncherImpulseZ = 700.0f;

	static AActor* M3_027F_SpawnFloor(UWorld& World)
	{
		FActorSpawnParameters Params;
		const FVector Center = M3_027F_SceneBase - FVector(0.0, 0.0, M3_027F_FloorHalfThickness);
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M3_027F_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M3_027F_FloorHalfExtentXY, M3_027F_FloorHalfExtentXY, M3_027F_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Center);
		return Actor;
	}

	static APrototypeCharacter* M3_027F_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M3_027F_SceneBase + FVector(0.0, 0.0, M3_027F_PlayerSpawnHeight),
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

	static AMeleeEnemy* M3_027F_SpawnEnemy(UWorld& World, const FVector& OffsetFromBase)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AMeleeEnemy* Enemy = World.SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(),
			M3_027F_SceneBase + OffsetFromBase + FVector(0.0, 0.0, M3_027F_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
		return Enemy;
	}

	static APrototypeHUD* M3_027F_SpawnHud(UWorld& World, APlayerController& Owner)
	{
		FActorSpawnParameters Params;
		APrototypeHUD* Hud = World.SpawnActor<APrototypeHUD>(
			APrototypeHUD::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (Hud != nullptr)
		{
			// The temp world has no GameMode HUD creation; the assignment is
			// the same state the production flow produces (the controller's
			// HUD), which the enemy registration path reads.
			Owner.MyHUD = Hud;
		}
		return Hud;
	}

	static bool M3_027F_TickFrames(FAutomationTestBase& Test, FTestWorldWrapper& Wrapper, int32 Frames)
	{
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_027F_FrameSeconds)))
			{
				return false;
			}
		}
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M3_027_Fixes;

// Fix lock 1: a spawned room enemy carries its health bar from SPAWN - the
// AMeleeEnemy::BeginPlay registers the enemy with the world's prototype HUD,
// so no accepted hit is needed for the bar to exist (the M3-026 hit-driven
// upsert stays; the user read the hit-only appearance as "no health bar").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_027EnemyBarTracksFromSpawn,
	"UEMMO.Tasks.M3_027.EnemyBarTracksFromSpawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_027EnemyBarTracksFromSpawn::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World)
		|| !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
	{
		return true;
	}
	// The registration resolves the HUD through the first player controller:
	// the temp world gets one (no pawn possession needed for the seam).
	APlayerController* Controller = World->SpawnActor<APlayerController>(
		APlayerController::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the temp player controller exists"), Controller))
	{
		return true;
	}
	APrototypeHUD* Hud = M3_027F_SpawnHud(*World, *Controller);
	if (!TestNotNull(TEXT("the prototype HUD exists"), Hud))
	{
		return true;
	}
	AMeleeEnemy* Enemy = M3_027F_SpawnEnemy(*World, FVector(M3_027F_EnemyFeetOffsetX, 0.0, 0.0));
	if (!TestNotNull(TEXT("the melee enemy exists"), Enemy))
	{
		return true;
	}
	if (!M3_027F_TickFrames(*this, Wrapper, 3))
	{
		return true;
	}
	TestEqual(TEXT("the spawned enemy registered its bar without any hit"), Hud->GetTrackedEnemyCount(), 1);
	TestTrue(TEXT("the tracked enemy is the spawned one"), Hud->PeekTrackedEnemy(0) == Enemy);
	TestTrue(TEXT("the tracked pool is the enemy's own"),
		Hud->PeekTrackedHealth(0) == Enemy->GetHealthComponent());
	return true;
}

// Fix lock 2: an accepted hit visibly reacts on the victim - the flash
// overlay (the mesh's own slot-0 material with every inherited vector
// parameter carrying the flash red) is applied exactly while the hit stun
// holds and removed afterwards (the minimal victim-side feedback; no
// reaction animation asset exists; the render probe proved the overlay
// base must come from the mesh's own material family).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_027HitStunTriggersHitFlash,
	"UEMMO.Tasks.M3_027.HitStunTriggersHitFlash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_027HitStunTriggersHitFlash::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World)
		|| !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld())
		|| !TestNotNull(TEXT("the floor exists"), M3_027F_SpawnFloor(*World)))
	{
		return true;
	}
	World->EnsureCollisionTreeIsBuilt();
	APrototypeCharacter* Player = M3_027F_SpawnPlayer(*World);
	if (!TestNotNull(TEXT("the player exists"), Player))
	{
		return true;
	}
	AMeleeEnemy* Enemy = M3_027F_SpawnEnemy(*World, FVector(M3_027F_EnemyFeetOffsetX, 0.0, 0.0));
	if (!TestNotNull(TEXT("the enemy exists"), Enemy))
	{
		return true;
	}
	if (!M3_027F_TickFrames(*this, Wrapper, M3_027F_MaxSettleFrames))
	{
		return true;
	}
	const FLinearColor TintBefore = Enemy->PeekHitFlashTint();
	TestFalse(TEXT("no flash before any hit"), Enemy->IsHitFlashActive());

	// The production intent entry starts the real light_01; its accepted hit
	// stuns the victim (0.22 s), which the flash follows.
	Player->SubmitCombatInput(ECombatInput::Light);
	bool bFlashSeen = false;
	FLinearColor TintDuring(0.0f, 0.0f, 0.0f, 0.0f);
	for (int32 Frame = 0; Frame < M3_027F_MaxWaitFrames && !bFlashSeen; ++Frame)
	{
		if (!M3_027F_TickFrames(*this, Wrapper, 1))
		{
			return true;
		}
		bFlashSeen = Enemy->IsHitFlashActive();
		TintDuring = Enemy->PeekHitFlashTint();
	}
	TestTrue(TEXT("the accepted hit flashed the enemy mesh"), bFlashSeen);
	TestTrue(TEXT("the flash tint reads red (channel R high, G low)"),
		TintDuring.R > 0.8f && TintDuring.G < 0.5f);

	// The stun end restores the captured original Tint.
	bool bFlashCleared = false;
	for (int32 Frame = 0; Frame < M3_027F_MaxWaitFrames && !bFlashCleared; ++Frame)
	{
		if (!M3_027F_TickFrames(*this, Wrapper, 1))
		{
			return true;
		}
		bFlashCleared = !Enemy->IsHitFlashActive();
	}
	TestTrue(TEXT("the flash cleared when the stun ended"), bFlashCleared);
	TestTrue(TEXT("the tint restored to the pre-hit value"),
		Enemy->PeekHitFlashTint().Equals(TintBefore, 0.01f));
	// The hit itself was real: the enemy pool dropped.
	TestTrue(TEXT("the enemy pool dropped from the accepted hit"),
		Enemy->GetHealthComponent()->GetHealth() < Enemy->GetHealthComponent()->GetMaxHealth());
	return true;
}

// Fix lock 3: a launched enemy ends its arc in the M1-026 landing recovery
// (Knockdown -> Recovering -> Free), so the launcher visibly ends on a
// downed enemy that holds before resuming the chase.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_027LaunchedEnemyKnocksDownOnLanding,
	"UEMMO.Tasks.M3_027.LaunchedEnemyKnocksDownOnLanding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_027LaunchedEnemyKnocksDownOnLanding::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World)
		|| !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld())
		|| !TestNotNull(TEXT("the floor exists"), M3_027F_SpawnFloor(*World)))
	{
		return true;
	}
	World->EnsureCollisionTreeIsBuilt();
	AMeleeEnemy* Enemy = M3_027F_SpawnEnemy(*World, FVector(M3_027F_EnemyFeetOffsetX, 0.0, 0.0));
	if (!TestNotNull(TEXT("the enemy exists"), Enemy))
	{
		return true;
	}
	if (!M3_027F_TickFrames(*this, Wrapper, M3_027F_MaxSettleFrames))
	{
		return true;
	}

	// The production launch dispatch: the virtual LaunchCharacter is called
	// through the victim's public ACharacter interface (the M3-025 test's
	// exact call form - the override itself stays protected).
	ACharacter& EnemyCharacter = *Enemy;
	EnemyCharacter.LaunchCharacter(FVector(0.0f, 0.0f, M3_027F_LauncherImpulseZ),
		/*bXYOverride*/ false, /*bZOverride*/ true);
	bool bBackOnGround = false;
	ECombatActionState StateAtLanding = ECombatActionState::Free;
	for (int32 Frame = 0; Frame < M3_027F_MaxWaitFrames && !bBackOnGround; ++Frame)
	{
		if (!M3_027F_TickFrames(*this, Wrapper, 1))
		{
			return true;
		}
		const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
		bBackOnGround = Movement != nullptr && Movement->IsMovingOnGround();
		StateAtLanding = Enemy->GetCombatComponent()->GetSnapshot().ActionState;
	}
	TestTrue(TEXT("the launched enemy landed again"), bBackOnGround);
	TestTrue(TEXT("the launched landing opened the landing recovery (Knockdown or Recovering)"),
		StateAtLanding == ECombatActionState::Knockdown || StateAtLanding == ECombatActionState::Recovering);

	// The process always totals 0.70 s (M1-026): afterwards the enemy is Free.
	if (!M3_027F_TickFrames(*this, Wrapper, 60))
	{
		return true;
	}
	TestTrue(TEXT("the landing recovery completed back to Free"),
		Enemy->GetCombatComponent()->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// Fix lock 4: a chasing enemy plays the jog clip while it moves (the user
// read the gliding idle loop as "the monsters do not move") and returns to
// the idle when it holds in the attack band.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_027ChasingEnemySwitchesLocomotionClip,
	"UEMMO.Tasks.M3_027.ChasingEnemySwitchesLocomotionClip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_027ChasingEnemySwitchesLocomotionClip::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World)
		|| !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld())
		|| !TestNotNull(TEXT("the floor exists"), M3_027F_SpawnFloor(*World)))
	{
		return true;
	}
	World->EnsureCollisionTreeIsBuilt();
	APrototypeCharacter* Player = M3_027F_SpawnPlayer(*World);
	if (!TestNotNull(TEXT("the player exists"), Player))
	{
		return true;
	}
	AMeleeEnemy* Enemy = M3_027F_SpawnEnemy(*World, M3_027F_ChaseSpawnOffset);
	if (!TestNotNull(TEXT("the enemy exists"), Enemy))
	{
		return true;
	}
	// The M2-002 brain drives the chase (the M3-025 production wiring pairs
	// this with the birth; the direct pairing is the EnemyApproachTests
	// precedent and keeps this suite free of the session pump). The target is
	// injected AFTER the settle so the approach runs inside the detection
	// loop below (an idle brain never moves during the settle).
	Enemy->SpawnDefaultController();
	AMeleeEnemyController* Brain = Cast<AMeleeEnemyController>(Enemy->GetController());
	if (!TestNotNull(TEXT("the enemy brain exists"), Brain))
	{
		return true;
	}
	if (!M3_027F_TickFrames(*this, Wrapper, M3_027F_MaxSettleFrames))
	{
		return true;
	}
	Brain->SetTarget(Player);

	// While the enemy walks toward the player the mesh runs the jog clip.
	UAnimationAsset* AssetWhileMoving = nullptr;
	bool bSawMoving = false;
	for (int32 Frame = 0; Frame < M3_027F_MaxWaitFrames && !bSawMoving; ++Frame)
	{
		if (!M3_027F_TickFrames(*this, Wrapper, 1))
		{
			return true;
		}
		const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
		bSawMoving = Movement != nullptr && Movement->Velocity.Size2D() > 50.0f;
		AssetWhileMoving = Enemy->PeekLocomotionAsset();
	}
	TestTrue(TEXT("the wired enemy approached the player"), bSawMoving);
	if (!TestNotNull(TEXT("the moving enemy presents a locomotion asset"), AssetWhileMoving))
	{
		return true;
	}
	TestTrue(FString::Printf(TEXT("the moving enemy runs the jog clip (asset %s)"), *AssetWhileMoving->GetName()),
		AssetWhileMoving->GetName().Contains(TEXT("Jog")));

	// Holding in the attack band (velocity ~0) returns the idle.
	bool bIdleAgain = false;
	for (int32 Frame = 0; Frame < M3_027F_MaxWaitFrames && !bIdleAgain; ++Frame)
	{
		if (!M3_027F_TickFrames(*this, Wrapper, 1))
		{
			return true;
		}
		const UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
		const bool bHolding = Movement != nullptr && Movement->Velocity.Size2D() <= 20.0f;
		UAnimationAsset* Current = Enemy->PeekLocomotionAsset();
		bIdleAgain = bHolding && Current != nullptr && Current->GetName().Contains(TEXT("Idle"));
	}
	TestTrue(TEXT("the holding enemy returned to the idle clip"), bIdleAgain);
	return true;
}

#endif
