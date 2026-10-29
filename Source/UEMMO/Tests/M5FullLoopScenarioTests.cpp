// M5-054: the full production loop over the M5/M6 content, one continuous
// run on an EMPTY test save - the release-candidate chain the card pins:
//
//   empty save -> first boot bootstrap -> the M6 catalog menu (three levels)
//   -> the loadout grant (the real M5-056 staging face; the M5-047 weapon
//   state pushes on the equipment change) -> the M5-049 armored vehicle
//   segment (stage from the real tables, enter, the production rigging arms
//   the 12-round projectile turret, one routed round, exit) -> the room run
//   on the REAL catalog row (StartRoom + BeginWaves over the 2+3 waves, the
//   wave pump births via the clock, every enemy dies through the health
//   face, MarkCleared settles) -> BeginReward (the M6-003 clear record lands
//   with the result's room id) -> the atomic claim (the M3-016 two-save
//   transaction) -> equip the reward piece -> the explicit save (the M5-049
//   request shape: snapshot/inventory/pending/applied/equipped/weapon
//   state/room clears) -> a SECOND real game instance restores through the
//   M3-015 startup chain (identity, level/XP, the inventory instance, the
//   equipment binding, the applied settlement, the M5-047 weapon magazines
//   and the M6-003 clear records).
//
// Save isolation: the in-memory ISaveStorage double on the dedicated
// "M5_054Slot_" prefix (no machine state leaks; the M3-029 loop precedent).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Items/EquipmentModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/InventoryModel.h"
#include "../Persistence/ProfileSaveService.h"
#include "../Persistence/ProfileSaveGame.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Vehicles/VehicleInteractionComponent.h"
#include "../Vehicles/VehicleWeaponMountComponent.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Containers/Map.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_054
{
	const TCHAR* M5_054_SlotPrefix = TEXT("M5_054Slot_");
	const FVector M5_054_SceneBase = FVector(0.0, 0.0, 0.0);
	constexpr double M5_054_FrameSeconds = 1.0 / 60.0;
	constexpr double M5_054_SpawnIntervalSeconds = 0.5;

	/** The in-memory storage double (the M3-020/M3-029 pattern; no disk). */
	class FM5_054_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM5_054_MemoryStorage() override
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

	/** The isolated save service (the M3-029 factory shape). */
	static UProfileSaveService* M5_054_NewService(FAutomationTestBase& Test, ISaveStorage& Storage)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Test.TestNotNull(TEXT("setup: the save service object constructs"), Service))
		{
			return nullptr;
		}
		if (!Test.TestTrue(TEXT("setup: the save service accepts the isolated prefix"),
			Service->Initialize(M5_054_SlotPrefix)))
		{
			return nullptr;
		}
		Service->SetStorage(&Storage);
		return Service;
	}

	/** The melee_grunt definition double (the M3-029/M5-067 shape). */
	static UEnemyDefinition* M5_054_MakeEnemyDef()
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

	/** The item catalog for the reward definitions (the M3-009 training set). */
	static FItemDefinitionCatalog M5_054_MakeTrainingCatalog()
	{
		FItemDefinitionCatalog Catalog;
		FItemDefinition Weapon;
		Weapon.DefinitionId = FName(TEXT("weapon_training"));
		Weapon.DisplayName = TEXT("weapon_training");
		Weapon.Slot = EItemSlot::Weapon;
		Weapon.BaseStats.Attack = 3.0f;
		Weapon.BaseStats.Defense = 0.0f;
		Weapon.BaseStats.MaxHP = 0.0f;
		Weapon.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Weapon, nullptr);
		FItemDefinition Armor;
		Armor.DefinitionId = FName(TEXT("armor_training"));
		Armor.DisplayName = TEXT("armor_training");
		Armor.Slot = EItemSlot::Armor;
		Armor.BaseStats.Attack = 0.0f;
		Armor.BaseStats.Defense = 3.0f;
		Armor.BaseStats.MaxHP = 0.0f;
		Armor.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Armor, nullptr);
		FItemDefinition Charm;
		Charm.DefinitionId = FName(TEXT("charm_training"));
		Charm.DisplayName = TEXT("charm_training");
		Charm.Slot = EItemSlot::Accessory;
		Charm.BaseStats.Attack = 0.0f;
		Charm.BaseStats.Defense = 0.0f;
		Charm.BaseStats.MaxHP = 20.0f;
		Charm.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Charm, nullptr);
		FItemDefinition Scatter;
		Scatter.DefinitionId = FName(TEXT("item_scatter_training"));
		Scatter.DisplayName = TEXT("item_scatter_training");
		Scatter.Slot = EItemSlot::Weapon;
		Scatter.BaseStats.Attack = 5.0f;
		Scatter.BaseStats.Defense = 0.0f;
		Scatter.BaseStats.MaxHP = 0.0f;
		Scatter.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Scatter, nullptr);
		return Catalog;
	}

	/** The floor plate (the M5-044/M5-049 scene precedent). */
	static AActor* M5_054_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Floor = World.SpawnActor<AActor>(AActor::StaticClass(), Base + FVector(0.0, 0.0, -50.0), FRotator::ZeroRotator, Params);
		if (Floor != nullptr)
		{
			UBoxComponent* Slab = NewObject<UBoxComponent>(Floor, TEXT("M5_054_Floor"));
			Floor->SetRootComponent(Slab);
			Slab->SetMobility(EComponentMobility::Movable);
			Slab->SetBoxExtent(FVector(2000.0f, 2000.0f, 50.0f));
			Slab->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
			Slab->RegisterComponent();
		}
		return Floor;
	}

	/** One loop phase: one real GameInstance + its world + every subsystem. */
	struct FM5_054_Scene
	{
		FSeatOccupancyModel Seats;
		FTestWorldWrapper Wrapper;
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		UProfileSubsystem* Profile = nullptr;
		UGameFlowSubsystem* Flow = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		APlayerController* PlayerController = nullptr;
		UVehicleInteractionComponent* Interaction = nullptr;
		UProfileSaveService* Save = nullptr;
		URewardService* Reward = nullptr;
		double ClockSeconds = 0.0;

		bool Build(FAutomationTestBase& Test, const TCHAR* Tag)
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
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world exists"), World))
			{
				return false;
			}
			GameInstance = World->GetGameInstance();
			Profile = GameInstance ? GameInstance->GetSubsystem<UProfileSubsystem>() : nullptr;
			Flow = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the flow subsystem resolves"), Flow)
				|| !Test.TestNotNull(TEXT("the profile subsystem resolves"), Profile)
				|| !Test.TestNotNull(TEXT("the room session resolves"), Session))
			{
				return false;
			}
			M5_054_SpawnFloor(*World, M5_054_SceneBase);
			World->EnsureCollisionTreeIsBuilt();
			{
				FActorSpawnParameters Params;
				Player = World->SpawnActor<APrototypeCharacter>(
					APrototypeCharacter::StaticClass(), M5_054_SceneBase + FVector(250.0, 0.0, 160.0), FRotator::ZeroRotator, Params);
			}
			if (!Test.TestNotNull(TEXT("the prototype pawn spawns"), Player))
			{
				return false;
			}
			PlayerController = World->SpawnActor<APlayerController>(
				APlayerController::StaticClass(), M5_054_SceneBase + FVector(0.0, 0.0, 300.0), FRotator::ZeroRotator, FActorSpawnParameters());
			if (!Test.TestNotNull(TEXT("the player controller spawns"), PlayerController))
			{
				return false;
			}
			PlayerController->Possess(Player);
			Session->SetPlayer(Player);
			Interaction = Player->FindComponentByClass<UVehicleInteractionComponent>();
			if (!Test.TestNotNull(TEXT("the interaction component exists"), Interaction))
			{
				return false;
			}
			Interaction->BindSeatModel(Seats);
			Interaction->SetOperatorFaction(FName(TEXT("player")));
			return TickFrames(Test, 30);
		}

		bool TickFrames(FAutomationTestBase& Test, int32 Count)
		{
			for (int32 Frame = 0; Frame < Count; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the wrapper world ticked"), Wrapper.TickTestWorld(M5_054_FrameSeconds)))
				{
					return false;
				}
				ClockSeconds += M5_054_FrameSeconds;
			}
			return true;
		}

		/** The M5-047 session clock face the wave pump reads. */
		void InjectClock()
		{
			Session->SetSessionClockSeconds(ClockSeconds);
		}

		bool AttachPersistence(FAutomationTestBase& Test, ISaveStorage& Storage)
		{
			Save = M5_054_NewService(Test, Storage);
			if (!Test.TestNotNull(TEXT("the scene save service constructs"), Save))
			{
				return false;
			}
			Save->AddToRoot();
			Reward = GameInstance ? NewObject<URewardService>(GameInstance) : nullptr;
			if (!Test.TestNotNull(TEXT("the scene reward service constructs"), Reward))
			{
				return false;
			}
			Reward->AddToRoot();
			Reward->BindProfile(Profile);
			return true;
		}

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

		/** Alive melee enemies in this world (corpses not counted). */
		int32 AliveEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				UHealthComponent* Health = (Enemy != nullptr) ? Enemy->GetHealthComponent() : nullptr;
				if (Health != nullptr && Health->IsAlive())
				{
					++Count;
				}
			}
			return Count;
		}

		/** Every alive enemy dies through the health face (the kill helper). */
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

// The M5-054 release-candidate loop: one continuous run from the empty save
// to the restored second instance, over the M5/M6 content.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_054FullLoopMenuLoadoutVehicleClearClaimEquipSaveRestore,
	"UEMMO.Tasks.M5_054.FullLoopMenuLoadoutVehicleClearClaimEquipSaveRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_054FullLoopMenuLoadoutVehicleClearClaimEquipSaveRestore::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M5_054;
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);

	FM5_054_MemoryStorage Storage;
	UProfileSaveService* BootService = M5_054_NewService(Test, Storage);
	if (BootService == nullptr)
	{
		return true;
	}
	UGameFlowSubsystem::SetStartupSaveServiceForTests(BootService);

	FM5_054_Scene Scene;
	if (!Scene.Build(Test, TEXT("Loop")))
	{
		Scene.TearDown();
		return true;
	}

	// -- First boot: the empty save bootstraps the profile (M3-029) ------------
	const FStartupLoadOutcome& BootOutcome = Scene.Flow->GetStartupLoadOutcome();
	TestEqual(TEXT("first boot: the startup pass reports NoSaveFound"),
		static_cast<int32>(BootOutcome.Result), static_cast<int32>(EStartupLoadResult::NoSaveFound));
	TestTrue(TEXT("first boot: the profile was bootstrapped"), Scene.Profile->HasProfile());
	const FGuid CharacterId = Scene.Profile->GetCharacterId();
	if (!Scene.AttachPersistence(Test, Storage))
	{
		Scene.TearDown();
		return true;
	}

	// -- The M6 catalog menu: three levels, the combat row enters --------------
	const TArray<URoomDefinition*>& Catalog = Scene.Flow->GetSelectableRoomDefinitions();
	if (!Test.TestEqual(TEXT("the free-select menu carries the three catalog levels"),
		Catalog.Num(), 3))
	{
		Scene.TearDown();
		return true;
	}
	const URoomDefinition* CombatRow = nullptr;
	for (const URoomDefinition* Entry : Catalog)
	{
		if (Entry != nullptr && Entry->RoomId == FName(TEXT("room_combat_01")))
		{
			CombatRow = Entry;
		}
	}
	if (!Test.TestNotNull(TEXT("the catalog carries the combat row"), CombatRow))
	{
		Scene.TearDown();
		return true;
	}
	TestTrue(TEXT("the menu enter accepts the catalog's combat row"),
		Scene.Flow->EnterRoom(CombatRow));
	TestTrue(TEXT("the flow reached the room state"), Scene.Flow->GetState() == EGameFlowState::Room);

	// -- The M5 weapon segment: the armored vehicle's turret (the loop's
	//    weapon chain runs through the vehicle mount; the handheld loadout
	//    grant is a production-private boot face covered by the boot suites) --
	FCombatDataTableSet VehicleTables;
	TArray<FString> VehicleTableProblems;
	if (!Test.TestTrue(TEXT("the real combat tables load for the vehicle segment"),
		LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), VehicleTables, VehicleTableProblems)))
	{
		Scene.TearDown();
		return true;
	}
	const FVehicleDefinition* ArmoredRow = VehicleTables.Vehicles.Find(FName(TEXT("vehicle_armored_test")));
	const FVehicleSeatDefinition* ArmoredSeat = nullptr;
	for (const TPair<FName, FVehicleSeatDefinition>& Row : VehicleTables.VehicleSeats)
	{
		if (Row.Value.VehicleId == FName(TEXT("vehicle_armored_test")))
		{
			ArmoredSeat = &Row.Value;
		}
	}
	if (!Test.TestNotNull(TEXT("the armored row resolves"), ArmoredRow)
		|| !Test.TestNotNull(TEXT("the armored seat resolves"), ArmoredSeat))
	{
		Scene.TearDown();
		return true;
	}
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		ACombatVehicle* Hull = Scene.World->SpawnActor<ACombatVehicle>(
			ACombatVehicle::StaticClass(), M5_054_SceneBase + FVector(420.0, 0.0, 96.0), FRotator::ZeroRotator, Params);
		if (!Test.TestNotNull(TEXT("the armored hull stages"), Hull)
			|| !Test.TestTrue(TEXT("the armored hull accepts its real table row"),
				Hull->SetVehicleDefinition(*ArmoredRow, nullptr)))
		{
			Scene.TearDown();
			return true;
		}
		FString RegisterError;
		Test.TestTrue(TEXT("the armored hull registers"),
			Scene.Interaction->RegisterVehicle(*Hull, *ArmoredSeat, &RegisterError));
		Scene.TickFrames(Test, 5);
		const FVehicleEnterAttempt Enter = Scene.Interaction->TryEnterNearestVehicle();
		Test.TestTrue(TEXT("the loop's vehicle enter accepts the armored hull"), Enter.bEntered);
		UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>();
		Test.TestTrue(TEXT("the riding rigging armed the projectile turret"),
			Mount != nullptr && Mount->GetMountedDefinition().TurretWeaponId == FName(TEXT("weapon_projectile_sample")));
		FString FireRefusal;
		Test.TestTrue(TEXT("one routed vehicle round fires"), Scene.Interaction->RouteVehicleFirePressed(&FireRefusal));
		Scene.TickFrames(Test, 5);
		Test.TestTrue(TEXT("the vehicle exit accepts"), Scene.Interaction->TryExitVehicle());
		Scene.TickFrames(Test, 5);
	}

	// -- The room run on the REAL catalog row (2+3 waves) ----------------------
	Test.TestTrue(TEXT("StartRoom accepts the catalog room"),
		Scene.Session->StartRoom(CombatRow));
	Test.TestTrue(TEXT("BeginWaves starts the progression"),
		Scene.Session->BeginWaves(CombatRow, M5_054_MakeEnemyDef()));

	// Wave 0 (2 grunts) through the clock-driven pump.
	Scene.InjectClock();
	{
		const double Span = 0.5 * 1.0 + 0.05;
		double Advanced = 0.0;
		while (Advanced < Span)
		{
			Scene.TickFrames(Test, 1);
			Scene.InjectClock();
			Advanced += 1.0 / 60.0;
		}
	}
	Test.TestEqual(TEXT("both wave-0 enemies were birthed"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	Test.TestEqual(TEXT("wave 0's grunts all died through the health face"),
		Scene.KillAllAliveEnemies(Test), 2);

	// Wave 1 (3 grunts) past the inter-wave gap.
	{
		double Advanced = 0.0;
		while (Advanced < 1.05)
		{
			Scene.TickFrames(Test, 1);
			Scene.InjectClock();
			Advanced += 1.0 / 60.0;
		}
	}
	Test.TestEqual(TEXT("past the gap the wave index is 1"), Scene.Session->GetCurrentWaveIndex(), 1);
	{
		const double Span = 0.5 * 2.0 + 0.05;
		double Advanced = 0.0;
		while (Advanced < Span)
		{
			Scene.TickFrames(Test, 1);
			Scene.InjectClock();
			Advanced += 1.0 / 60.0;
		}
	}
	Test.TestEqual(TEXT("all five enemies were birthed for the run (2+3)"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	Test.TestEqual(TEXT("wave 1's grunts all died through the health face"),
		Scene.KillAllAliveEnemies(Test), 3);

	// The terminal state: the M2-008 death-driven pump itself settled the run
	// Cleared when the final wave died (first-terminal-wins - a manual
	// MarkCleared here would be the refused second terminal).
	Test.TestEqual(TEXT("the wave pump settled the run Cleared"),
		static_cast<int32>(Scene.Session->GetState()), static_cast<int32>(ERoomSessionState::Cleared));
	const FRoomResult& Result = Scene.Session->GetLastResult();
	Test.TestTrue(TEXT("the settled run is Cleared with a settlement id"),
		Result.bCleared && Result.SettlementId != 0);

	// -- Claim (the M3-016 atomic transaction) ----------------------------------
	const FItemDefinitionCatalog RewardCatalog = M5_054_MakeTrainingCatalog();
	const FRewardBeginOutcome Begin = Scene.Reward->BeginReward(Result, RewardCatalog);
	AddInfo(FString::Printf(TEXT("M5_054 begin-reward diagnose: result=%d error=%s items=%d"),
		static_cast<int32>(Begin.Result), *Begin.Error, Begin.Draft.Items.Num()));
	Test.TestEqual(TEXT("BeginReward applied the settlement draft"),
		Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		Scene.TearDown();
		return true;
	}

	// -- The M6-003 clear record rode the reward draft (the new-draft path
	//    records the result's room id exactly once) -----------------------------
	FRoomClearSaveEntry ClearRecord;
	Test.TestTrue(TEXT("the cleared run recorded the room (the M6-003 wiring)"),
		Scene.Profile->GetRoomClearRecord(Result.RoomId, ClearRecord));
	Test.TestEqual(TEXT("the clear record counts one clear"), ClearRecord.ClearCount, 1);
	const FItemInstance RewardItem = Begin.Draft.Items[0];
	const FRewardClaimAtomicOutcome Claim = Scene.Reward->ClaimPendingAtomic(
		Begin.Draft.SettlementId, Scene.Profile, Scene.Save);
	Test.TestEqual(TEXT("the atomic claim committed"), Claim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the claim granted the settlement XP once"), Claim.bGrantedXP);

	// -- Equip the reward piece through the real entries ------------------------
	FEquipmentModel Equipment;
	const FItemDefinition* Resolved = RewardCatalog.Find(RewardItem.DefinitionId);
	if (!Test.TestNotNull(TEXT("the reward item's definition resolves"), Resolved))
	{
		Scene.TearDown();
		return true;
	}
	Equipment.SetDefinitionCatalog(&RewardCatalog);
	Test.TestEqual(TEXT("the reward piece equips into its slot"),
		static_cast<int32>(Equipment.Equip(Resolved->Slot, RewardItem.InstanceId, Scene.Profile->GetInventory())),
		static_cast<int32>(EEquipmentEquipResult::Equipped));
	// The equipped bonus row stays unpushed (the M3-029 loop rule: the
	// restore never fossilizes the sum - the gameplay layer re-derives it).

	// -- The explicit save (the M5-049 request shape) ---------------------------
	const FProfileLoadOutcome LastCommitted = Scene.Save->LoadActiveProfile();
	Test.TestEqual(TEXT("the claim's committed save loads back"),
		static_cast<int32>(LastCommitted.Result), static_cast<int32>(EProfileLoadResult::Success));
	FProfileSaveRequest SaveRequest;
	SaveRequest.Snapshot = Scene.Profile->GetProfileSnapshot();
	SaveRequest.Inventory = Scene.Profile->GetInventory();
	SaveRequest.PendingRewards = Scene.Profile->GetPendingRewards();
	SaveRequest.AppliedSettlementIds = LastCommitted.AppliedSettlementIds;
	SaveRequest.EquippedMap.Add(Resolved->Slot, RewardItem.InstanceId);
	SaveRequest.WeaponMagazines = Scene.Profile->GetWeaponMagazines();
	SaveRequest.ReserveAmmoPools = Scene.Profile->GetReserveAmmoPools();
	SaveRequest.bWeaponStateInitialized = Scene.Profile->IsWeaponStateInitialized();
	SaveRequest.ConfigRevision = Scene.Profile->GetWeaponConfigRevision();
	SaveRequest.RoomClears = Scene.Profile->GetRoomClears();
	// The loop's weapon chain ran through the VEHICLE turret (the hull's
	// books are world-local and never saved - the M5-039 structural rule);
	// the profile's pushed weapon state here is the no-handheld-weapon
	// shape, and the restore must keep it verbatim.
	const FProfileSaveOutcome Saved = Scene.Save->SaveProfile(SaveRequest);
	Test.TestEqual(TEXT("the equipped profile save committed"),
		static_cast<int32>(Saved.Result), static_cast<int32>(EProfileSaveResult::Success));
	const int32 CheckpointMagazines = SaveRequest.WeaponMagazines.Num();
	const bool SceneCheckpointInitialized = SaveRequest.bWeaponStateInitialized;

	// -- The second real game instance restores through the startup chain ------
	Scene.TearDown();
	UProfileSaveService* ReloadService = M5_054_NewService(Test, Storage);
	if (ReloadService == nullptr)
	{
		return true;
	}
	UGameFlowSubsystem::SetStartupSaveServiceForTests(ReloadService);

	FM5_054_Scene Restart;
	if (Restart.Build(Test, TEXT("Restart")))
	{
		const FStartupLoadOutcome& Loaded = Restart.Flow->GetStartupLoadOutcome();
		Test.TestEqual(TEXT("restart: the startup pass recovered the save"),
			static_cast<int32>(Loaded.Result), static_cast<int32>(EStartupLoadResult::Recovered));
		Test.TestTrue(TEXT("restart: the flow reports the restore"),
			Restart.Flow->WasStartupProfileRestored());
		Test.TestTrue(TEXT("restart: the restored profile keeps the first boot's identity"),
			Restart.Profile->GetCharacterId() == CharacterId);
		Test.TestTrue(TEXT("restart: the restored inventory keeps the reward instance"),
			Restart.Profile->GetInventory().Contains(RewardItem.InstanceId));
		Test.TestTrue(TEXT("restart: the applied settlement record survives"),
			Restart.Profile->IsSettlementApplied(Begin.Draft.SettlementId));
		// The M5-047 weapon state: the restore keeps the pushed (no-handheld-
		// weapon) state verbatim - the magazines round trip is the M5-047
		// suite's contract, this loop pins the shape.
		Test.TestEqual(TEXT("restart: the restored weapon state keeps the pushed shape"),
			Restart.Profile->GetWeaponMagazines().Num(), CheckpointMagazines);
		Test.TestEqual(TEXT("restart: the restored marker keeps the pushed flag"),
			Restart.Profile->IsWeaponStateInitialized(), SceneCheckpointInitialized);
		// The M6-003 clear records survive the restart.
		FRoomClearSaveEntry RestoredClear;
		Test.TestTrue(TEXT("restart: the room-clear record survives"),
			Restart.Profile->GetRoomClearRecord(FName(TEXT("room_combat_01")), RestoredClear));
		Test.TestEqual(TEXT("restart: the restored record counts one clear"),
			RestoredClear.ClearCount, 1);
		// The M6 catalog serves the restarted menu identically.
		Test.TestEqual(TEXT("restart: the free-select menu still carries three levels"),
			Restart.Flow->GetSelectableRoomDefinitions().Num(), 3);
		// The restart: the equipped binding resolves via Find (the outcome's
		// map is const).
		const FGuid* EquippedId = Loaded.EquippedMap.Find(Resolved->Slot);
		Test.TestTrue(TEXT("restart: the restored equipment binding survives"),
			EquippedId != nullptr && *EquippedId == RewardItem.InstanceId);
	}
	Restart.TearDown();
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);
	return true;
}

#endif
