// M3-012: the inventory screen's equip/unequip actions with the stat
// comparison preview. The suite locks the card's behaviors (pure ViewModel
// functions first, then the real HUD request paths):
//
//   1. The preview is pure display data: running it against a REAL profile
//      (before/after GetProfileSnapshot comparison) never mutates the profile,
//      and dismissing/reopening the screen cancels it (still no mutation).
//   2. The preview lines and the readable result/refusal texts are pure and
//      cover the equip / replace / already-equipped / no-selection shapes.
//   3. A successful Equip through the real button path moves the equipment
//      model mapping, pushes the bonus through the pawn's TryEquipStatBonus
//      (wired stats change), refreshes the equipped markers and shows the
//      readable result; afterwards the preview reports "No change".
//   4. Refused actions show readable reasons (missing player, unequip of an
//      empty slot) and never change the mapping or the snapshot.
//   5. 100 equip/unequip cycles leave no stat drift: every equip restores the
//      exact expected final row, every unequip the exact base row, the list
//      markers follow (UI sync) and the inventory never gains or loses items.
//   6. A fast double click on Equip is processed exactly once (one-shot
//      guard); re-selecting re-arms, so the next click works again.
//   7. While a room run is Running the action buttons disable (the visible
//      half) and the pawn's TryEquipStatBonus refuses (the bottom half).
//   8. A same-slot replacement moves the mapping to the new item and BOTH
//      items stay stored in the inventory.
//
// Harness: the engine FTestWorldWrapper precedent (M3-010/M3-011 suites) -
// a manually ticked temp world whose Game world carries a real UGameInstance,
// so the production UProfileSubsystem resolution, the session binding and the
// pawn's BeginPlay profile load run exactly like the game's. No world ticking
// is needed: the equip path is synchronous pure logic plus component writes.
//
// Stub-failure note: the red stub leaves the preview and every result/refusal
// text empty and makes the HUD request paths no-ops (the staging keeps no
// model mapping), so each test below fails on at least one text, mapping,
// snapshot or guard assertion.

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Items/EquipmentModel.h"
#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Profile/ProfileSubsystem.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../UI/InventoryWidget.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_012
{
	// The card's loop acceptance: 100 full equip/unequip cycles.
	constexpr int32 M3_012_CycleCount = 100;

	// Builds one legal definition double (the M3-001 field rules; stats >= 0).
	static FItemDefinition M3_012_MakeDefinition(const TCHAR* Id, EItemSlot Slot,
		float Attack, float Defense, float MaxHP)
	{
		FItemDefinition Definition;
		Definition.DefinitionId = FName(Id);
		Definition.DisplayName = FString(Id);
		Definition.Slot = Slot;
		Definition.BaseStats.Attack = Attack;
		Definition.BaseStats.Defense = Defense;
		Definition.BaseStats.MaxHP = MaxHP;
		Definition.Rarity = EItemRarity::Normal;
		return Definition;
	}

	// One owned occurrence with a deterministic FGuid (no NewGuid randomness).
	static FItemInstance M3_012_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(Seed, 0xA11Cu, Seed + 7u, Seed * 3u + 1u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(Seed);
		Instance.RolledStats.Attack = Attack;
		Instance.RolledStats.Defense = Defense;
		Instance.RolledStats.MaxHP = MaxHP;
		Instance.Level = 1;
		return Instance;
	}

	// One stats row (plain aggregate; no positional constructor is assumed).
	static FItemStats M3_012_MakeStats(float Attack, float Defense, float MaxHP)
	{
		FItemStats Stats;
		Stats.Attack = Attack;
		Stats.Defense = Defense;
		Stats.MaxHP = MaxHP;
		return Stats;
	}

	// Full snapshot equality: identity, progress, derived stats and every
	// stored inventory payload in insertion order. OutDiff names the first
	// difference (readable failure output).
	static bool M3_012_SnapshotsEqual(const FProfileSnapshot& A, const FProfileSnapshot& B, FString& OutDiff)
	{
		if (A.CharacterId != B.CharacterId) { OutDiff = TEXT("CharacterId"); return false; }
		if (A.Level != B.Level) { OutDiff = TEXT("Level"); return false; }
		if (A.XP != B.XP) { OutDiff = TEXT("XP"); return false; }
		if (A.MaxHP != B.MaxHP) { OutDiff = TEXT("MaxHP"); return false; }
		if (A.Attack != B.Attack) { OutDiff = TEXT("Attack"); return false; }
		if (A.Defense != B.Defense) { OutDiff = TEXT("Defense"); return false; }
		if (A.Inventory.Count() != B.Inventory.Count()) { OutDiff = TEXT("Inventory.Count"); return false; }
		for (int32 Index = 0; Index < A.Inventory.Count(); ++Index)
		{
			const FItemInstance* Left = A.Inventory.GetByIndex(Index);
			const FItemInstance* Right = B.Inventory.GetByIndex(Index);
			if (Left == nullptr || Right == nullptr) { OutDiff = TEXT("Inventory.NullEntry"); return false; }
			if (Left->InstanceId != Right->InstanceId) { OutDiff = TEXT("Inventory.InstanceId"); return false; }
			if (Left->DefinitionId != Right->DefinitionId) { OutDiff = TEXT("Inventory.DefinitionId"); return false; }
			if (Left->RollSeed != Right->RollSeed) { OutDiff = TEXT("Inventory.RollSeed"); return false; }
			if (Left->Level != Right->Level) { OutDiff = TEXT("Inventory.Level"); return false; }
			if (Left->RolledStats.Attack != Right->RolledStats.Attack
				|| Left->RolledStats.Defense != Right->RolledStats.Defense
				|| Left->RolledStats.MaxHP != Right->RolledStats.MaxHP)
			{
				OutDiff = TEXT("Inventory.RolledStats");
				return false;
			}
		}
		OutDiff.Reset();
		return true;
	}

	// Spawns the real prototype pawn (no floor needed: the equip path is
	// synchronous logic plus component writes, nothing here ticks physics).
	static APrototypeCharacter* M3_012_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(), FVector(96000.0, 46000.0, 600.0), FRotator::ZeroRotator, Params);
		if (Player != nullptr && Player->GetCharacterMovement() != nullptr)
		{
			Player->GetCharacterMovement()->bRunPhysicsWithNoController = true;
		}
		return Player;
	}

	// Minimal one-wave room definition double (the M2-011/M3-010 precedent);
	// the Running test only calls StartRoom (no BeginWaves, no enemies).
	static URoomDefinition* M3_012_MakeRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m3_012_stage"));
		Room->RewardTableId = FName(TEXT("reward_m3_012"));
		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 1;
		Wave0.SpawnLocations.Add(FVector(95000.0, 46000.0, 600.0));
		Room->Waves.Add(Wave0);
		return Room;
	}

	/**
	 * One full scene: wrapper world, a fresh profile (created BEFORE play so
	 * the pawn's BeginPlay loads it), the session and the HUD. bWithPlayer
	 * spawns the real pawn (needed by the production equip push through
	 * TryEquipStatBonus); the no-player scenes prove the readable refusal.
	 */
	struct FM3_012_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeHUD* Hud = nullptr;
		APrototypeCharacter* Player = nullptr;
		UProfileSubsystem* Profile = nullptr;
		URoomSessionSubsystem* Session = nullptr;

		bool Build(FAutomationTestBase& Test, bool bWithPlayer)
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
			Profile = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UProfileSubsystem>() : nullptr;
			if (!Test.TestNotNull(TEXT("the profile subsystem exists in the test game instance"), Profile))
			{
				return false;
			}
			Profile->NewProfile();
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			if (bWithPlayer)
			{
				Player = M3_012_SpawnPlayer(*World);
				if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
				{
					return false;
				}
			}
			Hud = World->SpawnActor<APrototypeHUD>(APrototypeHUD::StaticClass(),
				FVector::ZeroVector, FRotator::ZeroRotator, FActorSpawnParameters());
			if (!Test.TestNotNull(TEXT("the prototype HUD spawns"), Hud))
			{
				return false;
			}
			return true;
		}

		/** Opens the screen through the production exec staging (3 starter items). */
		UInventoryWidget* OpenInventory(FAutomationTestBase& Test)
		{
			Hud->UEMMODebugInventory(1);
			if (!Test.TestTrue(TEXT("the exec opened the inventory screen"), Hud->HasInventoryScreen()))
			{
				return nullptr;
			}
			UInventoryWidget* Widget = Hud->PeekInventoryWidget();
			if (!Test.TestNotNull(TEXT("the inventory widget is reachable through the HUD"), Widget))
			{
				return nullptr;
			}
			Test.TestEqual(TEXT("the staged starter inventory presents three rows"),
				Widget->PeekRowCount(), 3);
			return Widget;
		}

		/** Clicks one row through its real dynamic OnClicked bridge. */
		bool ClickRow(FAutomationTestBase& Test, UInventoryWidget* Widget, int32 Index)
		{
			UButton* Row = Widget ? Widget->PeekRowButton(Index) : nullptr;
			if (!Test.TestNotNull(TEXT("the row button exists"), Row))
			{
				return false;
			}
			Row->OnClicked.Broadcast();
			return true;
		}

		/** Clicks the Equip / Unequip button through its real dynamic OnClicked. */
		bool ClickEquip(FAutomationTestBase& Test, UInventoryWidget* Widget)
		{
			UButton* Button = Widget ? Widget->PeekEquipButton() : nullptr;
			if (!Test.TestNotNull(TEXT("the equip button exists"), Button))
			{
				return false;
			}
			Button->OnClicked.Broadcast();
			return true;
		}

		bool ClickUnequip(FAutomationTestBase& Test, UInventoryWidget* Widget)
		{
			UButton* Button = Widget ? Widget->PeekUnequipButton() : nullptr;
			if (!Test.TestNotNull(TEXT("the unequip button exists"), Button))
			{
				return false;
			}
			Button->OnClicked.Broadcast();
			return true;
		}

		/** The level base row the preview's "current" side starts from. */
		FItemStats BaseStats() const
		{
			FItemStats Base;
			Base.MaxHP = static_cast<float>(UProfileSubsystem::GetMaxHPForLevel(Profile->GetLevel()));
			Base.Attack = static_cast<float>(UProfileSubsystem::GetAttackForLevel(Profile->GetLevel()));
			Base.Defense = static_cast<float>(UProfileSubsystem::GetDefenseForLevel(Profile->GetLevel()));
			return Base;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_012;

// 1. Acceptance: the preview never modifies the real profile - the pure
//    function runs on copies, the widget's selection preview likewise, and
//    dismissing/reopening the screen cancels it; every snapshot comparison
//    reports the exact same profile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012PreviewDoesNotTouchRealProfile,
	"UEMMO.Tasks.M3_012.PreviewDoesNotTouchRealProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012PreviewDoesNotTouchRealProfile::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ false))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr)
	{
		return true;
	}

	// Pure level: run the preview against the REAL profile's data copies.
	const TArray<FItemInstance>& Stored = Scene.Profile->GetInventory().GetAll();
	TestTrue(TEXT("the staged inventory provided the preview fixtures"), Stored.Num() >= 2);
	FProfileSnapshot Before = Scene.Profile->GetProfileSnapshot();
	const FInventoryStatPreviewViewModel PurePreview = MakeInventoryStatPreviewViewModel(
		Scene.BaseStats(), TArray<FItemStats>(), /*bHasSelection*/ true,
		Stored[1].RolledStats, /*bSelectedAlreadyEquipped*/ false,
		/*bSlotOccupied*/ false, FItemStats());
	TestTrue(TEXT("the pure preview is valid"), PurePreview.bValid);
	FString Diff;
	TestTrue(TEXT("the pure preview left the profile untouched"),
		M3_012_SnapshotsEqual(Before, Scene.Profile->GetProfileSnapshot(), Diff));

	// Widget level: selecting an item computes the preview display without any
	// profile write; the selection stays cancellable (nothing was equipped).
	if (!Scene.ClickRow(*this, Widget, 1))
	{
		return true;
	}
	TestTrue(TEXT("the row click selected the item"), Widget->HasSelection());
	TestEqual(TEXT("the selection is the clicked row's instance"),
		Widget->PeekSelectedInstanceId(), Stored[1].InstanceId);
	TestTrue(TEXT("the widget preview carries a selection"), Widget->PeekPreview().bHasSelection);
	TestTrue(TEXT("the widget preview is non-empty after the selection"),
		Widget->PeekPreview().bValid && !Widget->PeekPreview().CurrentLine.IsEmpty());
	TestTrue(TEXT("the selection preview left the profile untouched"),
		M3_012_SnapshotsEqual(Before, Scene.Profile->GetProfileSnapshot(), Diff));
	TestTrue(TEXT("the preview also left the equipped bonus row untouched"),
		Scene.Profile->GetEquippedStatBonus().Attack == Before.Attack - static_cast<float>(UProfileSubsystem::GetAttackForLevel(Before.Level)));

	// Cancel: close and reopen - the fresh presentation has no selection, no
	// preview content and the profile is still exactly the starting snapshot.
	Scene.Hud->UEMMODebugInventory(0);
	TestFalse(TEXT("the screen dismissed"), Scene.Hud->HasInventoryScreen());
	Scene.Hud->UEMMODebugInventory(1);
	TestTrue(TEXT("the screen reopened"), Scene.Hud->HasInventoryScreen());
	if (Scene.Hud->PeekInventoryWidget() != nullptr)
	{
		TestFalse(TEXT("the reopened screen starts without a selection"),
			Scene.Hud->PeekInventoryWidget()->HasSelection());
	}
	TestFalse(TEXT("the reopened screen's preview has no selection"),
		Scene.Hud->PeekInventoryPreview().bHasSelection);
	TestTrue(TEXT("the cancelled preview left the profile untouched"),
		M3_012_SnapshotsEqual(Before, Scene.Profile->GetProfileSnapshot(), Diff));
	Scene.Hud->UEMMODebugInventory(0);
	return true;
}

// 2. Acceptance: the preview lines and the readable texts are pure and cover
//    every shape: no selection, empty-slot equip, weaker replacement,
//    already-equipped no-op and the result/refusal text tables.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012PreviewAndResultTextsArePure,
	"UEMMO.Tasks.M3_012.PreviewAndResultTextsArePure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012PreviewAndResultTextsArePure::RunTest(const FString& Parameters)
{
	FItemStats Base;
	Base.Attack = 0.0f;
	Base.Defense = 0.0f;
	Base.MaxHP = 100.0f;

	// No selection: a valid but empty preview (the widget shows the hint).
	const FInventoryStatPreviewViewModel Empty = MakeInventoryStatPreviewViewModel(
		Base, TArray<FItemStats>(), false, FItemStats(), false, false, FItemStats());
	TestTrue(TEXT("the empty preview is still valid"), Empty.bValid);
	TestFalse(TEXT("the empty preview has no selection"), Empty.bHasSelection);
	TestTrue(TEXT("the empty preview keeps the current line empty"), Empty.CurrentLine.IsEmpty());
	TestTrue(TEXT("the empty preview keeps the after line empty"), Empty.AfterLine.IsEmpty());
	TestTrue(TEXT("the empty preview keeps the delta line empty"), Empty.DeltaLine.IsEmpty());

	// Empty-slot equip: the after row gains exactly the selected stats.
	TArray<FItemStats> NoEquipped;
	const FInventoryStatPreviewViewModel Fresh = MakeInventoryStatPreviewViewModel(
		Base, NoEquipped, true, M3_012_MakeStats(5.0f, 0.0f, 0.0f), false, false, FItemStats());
	TestEqual(TEXT("the fresh-equip current row is the bare base"),
		Fresh.CurrentLine, FString(TEXT("Current: Atk+0 Def+0 HP+100")));
	TestEqual(TEXT("the fresh-equip after row carries the selection"),
		Fresh.AfterLine, FString(TEXT("After equip: Atk+5 Def+0 HP+100")));
	TestEqual(TEXT("the fresh-equip delta is the selection"),
		Fresh.DeltaLine, FString(TEXT("Delta: Atk+5 Def+0 HP+0")));

	// Weaker replacement: the occupant's row leaves, the selection enters.
	TArray<FItemStats> WeaponEquipped;
	WeaponEquipped.Add(M3_012_MakeStats(7.0f, 0.0f, 0.0f));
	const FInventoryStatPreviewViewModel Replaced = MakeInventoryStatPreviewViewModel(
		Base, WeaponEquipped, true, M3_012_MakeStats(5.0f, 0.0f, 0.0f), false, true, M3_012_MakeStats(7.0f, 0.0f, 0.0f));
	TestEqual(TEXT("the replacement current row keeps the occupant"),
		Replaced.CurrentLine, FString(TEXT("Current: Atk+7 Def+0 HP+100")));
	TestEqual(TEXT("the replacement after row swaps the rows"),
		Replaced.AfterLine, FString(TEXT("After equip: Atk+5 Def+0 HP+100")));
	TestEqual(TEXT("the replacement delta is signed"),
		Replaced.DeltaLine, FString(TEXT("Delta: Atk-2 Def+0 HP+0")));

	// Already equipped: the idempotent no-op changes nothing (the row VALUE
	// stays the equipped one; the delta is the semantic no-change report).
	const FInventoryStatPreviewViewModel Same = MakeInventoryStatPreviewViewModel(
		Base, WeaponEquipped, true, M3_012_MakeStats(7.0f, 0.0f, 0.0f), true, false, FItemStats());
	TestEqual(TEXT("the already-equipped current row keeps the equipped row"),
		Same.CurrentLine, FString(TEXT("Current: Atk+7 Def+0 HP+100")));
	TestEqual(TEXT("the already-equipped after row keeps the equipped row"),
		Same.AfterLine, FString(TEXT("After equip: Atk+7 Def+0 HP+100")));
	TestEqual(TEXT("the already-equipped delta reports no change"),
		Same.DeltaLine, FString(TEXT("Delta: No change")));

	// An armor over an equipped weapon: the composite after row.
	TArray<FItemStats> Composite;
	Composite.Add(M3_012_MakeStats(5.0f, 0.0f, 0.0f));
	const FInventoryStatPreviewViewModel Armored = MakeInventoryStatPreviewViewModel(
		Base, Composite, true, M3_012_MakeStats(0.0f, 3.0f, 20.0f), false, false, FItemStats());
	TestEqual(TEXT("the composite after row sums base and both items"),
		Armored.AfterLine, FString(TEXT("After equip: Atk+5 Def+3 HP+120")));
	TestEqual(TEXT("the composite delta shows all three fields"),
		Armored.DeltaLine, FString(TEXT("Delta: Atk+0 Def+3 HP+20")));

	// The result tables name every enum value, pairwise distinct.
	const EEquipmentEquipResult EquipValues[6] = {
		EEquipmentEquipResult::Equipped, EEquipmentEquipResult::AlreadyEquipped,
		EEquipmentEquipResult::NotInInventory, EEquipmentEquipResult::SlotMismatch,
		EEquipmentEquipResult::InvalidSlot, EEquipmentEquipResult::MissingDefinitions };
	TSet<FString> EquipTexts;
	for (int32 Index = 0; Index < 6; ++Index)
	{
		const FString Text = MakeEquipResultText(EquipValues[Index]);
		TestFalse(TEXT("every equip result has readable text"), Text.IsEmpty());
		EquipTexts.Add(Text);
	}
	TestEqual(TEXT("the equip result texts are pairwise distinct"), EquipTexts.Num(), 6);

	const EEquipmentUnequipResult UnequipValues[3] = {
		EEquipmentUnequipResult::Unequipped, EEquipmentUnequipResult::NotEquipped,
		EEquipmentUnequipResult::InvalidSlot };
	TSet<FString> UnequipTexts;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const FString Text = MakeUnequipResultText(UnequipValues[Index]);
		TestFalse(TEXT("every unequip result has readable text"), Text.IsEmpty());
		UnequipTexts.Add(Text);
	}
	TestEqual(TEXT("the unequip result texts are pairwise distinct"), UnequipTexts.Num(), 3);

	// The refusal reason priority: missing profile > Running > missing player.
	TestEqual(TEXT("a missing profile outranks the other blocks"),
		MakeInventoryEquipBlockText(true, true, true), MakeInventoryEquipBlockText(true, false, false));
	TestFalse(TEXT("the running block has readable text"),
		MakeInventoryEquipBlockText(false, true, false).IsEmpty());
	TestTrue(TEXT("the running block names the running room"),
		MakeInventoryEquipBlockText(false, true, false).Contains(TEXT("running")));
	TestFalse(TEXT("the missing-player block has readable text"),
		MakeInventoryEquipBlockText(false, false, true).IsEmpty());
	TestTrue(TEXT("an unblocked context yields no refusal text"),
		MakeInventoryEquipBlockText(false, false, false).IsEmpty());
	return true;
}

// 3. Acceptance: a successful Equip through the real button path moves the
//    equipment model mapping, pushes the bonus through the pawn's production
//    entry (the wired stats change), refreshes the list markers and shows the
//    readable result; afterwards the preview reports "No change".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012EquipUpdatesMappingSnapshotAndUi,
	"UEMMO.Tasks.M3_012.EquipUpdatesMappingSnapshotAndUi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012EquipUpdatesMappingSnapshotAndUi::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ true))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr)
	{
		return true;
	}
	const TArray<FItemInstance>& Stored = Scene.Profile->GetInventory().GetAll();
	const FGuid WeaponId = Stored[0].InstanceId;
	const FGuid ArmorId = Stored[1].InstanceId;

	// The staged weapon is equipped through the real model; the armor is not.
	TestTrue(TEXT("the staged weapon occupies the Weapon slot"),
		Scene.Hud->PeekInventoryEquipment().IsSlotEquipped(EItemSlot::Weapon));
	TestFalse(TEXT("the Armor slot starts empty"),
		Scene.Hud->PeekInventoryEquipment().IsSlotEquipped(EItemSlot::Armor));

	// Select the armor row and press Equip.
	if (!Scene.ClickRow(*this, Widget, 1) || !Scene.ClickEquip(*this, Widget))
	{
		return true;
	}

	// The mapping moved (the armor entered its slot; the weapon stayed).
	const FEquipmentModel& Equipment = Scene.Hud->PeekInventoryEquipment();
	TestTrue(TEXT("the armor mapping exists after the equip"), Equipment.IsSlotEquipped(EItemSlot::Armor));
	if (const FGuid* Mapped = Equipment.GetEquippedId(EItemSlot::Armor))
	{
		TestTrue(TEXT("the Armor slot maps exactly the selected instance"), *Mapped == ArmorId);
	}
	if (const FGuid* WeaponMapped = Equipment.GetEquippedId(EItemSlot::Weapon))
	{
		TestTrue(TEXT("the Weapon slot still maps the staged weapon"), *WeaponMapped == WeaponId);
	}
	else
	{
		AddError(TEXT("the Weapon slot lost the staged weapon mapping"));
	}

	// The profile snapshot re-derived: base 100/0/0 + weapon 5/0/0 + armor 0/3/20.
	const FProfileSnapshot Snapshot = Scene.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the snapshot Attack includes the weapon bonus"), Snapshot.Attack, 5);
	TestEqual(TEXT("the snapshot Defense includes the armor bonus"), Snapshot.Defense, 3);
	TestEqual(TEXT("the snapshot MaxHP includes the armor bonus"), Snapshot.MaxHP, 120);

	// The production push applied to the pawn (the bottom entry ran).
	TestNotNull(TEXT("the scene has a player"), Scene.Player);
	if (Scene.Player != nullptr)
	{
		TestEqual(TEXT("the pawn health max follows the snapshot"), Scene.Player->GetHealth()->GetMaxHealth(), 120.0f, 0.01f);
		TestEqual(TEXT("the pawn attack follows the snapshot"), Scene.Player->GetCombat()->GetAttackPower(), 5.0f, 0.01f);
	}

	// The UI synced: the armor row carries the marker, the status names the result.
	TestEqual(TEXT("the status line shows the equip result"),
		Widget->PeekStatusBlock()->GetText().ToString(), MakeEquipResultText(EEquipmentEquipResult::Equipped));
	TestTrue(TEXT("the refreshed list marks the armor row"),
		Scene.Hud->PeekInventoryViewModel().Rows[1].bEquipped);
	TestTrue(TEXT("the refreshed list still marks the weapon row"),
		Scene.Hud->PeekInventoryViewModel().Rows[0].bEquipped);

	// The preview recomputed: the selected armor is now equipped -> no change.
	TestEqual(TEXT("the post-equip preview reports no change"),
		Scene.Hud->PeekInventoryPreview().DeltaLine, FString(TEXT("Delta: No change")));
	return true;
}

// 4a. Acceptance: the no-player context blocks the equip push with a readable
//     reason, and a refused request never changes the mapping or the snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012RefusedEquipShowsBlockReasonWithoutPlayer,
	"UEMMO.Tasks.M3_012.RefusedEquipShowsBlockReasonWithoutPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012RefusedEquipShowsBlockReasonWithoutPlayer::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ false))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr)
	{
		return true;
	}
	const FProfileSnapshot Before = Scene.Profile->GetProfileSnapshot();
	const FGuid* WeaponBefore = Scene.Hud->PeekInventoryEquipment().GetEquippedId(EItemSlot::Weapon);
	if (!Scene.ClickRow(*this, Widget, 1) || !Scene.ClickEquip(*this, Widget))
	{
		return true;
	}
	TestEqual(TEXT("the no-player equip shows the block reason"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeInventoryEquipBlockText(false, false, true));
	TestTrue(TEXT("the refused equip kept the weapon mapping"),
		WeaponBefore != nullptr && Scene.Hud->PeekInventoryEquipment().GetEquippedId(EItemSlot::Weapon) != nullptr
		&& *Scene.Hud->PeekInventoryEquipment().GetEquippedId(EItemSlot::Weapon) == *WeaponBefore);
	TestFalse(TEXT("the refused equip left the Armor slot empty"),
		Scene.Hud->PeekInventoryEquipment().IsSlotEquipped(EItemSlot::Armor));
	FString Diff;
	TestTrue(TEXT("the refused equip left the snapshot untouched"),
		M3_012_SnapshotsEqual(Before, Scene.Profile->GetProfileSnapshot(), Diff));
	Scene.Hud->UEMMODebugInventory(0);
	return true;
}

// 4b. Acceptance: unequipping an EMPTY slot shows the NotEquipped text and is
//     a pure no-op on the mapping and the snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012UnequipEmptySlotShowsNotEquippedText,
	"UEMMO.Tasks.M3_012.UnequipEmptySlotShowsNotEquippedText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012UnequipEmptySlotShowsNotEquippedText::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ true))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr)
	{
		return true;
	}
	const FProfileSnapshot SnapshotBefore = Scene.Profile->GetProfileSnapshot();
	// The armor row (index 1) is unequipped: its slot is empty.
	if (!Scene.ClickRow(*this, Widget, 1) || !Scene.ClickUnequip(*this, Widget))
	{
		return true;
	}
	TestEqual(TEXT("the empty-slot unequip shows the NotEquipped text"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeUnequipResultText(EEquipmentUnequipResult::NotEquipped));
	FString Diff;
	TestTrue(TEXT("the empty-slot unequip left the snapshot untouched"),
		M3_012_SnapshotsEqual(SnapshotBefore, Scene.Profile->GetProfileSnapshot(), Diff));
	Scene.Hud->UEMMODebugInventory(0);
	return true;
}

// 5. Acceptance: 100 equip/unequip cycles leave no stat drift - every equip
//    restores the exact expected final row, every unequip the exact base row,
//    the list markers follow (UI sync) and the inventory never changes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012HundredEquipUnequipCyclesLeaveNoDrift,
	"UEMMO.Tasks.M3_012.HundredEquipUnequipCyclesLeaveNoDrift",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012HundredEquipUnequipCyclesLeaveNoDrift::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ true))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr)
	{
		return true;
	}
	const int32 StagedCount = Scene.Profile->GetInventory().Count();

	// The staged weapon starts equipped: the first unequip opens the cycle.
	if (!Scene.ClickRow(*this, Widget, 0) || !Scene.ClickUnequip(*this, Widget))
	{
		return true;
	}
	TestEqual(TEXT("the opening unequip cleared the weapon mapping"),
		Scene.Hud->PeekInventoryEquipment().NumEquippedSlots(), 0);
	TestEqual(TEXT("the opening unequip restored the bare base Attack"),
		Scene.Profile->GetProfileSnapshot().Attack, 0);

	// The 100 cycles: each direction must land on the EXACT expected row.
	int32 Mismatches = 0;
	FString FirstProblem;
	for (int32 Cycle = 1; Cycle <= M3_012_CycleCount; ++Cycle)
	{
		// Equip half (a fresh selection re-arms the one-shot guard).
		if (!Scene.ClickRow(*this, Widget, 0) || !Scene.ClickEquip(*this, Widget))
		{
			return true;
		}
		const FProfileSnapshot Equipped = Scene.Profile->GetProfileSnapshot();
		const bool bEquippedOk = Equipped.Attack == 5 && Equipped.Defense == 0 && Equipped.MaxHP == 100
			&& Scene.Hud->PeekInventoryEquipment().NumEquippedSlots() == 1
			&& Scene.Hud->PeekInventoryViewModel().Rows[0].bEquipped
			&& Scene.Profile->GetInventory().Count() == StagedCount;
		if (!bEquippedOk && Mismatches == 0)
		{
			FirstProblem = FString::Printf(TEXT("equip cycle %d (Atk=%d Def=%d MaxHP=%d slots=%d marker=%d count=%d status=%s guardA=%d guardR=%d)"),
				Cycle, Equipped.Attack, Equipped.Defense, Equipped.MaxHP,
				Scene.Hud->PeekInventoryEquipment().NumEquippedSlots(),
				Scene.Hud->PeekInventoryViewModel().Rows[0].bEquipped ? 1 : 0,
				Scene.Profile->GetInventory().Count(),
				*Widget->PeekStatusBlock()->GetText().ToString(),
				Scene.Hud->PeekInventoryActionGuard().GetAcceptedCount(),
				Scene.Hud->PeekInventoryActionGuard().GetRejectedCount());
		}
		Mismatches += bEquippedOk ? 0 : 1;

		// Unequip half.
		if (!Scene.ClickRow(*this, Widget, 0) || !Scene.ClickUnequip(*this, Widget))
		{
			return true;
		}
		const FProfileSnapshot Unequipped = Scene.Profile->GetProfileSnapshot();
		const bool bUnequippedOk = Unequipped.Attack == 0 && Unequipped.Defense == 0 && Unequipped.MaxHP == 100
			&& Scene.Hud->PeekInventoryEquipment().NumEquippedSlots() == 0
			&& !Scene.Hud->PeekInventoryViewModel().Rows[0].bEquipped
			&& Scene.Profile->GetInventory().Count() == StagedCount;
		if (!bUnequippedOk && Mismatches == 0)
		{
			FirstProblem = FString::Printf(TEXT("unequip cycle %d (Atk=%d Def=%d MaxHP=%d slots=%d marker=%d count=%d status=%s guardA=%d guardR=%d)"),
				Cycle, Unequipped.Attack, Unequipped.Defense, Unequipped.MaxHP,
				Scene.Hud->PeekInventoryEquipment().NumEquippedSlots(),
				Scene.Hud->PeekInventoryViewModel().Rows[0].bEquipped ? 1 : 0,
				Scene.Profile->GetInventory().Count(),
				*Widget->PeekStatusBlock()->GetText().ToString(),
				Scene.Hud->PeekInventoryActionGuard().GetAcceptedCount(),
				Scene.Hud->PeekInventoryActionGuard().GetRejectedCount());
		}
		Mismatches += bUnequippedOk ? 0 : 1;
	}
	TestEqual(TEXT("the 100 equip/unequip cycles produced zero drift mismatches"), Mismatches, 0);
	if (!FirstProblem.IsEmpty())
	{
		AddError(FString::Printf(TEXT("the first cycle mismatch: %s"), *FirstProblem));
	}
	TestEqual(TEXT("the inventory kept exactly the staged items"),
		Scene.Profile->GetInventory().Count(), StagedCount);
	TestFalse(TEXT("the weapon mapping is cleared at the end"),
		Scene.Hud->PeekInventoryEquipment().IsSlotEquipped(EItemSlot::Weapon));
	Scene.Hud->UEMMODebugInventory(0);
	return true;
}

// 6. Acceptance: a fast double click on Equip is processed exactly once (the
//    one-shot guard drops the duplicate); re-selecting the item re-arms the
//    guard, so the next click works again and reports the idempotent no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012DoubleEquipClickProcessesOnce,
	"UEMMO.Tasks.M3_012.DoubleEquipClickProcessesOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012DoubleEquipClickProcessesOnce::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ true))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr)
	{
		return true;
	}
	const FGuid ArmorId = Scene.Profile->GetInventory().GetAll()[1].InstanceId;

	// First click: processed (accepted once).
	if (!Scene.ClickRow(*this, Widget, 1) || !Scene.ClickEquip(*this, Widget))
	{
		return true;
	}
	// The episode's legitimate state: the armor equip CHANGED the profile (the
	// card's job); the duplicate must not change it further, so the drift
	// comparison runs against this post-equip state.
	const FProfileSnapshot AfterFirstEquip = Scene.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the first click was accepted once"),
		Scene.Hud->PeekInventoryActionGuard().GetAcceptedCount(), 1);
	TestEqual(TEXT("the first click shows the equip result"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeEquipResultText(EEquipmentEquipResult::Equipped));

	// Duplicate click: dropped by the guard - no reprocessing (a processed
	// duplicate would show the AlreadyEquipped text), no state change.
	Scene.ClickEquip(*this, Widget);
	TestEqual(TEXT("the duplicate click was rejected once"),
		Scene.Hud->PeekInventoryActionGuard().GetRejectedCount(), 1);
	TestEqual(TEXT("the duplicate click did not run again"),
		Scene.Hud->PeekInventoryActionGuard().GetAcceptedCount(), 1);
	TestEqual(TEXT("the status still shows the first result"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeEquipResultText(EEquipmentEquipResult::Equipped));
	if (const FGuid* Mapped = Scene.Hud->PeekInventoryEquipment().GetEquippedId(EItemSlot::Armor))
	{
		TestTrue(TEXT("the armor mapping is unchanged by the duplicate"), *Mapped == ArmorId);
	}
	else
	{
		AddError(TEXT("the armor mapping vanished (the duplicate corrupted the state)"));
	}

	// Re-selecting re-arms: the next click processes and reports the no-op.
	if (!Scene.ClickRow(*this, Widget, 1))
	{
		return true;
	}
	Scene.ClickEquip(*this, Widget);
	TestEqual(TEXT("the re-armed click was accepted again"),
		Scene.Hud->PeekInventoryActionGuard().GetAcceptedCount(), 2);
	TestEqual(TEXT("the re-armed repeat reports the idempotent no-op"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeEquipResultText(EEquipmentEquipResult::AlreadyEquipped));
	FString Diff;
	TestTrue(TEXT("the duplicate clicks changed nothing after the first equip"),
		M3_012_SnapshotsEqual(AfterFirstEquip, Scene.Profile->GetProfileSnapshot(), Diff));
	Scene.Hud->UEMMODebugInventory(0);
	return true;
}

// 7. Acceptance: while a room run is Running the action buttons disable (the
//    visible half) AND the pawn's TryEquipStatBonus refuses (the bottom half);
//    after leaving the room the actions work again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012RunningDisablesButtonsAndBottomRefuses,
	"UEMMO.Tasks.M3_012.RunningDisablesButtonsAndBottomRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012RunningDisablesButtonsAndBottomRefuses::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ true))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr || Scene.Player == nullptr)
	{
		return true;
	}

	// Outside a run: with a selection the buttons are enabled.
	if (!Scene.ClickRow(*this, Widget, 1))
	{
		return true;
	}
	TestTrue(TEXT("the equip button is enabled outside a run"), Widget->PeekEquipButton()->GetIsEnabled());
	TestTrue(TEXT("the unequip button is enabled outside a run"), Widget->PeekUnequipButton()->GetIsEnabled());

	// The run starts (the OnRunStarted push disables the buttons live).
	if (!TestTrue(TEXT("the room run starts"), Scene.Session->StartRoom(M3_012_MakeRoom())))
	{
		return true;
	}
	TestTrue(TEXT("the session is Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestFalse(TEXT("the equip button disabled while Running"), Widget->PeekEquipButton()->GetIsEnabled());
	TestFalse(TEXT("the unequip button disabled while Running"), Widget->PeekUnequipButton()->GetIsEnabled());

	// The run start re-applied the staged weapon bonus (the M3-010 run-start
	// load point): the wired attack is 5 BEFORE the refusal.
	TestEqual(TEXT("the run start wired the staged attack 5"),
		Scene.Player->GetCombat()->GetAttackPower(), 5.0f, 0.01f);

	// The bottom half: the pawn's production entry refuses while Running.
	FItemStats Stronger;
	Stronger.Attack = 9.0f;
	TestFalse(TEXT("the pawn equip entry refuses while Running"),
		Scene.Player->TryEquipStatBonus(Stronger));
	TestEqual(TEXT("the refused push left the wired attack untouched"),
		Scene.Player->GetCombat()->GetAttackPower(), 5.0f, 0.01f);

	// A request that slips through the disabled buttons still refuses with
	// the readable Running reason and never half-applies.
	Scene.ClickEquip(*this, Widget);
	TestEqual(TEXT("the forced request shows the running block reason"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeInventoryEquipBlockText(false, true, false));
	TestFalse(TEXT("the forced request left the Armor slot empty"),
		Scene.Hud->PeekInventoryEquipment().IsSlotEquipped(EItemSlot::Armor));

	// Leaving the room lifts the block (the HUD-side refresh pushes the fresh
	// context) and the same equip works through the full production path.
	TestTrue(TEXT("the room is left"), Scene.Session->LeaveRoom());
	Scene.Hud->RefreshInventoryScreen();
	TestTrue(TEXT("the equip button re-enabled after leaving the room"),
		Widget->PeekEquipButton()->GetIsEnabled());
	TestTrue(TEXT("the re-enabled equip is processed"),
		Scene.ClickEquip(*this, Widget));
	TestEqual(TEXT("the post-run equip shows the equip result"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeEquipResultText(EEquipmentEquipResult::Equipped));
	TestTrue(TEXT("the post-run equip moved the mapping"),
		Scene.Hud->PeekInventoryEquipment().IsSlotEquipped(EItemSlot::Armor));
	Scene.Hud->UEMMODebugInventory(0);
	return true;
}

// 8. Acceptance: a same-slot replacement moves the mapping to the new item;
//    the replaced item stays stored in the inventory (nothing is deleted).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_012SlotReplacementKeepsReplacedItemStored,
	"UEMMO.Tasks.M3_012.SlotReplacementKeepsReplacedItemStored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_012SlotReplacementKeepsReplacedItemStored::RunTest(const FString& Parameters)
{
	FM3_012_Scene Scene;
	if (!Scene.Build(*this, /*bWithPlayer*/ true))
	{
		return true;
	}
	UInventoryWidget* Widget = Scene.OpenInventory(*this);
	if (Widget == nullptr)
	{
		return true;
	}
	const FGuid StagedWeaponId = Scene.Profile->GetInventory().GetAll()[0].InstanceId;

	// A second, stronger weapon enters the inventory through TryAdd.
	const FItemInstance Stronger = M3_012_MakeInstance(77u, TEXT("weapon_training"), 7.0f, 0.0f, 0.0f);
	TestEqual(TEXT("the stronger weapon is stored"),
		Scene.Profile->GetInventory().TryAdd(Stronger), EInventoryAddResult::Added);
	Scene.Hud->RefreshInventoryScreen();
	TestEqual(TEXT("the refreshed list shows four rows"), Widget->PeekRowCount(), 4);

	// Select the stronger weapon and equip: the Weapon slot re-maps.
	if (!Scene.ClickRow(*this, Widget, 3) || !Scene.ClickEquip(*this, Widget))
	{
		return true;
	}
	if (const FGuid* Mapped = Scene.Hud->PeekInventoryEquipment().GetEquippedId(EItemSlot::Weapon))
	{
		TestTrue(TEXT("the Weapon slot now maps the stronger weapon"), *Mapped == Stronger.InstanceId);
	}
	else
	{
		AddError(TEXT("the Weapon slot lost its mapping after the replacement"));
	}

	// The replaced weapon is STILL stored (replacement clears no inventory).
	bool bStagedStillStored = false;
	bool bStrongerStored = false;
	for (const FItemInstance& Instance : Scene.Profile->GetInventory().GetAll())
	{
		bStagedStillStored |= (Instance.InstanceId == StagedWeaponId);
		bStrongerStored |= (Instance.InstanceId == Stronger.InstanceId);
	}
	TestTrue(TEXT("the replaced weapon stays stored in the inventory"), bStagedStillStored);
	TestTrue(TEXT("the newly equipped weapon is stored too"), bStrongerStored);
	TestEqual(TEXT("the inventory count is unchanged by the replacement"),
		Scene.Profile->GetInventory().Count(), 4);

	// The snapshot sums ONLY the equipped item now (Atk 7), and the markers
	// moved in the refreshed list.
	const FProfileSnapshot Snapshot = Scene.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the snapshot Attack follows the new mapping"), Snapshot.Attack, 7);
	TestTrue(TEXT("the list moved the marker to the stronger weapon"),
		Scene.Hud->PeekInventoryViewModel().Rows[3].bEquipped);
	TestFalse(TEXT("the list cleared the replaced weapon's marker"),
		Scene.Hud->PeekInventoryViewModel().Rows[0].bEquipped);
	TestEqual(TEXT("the status shows the equip result"),
		Widget->PeekStatusBlock()->GetText().ToString(),
		MakeEquipResultText(EEquipmentEquipResult::Equipped));
	Scene.Hud->UEMMODebugInventory(0);
	return true;
}

#endif
