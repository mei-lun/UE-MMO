// M3-019: the three starter equipment kinds are distinguishable by icon and
// short slot tag without reading the stats. The suites lock the card's
// behaviors:
//
//   1. The shared slot icon config (ONE source for the inventory rows and the
//      settlement reward lines): the three closed slots resolve pairwise
//      distinct short tags (WPN/ARM/ACC) and pairwise distinct engine built-in
//      placeholder texture paths; an out-of-enum slot resolves no icon and the
//      readable "-" tag (nothing is invented).
//   2. The loader resolves the configured engine textures and fails closed
//      (nullptr) for an empty or unresolvable path - the fallback trigger.
//   3. The row view model carries the resolved tag/path from the definition's
//      slot; a missing definition degrades to "-" + empty (placeholder rule).
//   4. The real widget rows show the loaded placeholder icon; a row without a
//      resolvable icon falls back to the VISIBLE short tag (the tested
//      fallback path), and the three shown textures stay pairwise distinct.
//   5. The settlement reward lines carry the SAME tag from the SAME config
//      (one source, two surfaces; the reward text prefixes the tag).
//
// Harness: the M3-011/M3-012 precedent - pure functions first, then a
// manually ticked temp world for the widget level (CreateWidget needs a
// world, not a viewport), then the M3-011/M2-012 capture companion (real game
// world, real HUD exec path; -nullrhi regression runs skip it gracefully).
//
// Stub-failure note: the red stub configures NO icon (empty paths, empty
// tags, the loader always fails, the widget builds no icon area and the
// settlement line copies no tag), so every test below fails on at least one
// config, row, widget or settlement assertion.

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Profile/RewardService.h"
#include "../PrototypeHUD.h"
#include "../UI/InventoryWidget.h"
#include "../UI/RoomResultWidget.h"

#include "Blueprint/UserWidget.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_019
{
	// Builds one legal definition double (the M3-001 field rules; stats >= 0).
	static FItemDefinition M3_019_MakeDefinition(const TCHAR* Id, const TCHAR* DisplayName,
		EItemSlot Slot, float Attack, float Defense, float MaxHP)
	{
		FItemDefinition Definition;
		Definition.DefinitionId = FName(Id);
		Definition.DisplayName = DisplayName;
		Definition.Slot = Slot;
		Definition.BaseStats.Attack = Attack;
		Definition.BaseStats.Defense = Defense;
		Definition.BaseStats.MaxHP = MaxHP;
		Definition.Rarity = EItemRarity::Normal;
		return Definition;
	}

	// One owned occurrence with a deterministic FGuid (no NewGuid randomness).
	static FItemInstance M3_019_MakeInstance(const FItemDefinition& Definition, int64 RollSeed)
	{
		return MakeItemInstance(Definition, RollSeed);
	}

	// Creates a plain throwaway game world for the widget-level test (the
	// M3-011 precedent: CreateWidget needs a world, not a viewport).
	static UWorld* M3_019_MakeWidgetWorld(FAutomationTestBase& Test, FTestWorldWrapper& Wrapper)
	{
		if (!Test.TestTrue(TEXT("the widget test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
		{
			return nullptr;
		}
		UWorld* World = Wrapper.GetTestWorld();
		Test.TestNotNull(TEXT("the widget test world is available"), World);
		return World;
	}

	static bool M3_019_CanCaptureScreenshot()
	{
		return GEngine != nullptr && GEngine->GameViewport != nullptr && FApp::CanEverRender();
	}

	// Requests a screenshot only where rendering exists; creates the target
	// directory first (FScreenshotRequest does not create directories). The
	// screens are Slate/UMG layers, so the capture must include the UI
	// (bShowUI=true, the M3-011/M2-012 capture precedent).
	static void M3_019_RequestScreenshotIfPossible(const FString& AbsolutePath)
	{
		if (!M3_019_CanCaptureScreenshot())
		{
			UE_LOG(LogTemp, Display, TEXT("M3_019 screenshot skipped (no rendering): %s"), *AbsolutePath);
			return;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
		FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogTemp, Display, TEXT("M3_019 screenshot requested: %s"), *AbsolutePath);
	}

	// Latent step: requests one screenshot (the state was set before it runs).
	struct FM3_019_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM3_019_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			M3_019_RequestScreenshotIfPossible(AbsolutePath);
			return true;
		}
	};

	// Latent staging step: drives the real HUD debug exec entries (the exact
	// console path a user would drive) and logs the screen state as evidence;
	// the assertions live in the headless suites.
	struct FM3_019_ExecLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeHUD> HudPtr;
		int32 InventoryMode = -1;
		int32 ResultMode = -1;

		FM3_019_ExecLatentCommand(APrototypeHUD* InHud, int32 InInventoryMode, int32 InResultMode)
			: HudPtr(InHud)
			, InventoryMode(InInventoryMode)
			, ResultMode(InResultMode)
		{
		}

		virtual bool Update() override
		{
			APrototypeHUD* Hud = HudPtr.Get();
			if (Hud == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M3_019 exec staging skipped (hud gone)"));
				return true;
			}
			if (InventoryMode >= 0)
			{
				Hud->UEMMODebugInventory(InventoryMode);
				UE_LOG(LogTemp, Display, TEXT("M3_019 exec staging drove inventory mode %d (screenUp=%s, rows=%d)"),
					InventoryMode,
					Hud->HasInventoryScreen() ? TEXT("yes") : TEXT("no"),
					Hud->PeekInventoryViewModel().Rows.Num());
			}
			if (ResultMode >= 0)
			{
				Hud->UEMMODebugRoomResult(ResultMode);
				UE_LOG(LogTemp, Display, TEXT("M3_019 exec staging drove result mode %d (screenUp=%s, rewardValid=%s, items=%d)"),
					ResultMode,
					Hud->HasRoomResultScreen() ? TEXT("yes") : TEXT("no"),
					Hud->PeekRoomRewardViewModel().bValid ? TEXT("yes") : TEXT("no"),
					Hud->PeekRoomRewardViewModel().Items.Num());
			}
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_019;

// 1. Acceptance: the shared slot icon config resolves pairwise distinct short
//    tags and pairwise distinct engine built-in texture paths for the three
//    closed slots; the loader resolves them and fails closed on an empty or
//    unresolvable path; an out-of-enum slot resolves nothing (the "-" tag).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_019SlotIconConfigIsSharedAndDistinct,
	"UEMMO.Tasks.M3_019.SlotIconConfigIsSharedAndDistinct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_019SlotIconConfigIsSharedAndDistinct::RunTest(const FString& Parameters)
{
	const FInventorySlotIconConfig Weapon = MakeInventorySlotIconConfig(EItemSlot::Weapon);
	const FInventorySlotIconConfig Armor = MakeInventorySlotIconConfig(EItemSlot::Armor);
	const FInventorySlotIconConfig Accessory = MakeInventorySlotIconConfig(EItemSlot::Accessory);

	// The short tags are the readable fallback labels, pairwise distinct.
	TestEqual(TEXT("the weapon slot tag is WPN"), Weapon.Tag, FString(TEXT("WPN")));
	TestEqual(TEXT("the armor slot tag is ARM"), Armor.Tag, FString(TEXT("ARM")));
	TestEqual(TEXT("the accessory slot tag is ACC"), Accessory.Tag, FString(TEXT("ACC")));
	TSet<FString> Tags;
	Tags.Add(Weapon.Tag);
	Tags.Add(Armor.Tag);
	Tags.Add(Accessory.Tag);
	TestEqual(TEXT("the three slot tags are pairwise distinct"), Tags.Num(), 3);

	// The texture paths are configured, pairwise distinct and engine built-in
	// placeholder resources (no external download; the M3-H01 formal pass may
	// replace them, the config is the only place to touch).
	TestFalse(TEXT("the weapon texture path is configured"), Weapon.TexturePath.IsEmpty());
	TestFalse(TEXT("the armor texture path is configured"), Armor.TexturePath.IsEmpty());
	TestFalse(TEXT("the accessory texture path is configured"), Accessory.TexturePath.IsEmpty());
	TSet<FString> Paths;
	Paths.Add(Weapon.TexturePath);
	Paths.Add(Armor.TexturePath);
	Paths.Add(Accessory.TexturePath);
	TestEqual(TEXT("the three texture paths are pairwise distinct"), Paths.Num(), 3);
	TestTrue(TEXT("the weapon texture path names an engine built-in resource"),
		Weapon.TexturePath.StartsWith(TEXT("/Engine/")));
	TestTrue(TEXT("the armor texture path names an engine built-in resource"),
		Armor.TexturePath.StartsWith(TEXT("/Engine/")));
	TestTrue(TEXT("the accessory texture path names an engine built-in resource"),
		Accessory.TexturePath.StartsWith(TEXT("/Engine/")));

	// The loader resolves the configured engine textures (they ship with the
	// engine: no license question, always mounted in game).
	TestTrue(TEXT("the configured weapon texture resolves"),
		LoadInventoryRowIconTexture(Weapon.TexturePath) != nullptr);
	TestTrue(TEXT("the configured armor texture resolves"),
		LoadInventoryRowIconTexture(Armor.TexturePath) != nullptr);
	TestTrue(TEXT("the configured accessory texture resolves"),
		LoadInventoryRowIconTexture(Accessory.TexturePath) != nullptr);

	// The loader fails closed: empty and unresolvable paths give no texture -
	// exactly the state that flips a row to the visible short tag.
	TestTrue(TEXT("an empty path resolves no texture"),
		LoadInventoryRowIconTexture(FString()) == nullptr);
	TestTrue(TEXT("an unresolvable path resolves no texture (the fallback trigger)"),
		LoadInventoryRowIconTexture(TEXT("/Game/UEMMO/UI/Icons/T_M3_019_Missing.T_M3_019_Missing")) == nullptr);

	// An out-of-enum slot invents nothing: no texture, the readable dash tag.
	const EItemSlot OutOfEnum = static_cast<EItemSlot>(99);
	const FInventorySlotIconConfig Unknown = MakeInventorySlotIconConfig(OutOfEnum);
	TestTrue(TEXT("an out-of-enum slot configures no texture"), Unknown.TexturePath.IsEmpty());
	TestEqual(TEXT("an out-of-enum slot falls back to the dash tag"), Unknown.Tag, FString(TEXT("-")));
	return true;
}

// 2. Acceptance: the row view model carries the resolved tag/path from the
//    definition's slot (the same shared config); a missing definition or an
//    out-of-enum slot degrades to the dash tag with no icon path, while the
//    M3-011 display anchors (name / slot label / stats) stay unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_019RowViewModelCarriesIconSource,
	"UEMMO.Tasks.M3_019.RowViewModelCarriesIconSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_019RowViewModelCarriesIconSource::RunTest(const FString& Parameters)
{
	const FItemDefinition Sword = M3_019_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	const FItemDefinition Armor = M3_019_MakeDefinition(TEXT("armor_training"),
		TEXT("Training Armor"), EItemSlot::Armor, 0.0f, 3.0f, 0.0f);
	const FItemDefinition Charm = M3_019_MakeDefinition(TEXT("charm_training"),
		TEXT("Training Charm"), EItemSlot::Accessory, 0.0f, 0.0f, 20.0f);

	// Every resolved slot carries its config tag and the config texture path.
	const FInventoryRowViewModel WeaponRow = MakeInventoryRowViewModel(
		M3_019_MakeInstance(Sword, 1), &Sword, false);
	TestEqual(TEXT("the weapon row carries the WPN tag"), WeaponRow.SlotTag, FString(TEXT("WPN")));
	TestEqual(TEXT("the weapon row carries the config texture path"),
		WeaponRow.IconPath, MakeInventorySlotIconConfig(EItemSlot::Weapon).TexturePath);
	const FInventoryRowViewModel ArmorRow = MakeInventoryRowViewModel(
		M3_019_MakeInstance(Armor, 2), &Armor, false);
	TestEqual(TEXT("the armor row carries the ARM tag"), ArmorRow.SlotTag, FString(TEXT("ARM")));
	const FInventoryRowViewModel CharmRow = MakeInventoryRowViewModel(
		M3_019_MakeInstance(Charm, 3), &Charm, false);
	TestEqual(TEXT("the accessory row carries the ACC tag"), CharmRow.SlotTag, FString(TEXT("ACC")));

	// A missing definition degrades to the dash tag with no icon path (the
	// M3-011 placeholder rule; the instance's own stats still display).
	const FInventoryRowViewModel UnknownRow = MakeInventoryRowViewModel(
		M3_019_MakeInstance(Sword, 4), nullptr, false);
	TestEqual(TEXT("the missing-definition row falls back to the dash tag"),
		UnknownRow.SlotTag, FString(TEXT("-")));
	TestTrue(TEXT("the missing-definition row carries no icon path"), UnknownRow.IconPath.IsEmpty());
	TestEqual(TEXT("the missing-definition row keeps the unknown-item placeholder"),
		UnknownRow.DisplayName, FString(TEXT("<unknown item>")));

	// An out-of-enum slot invents nothing either.
	FItemDefinition Corrupted = Sword;
	Corrupted.Slot = static_cast<EItemSlot>(77);
	const FInventoryRowViewModel CorruptedRow = MakeInventoryRowViewModel(
		M3_019_MakeInstance(Corrupted, 5), &Corrupted, false);
	TestEqual(TEXT("the out-of-enum row falls back to the dash tag"),
		CorruptedRow.SlotTag, FString(TEXT("-")));
	TestTrue(TEXT("the out-of-enum row carries no icon path"), CorruptedRow.IconPath.IsEmpty());

	// The M3-011 anchors are untouched by the icon fields.
	TestEqual(TEXT("the weapon row keeps the M3-011 slot label"), WeaponRow.SlotName, FString(TEXT("Weapon")));
	TestEqual(TEXT("the weapon row keeps the M3-011 stats line"),
		WeaponRow.StatsText, FString(TEXT("Atk+5 Def+0 HP+0")));
	return true;
}

// 3. Acceptance: the real widget rows show the loaded placeholder icon; a row
//    without a resolvable icon (the unknown-definition row) falls back to the
//    VISIBLE short tag; the three shown textures stay pairwise distinct.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_019InventoryRowsShowIconWithShortTagFallback,
	"UEMMO.Tasks.M3_019.InventoryRowsShowIconWithShortTagFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_019InventoryRowsShowIconWithShortTagFallback::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_019_MakeWidgetWorld(*this, Wrapper);
	if (World == nullptr)
	{
		return true;
	}
	UInventoryWidget* Widget = CreateWidget<UInventoryWidget>(World, UInventoryWidget::StaticClass());
	if (!TestNotNull(TEXT("the native inventory widget is created"), Widget))
	{
		return true;
	}

	FItemDefinitionCatalog Catalog;
	const FItemDefinition Sword = M3_019_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	const FItemDefinition Armor = M3_019_MakeDefinition(TEXT("armor_training"),
		TEXT("Training Armor"), EItemSlot::Armor, 0.0f, 3.0f, 0.0f);
	const FItemDefinition Charm = M3_019_MakeDefinition(TEXT("charm_training"),
		TEXT("Training Charm"), EItemSlot::Accessory, 0.0f, 0.0f, 20.0f);
	Catalog.AddDefinition(Sword, nullptr);
	Catalog.AddDefinition(Armor, nullptr);
	Catalog.AddDefinition(Charm, nullptr);

	// Three resolvable rows plus one unknown-definition row (stale id): the
	// fallback row is the fourth.
	const TArray<FItemInstance> Snapshot = {
		M3_019_MakeInstance(Sword, 1),
		M3_019_MakeInstance(Armor, 2),
		M3_019_MakeInstance(Charm, 3),
		M3_019_MakeInstance(M3_019_MakeDefinition(TEXT("ghost_item"), TEXT("Ghost Item"), EItemSlot::Weapon, 1.0f, 0.0f, 0.0f), 4)
	};
	Widget->BindInventory(Snapshot, &Catalog, TSet<FGuid>());
	TestEqual(TEXT("the bind built four rows"), Widget->PeekRowCount(), 4);

	// The three resolved rows each show a LOADED texture (no purple/white
	// missing-resource block: the brush resource object is the real texture)
	// and the three textures are pairwise distinct - the icon alone tells the
	// kinds apart.
	UTexture2D* Loaded[3] = { nullptr, nullptr, nullptr };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		UImage* Icon = Widget->PeekRowIconImage(Index);
		if (!TestNotNull(TEXT("the resolved row owns the icon image control"), Icon))
		{
			return true;
		}
		const UObject* Resource = Icon->GetBrush().GetResourceObject();
		if (!TestTrue(TEXT("the resolved row's icon brush carries a texture"), Resource != nullptr))
		{
			return true;
		}
		Loaded[Index] = const_cast<UTexture2D*>(Cast<UTexture2D>(Resource));
		if (!TestNotNull(TEXT("the icon brush resource is a texture"), Loaded[Index]))
		{
			return true;
		}
		TestTrue(TEXT("the resolved row hides the fallback tag"),
			Widget->PeekRowIconTagText(Index) == nullptr
			|| Widget->PeekRowIconTagText(Index)->GetVisibility() == ESlateVisibility::Collapsed);
	}
	TestTrue(TEXT("the weapon and armor icons are distinct textures"),
		Loaded[0] != nullptr && Loaded[1] != nullptr && Loaded[0] != Loaded[1]);
	TestTrue(TEXT("the accessory icon differs from both others"),
		Loaded[2] != nullptr && Loaded[2] != Loaded[0] && Loaded[2] != Loaded[1]);

	// The unknown-definition row: the icon side stays collapsed and the short
	// tag "-" becomes visible (the tested fallback path).
	UImage* FallbackIcon = Widget->PeekRowIconImage(3);
	if (!TestNotNull(TEXT("the fallback row still owns the icon image control"), FallbackIcon))
	{
		return true;
	}
	TestTrue(TEXT("the fallback row's icon side is collapsed"),
		FallbackIcon->GetVisibility() == ESlateVisibility::Collapsed);
	UTextBlock* FallbackTag = Widget->PeekRowIconTagText(3);
	if (!TestNotNull(TEXT("the fallback row owns the tag text control"), FallbackTag))
	{
		return true;
	}
	TestTrue(TEXT("the fallback row shows the short tag"),
		FallbackTag->GetVisibility() != ESlateVisibility::Collapsed);
	TestEqual(TEXT("the fallback row's tag is the dash placeholder"),
		FallbackTag->GetText().ToString(), FString(TEXT("-")));
	return true;
}

// 4. Acceptance: the settlement reward lines carry the SAME tag from the SAME
//    shared config (one source, two surfaces); the reward text prefixes each
//    item line with its tag; an unknown draft item degrades to "[-]".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_019SettlementLineSharesTheIconTag,
	"UEMMO.Tasks.M3_019.SettlementLineSharesTheIconTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_019SettlementLineSharesTheIconTag::RunTest(const FString& Parameters)
{
	FItemDefinitionCatalog Catalog;
	const FItemDefinition Sword = M3_019_MakeDefinition(TEXT("weapon_training"),
		TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
	const FItemDefinition Armor = M3_019_MakeDefinition(TEXT("armor_training"),
		TEXT("Training Armor"), EItemSlot::Armor, 0.0f, 3.0f, 0.0f);
	Catalog.AddDefinition(Sword, nullptr);
	Catalog.AddDefinition(Armor, nullptr);

	FPendingReward Draft;
	Draft.SettlementId = 9001;
	Draft.XP = 50;
	Draft.Items.Add(M3_019_MakeInstance(Sword, 1));
	Draft.Items.Add(M3_019_MakeInstance(Armor, 2));

	const FRoomRewardViewModel ViewModel = MakeRoomRewardViewModelFromDraft(Draft, &Catalog);
	TestTrue(TEXT("the reward view model is valid"), ViewModel.bValid);
	TestEqual(TEXT("the view model carries both draft items"), ViewModel.Items.Num(), 2);
	TestEqual(TEXT("the weapon line carries the WPN tag"),
		ViewModel.Items[0].SlotTag, MakeInventorySlotIconConfig(EItemSlot::Weapon).Tag);
	TestEqual(TEXT("the armor line carries the ARM tag"),
		ViewModel.Items[1].SlotTag, MakeInventorySlotIconConfig(EItemSlot::Armor).Tag);
	TestTrue(TEXT("the reward text prefixes the weapon line with its tag"),
		ViewModel.RewardText.Contains(TEXT("[WPN] Training Sword")));
	TestTrue(TEXT("the reward text prefixes the armor line with its tag"),
		ViewModel.RewardText.Contains(TEXT("[ARM] Training Armor")));
	TestTrue(TEXT("the reward text still names the XP"), ViewModel.RewardText.Contains(TEXT("XP 50")));

	// An unknown draft item degrades to the dash tag prefix (nothing invented).
	FPendingReward GhostDraft;
	GhostDraft.SettlementId = 9002;
	GhostDraft.XP = 50;
	GhostDraft.Items.Add(M3_019_MakeInstance(
		M3_019_MakeDefinition(TEXT("ghost_item"), TEXT("Ghost Item"), EItemSlot::Weapon, 1.0f, 0.0f, 0.0f), 9));
	const FRoomRewardViewModel GhostViewModel = MakeRoomRewardViewModelFromDraft(GhostDraft, nullptr);
	TestEqual(TEXT("the unknown item line falls back to the dash tag"),
		GhostViewModel.Items[0].SlotTag, FString(TEXT("-")));
	TestTrue(TEXT("the unknown item line keeps the placeholder name"),
		GhostViewModel.RewardText.Contains(TEXT("[-] <unknown item>")));
	return true;
}

// 5. Capture companion (the M3-011/M2-012 precedent): in a real game world
//    with a viewport, drive the REAL HUD exec paths - the staged starter
//    inventory (three resolvable rows) and the victory settlement (the reward
//    area with the tagged item lines) - one screenshot per surface. Headless
//    -nullrhi automation runs skip the capture gracefully.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_019InventoryIconScreenshots,
	"UEMMO.Tasks.M3_019.InventoryIconScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_019InventoryIconScreenshots::RunTest(const FString& Parameters)
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
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M3-019"));

	// The inventory first: the debug exec stages the profile + starter items
	// (also the reward draft's prerequisite) and presents the three rows.
	ADD_LATENT_AUTOMATION_COMMAND(FM3_019_ExecLatentCommand(Hud, 1, -1));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_019_ScreenshotLatentCommand(Directory / TEXT("inventory-icons.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));

	// Close the inventory (the game input returns), then present the victory
	// settlement through the real session terminal entry - the reward area
	// binds the profile's pending draft (the same staged profile).
	ADD_LATENT_AUTOMATION_COMMAND(FM3_019_ExecLatentCommand(Hud, 0, -1));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_019_ExecLatentCommand(Hud, -1, 1));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_019_ScreenshotLatentCommand(Directory / TEXT("settlement-icons.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));

	// Close the settlement (the game input returns).
	ADD_LATENT_AUTOMATION_COMMAND(FM3_019_ExecLatentCommand(Hud, -1, 0));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	return true;
}

#endif
