#include "InventoryWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/SlateWrapperTypes.h"
#include "Engine/Texture2D.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"
#include "UObject/SoftObjectPtr.h"

#include "WeaponStatusWidget.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemInstance.h"
#include "../Items/StatCalculator.h"

namespace
{
    // M3-011: display constants of the read-only inventory panel (pure display
    // values; the M2-012 result panel palette is reused for coherence).
    const TCHAR* M3_011_UnknownItemName = TEXT("<unknown item>");
    const TCHAR* M3_011_UnknownSlotLabel = TEXT("-");
    const TCHAR* M3_011_EquippedPrefix = TEXT("[Equipped] ");
    const TCHAR* M3_011_TitleText = TEXT("Inventory");
    const TCHAR* M3_011_EmptyText = TEXT("Inventory is empty");
    const TCHAR* M3_011_CloseText = TEXT("Close (Esc)");

    // M3-012: display constants of the equip panel (preview / status / buttons).
    const TCHAR* M3_012_EquipText = TEXT("Equip");
    const TCHAR* M3_012_UnequipText = TEXT("Unequip");
    const TCHAR* M3_012_NoSelectionHint = TEXT("Select an item to compare stats");
    const TCHAR* M3_012_NoChangeText = TEXT("No change");
    const TCHAR* M3_012_SelectFirstText = TEXT("Select an item first");

    // Formats one attribute as "Atk+5" / "Def+0" / "HP+12". Integral values
    // lose the ".0" tail (the card's "Atk+5 Def+0 HP+0" line), fractions keep
    // their digits; a non-finite value renders as 0 (never "nan"/"inf"), a
    // negative stays visible (validated data never produces one, a
    // hand-corrupted instance must not silently hide its corruption).
    FString M3_011_FormatStat(const TCHAR* Label, float Value)
    {
        if (!FMath::IsFinite(Value))
        {
            Value = 0.0f;
        }
        const float Rounded = FMath::RoundToFloat(Value);
        const FString Number = FMath::IsNearlyEqual(Value, Rounded, 0.0001f)
            ? FString::Printf(TEXT("%d"), static_cast<int32>(Rounded))
            : FString::SanitizeFloat(Value);
        return FString::Printf(TEXT("%s+%s"), Label, *Number);
    }

    // M3-012: formats one DELTA attribute with an explicit sign ("+5"/"-2"/
    // "+0"). A non-finite delta renders as +0 (the recalculation hardens its
    // inputs, so this only guards a hand-poisoned caller).
    FString M3_012_FormatSigned(float Value)
    {
        if (!FMath::IsFinite(Value))
        {
            return TEXT("+0");
        }
        if (FMath::IsNearlyEqual(Value, 0.0f, 0.0001f))
        {
            return TEXT("+0");
        }
        if (Value > 0.0f)
        {
            return FString::Printf(TEXT("+%s"), *M3_011_FormatStat(TEXT(""), Value).Mid(1));
        }
        return FString::Printf(TEXT("-%s"), *M3_011_FormatStat(TEXT(""), -Value).Mid(1));
    }

    // M3-012: one full stats row "Atk+<a> Def+<d> HP+<h>" (the M3-011 line).
    FString M3_012_FormatStatsRow(const FItemStats& Stats)
    {
        return FString::Printf(TEXT("%s %s %s"),
            *M3_011_FormatStat(TEXT("Atk"), Stats.Attack),
            *M3_011_FormatStat(TEXT("Def"), Stats.Defense),
            *M3_011_FormatStat(TEXT("HP"), Stats.MaxHP));
    }

    // Slot label of one definition; only the three closed enum values get a
    // label, everything else degrades to "-" (no invented slot).
    const TCHAR* M3_011_SlotLabel(EItemSlot Slot)
    {
        switch (Slot)
        {
        case EItemSlot::Weapon: return TEXT("Weapon");
        case EItemSlot::Armor: return TEXT("Armor");
        case EItemSlot::Accessory: return TEXT("Accessory");
        default: return M3_011_UnknownSlotLabel;
        }
    }
}

// ----- M3-019: the shared icon config ----------------------------------------

namespace
{
	// M3-019: the ONE shared icon table (the inventory rows and the settlement
	// reward lines both resolve through it). All three textures are ENGINE
	// BUILT-IN placeholder resources (no external asset, no download, no
	// license question; the registration and the placeholder status live in
	// SourceAssets/manifest.json and Docs/03) - a formal icon pass belongs to
	// M3-H01 and only touches this table. Engine content is always mounted in
	// game, so the paths resolve on every target.
	const TCHAR* M3_019_WeaponIconPath = TEXT("/Engine/EngineResources/AICON-Red.AICON-Red");
	const TCHAR* M3_019_ArmorIconPath = TEXT("/Engine/EngineResources/AICON-Green.AICON-Green");
	const TCHAR* M3_019_AccessoryIconPath = TEXT("/Engine/EngineResources/GradientTexture0.GradientTexture0");
	const TCHAR* M3_019_WeaponTag = TEXT("WPN");
	const TCHAR* M3_019_ArmorTag = TEXT("ARM");
	const TCHAR* M3_019_AccessoryTag = TEXT("ACC");
}

FInventorySlotIconConfig MakeInventorySlotIconConfig(EItemSlot Slot)
{
	// Only the three closed enum values resolve an icon; every other value
	// degrades to an empty path and the readable dash tag (no slot invented).
	FInventorySlotIconConfig Config;
	switch (Slot)
	{
	case EItemSlot::Weapon:
		Config.TexturePath = M3_019_WeaponIconPath;
		Config.Tag = M3_019_WeaponTag;
		break;
	case EItemSlot::Armor:
		Config.TexturePath = M3_019_ArmorIconPath;
		Config.Tag = M3_019_ArmorTag;
		break;
	case EItemSlot::Accessory:
		Config.TexturePath = M3_019_AccessoryIconPath;
		Config.Tag = M3_019_AccessoryTag;
		break;
	default:
		Config.TexturePath.Reset();
		Config.Tag = M3_011_UnknownSlotLabel;
		break;
	}
	return Config;
}

UTexture2D* LoadInventoryRowIconTexture(const FString& SoftObjectPath)
{
	if (SoftObjectPath.IsEmpty())
	{
		return nullptr;
	}
	// Synchronous load on purpose: the config points at tiny engine built-in
	// textures, the row build is the only caller and a missing texture must
	// degrade to the visible short tag within the same row build (no pop-in).
	// An already-resolved object short-circuits the load.
	const FSoftObjectPath Path(SoftObjectPath);
	if (UTexture2D* Resolved = Cast<UTexture2D>(Path.ResolveObject()))
	{
		return Resolved;
	}
	return Cast<UTexture2D>(Path.TryLoad());
}

// ----- M3-011: pure view model -----------------------------------------------

FInventoryRowViewModel MakeInventoryRowViewModel(
	const FItemInstance& Instance, const FItemDefinition* Definition, bool bEquipped)
{
	FInventoryRowViewModel Row;
	Row.InstanceId = Instance.InstanceId;
	Row.bEquipped = bEquipped;

	// Display name: only the definition knows the kind's human label; a
	// missing/unknown definition degrades to the readable placeholder.
	Row.DisplayName = (Definition != nullptr) ? Definition->DisplayName : M3_011_UnknownItemName;

	// Slot: definition data only; missing definition or an out-of-enum slot
	// value shows "-" instead of an invented label.
	Row.SlotName = (Definition != nullptr) ? M3_011_SlotLabel(Definition->Slot) : M3_011_UnknownSlotLabel;

	// M3-019: the icon source resolves from the SLOT through the ONE shared
	// config (the settlement reward lines read the same fields via the same
	// row view model). A missing definition degrades to the dash tag with no
	// icon path; an out-of-enum slot is degraded by the config itself.
	Row.SlotTag = M3_011_UnknownSlotLabel;
	if (Definition != nullptr)
	{
		const FInventorySlotIconConfig Icon = MakeInventorySlotIconConfig(Definition->Slot);
		Row.SlotTag = Icon.Tag;
		Row.IconPath = Icon.TexturePath;
	}

	// Stats: the INSTANCE's rolled stats are owned data and display even when
	// the definition is stale; all three attributes show (zeros included).
	Row.StatsText = FString::Printf(TEXT("%s %s %s"),
		*M3_011_FormatStat(TEXT("Atk"), Instance.RolledStats.Attack),
		*M3_011_FormatStat(TEXT("Def"), Instance.RolledStats.Defense),
		*M3_011_FormatStat(TEXT("HP"), Instance.RolledStats.MaxHP));
	return Row;
}

FInventoryListViewModel MakeInventoryListViewModel(
	const TArray<FItemInstance>& Instances, const FItemDefinitionCatalog* Catalog,
	const TSet<FGuid>& EquippedInstanceIds)
{
	FInventoryListViewModel ViewModel;
	ViewModel.bValid = true;

	// Hard cap at the design capacity (defensive: the inventory itself can
	// never store more, but a caller bug must not build an unbounded list).
	const int32 RowCount = FMath::Min(Instances.Num(), FInventoryModel::Capacity);
	ViewModel.Rows.Reserve(RowCount);
	for (int32 Index = 0; Index < RowCount; ++Index)
	{
		const FItemInstance& Instance = Instances[Index];
		const FItemDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(Instance.DefinitionId) : nullptr;
		ViewModel.Rows.Add(MakeInventoryRowViewModel(Instance, Definition,
			EquippedInstanceIds.Contains(Instance.InstanceId)));
	}
	return ViewModel;
}

FString MakeInventorySnapshotFingerprint(
	const TArray<FItemInstance>& Instances, const TSet<FGuid>& EquippedInstanceIds)
{
	// Deterministic text of everything the render depends on (count, per-item
	// id/definition/level/stats and the equipped flag). Catalog CONTENT changes
	// for a known id are not captured (the id is): today no runtime path
	// mutates a registered definition, and the card's change points are
	// inventory/equipment changes.
	FString Fingerprint = FString::Printf(TEXT("n=%d"), Instances.Num());
	for (const FItemInstance& Instance : Instances)
	{
		const int32 bEquipped = EquippedInstanceIds.Contains(Instance.InstanceId) ? 1 : 0;
		Fingerprint += FString::Printf(TEXT("|i=%s;d=%s;l=%d;a=%s;s=%s;h=%s;e=%d"),
			*Instance.InstanceId.ToString(),
			*Instance.DefinitionId.ToString(),
			Instance.Level,
			*M3_011_FormatStat(TEXT(""), Instance.RolledStats.Attack),
			*M3_011_FormatStat(TEXT(""), Instance.RolledStats.Defense),
			*M3_011_FormatStat(TEXT(""), Instance.RolledStats.MaxHP),
			bEquipped);
	}
	return Fingerprint;
}

bool FInventoryRefreshGuard::AcceptSnapshot(const FString& SnapshotFingerprint)
{
	if (bHasLastSnapshot && LastFingerprint.Equals(SnapshotFingerprint))
	{
		// Identical snapshot: the render would not change, so the rebuild is
		// skipped (the "no per-Tick full rebuild" rule).
		++SkippedCount;
		return false;
	}
	LastFingerprint = SnapshotFingerprint;
	bHasLastSnapshot = true;
	++PerformedCount;
	return true;
}

// ----- M3-012: pure preview and result texts ---------------------------------

FInventoryStatPreviewViewModel MakeInventoryStatPreviewViewModel(
	const FItemStats& BaseStats, const TArray<FItemStats>& EquippedStats,
	bool bHasSelection, const FItemStats& SelectedStats, bool bSelectedAlreadyEquipped,
	bool bSlotOccupied, const FItemStats& ReplacedStats)
{
	FInventoryStatPreviewViewModel Preview;
	Preview.bValid = true;
	Preview.bHasSelection = bHasSelection;

	// Current: the COMPLETE recalculation over the level base plus every
	// equipped row (the interface contract section 8 rule - never incremental
	// add/remove bookkeeping, so repeated previews can never drift).
	const FItemStats CurrentFinal = FStatCalculator::Recalculate(BaseStats, EquippedStats);

	// After: the SAME complete recalculation with the selection swapped into
	// its slot. An already-equipped selection is the idempotent no-op (the
	// real Equip's AlreadyEquipped result changes nothing), so the equipped
	// rows stay untouched. Otherwise the occupant's row leaves (removed once,
	// exactly what the replacement does to the mapping) and the selection's
	// row enters.
	TArray<FItemStats> AfterEquipped = EquippedStats;
	if (bHasSelection && !bSelectedAlreadyEquipped)
	{
		if (bSlotOccupied)
		{
			for (int32 Index = 0; Index < AfterEquipped.Num(); ++Index)
			{
				const FItemStats& Candidate = AfterEquipped[Index];
				if (Candidate.Attack == ReplacedStats.Attack
					&& Candidate.Defense == ReplacedStats.Defense
					&& Candidate.MaxHP == ReplacedStats.MaxHP)
				{
					AfterEquipped.RemoveAt(Index);
					break;
				}
			}
		}
		AfterEquipped.Add(SelectedStats);
	}
	const FItemStats AfterFinal = FStatCalculator::Recalculate(BaseStats, AfterEquipped);

	if (bHasSelection)
	{
		Preview.CurrentLine = FString::Printf(TEXT("Current: %s"), *M3_012_FormatStatsRow(CurrentFinal));
		Preview.AfterLine = FString::Printf(TEXT("After equip: %s"), *M3_012_FormatStatsRow(AfterFinal));
		const float DeltaAttack = AfterFinal.Attack - CurrentFinal.Attack;
		const float DeltaDefense = AfterFinal.Defense - CurrentFinal.Defense;
		const float DeltaMaxHP = AfterFinal.MaxHP - CurrentFinal.MaxHP;
		const bool bNoChange = FMath::IsNearlyEqual(DeltaAttack, 0.0f, 0.0001f)
			&& FMath::IsNearlyEqual(DeltaDefense, 0.0f, 0.0001f)
			&& FMath::IsNearlyEqual(DeltaMaxHP, 0.0f, 0.0001f);
		Preview.DeltaLine = bNoChange
			? FString::Printf(TEXT("Delta: %s"), M3_012_NoChangeText)
			: FString::Printf(TEXT("Delta: Atk%s Def%s HP%s"),
				*M3_012_FormatSigned(DeltaAttack),
				*M3_012_FormatSigned(DeltaDefense),
				*M3_012_FormatSigned(DeltaMaxHP));
	}
	return Preview;
}

FString MakeEquipResultText(EEquipmentEquipResult Result)
{
	switch (Result)
	{
	case EEquipmentEquipResult::Equipped: return TEXT("Equipped");
	case EEquipmentEquipResult::AlreadyEquipped: return TEXT("Already equipped");
	case EEquipmentEquipResult::NotInInventory: return TEXT("Item not in inventory");
	case EEquipmentEquipResult::SlotMismatch: return TEXT("Item does not fit that slot");
	case EEquipmentEquipResult::InvalidSlot: return TEXT("Invalid equipment slot");
	case EEquipmentEquipResult::MissingDefinitions: return TEXT("Item definitions unavailable");
	default: return TEXT("Unknown equip result");
	}
}

FString MakeUnequipResultText(EEquipmentUnequipResult Result)
{
	switch (Result)
	{
	case EEquipmentUnequipResult::Unequipped: return TEXT("Unequipped");
	case EEquipmentUnequipResult::NotEquipped: return TEXT("Nothing was equipped in that slot");
	case EEquipmentUnequipResult::InvalidSlot: return TEXT("Invalid equipment slot");
	default: return TEXT("Unknown unequip result");
	}
}

FString MakeInventoryEquipBlockText(bool bMissingProfile, bool bSessionRunning,
	bool bMissingPlayer)
{
	// Priority: a missing profile blocks everything; a Running room run is
	// the card's first-version rule (exit the room first); the pawn entry is
	// the production push surface, so without it nothing can be pushed.
	if (bMissingProfile)
	{
		return TEXT("No character profile exists yet");
	}
	if (bSessionRunning)
	{
		return TEXT("Blocked while the room run is running - leave the room first");
	}
	if (bMissingPlayer)
	{
		return TEXT("The equip entry is unavailable (no player character)");
	}
	return FString();
}

// ----- M3-012: the native inventory equip panel --------------------------------

UInventoryRowButton::UInventoryRowButton()
	: Super()
{
}

void UInventoryRowButton::HandleRowClicked()
{
	// The per-row bridge: the dynamic click carries no payload, the button
	// object itself owns the id it displays.
	OnRowSelected.ExecuteIfBound(InstanceId);
}

// ----- M3-011: the native inventory screen widget -----------------------------

UInventoryWidget::UInventoryWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// The screen takes the UI focus when the HUD captures the input
	// (FInputModeUIOnly::SetWidgetToFocus requires a focusable widget) and
	// only then can see the Esc key that leaves it.
	bIsFocusable = true;
}

void UInventoryWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildControls();
}

FReply UInventoryWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		// The focus exit: the widget only broadcasts; the HUD performs the
		// real dismissal and restores the game input focus.
		CloseRequested.Broadcast();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UInventoryWidget::BindInventory(const TArray<FItemInstance>& Instances,
	const FItemDefinitionCatalog* Catalog, const TSet<FGuid>& EquippedInstanceIds)
{
	// A bind is an unconditional (re)fill; it also records the refresh
	// baseline, so a following identical RefreshIfChanged is skipped.
	RefreshGuard.AcceptSnapshot(MakeInventorySnapshotFingerprint(Instances, EquippedInstanceIds));
	ViewModel = MakeInventoryListViewModel(Instances, Catalog, EquippedInstanceIds);
	// M3-012: keep the copies the preview draws from (plain display data).
	BoundInstances = Instances;
	BoundCatalog = Catalog;
	BoundEquippedIds = EquippedInstanceIds;
	BuildControls(); // safety net for a fill before Initialize built the tree
	ApplyViewModelToControls();
	RefreshEquipPreviewAndActions();
}

bool UInventoryWidget::RefreshIfChanged(const TArray<FItemInstance>& Instances,
	const FItemDefinitionCatalog* Catalog, const TSet<FGuid>& EquippedInstanceIds)
{
	// Throttled refresh: the fingerprint gate decides; an identical snapshot
	// leaves the rows (and the controls) completely untouched.
	if (!RefreshGuard.AcceptSnapshot(MakeInventorySnapshotFingerprint(Instances, EquippedInstanceIds)))
	{
		return false;
	}
	ViewModel = MakeInventoryListViewModel(Instances, Catalog, EquippedInstanceIds);
	BoundInstances = Instances;
	BoundCatalog = Catalog;
	BoundEquippedIds = EquippedInstanceIds;
	BuildControls();
	ApplyViewModelToControls();
	RefreshEquipPreviewAndActions();
	return true;
}

int32 UInventoryWidget::PeekRowCount() const
{
	return (bControlsBuilt && RowsBox != nullptr) ? RowsBox->GetChildrenCount() : 0;
}

UButton* UInventoryWidget::PeekRowButton(int32 Index) const
{
	if (!bControlsBuilt || RowsBox == nullptr || Index < 0 || Index >= RowsBox->GetChildrenCount())
	{
		return nullptr;
	}
	return Cast<UButton>(RowsBox->GetChildAt(Index));
}

UImage* UInventoryWidget::PeekRowIconImage(int32 Index) const
{
	// The parallel arrays fill while the rows rebuild; a peek before the first
	// build (or out of range) misses.
	return (Index >= 0 && Index < RowIconImages.Num()) ? RowIconImages[Index].Get() : nullptr;
}

UTextBlock* UInventoryWidget::PeekRowIconTagText(int32 Index) const
{
	return (Index >= 0 && Index < RowIconTags.Num()) ? RowIconTags[Index].Get() : nullptr;
}

void UInventoryWidget::SetEquipContext(bool bHasProfile, const FItemStats& BaseStats,
	bool bNewSessionRunning)
{
	bHasEquipContext = bHasProfile;
	EquipBaseStats = bHasProfile ? BaseStats : FItemStats();
	bSessionRunning = bNewSessionRunning;
	BuildControls(); // safety net for a context pushed before the tree exists
	RefreshEquipPreviewAndActions();
}

void UInventoryWidget::SetInventoryStatusText(const FString& Text)
{
	BuildControls(); // safety net for a call before Initialize built the tree
	if (!bControlsBuilt)
	{
		return;
	}
	StatusBlock->SetText(FText::FromString(Text));
}

void UInventoryWidget::HandleRowSelected(const FGuid& InstanceId)
{
	// Every row click is an explicit intent (re-clicking the same row too):
	// the selection updates, the preview recomputes and the HUD re-arms its
	// one-shot action guard through SelectionChanged.
	SelectedInstanceId = InstanceId;
	bHasSelection = true;
	RefreshEquipPreviewAndActions();
	SelectionChanged.Broadcast();
}

void UInventoryWidget::HandleEquipClicked()
{
	// The visible anti-double-click half (the M2-012 pattern): the first
	// click disables both action buttons; the HUD's guard is the second half
	// and its post-action refresh re-enables them (or keeps them disabled
	// while the context says so).
	if (bControlsBuilt)
	{
		EquipButton->SetIsEnabled(false);
		UnequipButton->SetIsEnabled(false);
	}
	EquipRequested.Broadcast();
}

void UInventoryWidget::HandleUnequipClicked()
{
	if (bControlsBuilt)
	{
		EquipButton->SetIsEnabled(false);
		UnequipButton->SetIsEnabled(false);
	}
	UnequipRequested.Broadcast();
}

void UInventoryWidget::UpdateEquipActionButtons()
{
	if (!bControlsBuilt)
	{
		return;
	}
	// Enabled only with a full context: a profile to write into, no Running
	// room run (the two-layer gate's visible half) and a selected item.
	const bool bEnabled = bHasEquipContext && !bSessionRunning && bHasSelection;
	EquipButton->SetIsEnabled(bEnabled);
	UnequipButton->SetIsEnabled(bEnabled);
}

void UInventoryWidget::RefreshEquipPreviewAndActions()
{
	BuildControls(); // safety net for a call before Initialize built the tree
	if (!bControlsBuilt)
	{
		return;
	}

	// Pure inputs, all from the bound snapshot copies (no profile access):
	// the per-equipped-instance stats rows in insertion order, the selected
	// instance and the occupant of its slot.
	TArray<FItemStats> EquippedStats;
	const FItemInstance* Selected = nullptr;
	const FItemInstance* Occupant = nullptr;
	EItemSlot SelectedSlot = EItemSlot::Weapon;
	bool bSelectedSlotKnown = false;
	for (const FItemInstance& Instance : BoundInstances)
	{
		if (BoundEquippedIds.Contains(Instance.InstanceId))
		{
			EquippedStats.Add(Instance.RolledStats);
		}
	}
	if (bHasSelection)
	{
		for (const FItemInstance& Instance : BoundInstances)
		{
			if (Instance.InstanceId == SelectedInstanceId)
			{
				Selected = &Instance;
				break;
			}
		}
	}
	if (Selected != nullptr && BoundCatalog != nullptr)
	{
		if (const FItemDefinition* SelectedDefinition = BoundCatalog->Find(Selected->DefinitionId))
		{
			SelectedSlot = SelectedDefinition->Slot;
			bSelectedSlotKnown = IsValidItemSlot(SelectedSlot);
		}
	}
	if (Selected != nullptr && bSelectedSlotKnown)
	{
		for (const FItemInstance& Instance : BoundInstances)
		{
			if (Instance.InstanceId == Selected->InstanceId
				|| !BoundEquippedIds.Contains(Instance.InstanceId))
			{
				continue;
			}
			const FItemDefinition* Definition = BoundCatalog ? BoundCatalog->Find(Instance.DefinitionId) : nullptr;
			if (Definition != nullptr && Definition->Slot == SelectedSlot)
			{
				Occupant = &Instance;
				break;
			}
		}
	}

	Preview = MakeInventoryStatPreviewViewModel(EquipBaseStats, EquippedStats,
		Selected != nullptr, Selected != nullptr ? Selected->RolledStats : FItemStats(),
		Selected != nullptr && BoundEquippedIds.Contains(Selected->InstanceId),
		Occupant != nullptr, Occupant != nullptr ? Occupant->RolledStats : FItemStats());

	// Preview line: the three comparison lines, or the explicit select hint.
	if (Preview.bHasSelection)
	{
		PreviewBlock->SetText(FText::FromString(FString::Printf(TEXT("%s\n%s\n%s"),
			*Preview.CurrentLine, *Preview.AfterLine, *Preview.DeltaLine)));
	}
	else
	{
		PreviewBlock->SetText(FText::FromString(M3_012_NoSelectionHint));
	}

	UpdateEquipActionButtons();
}

void UInventoryWidget::BuildControls()
{
	if (bControlsBuilt || WidgetTree == nullptr)
	{
		return;
	}

	// Centered fixed-size panel: canvas root -> dark border panel -> vertical
	// stack (title, empty state, scrollable rows, close button). Everything is
	// a code-built native control: no UMG asset is involved.
	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
	PanelBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	PanelBorder->SetBrushColor(FLinearColor(0.02f, 0.03f, 0.05f, 0.88f));
	PanelBorder->SetPadding(FMargin(20.0f, 14.0f));
	UCanvasPanelSlot* BorderSlot = RootCanvas->AddChildToCanvas(PanelBorder);
	if (BorderSlot != nullptr)
	{
		BorderSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		BorderSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		BorderSlot->SetAutoSize(false);
		BorderSlot->SetSize(FVector2D(560.0f, 520.0f));
	}

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	PanelBorder->SetContent(Stack);

	TitleBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	TitleBlock->SetText(FText::FromString(M3_011_TitleText));
	TitleBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 22));
	TitleBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.85f, 0.3f, 1.0f)));
	TitleBlock->SetJustification(ETextJustify::Center);

	EmptyBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	EmptyBlock->SetText(FText::FromString(M3_011_EmptyText));
	EmptyBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.65f, 0.7f, 1.0f)));
	EmptyBlock->SetJustification(ETextJustify::Center);
	// Hidden by default; ApplyViewModelToControls shows it for an empty list.
	EmptyBlock->SetVisibility(ESlateVisibility::Collapsed);

	// The scrollable list: a scroll box holding one vertical box of row
	// buttons (30 rows at the design capacity never fit the panel at once).
	ListScrollBox = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	RowsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	ListScrollBox->AddChild(RowsBox);

	// M3-012: the equip panel - comparison preview, status line and the
	// Equip/Unequip action pair. The buttons only broadcast; the HUD owns the
	// real equipment model, the one-shot guard and the Running gate.
	PreviewBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	PreviewBlock->SetText(FText::FromString(M3_012_NoSelectionHint));
	PreviewBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12));
	PreviewBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.9f, 1.0f, 1.0f)));
	PreviewBlock->SetJustification(ETextJustify::Left);

	StatusBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	StatusBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12));
	StatusBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.95f, 0.6f, 1.0f)));
	StatusBlock->SetJustification(ETextJustify::Left);

	EquipButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	EquipLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	EquipLabel->SetText(FText::FromString(M3_012_EquipText));
	EquipButton->AddChild(EquipLabel);
	EquipButton->OnClicked.AddDynamic(this, &UInventoryWidget::HandleEquipClicked);

	UnequipButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	UnequipLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	UnequipLabel->SetText(FText::FromString(M3_012_UnequipText));
	UnequipButton->AddChild(UnequipLabel);
	UnequipButton->OnClicked.AddDynamic(this, &UInventoryWidget::HandleUnequipClicked);

	CloseButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	CloseLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	CloseLabel->SetText(FText::FromString(M3_011_CloseText));
	CloseButton->AddChild(CloseLabel);
	CloseButton->OnClicked.AddDynamic(this, &UInventoryWidget::HandleCloseClicked);

	Stack->AddChildToVerticalBox(TitleBlock);
	Stack->AddChildToVerticalBox(EmptyBlock);
	if (UVerticalBoxSlot* ScrollSlot = Stack->AddChildToVerticalBox(ListScrollBox))
	{
		// The list takes all remaining panel height; the rows scroll inside.
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 10.0f));
	}
	if (UVerticalBoxSlot* PreviewSlot = Stack->AddChildToVerticalBox(PreviewBlock))
	{
		PreviewSlot->SetPadding(FMargin(4.0f, 4.0f));
	}
	if (UVerticalBoxSlot* StatusSlot = Stack->AddChildToVerticalBox(StatusBlock))
	{
		StatusSlot->SetPadding(FMargin(4.0f, 2.0f));
	}
	UHorizontalBox* ActionButtons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UHorizontalBoxSlot* EquipSlot = ActionButtons->AddChildToHorizontalBox(EquipButton))
	{
		EquipSlot->SetPadding(FMargin(14.0f, 4.0f));
	}
	if (UHorizontalBoxSlot* UnequipSlot = ActionButtons->AddChildToHorizontalBox(UnequipButton))
	{
		UnequipSlot->SetPadding(FMargin(14.0f, 4.0f));
	}
	Stack->AddChildToVerticalBox(ActionButtons);
	if (UVerticalBoxSlot* CloseSlot = Stack->AddChildToVerticalBox(CloseButton))
	{
		CloseSlot->SetPadding(FMargin(120.0f, 4.0f));
	}

	// M5-034: the embedded weapon status panel sits between the equip panel
	// and the close row. It is display-only (the HUD pushes the REAL mount
	// state through the panel's refresh entry); the screen's own Close/Esc
	// path dismisses the whole presentation, so the panel adds no second
	// input-focus switch and no second dismissal.
	WeaponStatus = WidgetTree->ConstructWidget<UWeaponStatusWidget>(UWeaponStatusWidget::StaticClass());
	if (UVerticalBoxSlot* StatusSlot = Stack->AddChildToVerticalBox(WeaponStatus))
	{
		StatusSlot->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
	}

	WidgetTree->RootWidget = RootCanvas;
	bControlsBuilt = true;
	ApplyViewModelToControls();
	UpdateEquipActionButtons();
}

void UInventoryWidget::ApplyViewModelToControls()
{
	if (!bControlsBuilt)
	{
		return;
	}

	// Empty state: the explicit line replaces the list for an empty inventory.
	EmptyBlock->SetVisibility(ViewModel.IsEmpty() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);

	// Rebuild the row buttons (ClearChildren drops the previous row controls;
	// this runs only on a real snapshot change - the fingerprint gate above).
	// M3-012: each row is a clickable button carrying its instance id; the
	// dynamic click bridges into the row's native select event (per-row ids
	// cannot bind as dynamic-delegate lambdas).
	// M3-019: each row also carries an icon area (a fixed-size image plus the
	// short-tag text); the parallel control arrays are rebuilt with the rows.
	RowsBox->ClearChildren();
	RowIconImages.Reset();
	RowIconTags.Reset();
	for (const FInventoryRowViewModel& Row : ViewModel.Rows)
	{
		UInventoryRowButton* RowButton = WidgetTree->ConstructWidget<UInventoryRowButton>(
			UInventoryRowButton::StaticClass());
		RowButton->InstanceId = Row.InstanceId;
		RowButton->OnRowSelected.BindLambda([this](const FGuid& InstanceId)
		{
			HandleRowSelected(InstanceId);
		});
		RowButton->OnClicked.AddDynamic(RowButton, &UInventoryRowButton::HandleRowClicked);

		// M3-019: the icon area. The configured engine placeholder texture is
		// loaded synchronously; when it resolves the image shows and the tag
		// stays collapsed. When it does NOT resolve (no config, a stale path
		// or a load failure) the image stays collapsed and the readable short
		// tag becomes visible - the tested fallback path. Both controls are
		// built either way, so the row layout never depends on the data.
		UImage* IconImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		IconImage->SetDesiredSizeOverride(FVector2D(18.0f, 18.0f));
		IconImage->SetVisibility(ESlateVisibility::Collapsed);
		UTextBlock* IconTag = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		IconTag->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 10));
		IconTag->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.75f, 1.0f, 1.0f)));
		IconTag->SetVisibility(ESlateVisibility::Collapsed);
		UTexture2D* IconTexture = LoadInventoryRowIconTexture(Row.IconPath);
		if (IconTexture != nullptr)
		{
			IconImage->SetBrushFromTexture(IconTexture);
			IconImage->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		}
		else
		{
			IconTag->SetText(FText::FromString(Row.SlotTag));
			IconTag->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		}

		UTextBlock* RowBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		const FString RowText = FString::Printf(TEXT("%s%s | %s | %s"),
			Row.bEquipped ? M3_011_EquippedPrefix : TEXT(""),
			*Row.DisplayName, *Row.SlotName, *Row.StatsText);
		RowBlock->SetText(FText::FromString(RowText));
		RowBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
		RowBlock->SetColorAndOpacity(FSlateColor(Row.bEquipped
			? FLinearColor(1.0f, 0.85f, 0.3f, 1.0f)
			: FLinearColor(0.85f, 0.95f, 1.0f, 1.0f)));
		RowBlock->SetJustification(ETextJustify::Left);

		UHorizontalBox* RowContent = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		if (UHorizontalBoxSlot* IconSlot = RowContent->AddChildToHorizontalBox(IconImage))
		{
			// Tight paddings: the longest row ([Equipped] + a full stats line)
			// must still fit the fixed panel width without clipping.
			IconSlot->SetPadding(FMargin(0.0f, 0.0f, 4.0f, 0.0f));
		}
		if (UHorizontalBoxSlot* TagSlot = RowContent->AddChildToHorizontalBox(IconTag))
		{
			TagSlot->SetPadding(FMargin(0.0f, 0.0f, 4.0f, 0.0f));
		}
		RowContent->AddChildToHorizontalBox(RowBlock);
		RowButton->AddChild(RowContent);
		if (UVerticalBoxSlot* RowSlot = RowsBox->AddChildToVerticalBox(RowButton))
		{
			RowSlot->SetPadding(FMargin(4.0f, 3.0f));
		}

		RowIconImages.Add(IconImage);
		RowIconTags.Add(IconTag);
	}
}

void UInventoryWidget::HandleCloseClicked()
{
	// The HUD owns the real dismissal and the input-focus restore.
	CloseRequested.Broadcast();
}
