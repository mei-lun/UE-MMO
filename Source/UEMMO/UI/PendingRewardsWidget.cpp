// M3-018: the pending reward list screen and its pure view model. GREEN
// implementation of the stubbed contract: the list builds one row per pending
// draft from the profile's PendingRewards SNAPSHOT (settlement id, pending XP,
// pre-generated item names - the M3-011 placeholder rule for missing
// definitions, an explicit "(no items)" line for item-less drafts), the
// full-inventory state shows the shared readable prompt, and the menu badge
// line carries the pending count. The widget is a pure display surface: it
// never claims, never mutates the profile and never re-rolls anything.

#include "PendingRewardsWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ScrollBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"

#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Profile/RewardService.h"
#include "InventoryWidget.h"

namespace
{
	// M3-018: display constants of the pending reward list (the M3-011 panel
	// palette is reused for coherence).
	const TCHAR* M3_018_TitleText = TEXT("Pending Rewards");
	const TCHAR* M3_018_EmptyText = TEXT("No pending rewards");
	const TCHAR* M3_018_NoItemsText = TEXT("(no items)");

	// The card's full-inventory prompt (one shared source; the result screen's
	// claim feedback shows the exact same line via MakeRewardInventoryFullText).
	const TCHAR* M3_018_InventoryFullPrompt = TEXT("Inventory is full - free a slot and try again.");
}

// ----- M3-018: pure view model --------------------------------------------------

FString MakeRewardInventoryFullText()
{
	return FString(M3_018_InventoryFullPrompt);
}

FString MakePendingRewardsBadgeText(int32 PendingCount)
{
	if (PendingCount <= 0)
	{
		// A clean menu carries no badge line (no noise for an empty state).
		return FString();
	}
	return FString::Printf(TEXT("Pending rewards: %d"), PendingCount);
}

FPendingRewardRowViewModel MakePendingRewardRowViewModel(const FPendingReward& Draft, const FItemDefinitionCatalog* Catalog)
{
	FPendingRewardRowViewModel Row;
	Row.SettlementId = Draft.SettlementId;
	Row.SettlementText = FString::Printf(TEXT("Settlement %llu"), Draft.SettlementId);
	Row.XPText = FString::Printf(TEXT("XP %d"), Draft.XP);

	// Item lines from the draft's own pre-generated instances (no re-roll, no
	// invented entries): the definition display name when the catalog resolves
	// it, the readable "<unknown item>" placeholder otherwise, and the
	// INSTANCE's rolled stats in both cases (owned data, the M3-011 rule).
	TArray<FString> ItemTexts;
	for (int32 Index = 0; Index < Draft.Items.Num(); ++Index)
	{
		const FItemInstance& Item = Draft.Items[Index];
		const FItemDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(Item.DefinitionId) : nullptr;
		const FInventoryRowViewModel ItemRow = MakeInventoryRowViewModel(Item, Definition, /*bEquipped*/ false);
		ItemTexts.Add(FString::Printf(TEXT("%s (%s)"), *ItemRow.DisplayName, *ItemRow.StatsText));
	}
	Row.ItemsText = (ItemTexts.Num() > 0)
		? FString::Join(ItemTexts, TEXT("    "))
		: FString(M3_018_NoItemsText);
	return Row;
}

FPendingRewardListViewModel MakePendingRewardListViewModel(const TArray<FPendingReward>& Drafts,
	const FItemDefinitionCatalog* Catalog, bool bInventoryFull)
{
	FPendingRewardListViewModel ViewModel;
	ViewModel.bValid = true;
	for (int32 Index = 0; Index < Drafts.Num(); ++Index)
	{
		ViewModel.Rows.Add(MakePendingRewardRowViewModel(Drafts[Index], Catalog));
	}
	// The full-bag prompt is the card's claim-retry hint; it travels with the
	// caller's inventory read (Count() >= Capacity), never with an invention.
	ViewModel.StatusText = bInventoryFull ? MakeRewardInventoryFullText() : FString();
	return ViewModel;
}

// ----- M3-018: the native pending rewards widget ---------------------------------

UPendingRewardsWidget::UPendingRewardsWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsFocusable = true;
}

void UPendingRewardsWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildControls();
}

void UPendingRewardsWidget::BuildControls()
{
	if (bControlsBuilt || WidgetTree == nullptr)
	{
		return;
	}

	// Centered fixed-size panel: canvas root -> dark border panel -> vertical
	// stack (title, empty state, scrollable rows, status line). Everything is a
	// code-built native control: no UMG asset is involved (the M3-011 shape).
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
		BorderSlot->SetSize(FVector2D(560.0f, 420.0f));
	}

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	PanelBorder->SetContent(Stack);

	TitleBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	TitleBlock->SetText(FText::FromString(M3_018_TitleText));
	TitleBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 22));
	TitleBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.85f, 0.3f, 1.0f)));
	TitleBlock->SetJustification(ETextJustify::Center);

	EmptyBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	EmptyBlock->SetText(FText::FromString(M3_018_EmptyText));
	EmptyBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.65f, 0.7f, 1.0f)));
	EmptyBlock->SetJustification(ETextJustify::Center);
	// Hidden by default; ApplyViewModelToControls shows it for an empty list.
	EmptyBlock->SetVisibility(ESlateVisibility::Collapsed);

	ListScrollBox = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	RowsBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	ListScrollBox->AddChild(RowsBox);

	StatusBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	StatusBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	StatusBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.35f, 1.0f)));
	StatusBlock->SetJustification(ETextJustify::Center);
	StatusBlock->SetVisibility(ESlateVisibility::Collapsed);

	Stack->AddChildToVerticalBox(TitleBlock);
	Stack->AddChildToVerticalBox(EmptyBlock);
	if (UVerticalBoxSlot* ScrollSlot = Stack->AddChildToVerticalBox(ListScrollBox))
	{
		// The list takes all remaining panel height; the rows scroll inside.
		ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		ScrollSlot->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 10.0f));
	}
	if (UVerticalBoxSlot* StatusSlot = Stack->AddChildToVerticalBox(StatusBlock))
	{
		StatusSlot->SetPadding(FMargin(4.0f, 6.0f));
	}

	WidgetTree->RootWidget = RootCanvas;
	bControlsBuilt = true;
	ApplyViewModelToControls();
}

void UPendingRewardsWidget::BindPendingRewards(const TArray<FPendingReward>& Drafts,
	const FItemDefinitionCatalog* Catalog, bool bInventoryFull)
{
	ViewModel = MakePendingRewardListViewModel(Drafts, Catalog, bInventoryFull);
	BuildControls(); // safety net for a fill before Initialize built the tree
	ApplyViewModelToControls();
}

void UPendingRewardsWidget::ApplyViewModelToControls()
{
	if (!bControlsBuilt)
	{
		return;
	}

	// The rows rebuild per bind (the pending list is small and the bind is the
	// only change point - no per-Tick refresh exists on this screen).
	if (RowsBox != nullptr)
	{
		RowsBox->ClearChildren();
	}
	RowBlocks.Reset();
	for (int32 Index = 0; Index < ViewModel.Rows.Num(); ++Index)
	{
		const FPendingRewardRowViewModel& Row = ViewModel.Rows[Index];
		UTextBlock* RowBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		RowBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12));
		RowBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.95f, 1.0f, 1.0f)));
		RowBlock->SetJustification(ETextJustify::Left);
		RowBlock->SetText(FText::FromString(FString::Printf(TEXT("%s    %s    %s"),
			*Row.SettlementText, *Row.XPText, *Row.ItemsText)));
		if (UVerticalBoxSlot* RowSlot = RowsBox->AddChildToVerticalBox(RowBlock))
		{
			RowSlot->SetPadding(FMargin(4.0f, 4.0f));
		}
		RowBlocks.Add(RowBlock);
	}

	const bool bEmpty = ViewModel.IsEmpty();
	if (EmptyBlock != nullptr)
	{
		EmptyBlock->SetVisibility(bEmpty
			? ESlateVisibility::SelfHitTestInvisible
			: ESlateVisibility::Collapsed);
	}
	if (ListScrollBox != nullptr)
	{
		ListScrollBox->SetVisibility(bEmpty
			? ESlateVisibility::Collapsed
			: ESlateVisibility::SelfHitTestInvisible);
	}
	if (StatusBlock != nullptr)
	{
		if (ViewModel.StatusText.IsEmpty())
		{
			StatusBlock->SetVisibility(ESlateVisibility::Collapsed);
		}
		else
		{
			StatusBlock->SetText(FText::FromString(ViewModel.StatusText));
			StatusBlock->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		}
	}
}

int32 UPendingRewardsWidget::PeekRowCount() const
{
	return RowBlocks.Num();
}

UTextBlock* UPendingRewardsWidget::PeekRowText(int32 Index) const
{
	return RowBlocks.IsValidIndex(Index) ? RowBlocks[Index].Get() : nullptr;
}
