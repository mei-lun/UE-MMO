#include "InventoryWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/SlateWrapperTypes.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemInstance.h"

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
	BuildControls(); // safety net for a fill before Initialize built the tree
	ApplyViewModelToControls();
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
	BuildControls();
	ApplyViewModelToControls();
	return true;
}

int32 UInventoryWidget::PeekRowCount() const
{
	return (bControlsBuilt && RowsBox != nullptr) ? RowsBox->GetChildrenCount() : 0;
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
		BorderSlot->SetSize(FVector2D(560.0f, 440.0f));
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

	// The scrollable list: a scroll box holding one vertical box of row texts
	// (30 rows at the design capacity never fit the 440px panel at once).
	ListScrollBox = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	RowsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	ListScrollBox->AddChild(RowsBox);

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
	if (UVerticalBoxSlot* CloseSlot = Stack->AddChildToVerticalBox(CloseButton))
	{
		CloseSlot->SetPadding(FMargin(120.0f, 4.0f));
	}

	WidgetTree->RootWidget = RootCanvas;
	bControlsBuilt = true;
	ApplyViewModelToControls();
}

void UInventoryWidget::ApplyViewModelToControls()
{
	if (!bControlsBuilt)
	{
		return;
	}

	// Empty state: the explicit line replaces the list for an empty inventory.
	EmptyBlock->SetVisibility(ViewModel.IsEmpty() ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);

	// Rebuild the row texts (ClearChildren drops the previous row controls;
	// this runs only on a real snapshot change - the fingerprint gate above).
	RowsBox->ClearChildren();
	for (const FInventoryRowViewModel& Row : ViewModel.Rows)
	{
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
		if (UVerticalBoxSlot* RowSlot = RowsBox->AddChildToVerticalBox(RowBlock))
		{
			RowSlot->SetPadding(FMargin(4.0f, 3.0f));
		}
	}
}

void UInventoryWidget::HandleCloseClicked()
{
	// The HUD owns the real dismissal and the input-focus restore.
	CloseRequested.Broadcast();
}
