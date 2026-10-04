// M3-029: the new-game first-boot profile bootstrap and the FULL combat loop
// on a fresh install, driven through real ticks and the real service chain.
// The coordinator audit found two P0 gaps this suite locks shut:
//
//   1. The GameFlow NoSaveFound branch never created a profile (the M3-017
//      note said "never automatic"), so a NEW PLAYER's first boot had no
//      profile: ClaimPendingAtomic answered RejectedNoProfile and the growth
//      chain was dead. The M3-029 semantic: NoSaveFound is not a failure but
//      the legal FIRST BOOT answer of the M3-015 StartupLoad, and the game
//      flow bootstraps the new-game profile right there (NewProfile: fresh
//      CharacterId, Level 1, XP 0). RecoveryError stays a failure and keeps
//      the "no profile" state (the M3-015 evidence rule, unchanged).
//   2. The enemy->player chain (Telegraph -> Attack -> player HP loss ->
//      death -> Failed -> retry) had never been verified in one production
//      loop. This suite drives it with the real M3-025 spawner wiring: a
//      world with a player controller + possessed pawn, so every wave-spawned
//      enemy gets its AI controller and the player pawn as the chase target.
//
// The scenes (all on the engine FTestWorldWrapper precedent, real ticks, the
// injected session clock driven alongside every tick exactly the way a game
// frame driver performs - the M2-014/M3-020 scaffold copied with unique
// M3_029 names):
//
//   1. First boot: an isolated EMPTY save prefix -> flow Initialize reports
//      NoSaveFound and the profile EXISTS (valid CharacterId, Level 1, XP 0,
//      empty containers). RED before the fix: HasProfile was false.
//   2. Full loop on that first boot: EnterRoom (counting map opener) ->
//      StartRoom + BeginWaves -> enemies approach the player -> Telegraph ->
//      Attack -> the player HP really drops -> the player kills every enemy
//      through the legal lethal ApplyDamage -> Cleared -> the archived result
//      feeds BeginReward (Applied, no longer RejectedNoProfile) ->
//      ClaimPendingAtomic (XP +50 exactly once, the ORIGINAL pre-generated
//      instance into the inventory) -> equip through the real entries
//      (FEquipmentModel + the pawn's TryEquipStatBonus) -> SaveProfile ->
//      simulated restart through a SECOND real game instance whose flow
//      Initialize restores the committed save (identity, XP, instance id,
//      equipment binding, applied record all match).
//   3. Player death / retry: the lone wave enemy beats the player to 0 HP
//      (the real PlayerDied broadcast), the run fails first-terminal-wins,
//      RetryRoom destroys the failed run's enemies, revives the player with a
//      FULL pool and starts a fresh run that clears normally.
//   4. RecoveryError boundary: two corrupt slots -> the startup pass reports
//      RecoveryError and the profile stays ABSENT (the M3-015 rule is NOT
//      relaxed by the M3-029 bootstrap).
//
// Save isolation: every service runs on the dedicated "M3_029Slot_" prefix
// over an in-memory ISaveStorage - no real file is touched, the player's real
// "Profile_" saves are unreachable by construction.
#include "Misc/AutomationTest.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/MeleeEnemyController.h"
#include "../Items/EquipmentModel.h"
#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Persistence/ProfileSaveGame.h"
#include "../Persistence/ProfileSaveService.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomRetryService.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_029
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_029_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with the other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M3_029_SceneBase(112000.0, 97000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_029_FloorHalfThickness = 100.0f;
	const float M3_029_FloorHalfExtentXY = 4000.0f;

	// Rectangular field sealed by four blocking walls (the M3-025 shape).
	const float M3_029_RoomHalfExtentX = 800.0f;
	const float M3_029_RoomHalfExtentY = 500.0f;
	const float M3_029_WallHalfThickness = 25.0f;
	const float M3_029_WallHalfHeight = 200.0f;

	// Spawn heights above the floor top: characters drop a short distance and
	// settle onto the floor through real physics.
	const float M3_029_PlayerSpawnHeight = 120.0f;
	const float M3_029_EnemySpawnHeight = 90.0f;

	// The wave spawner's slot interval and the session's inter-wave wait (the
	// M2-007/M2-008 production constants the loop rides on).
	constexpr double M3_029_SpawnIntervalSeconds = 0.3;
	constexpr double M3_029_WaveGapSeconds = 1.0;

	// Frame caps: the settle phase waits for the player's ground contact; the
	// attack waits cover the full approach (up to 750 cm at 220 cm/s) plus
	// telegraph plus the attack instance, several times over; the death wait
	// covers ten light_01 hits on the 100 HP pool with margin.
	constexpr int32 M3_029_MaxSettleFrames = 300;
	constexpr double M3_029_MaxAttackWaitSeconds = 25.0;
	constexpr double M3_029_MaxDeathWaitSeconds = 40.0;

	// The isolated save prefix of every service operation in this suite (never
	// "Profile_": the automation cannot reach the player's real saves).
	const TCHAR* M3_029_SlotPrefix = TEXT("M3_029Slot_");

	// The three M3_029Slot_ slot names the cleanup asserts gone.
	static FString M3_029_SlotAName() { return FString(TEXT("M3_029Slot_A")); }
	static FString M3_029_SlotBName() { return FString(TEXT("M3_029Slot_B")); }
	static FString M3_029_IndexSlotName() { return FString(TEXT("M3_029Slot_Index")); }

	// World-static blocking box (floor and walls share the builder; the M3-025
	// pattern, renamed).
	static AActor* M3_029_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor, Name);
		Actor->SetRootComponent(Box);
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetBoxExtent(HalfExtent);
		Box->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Box->RegisterComponent();
		Box->SetWorldLocation(Center);
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top and enables
	// no-controller physics so it settles like a possessed game pawn before
	// the player controller takes over (the M3-025 two-step).
	static APrototypeCharacter* M3_029_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M3_029_SceneBase + FVector(0.0, 0.0, M3_029_PlayerSpawnHeight),
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

	// Definition double of the Data/enemies.json "melee_grunt" row WITH the
	// M1 attack id: the enemy brain runs the full M2-003 pipeline (approach ->
	// telegraph -> light_01) against the player pawn (the M2-003/M3-020 form).
	static UEnemyDefinition* M3_029_MakeEnemyDef()
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = FName(TEXT("melee_grunt"));
		Def->MaxHP = 60.0f;
		Def->AttackPower = 0.0f;
		Def->MoveSpeed = 220.0f;
		Def->AttackRangeX = 160.0f;
		Def->AlignYTolerance = 35.0f;
		Def->TelegraphSeconds = 0.35f;
		Def->SpawnGraceSeconds = 0.5f;
		Def->MeleeAttackId = FName(TEXT("light_01"));
		return Def;
	}

	// Hand-built room definition doubles (the M3-020 precedent). Both carry
	// the selectable map path - GameFlowSubsystem::EnterRoom validates RoomId
	// AND MapPath before accepting an entry. Variant 0: the 2+3 wave shape of
	// room_training_01. Variant 1: one wave of two enemies (the death scene).
	static URoomDefinition* M3_029_MakeRoom(const TCHAR* RoomId, int32 Variant)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(RoomId);
		Room->RewardTableId = FName(TEXT("reward_m3_029"));
		Room->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/UEMMO/Maps/L_TrainingArena")));

		if (Variant == 0)
		{
			FRoomWaveDefinition Wave0;
			Wave0.EnemyId = FName(TEXT("melee_grunt"));
			Wave0.Count = 2;
			Wave0.SpawnLocations.Add(M3_029_SceneBase + FVector(300.0, 0.0, M3_029_EnemySpawnHeight));
			Wave0.SpawnLocations.Add(M3_029_SceneBase + FVector(450.0, -150.0, M3_029_EnemySpawnHeight));
			Room->Waves.Add(Wave0);

			FRoomWaveDefinition Wave1;
			Wave1.EnemyId = FName(TEXT("melee_grunt"));
			Wave1.Count = 3;
			Wave1.SpawnLocations.Add(M3_029_SceneBase + FVector(600.0, 100.0, M3_029_EnemySpawnHeight));
			Wave1.SpawnLocations.Add(M3_029_SceneBase + FVector(750.0, -100.0, M3_029_EnemySpawnHeight));
			Wave1.SpawnLocations.Add(M3_029_SceneBase + FVector(0.0, 300.0, M3_029_EnemySpawnHeight));
			Room->Waves.Add(Wave1);
		}
		else
		{
			// Variant 1 (the death scene): ONE enemy. A lone chaser has no
			// separation peer, so its attack cycle is the deterministic
			// 10 HP per ~1.28 s that kills the 100 HP pool without the
			// multi-chaser standoff jitter two chasers produce around the
			// first arriver's pocket.
			FRoomWaveDefinition Wave0;
			Wave0.EnemyId = FName(TEXT("melee_grunt"));
			Wave0.Count = 1;
			Wave0.SpawnLocations.Add(M3_029_SceneBase + FVector(300.0, 0.0, M3_029_EnemySpawnHeight));
			Room->Waves.Add(Wave0);
		}
		return Room;
	}

	// The three training definitions in a fresh catalog (mirrors
	// Data/items.json; the M3-020 fixture, renamed). The starter drop table
	// picks among these, so the drafted reward piece always equips.
	static FItemDefinitionCatalog M3_029_MakeTrainingCatalog()
	{
		FItemDefinitionCatalog Catalog;

		FItemDefinition Weapon;
		Weapon.DefinitionId = FName(TEXT("weapon_training"));
		Weapon.DisplayName = TEXT("weapon_training");
		Weapon.Slot = EItemSlot::Weapon;
		Weapon.BaseStats.Attack = 5.0f;
		Weapon.BaseStats.Defense = 0.0f;
		Weapon.BaseStats.MaxHP = 0.0f;
		Weapon.IconPath = TEXT("");
		Weapon.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Weapon, nullptr);

		FItemDefinition Armor;
		Armor.DefinitionId = FName(TEXT("armor_training"));
		Armor.DisplayName = TEXT("armor_training");
		Armor.Slot = EItemSlot::Armor;
		Armor.BaseStats.Attack = 0.0f;
		Armor.BaseStats.Defense = 3.0f;
		Armor.BaseStats.MaxHP = 0.0f;
		Armor.IconPath = TEXT("");
		Armor.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Armor, nullptr);

		FItemDefinition Charm;
		Charm.DefinitionId = FName(TEXT("charm_training"));
		Charm.DisplayName = TEXT("charm_training");
		Charm.Slot = EItemSlot::Accessory;
		Charm.BaseStats.Attack = 0.0f;
		Charm.BaseStats.Defense = 0.0f;
		Charm.BaseStats.MaxHP = 20.0f;
		Charm.IconPath = TEXT("");
		Charm.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Charm, nullptr);

		return Catalog;
	}

	/**
	 * In-memory ISaveStorage double (the M3-017 copy, renamed): every written
	 * slot is stored as an independent rooted duplicate and every read returns
	 * a fresh duplicate - the same semantics as disk bytes.
	 */
	class FM3_029_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_029_MemoryStorage() override
		{
			for (TPair<FString, USaveGame*>& Pair : Slots)
			{
				if (Pair.Value)
				{
					Pair.Value->RemoveFromRoot();
				}
			}
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			if (!Data)
			{
				return false;
			}
			USaveGame* Copy = DuplicateObject(Data, GetTransientPackage());
			if (!Copy)
			{
				return false;
			}
			Copy->AddToRoot();
			USaveGame** Existing = Slots.Find(SlotName);
			if (Existing && *Existing)
			{
				(*Existing)->RemoveFromRoot();
			}
			Slots.Add(SlotName, Copy);
			return true;
		}

		virtual USaveGame* ReadSlot(const FString& SlotName) override
		{
			USaveGame* const* Found = Slots.Find(SlotName);
			return Found ? DuplicateObject(*Found, GetTransientPackage()) : nullptr;
		}

		virtual bool DeleteSlot(const FString& SlotName) override
		{
			USaveGame** Existing = Slots.Find(SlotName);
			if (!Existing)
			{
				return false;
			}
			if (*Existing)
			{
				(*Existing)->RemoveFromRoot();
			}
			Slots.Remove(SlotName);
			return true;
		}

		virtual bool DoesSlotExist(const FString& SlotName) override
		{
			return Slots.Contains(SlotName);
		}

	private:
		TMap<FString, USaveGame*> Slots;
	};

	/** Creates a save service on the isolated prefix with the given storage. */
	static UProfileSaveService* M3_029_NewService(FAutomationTestBase& Test, ISaveStorage& Storage)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Service)
		{
			Test.AddError(TEXT("setup: NewObject<UProfileSaveService> returned null"));
			return nullptr;
		}
		if (!Service->Initialize(M3_029_SlotPrefix))
		{
			Test.AddError(TEXT("setup: the save service rejected the M3_029Slot_ prefix"));
			return nullptr;
		}
		Service->SetStorage(&Storage);
		return Service;
	}

	/**
	 * Writes a CORRUPT slot save straight into storage (the M3-015 corruption
	 * model): a well-shaped UProfileSlotSaveGame whose stored integrity digest
	 * deliberately does not match its payload.
	 */
	static bool M3_029_WriteCorruptSlot(ISaveStorage& Storage, const FString& SlotName,
		const FGuid& TagId, int32 TagLevel, int32 TagGeneration, uint32 TagDigest)
	{
		UProfileSlotSaveGame* Corrupt = NewObject<UProfileSlotSaveGame>(GetTransientPackage());
		Corrupt->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
		Corrupt->CharacterId = TagId;
		Corrupt->CharacterIdSummary = TagId.ToString();
		Corrupt->Level = TagLevel;
		Corrupt->PayloadInstanceCount = 0;
		Corrupt->PayloadDigest = TagDigest; // deliberately WRONG: the digest gate must reject it
		Corrupt->SlotGeneration = TagGeneration;
		return Storage.WriteSlot(SlotName, Corrupt);
	}

	/** Deletes the suite's three slots and asserts they are gone. */
	static void M3_029_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service, ISaveStorage& Storage)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: M3_029Slot_A deleted"), !Storage.DoesSlotExist(M3_029_SlotAName()));
		Test.TestTrue(TEXT("cleanup: M3_029Slot_B deleted"), !Storage.DoesSlotExist(M3_029_SlotBName()));
		Test.TestTrue(TEXT("cleanup: M3_029Slot_Index deleted"), !Storage.DoesSlotExist(M3_029_IndexSlotName()));
	}

	// -- The world scene -------------------------------------------------------

	/**
	 * One combat scene: the engine FTestWorldWrapper owns the manually ticked
	 * temp world with a REAL game instance (created and Init()ed inside
	 * CreateTestWorld, so a save service parked BEFORE Create runs the
	 * production startup chain inside the flow's Initialize). The scene builds
	 * the walled arena, the player pawn, the possessing player controller and
	 * the session bindings - the exact form the M3-025 production wiring
	 * resolves its chase targets from.
	 */
	struct FM3_029_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		UGameInstance* GameInstance = nullptr;
		UProfileSubsystem* Profile = nullptr;
		UGameFlowSubsystem* Flow = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		APlayerController* PlayerController = nullptr;
		URoomDefinition* Room = nullptr;
		UProfileSaveService* Save = nullptr;
		URewardService* Reward = nullptr;

		// The counting map-open simulator (a temp world cannot truly travel).
		int32 MapOpenCount = 0;
		FString LastOpenedMapPath;

		// The injected scenario clock (driven alongside every tick).
		double ClockSeconds = 0.0;

		// Delegate counters (the assertions read these only).
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;
		int32 PlayerDiedCount = 0;

		// Enemy brain observations, sampled every frame of the attack waits:
		// any wired enemy controller seen in Telegraph / Attack before the
		// player HP dropped (the card's Telegraph -> Attack chain evidence).
		bool bSawTelegraph = false;
		bool bSawAttack = false;

		// Per-frame wave sampling state (the M2-014 pattern).
		int32 LastStartedWaveCount = 0;
		TArray<TWeakObjectPtr<AMeleeEnemy>> TrackedEnemies;
		int32 EnemySerial = 0;

		/**
		 * Creates the world (the caller may park a startup save service BEFORE
		 * this, so the flow's Initialize inside consumes it), resolves the
		 * subsystems, arms the seams and bindings and builds the arena with
		 * the player + player controller. Returns false after reporting.
		 */
		bool Build(FAutomationTestBase& Test, const TCHAR* Tag, int32 RoomVariant)
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
			GameInstance = World->GetGameInstance();
			Profile = GameInstance ? GameInstance->GetSubsystem<UProfileSubsystem>() : nullptr;
			Flow = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the profile subsystem exists in the test game instance"), Profile)
				|| !Test.TestNotNull(TEXT("the game flow subsystem exists in the test game instance"), Flow)
				|| !Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}

			// The counting map-open simulator (production never travels in a
			// temp world; the seam keeps the flow's state machine observable).
			Flow->SetMapOpenerForTests([this](const FString& MapPath)
			{
				++MapOpenCount;
				LastOpenedMapPath = MapPath;
				return true;
			});

			Session->OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
			});
			Session->OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
			});

			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}

			AActor* Floor = M3_029_SpawnBoxActor(*World,
				M3_029_SceneBase - FVector(0.0, 0.0, M3_029_FloorHalfThickness),
				FVector(M3_029_FloorHalfExtentXY, M3_029_FloorHalfExtentXY, M3_029_FloorHalfThickness),
				TEXT("M3_029_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first tick (the M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			// Four blocking walls sealing the rectangular field.
			const float WallShiftX = M3_029_RoomHalfExtentX + M3_029_WallHalfThickness;
			const float WallShiftY = M3_029_RoomHalfExtentY + M3_029_WallHalfThickness;
			const float WallSpanX = M3_029_RoomHalfExtentX + 2.0f * M3_029_WallHalfThickness;
			const float WallSpanY = M3_029_RoomHalfExtentY + 2.0f * M3_029_WallHalfThickness;
			M3_029_SpawnBoxActor(*World, M3_029_SceneBase + FVector(-WallShiftX, 0.0, M3_029_WallHalfHeight),
				FVector(M3_029_WallHalfThickness, WallSpanY, M3_029_WallHalfHeight), TEXT("M3_029_WallNegX"));
			M3_029_SpawnBoxActor(*World, M3_029_SceneBase + FVector(WallShiftX, 0.0, M3_029_WallHalfHeight),
				FVector(M3_029_WallHalfThickness, WallSpanY, M3_029_WallHalfHeight), TEXT("M3_029_WallPosX"));
			M3_029_SpawnBoxActor(*World, M3_029_SceneBase + FVector(0.0, -WallShiftY, M3_029_WallHalfHeight),
				FVector(WallSpanX, M3_029_WallHalfThickness, M3_029_WallHalfHeight), TEXT("M3_029_WallNegY"));
			M3_029_SpawnBoxActor(*World, M3_029_SceneBase + FVector(0.0, WallShiftY, M3_029_WallHalfHeight),
				FVector(WallSpanX, M3_029_WallHalfThickness, M3_029_WallHalfHeight), TEXT("M3_029_WallPosY"));

			Player = M3_029_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			// The production wiring resolves the chase target from the world's
			// first player controller, so the scene spawns one and possesses
			// the pawn with it (the M3-025 in-game possession form).
			PlayerController = World->SpawnActor<APlayerController>(
				APlayerController::StaticClass(),
				M3_029_SceneBase + FVector(0.0, 0.0, 2.0 * M3_029_WallHalfHeight + 100.0),
				FRotator::ZeroRotator, FActorSpawnParameters());
			if (!Test.TestNotNull(TEXT("the player controller spawns"), PlayerController))
			{
				return false;
			}
			PlayerController->Possess(Player);

			Player->PlayerDied.AddLambda([this]()
			{
				++PlayerDiedCount;
			});
			// The death binding under test (the run failure hook).
			Session->SetPlayer(Player);
			Room = M3_029_MakeRoom(*FString::Printf(TEXT("room_m3_029_%s"), Tag), RoomVariant);

			return SettlePlayer(Test);
		}

		/** Attaches the persistence services (rooted) to the scene. */
		bool AttachPersistence(FAutomationTestBase& Test, ISaveStorage& Storage)
		{
			Save = M3_029_NewService(Test, Storage);
			if (!Test.TestNotNull(TEXT("the scene save service is constructible"), Save))
			{
				return false;
			}
			Save->AddToRoot();
			Reward = GameInstance ? NewObject<URewardService>(GameInstance) : nullptr;
			if (!Test.TestNotNull(TEXT("the scene reward service is constructible"), Reward))
			{
				return false;
			}
			Reward->AddToRoot();
			Reward->BindProfile(Profile);
			return true;
		}

		/** Clears the static startup seam and unroots the scene services. */
		void TearDown()
		{
			UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);
			if (Save)
			{
				Save->RemoveFromRoot();
				Save = nullptr;
			}
			if (Reward)
			{
				Reward->RemoveFromRoot();
				Reward = nullptr;
			}
		}

		/** Ticks the world until the player stands grounded on the floor. */
		bool SettlePlayer(FAutomationTestBase& Test)
		{
			if (Player == nullptr)
			{
				return false;
			}
			for (int32 Frame = 0; Frame < M3_029_MaxSettleFrames; ++Frame)
			{
				if (!TickSeconds(Test, M3_029_FrameSeconds))
				{
					return false;
				}
				const UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
				if (Movement != nullptr
					&& Movement->MovementMode == MOVE_Walking
					&& Movement->Velocity.Size() < 1.0f)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the player never settled onto the floor within the settle cap"));
			return false;
		}

		/** One event-log-free world observation (wave starts + brain states). */
		void Sample()
		{
			if (Session != nullptr)
			{
				const int32 Started = Session->GetStartedWaveCount();
				if (Started > LastStartedWaveCount)
				{
					LastStartedWaveCount = Started;
				}
			}
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				if (!IsValid(Enemy))
				{
					continue;
				}
				bool bTracked = false;
				for (const TWeakObjectPtr<AMeleeEnemy>& Known : TrackedEnemies)
				{
					if (Known.Get() == Enemy)
					{
						bTracked = true;
						break;
					}
				}
				if (bTracked)
				{
					continue;
				}
				TrackedEnemies.Add(Enemy);
				++EnemySerial;
			}
			SampleEnemyBrainStates();
		}

		/** Records whether any wired enemy controller is mid wind-up/attack. */
		void SampleEnemyBrainStates()
		{
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				if (!IsValid(Enemy))
				{
					continue;
				}
				const AMeleeEnemyController* Controller = Cast<AMeleeEnemyController>(Enemy->GetController());
				if (Controller == nullptr)
				{
					continue;
				}
				if (Controller->GetState() == EMeleeEnemyState::Telegraph)
				{
					bSawTelegraph = true;
				}
				else if (Controller->GetState() == EMeleeEnemyState::Attack)
				{
					bSawAttack = true;
				}
			}
		}

		/** Injects one exact clock value and samples the world right after. */
		void InjectClock(double NowSeconds)
		{
			ClockSeconds = NowSeconds;
			Session->SetSessionClockSeconds(NowSeconds);
			Sample();
		}

		/** Ticks the world once with the requested delta. */
		bool TickSeconds(FAutomationTestBase& Test, float DeltaSeconds)
		{
			return Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(DeltaSeconds));
		}

		/**
		 * Advances the world with real 60 fps ticks while injecting the
		 * session clock alongside (the same per-frame duty a game driver
		 * performs), so due births, the inter-wave wait and the settlement
		 * elapse through the production pump path.
		 */
		bool AdvanceSeconds(FAutomationTestBase& Test, double Seconds)
		{
			const double StepSeconds = static_cast<double>(M3_029_FrameSeconds);
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double StepNow = FMath::Min(StepSeconds, Remaining);
				InjectClock(ClockSeconds + StepNow);
				if (!TickSeconds(Test, M3_029_FrameSeconds))
				{
					return false;
				}
				Sample();
				Remaining -= StepNow;
			}
			return true;
		}

		/** Anchors/continues the CURRENT wave and advances past every slot. */
		bool SpawnCurrentWaveFully(FAutomationTestBase& Test, int32 WaveIndex)
		{
			const int32 Count = FMath::Max(0, Room->Waves[WaveIndex].Count);
			const double Span = M3_029_SpawnIntervalSeconds * static_cast<double>(Count - 1) + 0.05;
			return AdvanceSeconds(Test, Span);
		}

		/**
		 * Ticks until the player HP drops below its value at call time (the
		 * first landed enemy attack), sampling the enemy brain states every
		 * frame. Returns false (and reports) when nothing landed within the
		 * cap; HpBefore/HpAfter receive the pool around the drop.
		 */
		bool WaitForPlayerHurt(FAutomationTestBase& Test, double CapSeconds, float& HpBefore, float& HpAfter)
		{
			HpBefore = Player->GetHealth()->GetHealth();
			const double StartClock = ClockSeconds;
			while (ClockSeconds - StartClock < CapSeconds)
			{
				if (!AdvanceSeconds(Test, static_cast<double>(M3_029_FrameSeconds)))
				{
					return false;
				}
				const float Current = Player->GetHealth()->GetHealth();
				if (Current < HpBefore - 0.01f)
				{
					HpAfter = Current;
					return true;
				}
			}
			HpAfter = Player->GetHealth()->GetHealth();
			Test.AddError(FString::Printf(TEXT("no enemy attack landed on the player within %.1f simulated seconds (HP %.1f -> %.1f, telegraph seen %d, attack seen %d)"),
				CapSeconds, HpBefore, HpAfter, bSawTelegraph ? 1 : 0, bSawAttack ? 1 : 0));
			return false;
		}

		/** Alive melee enemies (corpses deliberately not counted). */
		int32 AliveEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				UHealthComponent* Health = It->GetHealthComponent();
				if (Health != nullptr && Health->IsAlive())
				{
					++Count;
				}
			}
			return Count;
		}

		/** Every melee enemy actor in the world including corpses. */
		int32 TotalEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				++Count;
			}
			return Count;
		}

		/**
		 * The player's kills: every alive melee enemy takes the legal lethal
		 * ApplyDamage through its own health component (the same public API
		 * every suite uses; the death then flows through the spawner's real
		 * death binding into the session bookkeeping). Returns the killed
		 * count; reports per-enemy failures.
		 */
		int32 KillAllAliveEnemies(FAutomationTestBase& Test)
		{
			int32 Killed = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				UHealthComponent* Health = (Enemy != nullptr) ? Enemy->GetHealthComponent() : nullptr;
				if (Health == nullptr || !Health->IsAlive())
				{
					continue;
				}
				if (Test.TestTrue(FString::Printf(TEXT("enemy %s accepts the lethal damage"), *Enemy->GetName()),
					Health->ApplyDamage(9999.0f) > 0.0f))
				{
					++Killed;
				}
			}
			return Killed;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_029;

// 1. Acceptance: the first boot of a fresh install. The isolated prefix holds
//    NO save at all, so the flow's Initialize startup pass answers
//    NoSaveFound - and (M3-029) bootstraps the new-game profile right there:
//    the profile EXISTS with a valid CharacterId, Level 1, XP 0 and empty
//    containers. RED before M3-029: the branch left HasProfile false, so the
//    first-boot claim flow was dead (RejectedNoProfile).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_029FirstBootNoSaveFoundBootstrapsNewGameProfile,
	"UEMMO.Tasks.M3_029.FirstBootNoSaveFoundBootstrapsNewGameProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_029FirstBootNoSaveFoundBootstrapsNewGameProfile::RunTest(const FString& Parameters)
{
	// Defensive: a leaked startup seam from any earlier suite must never
	// silently restore into this suite's fresh flow.
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);

	// An EMPTY isolated prefix: the genuine "nothing stored at all" state.
	FM3_029_MemoryStorage Storage;
	UProfileSaveService* Service = M3_029_NewService(*this, Storage);
	if (Service == nullptr)
	{
		return true;
	}
	UGameFlowSubsystem::SetStartupSaveServiceForTests(Service);

	FM3_029_Scene Scene;
	if (!Scene.Build(*this, TEXT("FirstBoot"), 0))
	{
		Scene.TearDown();
		return true;
	}

	// The startup outcome: the real pass ran and found nothing stored.
	const FStartupLoadOutcome& Outcome = Scene.Flow->GetStartupLoadOutcome();
	TestEqual(TEXT("the fresh-install startup pass reports NoSaveFound"),
		static_cast<int32>(Outcome.Result), static_cast<int32>(EStartupLoadResult::NoSaveFound));
	TestFalse(TEXT("the first boot is a bootstrap, not a restore"),
		Scene.Flow->WasStartupProfileRestored());

	// THE M3-029 FIX: the profile exists after the first boot.
	TestTrue(TEXT("the first boot bootstrapped the new-game profile (M3-029)"), Scene.Profile->HasProfile());
	TestTrue(TEXT("the bootstrapped CharacterId is a valid fresh identity"),
		Scene.Profile->GetCharacterId().IsValid());
	TestEqual(TEXT("the bootstrapped profile starts at level 1"), Scene.Profile->GetLevel(), 1);
	TestEqual(TEXT("the bootstrapped profile starts at 0 XP"), Scene.Profile->GetXP(), 0);
	TestEqual(TEXT("the bootstrapped inventory is empty"), Scene.Profile->GetInventory().Count(), 0);
	TestEqual(TEXT("the bootstrapped pending drafts are empty"), Scene.Profile->GetPendingRewards().Num(), 0);
	TestEqual(TEXT("the bootstrapped profile reports the level-1 base row"),
		Scene.Profile->GetProfileSnapshot().MaxHP, UProfileSubsystem::GetMaxHPForLevel(1));

	Scene.TearDown();
	return true;
}

// 2. Acceptance: the FULL combat loop on that first boot, every stage through
//    the real production chain: EnterRoom -> StartRoom + BeginWaves -> the
//    wave enemies approach the player and land real light_01 hits (Telegraph
//    -> Attack -> the player HP drops) -> the player kills every enemy
//    (ApplyDamage) -> Cleared -> BeginReward (Applied) -> ClaimPendingAtomic
//    (XP +50 once, the ORIGINAL instance stored) -> equip (FEquipmentModel +
//    TryEquipStatBonus) -> SaveProfile -> simulated restart through a second
//    real game instance restores identity, progress, instance and binding.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_029FullLoopFightClaimEquipSaveRestore,
	"UEMMO.Tasks.M3_029.FullLoopFightClaimEquipSaveRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_029FullLoopFightClaimEquipSaveRestore::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);

	FM3_029_MemoryStorage Storage;
	UProfileSaveService* BootService = M3_029_NewService(*this, Storage);
	if (BootService == nullptr)
	{
		return true;
	}
	UGameFlowSubsystem::SetStartupSaveServiceForTests(BootService);

	FM3_029_Scene Scene;
	if (!Scene.Build(*this, TEXT("Loop"), 0))
	{
		Scene.TearDown();
		return true;
	}

	// -- First boot: the flow bootstrapped the profile (M3-029) ---------------
	const FStartupLoadOutcome& BootOutcome = Scene.Flow->GetStartupLoadOutcome();
	TestEqual(TEXT("first boot: the startup pass reports NoSaveFound"),
		static_cast<int32>(BootOutcome.Result), static_cast<int32>(EStartupLoadResult::NoSaveFound));
	TestTrue(TEXT("first boot: the profile was bootstrapped (M3-029)"), Scene.Profile->HasProfile());
	TestTrue(TEXT("first boot: the CharacterId is valid"), Scene.Profile->GetCharacterId().IsValid());
	const FGuid CharacterIdA = Scene.Profile->GetCharacterId();
	TestEqual(TEXT("first boot: level 1"), Scene.Profile->GetLevel(), 1);
	TestEqual(TEXT("first boot: XP 0"), Scene.Profile->GetXP(), 0);

	if (!Scene.AttachPersistence(*this, Storage))
	{
		Scene.TearDown();
		return true;
	}

	// -- Enter the room through the flow state machine ------------------------
	if (!TestTrue(TEXT("EnterRoom accepts the first-boot menu entry"), Scene.Flow->EnterRoom(Scene.Room)))
	{
		Scene.TearDown();
		return true;
	}
	TestTrue(TEXT("the flow reached the room state"), Scene.Flow->GetState() == EGameFlowState::Room);
	TestEqual(TEXT("the map opener ran exactly once"), Scene.MapOpenCount, 1);

	// -- The run: StartRoom + BeginWaves through the session ------------------
	if (!TestTrue(TEXT("StartRoom accepts the run"), Scene.Session->StartRoom(Scene.Room)))
	{
		Scene.TearDown();
		return true;
	}
	const uint64 RunIdA = Scene.Session->GetRunId();
	if (!TestTrue(TEXT("BeginWaves starts the progression from wave 0"),
		Scene.Session->BeginWaves(Scene.Room, M3_029_MakeEnemyDef())))
	{
		Scene.TearDown();
		return true;
	}

	// Wave 0: birth both, wait for the first REAL enemy attack on the player.
	Scene.InjectClock(0.0);
	if (!Scene.SpawnCurrentWaveFully(*this, 0))
	{
		Scene.TearDown();
		return true;
	}
	TestEqual(TEXT("both wave-0 enemies were birthed"), Scene.Session->GetSpawnedEnemyCount(), 2);
	float HpBefore0 = -1.0f;
	float HpAfter0 = -1.0f;
	if (!Scene.WaitForPlayerHurt(*this, M3_029_MaxAttackWaitSeconds, HpBefore0, HpAfter0))
	{
		Scene.TearDown();
		return true;
	}
	TestTrue(TEXT("the enemy chain ran Telegraph -> Attack before the hit"),
		Scene.bSawTelegraph && Scene.bSawAttack);
	TestTrue(TEXT("the player HP really dropped from the enemy attack"),
		HpAfter0 < HpBefore0 - 0.01f && HpAfter0 > 0.0f);

	// The player's kills: every wave-0 enemy dies through ApplyDamage.
	const int32 Wave0Kills = Scene.KillAllAliveEnemies(*this);
	TestEqual(TEXT("both wave-0 enemies died from the player's kills"), Wave0Kills, 2);
	TestEqual(TEXT("the session counted both kills"), Scene.Session->GetKilledCount(), 2);

	// Wave 1: past the gap, three more enemies approach and attack.
	if (!Scene.AdvanceSeconds(*this, M3_029_WaveGapSeconds + 0.05))
	{
		Scene.TearDown();
		return true;
	}
	TestEqual(TEXT("past the wait the wave index is 1"), Scene.Session->GetCurrentWaveIndex(), 1);
	if (!Scene.SpawnCurrentWaveFully(*this, 1))
	{
		Scene.TearDown();
		return true;
	}
	TestEqual(TEXT("all five enemies were birthed for the run (2+3)"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	float HpBefore1 = -1.0f;
	float HpAfter1 = -1.0f;
	if (!Scene.WaitForPlayerHurt(*this, M3_029_MaxAttackWaitSeconds, HpBefore1, HpAfter1))
	{
		Scene.TearDown();
		return true;
	}
	const int32 Wave1Kills = Scene.KillAllAliveEnemies(*this);
	TestEqual(TEXT("all three wave-1 enemies died from the player's kills"), Wave1Kills, 3);

	// Settlement: cleared exactly once with exactly the five real kills.
	TestTrue(TEXT("the run is Cleared after the last kill"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the session counted exactly five kills"), Scene.Session->GetKilledCount(), 5);
	TestEqual(TEXT("exactly one end event fired"), Scene.EndedCount, 1);
	FRoomResult ResultA;
	TestTrue(TEXT("the cleared run's result is queryable from the archive"),
		Scene.Session->GetRunResult(RunIdA, ResultA));
	TestTrue(TEXT("the archived result records Cleared"), ResultA.bCleared);
	TestTrue(TEXT("the cleared run is reward eligible"), Scene.Session->IsRewardEligible(RunIdA));
	TestTrue(TEXT("the archived result carries a settlement id"), ResultA.SettlementId != 0);

	// -- Reward: BeginReward on the first-boot profile (no more rejection) ----
	const FItemDefinitionCatalog Catalog = M3_029_MakeTrainingCatalog();
	const FRewardBeginOutcome Begin = Scene.Reward->BeginReward(ResultA, Catalog);
	TestEqual(TEXT("BeginReward applied the settlement draft (no longer RejectedNoProfile)"),
		static_cast<int32>(Begin.Result), static_cast<int32>(ERewardBeginResult::Applied));

	if (Begin.Result == ERewardBeginResult::Applied && Begin.Draft.Items.Num() == 1)
	{
		const FItemInstance RewardItem = Begin.Draft.Items[0];
		const int32 XPBeforeClaim = Scene.Profile->GetXP();

		// The atomic claim: XP + the ORIGINAL instance commit as one snapshot.
		const FRewardClaimAtomicOutcome Claim = Scene.Reward->ClaimPendingAtomic(
			ResultA.SettlementId, Scene.Profile, Scene.Save);
		TestEqual(TEXT("the atomic claim committed"), Claim.Result, ERewardClaimAtomicResult::Claimed);
		TestTrue(TEXT("the claim granted the settlement XP once"), Claim.bGrantedXP);
		TestTrue(TEXT("the claim's snapshot save committed"), Claim.bSaveCommitted);
		TestEqual(TEXT("exactly one equipment piece was stored"), Claim.ClaimedItemCount, 1);
		TestEqual(TEXT("the XP grew by exactly the design 50"),
			Scene.Profile->GetXP() - XPBeforeClaim, URewardService::RewardXPPerClear);
		TestTrue(TEXT("the stored piece is the ORIGINAL instance"),
			Scene.Profile->GetInventory().Contains(RewardItem.InstanceId));
		TestEqual(TEXT("no pending draft is left"), Scene.Profile->GetPendingRewards().Num(), 0);
		TestTrue(TEXT("the settlement is recorded as applied"),
			Scene.Profile->IsSettlementApplied(ResultA.SettlementId));

		// -- Equip through the real entries (the run is no longer Running) ---
		const FItemDefinition* Definition = Catalog.Find(RewardItem.DefinitionId);
		if (TestNotNull(TEXT("the drafted piece's definition is in the catalog"), Definition))
		{
			FEquipmentModel Equipment;
			Equipment.SetDefinitionCatalog(&Catalog);
			TestEqual(TEXT("the equip mapping accepted the reward piece"),
				Equipment.Equip(Definition->Slot, RewardItem.InstanceId, Scene.Profile->GetInventory()),
				EEquipmentEquipResult::Equipped);
			// The gameplay-layer stat derivation: the plain component sum of
			// the equipped instance's rolled stats (the M3-020 convention).
			const FItemInstance* Stored = nullptr;
			for (const FItemInstance& Candidate : Scene.Profile->GetInventory().GetAll())
			{
				if (Candidate.InstanceId == RewardItem.InstanceId)
				{
					Stored = &Candidate;
					break;
				}
			}
			if (TestNotNull(TEXT("the equipped instance is stored in the inventory"), Stored))
			{
				FItemStats EquipRow;
				EquipRow.Attack = FMath::Max(0.0f, Stored->RolledStats.Attack);
				EquipRow.Defense = FMath::Max(0.0f, Stored->RolledStats.Defense);
				EquipRow.MaxHP = FMath::Max(0.0f, Stored->RolledStats.MaxHP);
				TestTrue(TEXT("the pawn equip entry accepted the derived row"),
					Scene.Player->TryEquipStatBonus(EquipRow));
				const FItemStats& StoredRow = Scene.Profile->GetEquippedStatBonus();
				TestTrue(TEXT("the profile stored the equipped row"),
					StoredRow.Attack == EquipRow.Attack && StoredRow.Defense == EquipRow.Defense
					&& StoredRow.MaxHP == EquipRow.MaxHP);
			}

			// -- Save the equipped profile (the explicit SaveProfile commit) --
			const FProfileLoadOutcome LastCommitted = Scene.Save->LoadActiveProfile();
			TestEqual(TEXT("the claim's committed save loads back"),
				static_cast<int32>(LastCommitted.Result), static_cast<int32>(EProfileLoadResult::Success));
			FProfileSaveRequest SaveRequest;
			SaveRequest.Snapshot = Scene.Profile->GetProfileSnapshot();
			SaveRequest.Inventory = Scene.Profile->GetInventory();
			SaveRequest.PendingRewards = Scene.Profile->GetPendingRewards();
			SaveRequest.AppliedSettlementIds = LastCommitted.AppliedSettlementIds;
			SaveRequest.EquippedMap.Add(Definition->Slot, RewardItem.InstanceId);
			const FProfileSaveOutcome Saved = Scene.Save->SaveProfile(SaveRequest);
			TestEqual(TEXT("the equipped profile save committed"),
				static_cast<int32>(Saved.Result), static_cast<int32>(EProfileSaveResult::Success));

			// -- Simulated restart: a SECOND real game instance ---------------
			// The flow's Initialize runs the production startup chain (the
			// M3-017 injected-service seam) and restores the committed save.
			UProfileSaveService* ReloadService = M3_029_NewService(*this, Storage);
			if (ReloadService != nullptr)
			{
				UGameFlowSubsystem::SetStartupSaveServiceForTests(ReloadService);

				FM3_029_Scene Restart;
				if (Restart.Build(*this, TEXT("Restart"), 1))
				{
					const FStartupLoadOutcome& Loaded = Restart.Flow->GetStartupLoadOutcome();
					TestEqual(TEXT("restart: the startup pass recovered the save"),
						static_cast<int32>(Loaded.Result), static_cast<int32>(EStartupLoadResult::Recovered));
					TestTrue(TEXT("restart: the flow reports the restore"),
						Restart.Flow->WasStartupProfileRestored());
					TestTrue(TEXT("restart: the restored profile keeps the first boot's identity"),
						Restart.Profile->GetCharacterId() == CharacterIdA);
					TestEqual(TEXT("restart: the restored level matches"), Restart.Profile->GetLevel(), 1);
					TestEqual(TEXT("restart: the restored XP includes the claimed 50"),
						Restart.Profile->GetXP(), URewardService::RewardXPPerClear);
					TestTrue(TEXT("restart: the restored inventory keeps the ORIGINAL instance"),
						Restart.Profile->GetInventory().Contains(RewardItem.InstanceId));
					TestTrue(TEXT("restart: the restored equipment binding survives"),
						Loaded.EquippedMap.Contains(Definition->Slot)
						&& Loaded.EquippedMap[Definition->Slot] == RewardItem.InstanceId);
					TestTrue(TEXT("restart: the applied settlement record survives"),
						Restart.Profile->IsSettlementApplied(ResultA.SettlementId));
					// The restore never fossilizes the equipped bonus row: the
					// gameplay layer re-derives it from the bindings.
					const FItemStats& RestoredRow = Restart.Profile->GetEquippedStatBonus();
					TestTrue(TEXT("restart: the restored bonus row is the zero row"),
						RestoredRow.Attack == 0.0f && RestoredRow.Defense == 0.0f && RestoredRow.MaxHP == 0.0f);
				}
				Restart.TearDown();

				M3_029_CleanupTestSlots(*this, *ReloadService, Storage);
				ReloadService = nullptr;
			}
		}
	}

	// -- Close the flow cycle: Result -> Menu ---------------------------------
	TestTrue(TEXT("NotifyRunEnded moves the room to the result surface"), Scene.Flow->NotifyRunEnded());
	TestTrue(TEXT("the flow shows the result state"), Scene.Flow->GetState() == EGameFlowState::Result);
	TestTrue(TEXT("ReturnToMenu returns to the menu"), Scene.Flow->ReturnToMenu());
	TestTrue(TEXT("the flow is back in the menu state"), Scene.Flow->GetState() == EGameFlowState::Menu);
	TestTrue(TEXT("the profile survives the room exit (the GameInstance lifecycle)"),
		Scene.Profile->HasProfile() && Scene.Profile->GetCharacterId() == CharacterIdA);

	Scene.TearDown();
	return true;
}

// 3. Acceptance: the player death / retry chain driven by a REAL enemy
//    attack. The lone wave enemy approaches and beats the player to 0 HP (the
//    real PlayerDied broadcast), the run fails first-terminal-wins, RetryRoom
//    destroys the failed run's enemies, revives the player with a FULL pool
//    and starts a fresh run that the player clears normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_029EnemyAttacksKillPlayerThenRetryRevivesAndClears,
	"UEMMO.Tasks.M3_029.EnemyAttacksKillPlayerThenRetryRevivesAndClears",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_029EnemyAttacksKillPlayerThenRetryRevivesAndClears::RunTest(const FString& Parameters)
{
	// No save service: the death/retry chain rides the M2-004 health pool,
	// which is independent of the profile by design.
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);

	FM3_029_Scene Scene;
	if (!Scene.Build(*this, TEXT("Death"), 1))
	{
		Scene.TearDown();
		return true;
	}

	if (!TestTrue(TEXT("StartRoom accepts the run"), Scene.Session->StartRoom(Scene.Room)))
	{
		Scene.TearDown();
		return true;
	}
	const uint64 OldRunId = Scene.Session->GetRunId();
	if (!TestTrue(TEXT("BeginWaves starts the progression"),
		Scene.Session->BeginWaves(Scene.Room, M3_029_MakeEnemyDef())))
	{
		Scene.TearDown();
		return true;
	}

	// Wave 0: birth the lone enemy and let it fight. No test-side healing, no
	// test-side kills: the enemy must whittle the 100 HP pool to zero.
	Scene.InjectClock(0.0);
	if (!Scene.SpawnCurrentWaveFully(*this, 0))
	{
		Scene.TearDown();
		return true;
	}
	TestEqual(TEXT("the wave-0 enemy was birthed"), Scene.Session->GetSpawnedEnemyCount(), 1);
	const float StartingHP = Scene.Player->GetHealth()->GetHealth();

	// Tick until the pawn dies from the enemy attacks (bounded wait). The
	// lone chaser's cycle deals 10 HP per ~1.28 s, so the death lands well
	// inside the cap.
	double Elapsed = 0.0;
	bool bFirstHitSeen = false;
	while (Elapsed < M3_029_MaxDeathWaitSeconds && Scene.Player->GetHealth()->IsAlive())
	{
		if (!Scene.AdvanceSeconds(*this, static_cast<double>(M3_029_FrameSeconds)))
		{
			Scene.TearDown();
			return true;
		}
		Elapsed += static_cast<double>(M3_029_FrameSeconds);
		if (!bFirstHitSeen && Scene.Player->GetHealth()->GetHealth() < StartingHP - 0.01f)
		{
			bFirstHitSeen = true;
		}
	}
	TestTrue(TEXT("the enemy attacks actually hurt the player before the death"), bFirstHitSeen);
	TestFalse(TEXT("the enemy attacks killed the player within the wait cap"),
		Scene.Player->GetHealth()->IsAlive());
	TestEqual(TEXT("the real PlayerDied broadcast fired exactly once"), Scene.PlayerDiedCount, 1);
	TestTrue(TEXT("the player death failed the running run"),
		Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the failure fired exactly one end event"), Scene.EndedCount, 1);
	if (Scene.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the end result records the failure"), !Scene.EndedResults[0].bCleared);
		TestEqual(TEXT("the end result keeps the failed run id"), Scene.EndedResults[0].RunId, OldRunId);
	}
	TestFalse(TEXT("the failed run is not reward eligible"), Scene.Session->IsRewardEligible(OldRunId));
	TestEqual(TEXT("no unborn spawns remain after the failure"), Scene.Session->GetRunPendingSpawnCount(), 0);

	// The retry: destroys the failed run's enemies, revives the player with a
	// full pool (a fresh death lifecycle) and starts a fresh run.
	if (!TestTrue(TEXT("RetryRoom restarts the failed run"),
		URoomRetryService::RetryRoom(Scene.World, Scene.Room, M3_029_MakeEnemyDef(), Scene.Player)))
	{
		Scene.TearDown();
		return true;
	}
	const uint64 NewRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the retry started a new, larger run id"), NewRunId > OldRunId);
	TestTrue(TEXT("the retried session is Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the retry destroyed the failed run's registered enemies"), Scene.TotalEnemies(), 0);
	TestTrue(TEXT("the player is alive again"), Scene.Player->GetHealth()->IsAlive());
	TestEqual(TEXT("the retry revived the player with the FULL pool"),
		Scene.Player->GetHealth()->GetHealth(), Scene.Player->GetHealth()->GetMaxHealth(), 0.01f);

	// The fresh run clears normally: the enemy born, then killed.
	Scene.InjectClock(Scene.ClockSeconds);
	if (!Scene.SpawnCurrentWaveFully(*this, 0))
	{
		Scene.TearDown();
		return true;
	}
	TestEqual(TEXT("the new run's wave 0 birthed exactly its enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	const int32 Kills = Scene.KillAllAliveEnemies(*this);
	TestEqual(TEXT("the new-run enemy died from the player's kill"), Kills, 1);
	TestTrue(TEXT("the retried run cleared"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("exactly two end events fired (failure, then clear)"), Scene.EndedCount, 2);
	if (Scene.EndedResults.Num() == 2)
	{
		TestTrue(TEXT("the retried run's result is Cleared"), Scene.EndedResults[1].bCleared);
		TestEqual(TEXT("the retried run's result carries the new run id"), Scene.EndedResults[1].RunId, NewRunId);
	}
	TestTrue(TEXT("the retried run is reward eligible"), Scene.Session->IsRewardEligible(NewRunId));

	Scene.TearDown();
	return true;
}

// 4. Acceptance: the RecoveryError boundary stays closed. Two corrupt slots
//    (existing-but-unusable data) report RecoveryError - NEVER NoSaveFound -
//    and the profile stays ABSENT: the M3-029 bootstrap is the NoSaveFound
//    semantic only, a corrupt save never silently starts over (M3-015).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_029RecoveryErrorKeepsNoProfile,
	"UEMMO.Tasks.M3_029.RecoveryErrorKeepsNoProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_029RecoveryErrorKeepsNoProfile::RunTest(const FString& Parameters)
{
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);

	FM3_029_MemoryStorage Storage;
	UProfileSaveService* Service = M3_029_NewService(*this, Storage);
	if (Service == nullptr)
	{
		return true;
	}

	// BOTH slot files go bad with distinctive evidence (the M3-015 model).
	const FGuid TagIdA(0x0BAD0029u, 0xA000u, 0xA0000000u, 0x000000A1u);
	const FGuid TagIdB(0x0BAD0029u, 0xB000u, 0xB0000000u, 0x000000B1u);
	TestTrue(TEXT("setup: the corrupt slot A was written"),
		M3_029_WriteCorruptSlot(Storage, M3_029_SlotAName(), TagIdA, 42, 7, 0xBADF00Du));
	TestTrue(TEXT("setup: the corrupt slot B was written"),
		M3_029_WriteCorruptSlot(Storage, M3_029_SlotBName(), TagIdB, 43, 8, 0xFEEDFACEu));

	UGameFlowSubsystem::SetStartupSaveServiceForTests(Service);

	// The recovery-error path of the flow's Initialize logs the structured
	// failure report at Error level (M3-015): an EXPECTED error here, so the
	// log capture does not fail the test - a SECOND occurrence would.
	AddExpectedError(TEXT("the startup recovery failed"),
		EAutomationExpectedErrorFlags::MatchType::Contains, /*Occurrences*/ 1);

	FM3_029_Scene Scene;
	if (!Scene.Build(*this, TEXT("Corrupt"), 0))
	{
		Scene.TearDown();
		return true;
	}

	// The startup outcome: an explicit recovery error, never a fresh state.
	const FStartupLoadOutcome& Outcome = Scene.Flow->GetStartupLoadOutcome();
	TestEqual(TEXT("two corrupt slots report RecoveryError"),
		static_cast<int32>(Outcome.Result), static_cast<int32>(EStartupLoadResult::RecoveryError));
	TestTrue(TEXT("RecoveryError is not a fresh state"),
		Outcome.Result != EStartupLoadResult::NoSaveFound);
	TestFalse(TEXT("the recovery error restored nothing"), Scene.Flow->WasStartupProfileRestored());
	TestTrue(TEXT("the recovery error names both corrupt slots"),
		Outcome.CorruptedSlotReports.Num() == 2);
	TestTrue(TEXT("the recovery error carries a summary"), !Outcome.Summary.IsEmpty());

	// THE BOUNDARY: the profile stays absent - the M3-029 bootstrap is the
	// NoSaveFound semantic only.
	TestFalse(TEXT("the recovery error keeps the no-profile state (M3-015 unchanged)"),
		Scene.Profile->HasProfile());
	TestFalse(TEXT("the no-profile state carries no CharacterId"),
		Scene.Profile->GetCharacterId().IsValid());
	TestEqual(TEXT("the no-profile state reports level 0"), Scene.Profile->GetLevel(), 0);

	Scene.TearDown();
	return true;
}

#endif
