// M5-034: the weapon status panel and the production inventory toggle. The
// card wires the REAL WeaponComponent state (binding, magazine, reserve,
// reload window, catalog error) into a display-only panel embedded in the
// inventory screen, adds the I key as the production open/close entry (the
// console is no longer the main entry) and keeps the fire path untouched:
// opening/closing/switching screens never fires, and the UI never grants
// ammo.
//
// Suites run in a real ticked world (the engine FTestWorldWrapper
// precedent). The weapon bind goes through the real UWeaponComponent public
// API (MountCatalogs + ApplyEquippedWeapon + ApplyWeaponBindWiring) with a
// production-shaped catalog fixture (the M5-033 rig pattern) because the
// shipped items source carries no weapon mappings yet. Damage flows only
// through the real weapon chain - no direct health writes anywhere.
//
// Stub-failure note: the red stub leaves the pure view model invalid/empty,
// the refresh gate never rebuilding, the inventory without the embedded
// panel and the HUD/pawn toggle entries as no-ops, so every test below
// fails on at least one state, text or count assertion.

#include "Misc/AutomationTest.h"

#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../UI/InventoryWidget.h"
#include "../UI/MapSelectWidget.h"
#include "../UI/WeaponStatusWidget.h"
#include "../Weapons/AmmoModel.h"
#include "../Weapons/WeaponBinding.h"
#include "../Weapons/WeaponComponent.h"
#include "../Weapons/WeaponTypes.h"

#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_034
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M5_034_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention).
	const FVector M5_034_SceneBase(152000.0, 46000.0, 600.0);
	const float M5_034_FloorHalfThickness = 100.0f;
	const float M5_034_FloorHalfExtentXY = 4000.0f;
	const float M5_034_PlayerSpawnHeight = 120.0f;

	// Weapon fixture values: 600 rpm = 0.1 s cooldown, 12-round magazine,
	// 40 pre-seeded reserve rounds (the M5-033 pre-seed pattern: the bind's
	// own zero-reserve registration refuses as a duplicate, the pool keeps
	// the seeded stock and the UI must never touch it).
	const FName M5_034_AmmoId(TEXT("ammo_ui"));
	const FName M5_034_WeaponId(TEXT("wpn_ui_rifle"));
	const FName M5_034_ItemId(TEXT("item_ui_rifle"));
	constexpr int32 M5_034_MagazineSize = 12;
	constexpr int32 M5_034_SeededReserve = 40;
	constexpr float M5_034_FireRateRpm = 600.0f;

	// The reload completes 2.0 s after the T press (the input-layer constant).
	constexpr double M5_034_ReloadDurationSeconds = 2.0;

	// Builds one pure view-model input bundle for the table-driven shapes.
	static FWeaponBindingRecord M5_034_MakeRecord(EWeaponFireMode Mode, int32 Capacity, int32 Loaded)
	{
		FWeaponBindingRecord Record;
		Record.InstanceId = FGuid(0xA11Cu, 1u, 2u, 3u);
		Record.ItemDefinitionId = M5_034_ItemId;
		Record.WeaponDefinitionId = M5_034_WeaponId;
		Record.FireMode = Mode;
		Record.MagazineCapacity = Capacity;
		Record.LoadedRounds = Loaded;
		return Record;
	}

	static FWeaponDefinition M5_034_MakeDefinition(EWeaponFireMode Mode)
	{
		FWeaponDefinition Definition;
		Definition.WeaponId = M5_034_WeaponId;
		Definition.FireMode = Mode;
		Definition.DamageProfileId = FName(TEXT("dmg_ui"));
		Definition.AmmoId = M5_034_AmmoId;
		Definition.MagazineSize = (Mode == EWeaponFireMode::Melee) ? 0 : M5_034_MagazineSize;
		Definition.FireRateRpm = (Mode == EWeaponFireMode::Melee) ? 0.0f : M5_034_FireRateRpm;
		Definition.BurstCount = 1;
		Definition.PelletCount = 1;
		Definition.RangeCm = (Mode == EWeaponFireMode::Hitscan) ? 5000.0f : 0.0f;
		return Definition;
	}

	// One full rig: the wrapper owns the manually ticked temp world, the rig
	// owns the catalog VALUES mounted into the real pawn's weapon component,
	// and the HUD actor presents the inventory screen (headless-safe).
	struct FM5_034_Rig
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		APrototypeHUD* Hud = nullptr;
		APlayerController* Controller = nullptr;
		FCombatCatalog CatalogValue;
		FItemDefinitionCatalog ItemsValue;
		FGuid BoundInstanceId;
		double ClockSeconds = 100.0;
		bool bInjectedClock = false;

		bool Build(FAutomationTestBase& Test, bool bWithController, bool bInInjectClock)
		{
			bInjectedClock = bInInjectClock;
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
			// World-static floor the pawn stands on (the M1-041 precedent).
			FActorSpawnParameters FloorParams;
			AActor* Floor = World->SpawnActor<AActor>(
				AActor::StaticClass(),
				M5_034_SceneBase - FVector(0.0, 0.0, M5_034_FloorHalfThickness), FRotator::ZeroRotator, FloorParams);
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			FActorSpawnParameters PlayerParams;
			Player = World->SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(),
				M5_034_SceneBase + FVector(0.0, 0.0, M5_034_PlayerSpawnHeight), FRotator::ZeroRotator, PlayerParams);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
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
			if (bInInjectClock)
			{
				// The shared rig clock feeds the pawn's input layer AND the
				// mount's fire clock (deterministic pacing; tests that need
				// the HUD reload countdown skip the injection so both clocks
				// stay the owner world's clock).
				Player->SetWeaponInputClockProvider([this]() { return ClockSeconds; });
				Player->GetWeaponMount()->SetFireClockProvider([this]() { return ClockSeconds; });
			}
			Hud = World->SpawnActor<APrototypeHUD>(APrototypeHUD::StaticClass(),
				FVector::ZeroVector, FRotator::ZeroRotator, FActorSpawnParameters());
			if (!Test.TestNotNull(TEXT("the prototype HUD spawns"), Hud))
			{
				return false;
			}
			if (bWithController)
			{
				// The menu-open gate resolves the HUD through the pawn's
				// controller, so the fire-gate test needs the real possess
				// chain (the M2-004 retry-flow precedent).
				Controller = World->SpawnActor<APlayerController>(
					APlayerController::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, FActorSpawnParameters());
				if (!Test.TestNotNull(TEXT("the player controller spawns"), Controller))
				{
					return false;
				}
				Controller->Possess(Player);
				Controller->MyHUD = Hud;
			}
			return true;
		}

		/** Ticks the world in fixed frames (advancing the injected clock). */
		bool Tick(FAutomationTestBase& Test, float Seconds)
		{
			const int32 Frames = FMath::Max(1, FMath::RoundToInt(Seconds / M5_034_FrameSeconds));
			for (int32 Frame = 0; Frame < Frames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M5_034_FrameSeconds)))
				{
					return false;
				}
				if (bInjectedClock)
				{
					ClockSeconds += M5_034_FrameSeconds;
				}
			}
			return true;
		}

		/** Ticks until the pawn settled onto the floor (the M3-031 pattern). */
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
			for (int32 Frame = 0; Frame < 300; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks (settle)"), Wrapper.TickTestWorld(M5_034_FrameSeconds)))
				{
					return false;
				}
				if (bInjectedClock)
				{
					ClockSeconds += M5_034_FrameSeconds;
				}
				if (Movement != nullptr && Movement->MovementMode == MOVE_Walking && Movement->Velocity.Size() < 1.0f)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the pawn never settled onto the floor"));
			return false;
		}

		/**
		 * Builds a production-shaped catalog (one damage profile, the weapon
		 * and its ammo), mounts it into the real pawn's weapon component,
		 * pre-seeds the shared reserve and binds a real item instance through
		 * the production bind chain (the M5-033 rig pattern verbatim).
		 */
		bool BindHitscanRifle(FAutomationTestBase& Test)
		{
			FParsedCombatConfig Parsed;
			Parsed.SchemaVersion = 1;
			FDamageProfile Profile;
			Profile.DamageProfileId = FName(TEXT("dmg_ui"));
			Profile.BaseDamage = 10.0f;
			Parsed.DamageProfiles.Add(Profile);
			FAmmoType Ammo;
			Ammo.AmmoId = M5_034_AmmoId;
			Ammo.MaxReserve = 120;
			Ammo.MagazineSize = M5_034_MagazineSize;
			Parsed.AmmoTypes.Add(Ammo);
			FWeaponDefinition Weapon = M5_034_MakeDefinition(EWeaponFireMode::Hitscan);
			Parsed.Weapons.Add(Weapon);
			FString BuildErrors;
			if (!Test.TestTrue(TEXT("the rig catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, CatalogValue, BuildErrors)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog build refused: %s"), *BuildErrors));
				return false;
			}
			FItemDefinition Item;
			Item.DefinitionId = M5_034_ItemId;
			Item.DisplayName = TEXT("UI Rig Rifle");
			Item.Slot = EItemSlot::Weapon;
			Item.Rarity = EItemRarity::Normal;
			Item.WeaponDefinitionId = M5_034_WeaponId;
			FString ItemError;
			if (!Test.TestTrue(TEXT("the rig item definition registers"), ItemsValue.AddDefinition(Item, &ItemError)))
			{
				Test.AddError(FString::Printf(TEXT("the rig item definition refused: %s"), *ItemError));
				return false;
			}
			UWeaponComponent* Mount = Player->GetWeaponMount();
			FString MountError;
			if (!Test.TestTrue(TEXT("the rig catalogs mount"),
				Mount->MountCatalogs(&CatalogValue, &ItemsValue, &MountError)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog mount refused: %s"), *MountError));
				return false;
			}
			// The pre-seed: the bind's own zero-reserve registration refuses as
			// a duplicate and the pool keeps the seeded stock (no UI grant).
			FString SeedError;
			if (!Test.TestTrue(TEXT("the shared reserve pre-seeds"),
				Mount->GetAmmoModel().RegisterAmmoType(M5_034_AmmoId, 120, M5_034_SeededReserve, &SeedError)))
			{
				Test.AddError(FString::Printf(TEXT("the reserve pre-seed refused: %s"), *SeedError));
				return false;
			}
			FItemInstance Instance;
			Instance.InstanceId = FGuid(0xB33Fu, 1u, 4u, 9u);
			Instance.DefinitionId = M5_034_ItemId;
			Instance.Level = 1;
			const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
			if (!Test.TestTrue(TEXT("the real bind accepts the rig weapon"), Outcome.bSucceeded))
			{
				Test.AddError(FString::Printf(TEXT("the real bind refused: %s"), *Outcome.RejectDetail));
				return false;
			}
			Player->ApplyWeaponBindWiring();
			if (!Test.TestNotNull(TEXT("the mount holds the active binding"), Mount->GetActiveBinding()))
			{
				return false;
			}
			BoundInstanceId = Instance.InstanceId;
			return true;
		}

		/**
		 * Mounts the production-shaped catalog pair WITHOUT binding a weapon
		 * (the real pawn's state once its session catalogs are up: healthy
		 * mount, no active binding). bRegisterUnboundTrainingItem additionally
		 * registers the staging weapon item WITHOUT a weapon mapping (the
		 * shipped production shape) so an equip of it refuses the way the
		 * production bind chain refuses.
		 */
		bool MountCatalogsOnly(FAutomationTestBase& Test, bool bRegisterUnboundTrainingItem)
		{
			FParsedCombatConfig Parsed;
			Parsed.SchemaVersion = 1;
			FDamageProfile Profile;
			Profile.DamageProfileId = FName(TEXT("dmg_ui"));
			Profile.BaseDamage = 10.0f;
			Parsed.DamageProfiles.Add(Profile);
			FAmmoType Ammo;
			Ammo.AmmoId = M5_034_AmmoId;
			Ammo.MaxReserve = 120;
			Ammo.MagazineSize = M5_034_MagazineSize;
			Parsed.AmmoTypes.Add(Ammo);
			Parsed.Weapons.Add(M5_034_MakeDefinition(EWeaponFireMode::Hitscan));
			FString BuildErrors;
			if (!Test.TestTrue(TEXT("the rig catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, CatalogValue, BuildErrors)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog build refused: %s"), *BuildErrors));
				return false;
			}
			if (bRegisterUnboundTrainingItem)
			{
				// The production shape: a weapon-slot item definition whose
				// WeaponDefinitionId stays None (the M5-019 lineage gap).
				FItemDefinition Unbound;
				Unbound.DefinitionId = FName(TEXT("weapon_training"));
				Unbound.DisplayName = TEXT("Training Sword");
				Unbound.Slot = EItemSlot::Weapon;
				Unbound.Rarity = EItemRarity::Normal;
				FString ItemError;
				if (!Test.TestTrue(TEXT("the unbound training item registers"),
					ItemsValue.AddDefinition(Unbound, &ItemError)))
				{
					Test.AddError(FString::Printf(TEXT("the unbound training item refused: %s"), *ItemError));
					return false;
				}
			}
			FString MountError;
			if (!Test.TestTrue(TEXT("the rig catalogs mount"),
				Player->GetWeaponMount()->MountCatalogs(&CatalogValue, &ItemsValue, &MountError)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog mount refused: %s"), *MountError));
				return false;
			}
			return true;
		}
	};

	// ----- 1. pure view model shapes ------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_ViewModelPureShapes, // placeholder replaced below
		"UEMMO.Tasks.M5_034.ViewModelPureShapes",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_ViewModelPureShapes::RunTest(const FString&)
	{
		// Shape A: nothing bound and nothing expected -> the explicit no-weapon state.
		{
			FWeaponStatusInputs Inputs;
			const FWeaponStatusViewModel ViewModel = MakeWeaponStatusViewModel(Inputs);
			TestTrue(TEXT("the no-weapon view model is valid"), ViewModel.bValid);
			TestEqual(TEXT("the no-weapon shape reports NoWeapon"),
				static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::NoWeapon));
			TestTrue(TEXT("the no-weapon status line names the state"),
				ViewModel.StatusLine.Contains(TEXT("No weapon equipped")));
		}
		// Shape B: a bound ranged weapon with a full magazine -> Ready with real rounds.
		{
			FWeaponStatusInputs Inputs;
			FWeaponBindingRecord Record = M5_034_MakeRecord(EWeaponFireMode::Hitscan, M5_034_MagazineSize, M5_034_MagazineSize);
			FWeaponDefinition Definition = M5_034_MakeDefinition(EWeaponFireMode::Hitscan);
			Inputs.Binding = &Record;
			Inputs.Definition = &Definition;
			Inputs.ReserveRounds = M5_034_SeededReserve;
			const FWeaponStatusViewModel ViewModel = MakeWeaponStatusViewModel(Inputs);
			TestTrue(TEXT("the ready view model is valid"), ViewModel.bValid);
			TestEqual(TEXT("the full-magazine shape reports Ready"),
				static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::Ready));
			TestTrue(TEXT("the weapon line carries the definition id"),
				ViewModel.WeaponLine.Contains(M5_034_WeaponId.ToString()));
			TestTrue(TEXT("the weapon line carries the hitscan type"), ViewModel.WeaponLine.Contains(TEXT("Hitscan")));
			TestTrue(TEXT("the ammo line carries the loaded rounds"),
				ViewModel.AmmoLine.Contains(TEXT("Loaded 12/12")));
			TestTrue(TEXT("the ammo line carries the reserve rounds"),
				ViewModel.AmmoLine.Contains(FString::Printf(TEXT("Reserve %d"), M5_034_SeededReserve)));
			TestTrue(TEXT("the abilities line carries the fire rate"), ViewModel.AbilitiesLine.Contains(TEXT("RPM")));
		}
		// Shape C: empty magazine with reserve left -> the reload hint state.
		{
			FWeaponStatusInputs Inputs;
			FWeaponBindingRecord Record = M5_034_MakeRecord(EWeaponFireMode::Hitscan, M5_034_MagazineSize, 0);
			FWeaponDefinition Definition = M5_034_MakeDefinition(EWeaponFireMode::Hitscan);
			Inputs.Binding = &Record;
			Inputs.Definition = &Definition;
			Inputs.ReserveRounds = 5;
			const FWeaponStatusViewModel ViewModel = MakeWeaponStatusViewModel(Inputs);
			TestEqual(TEXT("the empty-magazine shape reports EmptyMagazine"),
				static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::EmptyMagazine));
			TestTrue(TEXT("the empty-magazine status names the reload key"),
				ViewModel.StatusLine.Contains(TEXT("reload")));
		}
		// Shape D: empty magazine AND empty reserve -> the explicit reserve-empty state.
		{
			FWeaponStatusInputs Inputs;
			FWeaponBindingRecord Record = M5_034_MakeRecord(EWeaponFireMode::Hitscan, M5_034_MagazineSize, 0);
			FWeaponDefinition Definition = M5_034_MakeDefinition(EWeaponFireMode::Hitscan);
			Inputs.Binding = &Record;
			Inputs.Definition = &Definition;
			Inputs.ReserveRounds = 0;
			const FWeaponStatusViewModel ViewModel = MakeWeaponStatusViewModel(Inputs);
			TestEqual(TEXT("the reserve-empty shape reports ReserveEmpty"),
				static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::ReserveEmpty));
			TestTrue(TEXT("the reserve-empty status names the reserve"),
				ViewModel.StatusLine.Contains(TEXT("Reserve")));
		}
		// Shape E: an open reload window -> the reloading state with the countdown.
		{
			FWeaponStatusInputs Inputs;
			FWeaponBindingRecord Record = M5_034_MakeRecord(EWeaponFireMode::Hitscan, M5_034_MagazineSize, 0);
			FWeaponDefinition Definition = M5_034_MakeDefinition(EWeaponFireMode::Hitscan);
			Inputs.Binding = &Record;
			Inputs.Definition = &Definition;
			Inputs.ReserveRounds = 5;
			Inputs.RemainingReloadSeconds = 1.4;
			const FWeaponStatusViewModel ViewModel = MakeWeaponStatusViewModel(Inputs);
			TestEqual(TEXT("the reloading shape reports Reloading"),
				static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::Reloading));
			TestTrue(TEXT("the reloading status carries the countdown"),
				ViewModel.StatusLine.Contains(TEXT("Reloading")) && ViewModel.StatusLine.Contains(TEXT("1.4")));
		}
		// Shape F: a bound melee weapon -> Ready, no ammo line.
		{
			FWeaponStatusInputs Inputs;
			FWeaponBindingRecord Record = M5_034_MakeRecord(EWeaponFireMode::Melee, 0, 0);
			FWeaponDefinition Definition = M5_034_MakeDefinition(EWeaponFireMode::Melee);
			Inputs.Binding = &Record;
			Inputs.Definition = &Definition;
			const FWeaponStatusViewModel ViewModel = MakeWeaponStatusViewModel(Inputs);
			TestEqual(TEXT("the melee shape reports Ready"),
				static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::Ready));
			TestTrue(TEXT("the melee weapon line names the melee type"),
				ViewModel.WeaponLine.Contains(TEXT("Melee")));
			TestTrue(TEXT("the melee ammo line stays the dash placeholder"),
				ViewModel.AmmoLine == TEXT("-"));
		}
		// Shape G/H/I: the three explicit config-failure shapes.
		{
			FWeaponStatusInputs Inputs;
			Inputs.bCatalogError = true;
			const FWeaponStatusViewModel CatalogFailure = MakeWeaponStatusViewModel(Inputs);
			TestEqual(TEXT("the catalog-error shape reports ConfigFailure"),
				static_cast<int32>(CatalogFailure.State), static_cast<int32>(EWeaponStatusState::ConfigFailure));

			FWeaponStatusInputs UnboundItem;
			UnboundItem.bEquippedItemUnbound = true;
			const FWeaponStatusViewModel MappingFailure = MakeWeaponStatusViewModel(UnboundItem);
			TestEqual(TEXT("the unbound-weapon-item shape reports ConfigFailure"),
				static_cast<int32>(MappingFailure.State), static_cast<int32>(EWeaponStatusState::ConfigFailure));
			TestTrue(TEXT("the mapping failure names the missing mapping"),
				MappingFailure.StatusLine.Contains(TEXT("mapping")));

			FWeaponStatusInputs Unresolved;
			FWeaponBindingRecord Record = M5_034_MakeRecord(EWeaponFireMode::Hitscan, M5_034_MagazineSize, 6);
			Unresolved.Binding = &Record;
			const FWeaponStatusViewModel DefinitionFailure = MakeWeaponStatusViewModel(Unresolved);
			TestEqual(TEXT("the unresolved-definition shape reports ConfigFailure"),
				static_cast<int32>(DefinitionFailure.State), static_cast<int32>(EWeaponStatusState::ConfigFailure));
		}
		return true;
	}

	// ----- 2. the refresh gate --------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_SnapshotGateSkipsIdenticalRefresh,
		"UEMMO.Tasks.M5_034.SnapshotGateSkipsIdenticalRefresh",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_SnapshotGateSkipsIdenticalRefresh::RunTest(const FString&)
	{
		FWeaponStatusInputs Inputs;
		FWeaponBindingRecord Record = M5_034_MakeRecord(EWeaponFireMode::Hitscan, M5_034_MagazineSize, M5_034_MagazineSize);
		FWeaponDefinition Definition = M5_034_MakeDefinition(EWeaponFireMode::Hitscan);
		Inputs.Binding = &Record;
		Inputs.Definition = &Definition;
		Inputs.ReserveRounds = M5_034_SeededReserve;

		// Identical snapshots share one fingerprint; a render-relevant change
		// (loaded rounds, reserve, reload remainder) moves it.
		const FString First = MakeWeaponStatusSnapshotFingerprint(Inputs);
		const FString Second = MakeWeaponStatusSnapshotFingerprint(Inputs);
		TestEqual(TEXT("identical inputs produce one fingerprint"), First, Second);

		Record.LoadedRounds = 5;
		const FString Changed = MakeWeaponStatusSnapshotFingerprint(Inputs);
		TestTrue(TEXT("a render-relevant change moves the fingerprint"), Changed != First);

		FWeaponStatusRefreshGuard Guard;
		TestTrue(TEXT("the first accept rebuilds (no baseline yet)"), Guard.AcceptSnapshot(First));
		TestFalse(TEXT("an identical accept is skipped"), Guard.AcceptSnapshot(Second));
		TestTrue(TEXT("a changed accept rebuilds"), Guard.AcceptSnapshot(Changed));
		TestEqual(TEXT("two rebuilds performed"), Guard.GetPerformedCount(), 2);
		TestEqual(TEXT("one identical refresh skipped"), Guard.GetSkippedCount(), 1);
		return true;
	}

	// ----- 3. the inventory screen embeds the status panel ----------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_InventoryBuildsWeaponStatusSection,
		"UEMMO.Tasks.M5_034.InventoryBuildsWeaponStatusSection",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_InventoryBuildsWeaponStatusSection::RunTest(const FString&)
	{
		FM5_034_Rig Rig;
		if (!Rig.Build(*this, /*bWithController*/ false, /*bInInjectClock*/ false)
			|| !Rig.MountCatalogsOnly(*this, /*bRegisterUnboundTrainingItem*/ false))
		{
			return false;
		}
		// The production presentation path (the M3-012 exec-staging precedent
		// for the scene data; the OPEN path under test is the toggle entry).
		Rig.Hud->UEMMODebugInventory(1);
		if (!TestTrue(TEXT("the inventory screen is presented"), Rig.Hud->HasInventoryScreen()))
		{
			return false;
		}
		const UInventoryWidget* Widget = Rig.Hud->PeekInventoryWidget();
		if (!TestNotNull(TEXT("the inventory widget is reachable"), Widget))
		{
			return false;
		}
		const UWeaponStatusWidget* Status = Widget->PeekWeaponStatus();
		if (!TestNotNull(TEXT("the inventory screen embeds the weapon status panel"), Status))
		{
			return false;
		}
		TestNotNull(TEXT("the weapon block exists"), Status->PeekWeaponBlock());
		TestNotNull(TEXT("the ammo block exists"), Status->PeekAmmoBlock());
		TestNotNull(TEXT("the abilities block exists"), Status->PeekAbilitiesBlock());
		TestNotNull(TEXT("the status block exists"), Status->PeekStatusBlock());
		// The exec staging auto-equips the first staged weapon through the real
		// equipment model while the rig's catalog pair carries no item mapping,
		// so the first refresh names the honest illegal-configuration state.
		Rig.Hud->RefreshWeaponStatusScreen();
		TestEqual(TEXT("the auto-equipped mapping-less staging item reports ConfigFailure"),
			static_cast<int32>(Status->PeekViewModel().State), static_cast<int32>(EWeaponStatusState::ConfigFailure));

		// The REAL unequip path (select the weapon row, click the unequip
		// button; the M3-012 flow) clears the slot mapping. The mount keeps its
		// refused bind, the equipment model is empty again and the refreshed
		// panel fills the explicit no-weapon state (the display never invents
		// a weapon).
		UInventoryWidget* MutableWidget = Rig.Hud->PeekInventoryWidget();
		if (!TestNotNull(TEXT("the inventory widget stays reachable"), MutableWidget))
		{
			return false;
		}
		int32 WeaponRowIndex = INDEX_NONE;
		const FInventoryListViewModel& List = MutableWidget->PeekViewModel();
		for (int32 Index = 0; Index < List.Rows.Num(); ++Index)
		{
			if (List.Rows[Index].SlotName == TEXT("Weapon"))
			{
				WeaponRowIndex = Index;
				break;
			}
		}
		if (!TestTrue(TEXT("the staged inventory carries a weapon row"), WeaponRowIndex != INDEX_NONE))
		{
			return false;
		}
		UButton* Row = MutableWidget->PeekRowButton(WeaponRowIndex);
		if (!TestNotNull(TEXT("the weapon row button exists"), Row))
		{
			return false;
		}
		Row->OnClicked.Broadcast();
		if (!TestTrue(TEXT("the row click selected the weapon"), MutableWidget->HasSelection()))
		{
			return false;
		}
		UButton* Unequip = MutableWidget->PeekUnequipButton();
		if (!TestNotNull(TEXT("the unequip button exists"), Unequip))
		{
			return false;
		}
		Unequip->OnClicked.Broadcast();
		Rig.Hud->RefreshWeaponStatusScreen();

		const FWeaponStatusViewModel& ViewModel = Status->PeekViewModel();
		TestTrue(TEXT("the presentation built a valid view model"), ViewModel.bValid);
		TestEqual(TEXT("a weapon-less presentation shows the no-weapon state"),
			static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::NoWeapon));
		return true;
	}

	// ----- 4. the HUD pushes the REAL mount state (no ammo grants) --------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_HudShowsRealMountStateWithoutAmmoGrants,
		"UEMMO.Tasks.M5_034.HudShowsRealMountStateWithoutAmmoGrants",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_HudShowsRealMountStateWithoutAmmoGrants::RunTest(const FString&)
	{
		FM5_034_Rig Rig;
		if (!Rig.Build(*this, /*bWithController*/ false, /*bInInjectClock*/ false)
			|| !Rig.Settle(*this)
			|| !Rig.BindHitscanRifle(*this))
		{
			return false;
		}
		UWeaponComponent* Mount = Rig.Player->GetWeaponMount();
		const int32 ReserveBefore = Mount->GetAmmoModel().GetReserveRounds(M5_034_AmmoId);
		TestEqual(TEXT("the pre-seeded reserve is intact before the screen opens"),
			ReserveBefore, M5_034_SeededReserve);

		// The production open/close entry toggles the screen.
		Rig.Hud->ToggleInventoryScreen();
		if (!TestTrue(TEXT("the toggle opened the inventory screen"), Rig.Hud->HasInventoryScreen()))
		{
			return false;
		}
		const UInventoryWidget* Widget = Rig.Hud->PeekInventoryWidget();
		const UWeaponStatusWidget* Status = Widget ? Widget->PeekWeaponStatus() : nullptr;
		if (!TestNotNull(TEXT("the presented screen carries the status panel"), Status))
		{
			return false;
		}
		const FWeaponStatusViewModel& ViewModel = Status->PeekViewModel();
		TestTrue(TEXT("the real-mount presentation built a valid view model"), ViewModel.bValid);
		TestEqual(TEXT("the bound rifle reports Ready"),
			static_cast<int32>(ViewModel.State), static_cast<int32>(EWeaponStatusState::Ready));
		TestTrue(TEXT("the status panel shows the REAL binding's weapon id"),
			ViewModel.WeaponLine.Contains(M5_034_WeaponId.ToString()));
		TestTrue(TEXT("the status panel shows the REAL loaded rounds"),
			ViewModel.AmmoLine.Contains(TEXT("Loaded 12/12")));
		TestTrue(TEXT("the status panel shows the REAL reserve"),
			ViewModel.AmmoLine.Contains(FString::Printf(TEXT("Reserve %d"), M5_034_SeededReserve)));

		// The display never grants ammo: the pools and magazines are exactly
		// what the bind itself created.
		TestEqual(TEXT("the reserve is untouched after the screen opened"),
			Mount->GetAmmoModel().GetReserveRounds(M5_034_AmmoId), M5_034_SeededReserve);
		TestEqual(TEXT("exactly the bind's magazine slot exists"),
			Mount->GetAmmoModel().NumMagazines(), 1);
		TestEqual(TEXT("exactly the bind's ammo pool exists"),
			Mount->GetAmmoModel().NumAmmoTypes(), 1);

		// Close/reopen presents the same real state again (no drift).
		Rig.Hud->ToggleInventoryScreen();
		TestFalse(TEXT("the toggle closed the screen"), Rig.Hud->HasInventoryScreen());
		Rig.Hud->ToggleInventoryScreen();
		if (!TestTrue(TEXT("the toggle reopened the screen"), Rig.Hud->HasInventoryScreen()))
		{
			return false;
		}
		const UWeaponStatusWidget* Reopened = Rig.Hud->PeekInventoryWidget()
			? Rig.Hud->PeekInventoryWidget()->PeekWeaponStatus()
			: nullptr;
		if (!TestNotNull(TEXT("the reopened screen carries the status panel"), Reopened))
		{
			return false;
		}
		TestEqual(TEXT("the reopened presentation shows the same real state"),
			static_cast<int32>(Reopened->PeekViewModel().State), static_cast<int32>(EWeaponStatusState::Ready));
		TestEqual(TEXT("the reserve is still untouched after the reopen"),
			Mount->GetAmmoModel().GetReserveRounds(M5_034_AmmoId), M5_034_SeededReserve);
		return true;
	}

	// ----- 5. the toggle gates fire and restores the game input -----------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_InventoryToggleGatesFireAndRestoresFocus,
		"UEMMO.Tasks.M5_034.InventoryToggleGatesFireAndRestoresFocus",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_InventoryToggleGatesFireAndRestoresFocus::RunTest(const FString&)
	{
		FM5_034_Rig Rig;
		if (!Rig.Build(*this, /*bWithController*/ true, /*bInInjectClock*/ true)
			|| !Rig.Tick(*this, 1.0f)
			|| !Rig.BindHitscanRifle(*this))
		{
			return false;
		}
		UWeaponComponent* Mount = Rig.Player->GetWeaponMount();

		// The production room state: the boot flow presents its menu overlay
		// while the flow sits in Menu (the HUD BeginPlay surface), and the
		// enter path dismisses it before the room world runs. The bare test
		// world never leaves the default Menu state, so the rig runs the same
		// dismissal half through the widget's real EnterAccepted bridge - the
		// lingering overlay would otherwise keep HasMenuScreen true and gate
		// the mount's menu-open predicate against every fire press.
		if (UMapSelectWidget* BootMenu = Rig.Hud->PeekMenuWidget())
		{
			BootMenu->EnterAccepted.Broadcast();
		}
		TestFalse(TEXT("the rig dismissed the boot menu overlay"), Rig.Hud->HasMenuScreen());

		// The production open path runs through the pawn's I-key handler.
		Rig.Player->OnInventoryTogglePressed();
		if (!TestTrue(TEXT("the pawn toggle opened the screen"), Rig.Hud->HasInventoryScreen()))
		{
			return false;
		}
		TestEqual(TEXT("the input focus is captured to the UI"),
			static_cast<int32>(Rig.Hud->PeekInventoryInputFocus().GetPhase()),
			static_cast<int32>(ERoomResultInputPhase::CapturedToUI));

		// While the screen is open the menu gate refuses fire: the press edge
		// produces no committed shot.
		Rig.Player->OnFireInputPressed();
		Rig.Player->OnFireInputReleased();
		TestEqual(TEXT("no shot is committed while the screen is open"),
			Mount->GetCommittedShotCount(), 0);

		// The close restores the game input phase exactly once.
		Rig.Player->OnInventoryTogglePressed();
		TestFalse(TEXT("the pawn toggle closed the screen"), Rig.Hud->HasInventoryScreen());
		TestEqual(TEXT("the input focus is restored to the game"),
			static_cast<int32>(Rig.Hud->PeekInventoryInputFocus().GetPhase()),
			static_cast<int32>(ERoomResultInputPhase::RestoredToGame));
		TestEqual(TEXT("exactly one restore was recorded"),
			Rig.Hud->PeekInventoryInputFocus().GetRestoreCount(), 1);

		// With the game input back, the same press edge fires exactly once.
		Rig.Player->OnFireInputPressed();
		Rig.Player->OnFireInputReleased();
		TestEqual(TEXT("the restored game input fires exactly one shot"),
			Mount->GetCommittedShotCount(), 1);

		// Reopen/close cycles never fire by themselves (no duplicate shots).
		Rig.Player->OnInventoryTogglePressed();
		Rig.Player->OnInventoryTogglePressed();
		TestFalse(TEXT("the double toggle ended closed"), Rig.Hud->HasInventoryScreen());
		TestEqual(TEXT("the open/close cycles committed no additional shot"),
			Mount->GetCommittedShotCount(), 1);
		TestEqual(TEXT("each open captured exactly once more"),
			Rig.Hud->PeekInventoryInputFocus().GetCaptureCount(), 2);
		return true;
	}

	// ----- 6. the reload states are visible on the presented screen -------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_ReloadStatesVisibleOnScreen,
		"UEMMO.Tasks.M5_034.ReloadStatesVisibleOnScreen",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_ReloadStatesVisibleOnScreen::RunTest(const FString&)
	{
		// No clock injection: the reload window and the HUD countdown both run
		// on the owner world's clock (the production wiring).
		FM5_034_Rig Rig;
		if (!Rig.Build(*this, /*bWithController*/ false, /*bInInjectClock*/ false)
			|| !Rig.Settle(*this)
			|| !Rig.BindHitscanRifle(*this))
		{
			return false;
		}
		UWeaponComponent* Mount = Rig.Player->GetWeaponMount();

		// Spend three rounds through the real fire path (600 rpm pacing). The
		// deduction lands in the M5-019 fire book on the binding record (the
		// rounds the shots consume; the M5-021 model slot stays the ledger).
		for (int32 Shot = 0; Shot < 3; ++Shot)
		{
			Rig.Player->OnFireInputPressed();
			Rig.Player->OnFireInputReleased();
			if (!Rig.Tick(*this, 0.15f))
			{
				return false;
			}
		}
		const FWeaponBindingRecord* Binding = Mount->GetActiveBinding();
		if (!TestNotNull(TEXT("the active binding still holds"), Binding))
		{
			return false;
		}
		TestEqual(TEXT("three real shots left the fire book"),
			Binding->LoadedRounds, M5_034_MagazineSize - 3);
		const FMagazineState* Ledger = Mount->GetAmmoModel().FindMagazine(Rig.BoundInstanceId);
		if (!TestNotNull(TEXT("the bind's reload ledger slot exists"), Ledger))
		{
			return false;
		}

		// T opens the real reload window (the M5-033 entry).
		Rig.Player->OnReloadInputPressed();
		if (!TestTrue(TEXT("the reload window opened"), Rig.Player->IsWeaponReloadPending()))
		{
			return false;
		}

		// The presented screen shows the reloading state with the countdown.
		Rig.Hud->ToggleInventoryScreen();
		Rig.Hud->RefreshWeaponStatusScreen();
		const UWeaponStatusWidget* Status = Rig.Hud->PeekInventoryWidget()
			? Rig.Hud->PeekInventoryWidget()->PeekWeaponStatus()
			: nullptr;
		if (!TestNotNull(TEXT("the presented screen carries the status panel"), Status))
		{
			return false;
		}
		TestEqual(TEXT("the open window reports Reloading"),
			static_cast<int32>(Status->PeekViewModel().State), static_cast<int32>(EWeaponStatusState::Reloading));
		TestTrue(TEXT("the reloading status carries the countdown text"),
			Status->PeekViewModel().StatusLine.Contains(TEXT("Reloading")));

		// The pawn's deadline poll completes the window; the refreshed panel
		// shows the refilled magazine and the reduced reserve (the transfer is
		// the model's - the UI granted nothing).
		if (!Rig.Tick(*this, 2.2f))
		{
			return false;
		}
		TestFalse(TEXT("the reload window closed"), Rig.Player->IsWeaponReloadPending());
		Rig.Hud->RefreshWeaponStatusScreen();
		TestEqual(TEXT("the completed reload reports Ready"),
			static_cast<int32>(Status->PeekViewModel().State), static_cast<int32>(EWeaponStatusState::Ready));
		TestTrue(TEXT("the refreshed panel shows the refilled magazine"),
			Status->PeekViewModel().AmmoLine.Contains(TEXT("Loaded 12/12")));
		TestTrue(TEXT("the refreshed panel shows the reduced reserve"),
			Status->PeekViewModel().AmmoLine.Contains(FString::Printf(TEXT("Reserve %d"), M5_034_SeededReserve - 3)));
		TestEqual(TEXT("the reserve moved only by the real transfer"),
			Mount->GetAmmoModel().GetReserveRounds(M5_034_AmmoId), M5_034_SeededReserve - 3);
		return true;
	}

	// ----- 7. an equipped weapon item without a mapping shows the failure -------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_UnboundWeaponItemShowsConfigFailure,
		"UEMMO.Tasks.M5_034.UnboundWeaponItemShowsConfigFailure",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_UnboundWeaponItemShowsConfigFailure::RunTest(const FString&)
	{
		FM5_034_Rig Rig;
		if (!Rig.Build(*this, /*bWithController*/ false, /*bInInjectClock*/ false)
			|| !Rig.MountCatalogsOnly(*this, /*bRegisterUnboundTrainingItem*/ true))
		{
			return false;
		}
		// The exec staging (the M3-011 precedent) stages the starter items
		// shaped like the shipped production source: a weapon item WITHOUT a
		// weapon mapping (the known M5-019 lineage gap).
		Rig.Hud->UEMMODebugInventory(1);
		if (!TestTrue(TEXT("the inventory screen is presented"), Rig.Hud->HasInventoryScreen()))
		{
			return false;
		}
		UInventoryWidget* Widget = Rig.Hud->PeekInventoryWidget();
		if (!TestNotNull(TEXT("the inventory widget is reachable"), Widget))
		{
			return false;
		}

		// Select the weapon row through its real click bridge, then equip
		// through the real button path (the M3-012 flow).
		int32 WeaponRowIndex = INDEX_NONE;
		const FInventoryListViewModel& List = Widget->PeekViewModel();
		for (int32 Index = 0; Index < List.Rows.Num(); ++Index)
		{
			if (List.Rows[Index].SlotName == TEXT("Weapon"))
			{
				WeaponRowIndex = Index;
				break;
			}
		}
		if (!TestTrue(TEXT("the staged inventory carries a weapon row"), WeaponRowIndex != INDEX_NONE))
		{
			return false;
		}
		UButton* Row = Widget->PeekRowButton(WeaponRowIndex);
		if (!TestNotNull(TEXT("the weapon row button exists"), Row))
		{
			return false;
		}
		Row->OnClicked.Broadcast();
		if (!TestTrue(TEXT("the row click selected the weapon"), Widget->HasSelection()))
		{
			return false;
		}
		UButton* Equip = Widget->PeekEquipButton();
		if (!TestNotNull(TEXT("the equip button exists"), Equip))
		{
			return false;
		}
		Equip->OnClicked.Broadcast();

		// The real equip path moved the mapping; the real mount chain refused
		// the mapping-less item (named refusal, no binding, no ammo).
		const FGuid* EquippedWeaponId = Rig.Hud->PeekInventoryEquipment().GetEquippedId(EItemSlot::Weapon);
		TestNotNull(TEXT("the equipment model holds the weapon occupant"), EquippedWeaponId);
		TestNull(TEXT("the mount holds no binding for the mapping-less item"),
			Rig.Player->GetWeaponMount()->GetActiveBinding());

		// The refreshed panel names the illegal configuration explicitly.
		Rig.Hud->RefreshWeaponStatusScreen();
		UWeaponStatusWidget* Status = Widget->PeekWeaponStatus();
		if (!TestNotNull(TEXT("the presented screen carries the status panel"), Status))
		{
			return false;
		}
		TestEqual(TEXT("the unbound weapon item reports ConfigFailure"),
			static_cast<int32>(Status->PeekViewModel().State), static_cast<int32>(EWeaponStatusState::ConfigFailure));
		TestTrue(TEXT("the failure status names the missing mapping"),
			Status->PeekViewModel().StatusLine.Contains(TEXT("mapping")));

		// And the UI path never initialized any ammo bookkeeping.
		TestEqual(TEXT("no magazine slot was created by the UI"),
			Rig.Player->GetWeaponMount()->GetAmmoModel().NumMagazines(), 0);
		TestEqual(TEXT("no reserve pool was created by the UI"),
			Rig.Player->GetWeaponMount()->GetAmmoModel().NumAmmoTypes(), 0);
		return true;
	}

	// ----- 8. the capture companion (real -game runs only; the M3-011 #8
	//          precedent) -------------------------------------------------------
	//
	// Headless automation runs skip this suite gracefully (no viewport). In a
	// real game world it drives the REAL entries - the debug staging exec for
	// the item data, the production toggle for every open/close, the pawn's
	// real fire/reload input handlers - and screenshots three panel states:
	// the honest illegal-configuration display, the mid-reload countdown and
	// the completed Ready readout. No assertions on world shape: the viewed
	// images are the evidence.

	static bool M5_034_CanCaptureScreenshot()
	{
		return GEngine != nullptr && GEngine->GameViewport != nullptr && FApp::CanEverRender();
	}

	static void M5_034_RequestScreenshotIfPossible(const FString& AbsolutePath)
	{
		if (!M5_034_CanCaptureScreenshot())
		{
			UE_LOG(LogTemp, Display, TEXT("M5_034 screenshot skipped (no rendering): %s"), *AbsolutePath);
			return;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
		FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogTemp, Display, TEXT("M5_034 screenshot requested: %s"), *AbsolutePath);
	}

	struct FM5_034_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM5_034_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			M5_034_RequestScreenshotIfPossible(AbsolutePath);
			return true;
		}
	};

	// Drives one HUD entry: Mode >= 0 is the debug staging exec (the item DATA
	// seeding entry; modes 1/0 exactly like the M3-011 capture), Mode -1 is the
	// production toggle (the entry the pawn's I-key handler drives).
	struct FM5_034_HudEntryLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeHUD> HudPtr;
		int32 Mode = 0;

		FM5_034_HudEntryLatentCommand(APrototypeHUD* InHud, int32 InMode)
			: HudPtr(InHud), Mode(InMode)
		{
		}

		virtual bool Update() override
		{
			APrototypeHUD* Hud = HudPtr.Get();
			if (Hud == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M5_034 capture entry skipped (hud gone)"));
				return true;
			}
			if (Mode < 0)
			{
				Hud->ToggleInventoryScreen();
			}
			else
			{
				Hud->UEMMODebugInventory(Mode);
			}
			UE_LOG(LogTemp, Display, TEXT("M5_034 capture drove entry mode %d (inventoryUp=%s, menuUp=%s)"),
				Mode,
				Hud->HasInventoryScreen() ? TEXT("yes") : TEXT("no"),
				Hud->HasMenuScreen() ? TEXT("yes") : TEXT("no"));
			return true;
		}
	};

	// The production room state: dismisses the boot menu overlay if the flow
	// still sits in Menu (the enter path's dismissal half), so the fire path's
	// menu gate reads clear like it does once a room world runs.
	struct FM5_034_DismissMenuLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeHUD> HudPtr;

		explicit FM5_034_DismissMenuLatentCommand(APrototypeHUD* InHud)
			: HudPtr(InHud)
		{
		}

		virtual bool Update() override
		{
			APrototypeHUD* Hud = HudPtr.Get();
			if (Hud != nullptr && Hud->HasMenuScreen())
			{
				if (UMapSelectWidget* BootMenu = Hud->PeekMenuWidget())
				{
					BootMenu->EnterAccepted.Broadcast();
				}
				UE_LOG(LogTemp, Display, TEXT("M5_034 capture: the boot menu overlay dismissed (flow menu state)."));
			}
			return true;
		}
	};

	// The capture-world rifle: one production-shaped catalog pair bound
	// through the real mount API (the disposable capture run replaces the
	// pawn's session pair), the shared reserve pre-seeded, one real instance
	// bound and the input wiring applied - then three REAL fire presses drop
	// the fire book to 9/12 (one press per visited frame, five settle frames
	// between presses for the 0.1 s pacing window). Failures log warnings and
	// the capture continues (the viewed images stay the evidence).
	struct FM5_034_BindAndFireLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		int32 ShotsFired = 0;
		int32 SettleFrames = 0;

		explicit FM5_034_BindAndFireLatentCommand(APrototypeCharacter* InPlayer)
			: PlayerPtr(InPlayer)
		{
		}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			if (Player == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M5_034 capture bind skipped (pawn gone)"));
				return true;
			}
			UWeaponComponent* Mount = Player->GetWeaponMount();
			if (Mount == nullptr)
			{
				return true;
			}
			if (ShotsFired == 0 && SettleFrames == 0)
			{
				// The one-time bind half.
				static FCombatCatalog CatalogValue;
				static FItemDefinitionCatalog ItemsValue;
				FParsedCombatConfig Parsed;
				Parsed.SchemaVersion = 1;
				FDamageProfile Profile;
				Profile.DamageProfileId = FName(TEXT("dmg_ui"));
				Profile.BaseDamage = 10.0f;
				Parsed.DamageProfiles.Add(Profile);
				FAmmoType Ammo;
				Ammo.AmmoId = M5_034_AmmoId;
				Ammo.MaxReserve = 120;
				Ammo.MagazineSize = M5_034_MagazineSize;
				Parsed.AmmoTypes.Add(Ammo);
				Parsed.Weapons.Add(M5_034_MakeDefinition(EWeaponFireMode::Hitscan));
				FString BuildErrors;
				if (!FCombatCatalog::BuildFromParsed(Parsed, CatalogValue, BuildErrors))
				{
					UE_LOG(LogTemp, Warning, TEXT("M5_034 capture bind refused (catalog build): %s"), *BuildErrors);
					return true;
				}
				FItemDefinition Item;
				Item.DefinitionId = M5_034_ItemId;
				Item.DisplayName = TEXT("UI Capture Rifle");
				Item.Slot = EItemSlot::Weapon;
				Item.Rarity = EItemRarity::Normal;
				Item.WeaponDefinitionId = M5_034_WeaponId;
				FString ItemError;
				if (!ItemsValue.AddDefinition(Item, &ItemError))
				{
					UE_LOG(LogTemp, Warning, TEXT("M5_034 capture bind refused (item definition): %s"), *ItemError);
					return true;
				}
				FString MountError;
				if (!Mount->MountCatalogs(&CatalogValue, &ItemsValue, &MountError))
				{
					UE_LOG(LogTemp, Warning, TEXT("M5_034 capture bind refused (mount): %s"), *MountError);
					return true;
				}
				FString SeedError;
				if (!Mount->GetAmmoModel().RegisterAmmoType(M5_034_AmmoId, 120, M5_034_SeededReserve, &SeedError))
				{
					UE_LOG(LogTemp, Warning, TEXT("M5_034 capture bind refused (reserve seed): %s"), *SeedError);
					return true;
				}
				FItemInstance Instance;
				Instance.InstanceId = FGuid(0xB33Fu, 1u, 4u, 9u);
				Instance.DefinitionId = M5_034_ItemId;
				Instance.Level = 1;
				const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
				if (!Outcome.bSucceeded)
				{
					UE_LOG(LogTemp, Warning, TEXT("M5_034 capture bind refused: %s"), *Outcome.RejectDetail);
					return true;
				}
				Player->ApplyWeaponBindWiring();
				UE_LOG(LogTemp, Display, TEXT("M5_034 capture: the capture rifle bound (instance %s)."), *Instance.InstanceId.ToString());
			}
			if (SettleFrames > 0)
			{
				--SettleFrames;
				return false;
			}
			if (ShotsFired < 3)
			{
				Player->OnFireInputPressed();
				Player->OnFireInputReleased();
				++ShotsFired;
				SettleFrames = 10;
				return false;
			}
			UE_LOG(LogTemp, Display, TEXT("M5_034 capture: %d real presses driven (fire book expects 9/12)."), ShotsFired);
			return true;
		}
	};

	// The real T entry: the pawn's reload handler opens the production reload
	// window (the completed transfer is the model's own, polled by the pawn).
	struct FM5_034_ReloadPressLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;

		explicit FM5_034_ReloadPressLatentCommand(APrototypeCharacter* InPlayer)
			: PlayerPtr(InPlayer)
		{
		}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			if (Player == nullptr)
			{
				return true;
			}
			Player->OnReloadInputPressed();
			UE_LOG(LogTemp, Display, TEXT("M5_034 capture: the T entry drove the reload (pending=%s)."),
				Player->IsWeaponReloadPending() ? TEXT("yes") : TEXT("no"));
			return true;
		}
	};

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FM5_034_WeaponStatusScreenshots,
		"UEMMO.Tasks.M5_034.WeaponStatusScreenshots",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FM5_034_WeaponStatusScreenshots::RunTest(const FString&)
	{
		UWorld* World = GWorld;
		if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
			|| World->WorldType != EWorldType::Game)
		{
			AddInfo(TEXT("screenshot capture skipped: no running game world/viewport"));
			return true;
		}
		APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
		APrototypeHUD* Hud = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
		APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
		if (PC == nullptr || Hud == nullptr || Player == nullptr)
		{
			AddInfo(TEXT("screenshot capture skipped: no player controller/HUD/pawn"));
			return true;
		}

		const FString Directory = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-034"));

		// 1. The staging exec seeds the profile items and auto-equips the first
		//    weapon through the real model; the shipped production source
		//    carries no weapon mapping, so the presented panel names the
		//    illegal configuration honestly (never papered over).
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_HudEntryLatentCommand(Hud, 1));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_ScreenshotLatentCommand(Directory / TEXT("inventory-weapon-status-config-failure.png")));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_HudEntryLatentCommand(Hud, 0));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));

		// 2. The capture rifle: the real mount API binds a ranged weapon, three
		//    real presses drop the fire book to 9/12 and the real T entry opens
		//    the reload window (the production room state has the boot overlay
		//    dismissed first, exactly like the enter path leaves it).
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_DismissMenuLatentCommand(Hud));
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_BindAndFireLatentCommand(Player));
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_ReloadPressLatentCommand(Player));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f));

		// 3. The production toggle (the pawn's I-key entry) presents the panel
		//    mid-reload; the DrawHUD refresh keeps the countdown advancing, and
		//    after the window completes the same panel shows the refilled
		//    magazine and the reduced reserve.
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_HudEntryLatentCommand(Hud, -1));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_ScreenshotLatentCommand(Directory / TEXT("inventory-weapon-status-reloading.png")));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.6f));
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_ScreenshotLatentCommand(Directory / TEXT("inventory-weapon-status-ready.png")));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
		ADD_LATENT_AUTOMATION_COMMAND(FM5_034_HudEntryLatentCommand(Hud, -1));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
		return true;
	}
}

#endif
