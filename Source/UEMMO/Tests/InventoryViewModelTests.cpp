// M3-011: the read-only inventory list. The suite locks the card's five
// behaviors (pure ViewModel functions first, no world needed):
//
//   1. A row is composed from the owned FItemInstance plus its shared
//      FItemDefinition: display name, slot label, stats line and the equipped
//      marker judged from the caller-passed equipped-id set.
//   2. A missing/stale definition degrades to readable placeholders
//      ("<unknown item>" / slot "-") without crashing; the instance's own
//      rolled stats still display.
//   3. Thirty stored items produce exactly 30 distinct rows whose id set
//      matches the inventory exactly (insertion order, no duplicates); the
//      pure list function caps at 30 defensively.
//   4. An empty inventory is the explicit empty state ("Inventory is empty").
//   5. The refresh throttle: an identical snapshot is never rebuilt
//      (AcceptSnapshot/RefreshIfChanged report bChanged=false and count a skip).
//
// Tests 6/7 add the thin presentation smoke (native widget build, Esc close
// broadcast) and the HUD exec integration (open -> captured, close -> restored)
// following the M2-012 RoomResultUITests scaffolding (FTestWorldWrapper).
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Profile/ProfileSubsystem.h"
#include "../PrototypeHUD.h"
#include "../UI/InventoryWidget.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_011
{
	// The card's list cap is the inventory design capacity (30 slots).
	static_assert(FInventoryModel::Capacity == 30, "M3-011 assumes the 30-slot design capacity");

	// Builds one legal definition double (the M3-001 field rules; stats >= 0).
	static FItemDefinition M3_011_MakeDefinition(FName Id, const FString& DisplayName,
		EItemSlot Slot, float Attack, float Defense, float MaxHP)
	{
		FItemDefinition Definition;
		Definition.DefinitionId = Id;
		Definition.DisplayName = DisplayName;
		Definition.Slot = Slot;
		Definition.BaseStats.Attack = Attack;
		Definition.BaseStats.Defense = Defense;
		Definition.BaseStats.MaxHP = MaxHP;
		Definition.Rarity = EItemRarity::Normal;
		return Definition;
	}

	// One owned occurrence of Definition with a fresh unique InstanceId.
	static FItemInstance M3_011_MakeInstance(const FItemDefinition& Definition, int64 RollSeed)
	{
		return MakeItemInstance(Definition, RollSeed);
	}

	// Creates a plain throwaway game world for the widget-level tests (the
	// M2-012 test-5 precedent: CreateWidget needs a world, not a viewport).
	static UWorld* M3_011_MakeWidgetWorld(FAutomationTestBase& Test, FTestWorldWrapper& Wrapper)
	{
		if (!Test.TestTrue(TEXT("the widget test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
		{
			return nullptr;
		}
		UWorld* World = Wrapper.GetTestWorld();
		Test.TestNotNull(TEXT("the widget test world is available"), World);
		return World;
	}

	static bool M3_011_CanCaptureScreenshot()
	{
		return GEngine != nullptr && GEngine->GameViewport != nullptr && FApp::CanEverRender();
	}

	// Requests a screenshot only where rendering exists; creates the target
	// directory first (FScreenshotRequest does not create directories). The
	// inventory screen is a Slate/UMG layer, so the capture must include the
	// UI (bShowUI=true, the M2-012 capture precedent).
	static void M3_011_RequestScreenshotIfPossible(const FString& AbsolutePath)
	{
		if (!M3_011_CanCaptureScreenshot())
		{
			UE_LOG(LogTemp, Display, TEXT("M3_011 screenshot skipped (no rendering): %s"), *AbsolutePath);
			return;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
		FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogTemp, Display, TEXT("M3_011 screenshot requested: %s"), *AbsolutePath);
	}

	// Latent step: requests one screenshot (the state was set before it runs).
	struct FM3_011_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM3_011_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			M3_011_RequestScreenshotIfPossible(AbsolutePath);
			return true;
		}
	};

	// Latent staging step: drives the real HUD debug exec entry (the exact
	// console path a user would drive) and logs the screen state as evidence;
	// the assertions live in the headless suites.
	struct FM3_011_InventoryExecLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeHUD> HudPtr;
		int32 Mode = 0;

		FM3_011_InventoryExecLatentCommand(APrototypeHUD* InHud, int32 InMode)
			: HudPtr(InHud), Mode(InMode)
		{
		}

		virtual bool Update() override
		{
			APrototypeHUD* Hud = HudPtr.Get();
			if (Hud == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M3_011 exec staging skipped (hud gone)"));
				return true;
			}
			Hud->UEMMODebugInventory(Mode);
			UE_LOG(LogTemp, Display, TEXT("M3_011 exec staging drove mode %d (screenUp=%s, rows=%d, phase=%d)"),
				Mode,
				Hud->HasInventoryScreen() ? TEXT("yes") : TEXT("no"),
				Hud->PeekInventoryViewModel().Rows.Num(),
				static_cast<int32>(Hud->PeekInventoryInputFocus().GetPhase()));
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_011;

// 1. Acceptance: instance + definition compose the row (name, slot, stats,
//    equipped marker from the passed id set). All three stats always show
//    (zeros included) so a missing grant reads as an explicit 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011RowFillsFromInstanceAndDefinition,
	"UEMMO.Tasks.M3_011.RowFillsFromInstanceAndDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011RowFillsFromInstanceAndDefinition::RunTest(const FString& Parameters)
{
	const FItemDefinition Sword = M3_011_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	FItemInstance Instance = M3_011_MakeInstance(Sword, 1);
	Instance.RolledStats.Attack = 5.0f;
	Instance.RolledStats.Defense = 0.0f;
	Instance.RolledStats.MaxHP = 0.0f;

	// Unequipped first.
	const FInventoryRowViewModel Row = MakeInventoryRowViewModel(Instance, &Sword, false);
	TestEqual(TEXT("the row copies the instance identity"), Row.InstanceId, Instance.InstanceId);
	TestEqual(TEXT("the row shows the definition display name"), Row.DisplayName, FString(TEXT("Training Sword")));
	TestEqual(TEXT("the weapon row shows the Weapon slot label"), Row.SlotName, FString(TEXT("Weapon")));
	TestEqual(TEXT("the stats line shows all three attributes with zeros"),
		Row.StatsText, FString(TEXT("Atk+5 Def+0 HP+0")));
	TestFalse(TEXT("the unequipped row carries no equipped marker"), Row.bEquipped);

	// Equipped: the marker comes from the caller-passed id set only.
	const FInventoryRowViewModel EquippedRow = MakeInventoryRowViewModel(Instance, &Sword, true);
	TestTrue(TEXT("the equipped row carries the marker"), EquippedRow.bEquipped);

	// The other two closed slots keep their labels.
	const FItemDefinition Armor = M3_011_MakeDefinition(TEXT("armor_training"),
		TEXT("Training Armor"), EItemSlot::Armor, 0.0f, 3.0f, 20.0f);
	const FItemInstance ArmorInstance = M3_011_MakeInstance(Armor, 2);
	const FInventoryRowViewModel ArmorRow = MakeInventoryRowViewModel(ArmorInstance, &Armor, false);
	TestEqual(TEXT("the armor row shows the Armor slot label"), ArmorRow.SlotName, FString(TEXT("Armor")));

	const FItemDefinition Charm = M3_011_MakeDefinition(TEXT("accessory_training"),
		TEXT("Training Charm"), EItemSlot::Accessory, 1.0f, 0.0f, 5.0f);
	const FItemInstance CharmInstance = M3_011_MakeInstance(Charm, 3);
	const FInventoryRowViewModel CharmRow = MakeInventoryRowViewModel(CharmInstance, &Charm, false);
	TestEqual(TEXT("the accessory row shows the Accessory slot label"), CharmRow.SlotName, FString(TEXT("Accessory")));

	// Fractional stats keep their value in the line (no silent rounding).
	FItemInstance Fractional = M3_011_MakeInstance(Sword, 4);
	Fractional.RolledStats.Attack = 5.5f;
	const FInventoryRowViewModel FractionalRow = MakeInventoryRowViewModel(Fractional, &Sword, false);
	TestTrue(TEXT("the stats line keeps fractional values"),
		FractionalRow.StatsText.Contains(TEXT("Atk+5.5")));
	return true;
}

// 2. Acceptance: a missing definition (nullptr, unknown catalog id, or an
//    out-of-enum slot) never crashes and always yields readable placeholders;
//    the instance's own rolled stats still display.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011MissingDefinitionRowUsesPlaceholderWithoutCrash,
	"UEMMO.Tasks.M3_011.MissingDefinitionRowUsesPlaceholderWithoutCrash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011MissingDefinitionRowUsesPlaceholderWithoutCrash::RunTest(const FString& Parameters)
{
	const FItemDefinition Known = M3_011_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 2.0f, 0.0f, 0.0f);

	// A null definition: the placeholder name, no invented slot, real stats.
	const FItemInstance Instance = M3_011_MakeInstance(Known, 1);
	const FInventoryRowViewModel NullRow = MakeInventoryRowViewModel(Instance, nullptr, false);
	TestEqual(TEXT("a missing definition shows the placeholder name"),
		NullRow.DisplayName, FString(TEXT("<unknown item>")));
	TestEqual(TEXT("a missing definition invents no slot label"), NullRow.SlotName, FString(TEXT("-")));
	TestEqual(TEXT("the missing-definition row still shows the instance stats"),
		NullRow.StatsText, FString(TEXT("Atk+2 Def+0 HP+0")));
	TestFalse(TEXT("the missing-definition row is not marked equipped"), NullRow.bEquipped);

	// A catalog that does not know the id: same placeholder path.
	FItemDefinitionCatalog Catalog;
	FString Error;
	TestTrue(TEXT("the catalog accepts its definition"), Catalog.AddDefinition(Known, &Error));
	FItemInstance Stale = M3_011_MakeInstance(Known, 2);
	Stale.DefinitionId = TEXT("weapon_missing");
	const FInventoryListViewModel StaleList = MakeInventoryListViewModel({Stale}, &Catalog, TSet<FGuid>());
	TestTrue(TEXT("the stale-definition list is valid"), StaleList.bValid);
	TestEqual(TEXT("the stale-definition list keeps one row"), StaleList.Rows.Num(), 1);
	if (StaleList.Rows.Num() == 1)
	{
		TestEqual(TEXT("the unknown catalog id shows the placeholder name"),
			StaleList.Rows[0].DisplayName, FString(TEXT("<unknown item>")));
	}

	// A hand-corrupted slot enum: no label is invented (the "-" placeholder).
	FItemDefinition Corrupted = M3_011_MakeDefinition(TEXT("weapon_bad"),
		TEXT("Bad Slot"), static_cast<EItemSlot>(88), 1.0f, 1.0f, 1.0f);
	const FInventoryRowViewModel CorruptedRow = MakeInventoryRowViewModel(Instance, &Corrupted, false);
	TestEqual(TEXT("an out-of-enum slot shows the placeholder label"),
		CorruptedRow.SlotName, FString(TEXT("-")));

	// A non-finite stat never reaches the text (it renders as 0).
	FItemInstance Poisoned = M3_011_MakeInstance(Known, 3);
	Poisoned.RolledStats.Attack = std::numeric_limits<float>::quiet_NaN();
	const FInventoryRowViewModel PoisonedRow = MakeInventoryRowViewModel(Poisoned, &Known, false);
	TestTrue(TEXT("a non-finite stat renders as zero, never as nan/inf"),
		PoisonedRow.StatsText.Contains(TEXT("Atk+0")));
	return true;
}

// 3. Acceptance: 30 stored items produce exactly 30 distinct rows whose id
//    set equals the inventory's id set (insertion order); the pure list
//    function caps at 30 even for a caller bug that passes more.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011ThirtyRowsMatchInventoryIdsWithoutDuplicates,
	"UEMMO.Tasks.M3_011.ThirtyRowsMatchInventoryIdsWithoutDuplicates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011ThirtyRowsMatchInventoryIdsWithoutDuplicates::RunTest(const FString& Parameters)
{
	const FItemDefinition Sword = M3_011_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	FItemDefinitionCatalog Catalog;
	FString Error;
	TestTrue(TEXT("the catalog accepts its definition"), Catalog.AddDefinition(Sword, &Error));

	// Fill the real inventory model to capacity through its own entries.
	FInventoryModel Inventory;
	for (int32 Index = 0; Index < FInventoryModel::Capacity; ++Index)
	{
		const FItemInstance Instance = M3_011_MakeInstance(Sword, Index + 1);
		TestEqual(TEXT("each starter item is stored"),
			Inventory.TryAdd(Instance), EInventoryAddResult::Added);
	}
	const TArray<FItemInstance>& Stored = Inventory.GetAll();
	TestEqual(TEXT("the inventory holds the full capacity"), Inventory.Count(), 30);

	const FInventoryListViewModel List = MakeInventoryListViewModel(Stored, &Catalog, TSet<FGuid>());
	TestTrue(TEXT("the full list is valid"), List.bValid);
	TestEqual(TEXT("the full list has exactly 30 rows"), List.Rows.Num(), 30);

	// No duplicates: the row id set has the same size as the row array.
	TSet<FGuid> RowIds;
	for (const FInventoryRowViewModel& Row : List.Rows)
	{
		RowIds.Add(Row.InstanceId);
	}
	TestEqual(TEXT("the row ids are pairwise distinct"), RowIds.Num(), 30);

	// Complete correspondence in order: every stored id appears exactly once,
	// in the inventory's insertion order.
	TSet<FGuid> StoredIds;
	for (const FItemInstance& Instance : Stored)
	{
		StoredIds.Add(Instance.InstanceId);
	}
	// Complete correspondence: same size and every stored id present.
	bool bIdSetsMatch = RowIds.Num() == StoredIds.Num();
	if (bIdSetsMatch)
	{
		for (const FItemInstance& Instance : Stored)
		{
			if (!RowIds.Contains(Instance.InstanceId))
			{
				bIdSetsMatch = false;
				break;
			}
		}
	}
	TestTrue(TEXT("the row id set equals the inventory id set"), bIdSetsMatch);
	if (List.Rows.Num() == Stored.Num())
	{
		bool bOrderMatches = true;
		for (int32 Index = 0; Index < Stored.Num(); ++Index)
		{
			if (List.Rows[Index].InstanceId != Stored[Index].InstanceId)
			{
				bOrderMatches = false;
				break;
			}
		}
		TestTrue(TEXT("the rows follow the inventory insertion order"), bOrderMatches);
	}

	// Defensive cap: a caller bug passing 32 instances still yields 30 rows
	// (the first 30 in insertion order), never an unbounded list.
	TArray<FItemInstance> Overflow = Stored;
	Overflow.Add(M3_011_MakeInstance(Sword, 991));
	Overflow.Add(M3_011_MakeInstance(Sword, 992));
	const FInventoryListViewModel Capped = MakeInventoryListViewModel(Overflow, &Catalog, TSet<FGuid>());
	TestEqual(TEXT("an oversized input caps at 30 rows"), Capped.Rows.Num(), 30);
	if (Capped.Rows.Num() == 30)
	{
		TestTrue(TEXT("the cap keeps the first 30 in insertion order"),
			Capped.Rows[29].InstanceId == Overflow[29].InstanceId);
	}
	return true;
}

// 4. Acceptance: an empty inventory is an explicit empty state - the pure
//    list is valid but empty, and the widget shows the "Inventory is empty"
//    line instead of rows (collapsed again once an item arrives).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011EmptyInventoryShowsEmptyState,
	"UEMMO.Tasks.M3_011.EmptyInventoryShowsEmptyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011EmptyInventoryShowsEmptyState::RunTest(const FString& Parameters)
{
	// Pure level: an empty input is a valid, empty list.
	const FInventoryListViewModel Empty = MakeInventoryListViewModel(TArray<FItemInstance>(), nullptr, TSet<FGuid>());
	TestTrue(TEXT("the empty list is valid"), Empty.bValid);
	TestTrue(TEXT("the empty list reports the empty state"), Empty.IsEmpty());
	TestEqual(TEXT("the empty list has no rows"), Empty.Rows.Num(), 0);

	// Widget level: the empty-state line is visible with the exact text.
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_011_MakeWidgetWorld(*this, Wrapper);
	if (World == nullptr)
	{
		return true;
	}
	UInventoryWidget* Widget = CreateWidget<UInventoryWidget>(World, UInventoryWidget::StaticClass());
	if (!TestNotNull(TEXT("the native inventory widget is created (no UMG asset)"), Widget))
	{
		return true;
	}
	Widget->BindInventory(TArray<FItemInstance>(), nullptr, TSet<FGuid>());
	TestNotNull(TEXT("the empty-state text block exists"), Widget->PeekEmptyBlock());
	if (Widget->PeekEmptyBlock() != nullptr)
	{
		TestTrue(TEXT("the empty-state line is visible for an empty inventory"),
			Widget->PeekEmptyBlock()->GetVisibility() != ESlateVisibility::Collapsed);
		TestEqual(TEXT("the empty-state text is explicit"),
			Widget->PeekEmptyBlock()->GetText().ToString(), FString(TEXT("Inventory is empty")));
	}
	TestEqual(TEXT("an empty inventory builds no rows"), Widget->PeekRowCount(), 0);
	TestTrue(TEXT("the widget view model agrees (empty)"), Widget->PeekViewModel().IsEmpty());

	// One item arrives: the empty-state line collapses and a row appears.
	const FItemDefinition Sword = M3_011_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	const FItemInstance Instance = M3_011_MakeInstance(Sword, 1);
	Widget->BindInventory({Instance}, nullptr, TSet<FGuid>());
	if (Widget->PeekEmptyBlock() != nullptr)
	{
		TestTrue(TEXT("the empty-state line collapses once an item exists"),
			Widget->PeekEmptyBlock()->GetVisibility() == ESlateVisibility::Collapsed);
	}
	TestEqual(TEXT("one item builds exactly one row"), Widget->PeekRowCount(), 1);
	return true;
}

// 5. Acceptance: the refresh throttle - an identical snapshot is never
//    rebuilt (bChanged=false, a counted skip); a changed snapshot rebuilds.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011RefreshGuardSkipsIdenticalSnapshot,
	"UEMMO.Tasks.M3_011.RefreshGuardSkipsIdenticalSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011RefreshGuardSkipsIdenticalSnapshot::RunTest(const FString& Parameters)
{
	const FItemDefinition Sword = M3_011_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	const FItemInstance First = M3_011_MakeInstance(Sword, 1);
	const FItemInstance Second = M3_011_MakeInstance(Sword, 2);

	// Pure guard: first accept rebuilds, identical does not, changed does.
	FInventoryRefreshGuard Guard;
	const FString FirstFingerprint = MakeInventorySnapshotFingerprint({First}, TSet<FGuid>());
	TestTrue(TEXT("the first snapshot rebuilds (no baseline yet)"), Guard.AcceptSnapshot(FirstFingerprint));
	TestFalse(TEXT("the identical snapshot is not rebuilt"),
		Guard.AcceptSnapshot(MakeInventorySnapshotFingerprint({First}, TSet<FGuid>())));
	TestEqual(TEXT("the identical snapshot was counted as skipped"), Guard.GetSkippedCount(), 1);
	TestTrue(TEXT("a changed snapshot rebuilds again"),
		Guard.AcceptSnapshot(MakeInventorySnapshotFingerprint({First, Second}, TSet<FGuid>())));
	TestEqual(TEXT("the changed snapshot was counted as performed"), Guard.GetPerformedCount(), 2);

	// The equipped set participates: the same instances with a new equipped
	// id change the render, so they must not be throttled away.
	TestTrue(TEXT("an equipped-flag change rebuilds"),
		Guard.AcceptSnapshot(MakeInventorySnapshotFingerprint({First, Second}, TSet<FGuid>({First.InstanceId}))));

	// Widget level: BindInventory records the baseline, an identical refresh
	// is skipped (rows untouched) and a grown inventory rebuilds the rows.
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_011_MakeWidgetWorld(*this, Wrapper);
	if (World == nullptr)
	{
		return true;
	}
	UInventoryWidget* Widget = CreateWidget<UInventoryWidget>(World, UInventoryWidget::StaticClass());
	if (!TestNotNull(TEXT("the native inventory widget is created"), Widget))
	{
		return true;
	}
	const TArray<FItemInstance> Snapshot = {First, Second, M3_011_MakeInstance(Sword, 3)};
	Widget->BindInventory(Snapshot, nullptr, TSet<FGuid>());
	TestEqual(TEXT("the bind built three rows"), Widget->PeekRowCount(), 3);

	const bool bIdenticalRebuilt = Widget->RefreshIfChanged(Snapshot, nullptr, TSet<FGuid>());
	TestFalse(TEXT("the identical refresh reports no rebuild"), bIdenticalRebuilt);
	TestEqual(TEXT("the identical refresh was counted as skipped"),
		Widget->PeekRefreshGuard().GetSkippedCount(), 1);
	TestEqual(TEXT("the rows were not rebuilt by the identical refresh"), Widget->PeekRowCount(), 3);

	TArray<FItemInstance> Grown = Snapshot;
	Grown.Add(M3_011_MakeInstance(Sword, 4));
	const bool bGrownRebuilt = Widget->RefreshIfChanged(Grown, nullptr, TSet<FGuid>());
	TestTrue(TEXT("the grown snapshot rebuilds"), bGrownRebuilt);
	TestEqual(TEXT("the rebuilt list shows four rows"), Widget->PeekRowCount(), 4);
	return true;
}

// 6. Acceptance: the native widget builds its control tree in code (title,
//    scroll box, close button), renders composed row texts with the equipped
//    marker, and Esc broadcasts the close request exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011WidgetSmokeBuildsScrollListAndEscCloses,
	"UEMMO.Tasks.M3_011.WidgetSmokeBuildsScrollListAndEscCloses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011WidgetSmokeBuildsScrollListAndEscCloses::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_011_MakeWidgetWorld(*this, Wrapper);
	if (World == nullptr)
	{
		return true;
	}
	UInventoryWidget* Widget = CreateWidget<UInventoryWidget>(World, UInventoryWidget::StaticClass());
	if (!TestNotNull(TEXT("the native inventory widget is created"), Widget))
	{
		return true;
	}

	const FItemDefinition Sword = M3_011_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	const FItemInstance Equipped = M3_011_MakeInstance(Sword, 1);
	const FItemInstance Spare = M3_011_MakeInstance(Sword, 2);
	Widget->BindInventory({Equipped, Spare}, nullptr, TSet<FGuid>({Equipped.InstanceId}));

	TestNotNull(TEXT("the title text block exists"), Widget->PeekTitleBlock());
	TestNotNull(TEXT("the scroll box exists (the 30-item list scrolls)"), Widget->PeekListScrollBox());
	TestNotNull(TEXT("the close button exists"), Widget->PeekCloseButton());
	if (Widget->PeekTitleBlock() != nullptr)
	{
		TestEqual(TEXT("the title names the screen"),
			Widget->PeekTitleBlock()->GetText().ToString(), FString(TEXT("Inventory")));
	}
	TestEqual(TEXT("two items built two rows"), Widget->PeekRowCount(), 2);

	// Close via the button path (the delegate is the single close contract).
	int32 CloseRequests = 0;
	Widget->CloseRequested.AddLambda([&CloseRequests]()
	{
		++CloseRequests;
	});
	if (UButton* CloseButton = Widget->PeekCloseButton())
	{
		CloseButton->OnClicked.Broadcast();
		TestEqual(TEXT("the close button broadcast the close request"), CloseRequests, 1);
	}
	else
	{
		AddError(TEXT("the close button is missing (the close contract is broken)"));
	}

	// Esc while focused: consumed and broadcast; another key is not.
	const FModifierKeysState NoModifiers;
	FKeyEvent EscapeEvent(EKeys::Escape, NoModifiers, 0, /*bIsRepeat*/ false, 0, 0);
	const FReply EscapeReply = Widget->NativeOnKeyDown(FGeometry(), EscapeEvent);
	TestTrue(TEXT("Esc is consumed by the inventory screen"), EscapeReply.IsEventHandled());
	TestEqual(TEXT("Esc broadcast the close request"), CloseRequests, 2);

	FKeyEvent OtherEvent(EKeys::A, NoModifiers, 0, false, 0, 0);
	const FReply OtherReply = Widget->NativeOnKeyDown(FGeometry(), OtherEvent);
	TestFalse(TEXT("a game key is not consumed by the read-only screen"), OtherReply.IsEventHandled());
	TestEqual(TEXT("the game key sent no close request"), CloseRequests, 2);
	return true;
}

// 7. Acceptance: the HUD debug exec opens the screen from the real profile
//    inventory (staged through the production entries), captures the input to
//    UI once; the close path dismisses the screen and restores the game focus
//    exactly once; a fresh presentation captures again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011HudExecOpensInventoryAndCloseRestoresFocus,
	"UEMMO.Tasks.M3_011.HudExecOpensInventoryAndCloseRestoresFocus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011HudExecOpensInventoryAndCloseRestoresFocus::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World))
	{
		return true;
	}
	if (!TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
	{
		return true;
	}

	// The HUD under test, spawned like the game mode spawns it.
	FActorSpawnParameters HudParams;
	APrototypeHUD* Hud = World->SpawnActor<APrototypeHUD>(APrototypeHUD::StaticClass(),
		FVector::ZeroVector, FRotator::ZeroRotator, HudParams);
	if (!TestNotNull(TEXT("the prototype HUD spawns"), Hud))
	{
		return true;
	}

	// Open (mode 1): the debug staging runs through the production entries
	// (NewProfile for a world-less test profile + TryAdd) and presents the
	// read-only list from the REAL profile inventory.
	Hud->UEMMODebugInventory(1);
	TestTrue(TEXT("the exec opened the inventory screen"), Hud->HasInventoryScreen());
	const FInventoryListViewModel& ViewModel = Hud->PeekInventoryViewModel();
	TestTrue(TEXT("the presented list is valid"), ViewModel.bValid);
	TestEqual(TEXT("the staged starter inventory has three rows"), ViewModel.Rows.Num(), 3);
	if (ViewModel.Rows.Num() == 3)
	{
		TestTrue(TEXT("the staged first item is marked equipped"), ViewModel.Rows[0].bEquipped);
		TestTrue(TEXT("the staged spare items are not marked equipped"), !ViewModel.Rows[1].bEquipped);
	}
	TestTrue(TEXT("the input was captured to the UI"),
		Hud->PeekInventoryInputFocus().GetPhase() == ERoomResultInputPhase::CapturedToUI);

	// An identical HUD-side refresh (the change-point entry) must not rebuild
	// the rows and must not move the focus state.
	Hud->RefreshInventoryScreen();
	TestTrue(TEXT("the identical refresh keeps the screen up"), Hud->HasInventoryScreen());
	TestEqual(TEXT("the identical refresh keeps the same rows"),
		Hud->PeekInventoryViewModel().Rows.Num(), 3);

	// Close (mode 0): the screen is dismissed and the input focus restored.
	Hud->UEMMODebugInventory(0);
	TestFalse(TEXT("the exec closed the inventory screen"), Hud->HasInventoryScreen());
	TestTrue(TEXT("the input focus returned to the game"),
		Hud->PeekInventoryInputFocus().GetPhase() == ERoomResultInputPhase::RestoredToGame);
	TestEqual(TEXT("the restore happened exactly once"),
		Hud->PeekInventoryInputFocus().GetRestoreCount(), 1);
	TestEqual(TEXT("the capture happened exactly once"),
		Hud->PeekInventoryInputFocus().GetCaptureCount(), 1);

	// A second presentation captures again (per-presentation one-time rule).
	Hud->UEMMODebugInventory(1);
	TestTrue(TEXT("the second presentation opened the screen again"), Hud->HasInventoryScreen());
	TestEqual(TEXT("the second presentation captured again"),
		Hud->PeekInventoryInputFocus().GetCaptureCount(), 2);
	Hud->UEMMODebugInventory(0);
	return true;
}

// 8. Capture companion (the M2-012/M2-009 precedent): in a real game world
//    with a viewport, drive the real HUD exec path - open the staged starter
//    inventory (one equipped marker), fill to the 30-slot capacity (the list
//    rebuilds and scrolls), then close (the game input returns) - one
//    screenshot per state. Headless -nullrhi automation runs skip the capture
//    gracefully.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_011InventoryScreenshots,
	"UEMMO.Tasks.M3_011.InventoryScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_011InventoryScreenshots::RunTest(const FString& Parameters)
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
	if (PC == nullptr || Hud == nullptr)
	{
		AddInfo(TEXT("screenshot capture skipped: no player controller/HUD"));
		return true;
	}

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M3-011"));

	// Open (mode 1): the debug staging runs through the production entries and
	// presents the read-only list (three items, the first marked equipped).
	ADD_LATENT_AUTOMATION_COMMAND(FM3_011_InventoryExecLatentCommand(Hud, 1));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_011_ScreenshotLatentCommand(Directory / TEXT("inventory-open.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));

	// Fill to 30 (mode 2): the changed snapshot rebuilds the rows; the panel
	// shows the scroll box with the full 30-item list.
	ADD_LATENT_AUTOMATION_COMMAND(FM3_011_InventoryExecLatentCommand(Hud, 2));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_011_ScreenshotLatentCommand(Directory / TEXT("inventory-full-30.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));

	// Close (mode 0): the screen dismisses and the game input focus returns.
	ADD_LATENT_AUTOMATION_COMMAND(FM3_011_InventoryExecLatentCommand(Hud, 0));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	return true;
}

#endif
