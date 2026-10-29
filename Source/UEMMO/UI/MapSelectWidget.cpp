// M3-017: the map select menu widget. GREEN implementation of the stubbed
// contract: the code-built control tree (the M2-012 result screen's native
// pattern, no UMG asset), the selectable map row, the settings/volume text
// placeholder, and the click path with the loading guard plus the failure
// error line and button recovery.
// M6-002 (the user's free-select decision, 2026-10-29): the level list is the
// M6-001 catalog - one dynamic row per entry with its own Enter button and
// the M6-003 clear record as a read-only status line; nothing is gated. The
// M5-018A SystemTestRoom row and the M3-030 override enter button (with the
// M5-056 gameplay loadout bridge) stay unchanged above/below the list.

#include "MapSelectWidget.h"

#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Room/RoomDefinition.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"

namespace
{
	// M3-017 widget copy (ASCII; the real localization belongs to a later UI
	// task): the title, the enter label and the settings placeholder. Unique
	// M3_017 names per the per-file constant rule.
	const TCHAR* GM3_017_TitleText = TEXT("Map Select");
	const TCHAR* GM3_017_EnterLabelText = TEXT("Enter");
	const TCHAR* GM3_017_SettingsPlaceholderText = TEXT("Settings / Volume: text placeholder (not in the M3-017 scope)");

	// M5-018A: the appended config-driven system test room row.
	const TCHAR* GM5_018A_SystemTestRoomText = TEXT("SystemTestRoom (config-driven)");

	// M6-002: the free-select row status lines. The clear record is READ-ONLY
	// bookkeeping (the M6-003 free-select decision: no gate, no lock state).
	const TCHAR* GM6_002_NotClearedSuffix = TEXT(" - not cleared");
	const TCHAR* GM6_002_FlowUnavailableText = TEXT("M6-002: the game flow subsystem is unavailable - cannot enter the level.");
	const TCHAR* GM6_002_RowDefinitionUnavailableText = TEXT("M6-002: the catalog row's level definition is unavailable.");
}

/**
 * M6-002: the visible text of one catalog row - the DisplayName (the RoomId
 * fallback) plus the room's clear record as a read-only status. A missing
 * record (the room has no cleared run yet) shows the explicit "not cleared"
 * line instead of an implicit blank.
 */
static FString GM6_002_BuildRowText(const URoomDefinition& Definition, UProfileSubsystem* Profile)
{
	FString Name = Definition.DisplayName;
	if (Name.IsEmpty())
	{
		Name = Definition.RoomId.ToString();
	}
	FRoomClearSaveEntry Record;
	if (Profile != nullptr && Profile->GetRoomClearRecord(Definition.RoomId, Record))
	{
		return FString::Printf(TEXT("%s - cleared x%d"), *Name, Record.ClearCount);
	}
	return Name + GM6_002_NotClearedSuffix;
}

// ----- UMapSelectCatalogRow -----------------------------------------------------

void UMapSelectCatalogRow::SetupRow(const URoomDefinition* Definition, int32 InRowIndex,
	UBorder* InRowPanel, UTextBlock* InRowText, UButton* InRowButton, UTextBlock* InRowButtonLabel)
{
	RowDefinition = Definition;
	RowIndex = InRowIndex;
	RowPanel = InRowPanel;
	RowText = InRowText;
	RowButton = InRowButton;
	RowButtonLabel = InRowButtonLabel;
	// The per-row identity: the click binds THIS instance (each row binds its
	// own UFUNCTION - the standard dynamic-delegate pattern), so the request
	// forwarded through RowEnterRequested carries this row's index.
	if (RowButton != nullptr)
	{
		RowButton->OnClicked.AddDynamic(this, &UMapSelectCatalogRow::HandleRowClicked);
	}
}

void UMapSelectCatalogRow::SetRowButtonEnabled(bool bNewEnabled) const
{
	if (RowButton != nullptr)
	{
		RowButton->SetIsEnabled(bNewEnabled);
	}
}

void UMapSelectCatalogRow::HandleRowClicked()
{
	RowEnterRequested.Broadcast(RowIndex);
}

// ----- UMapSelectWidget --------------------------------------------------------

UMapSelectWidget::UMapSelectWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// The M3-030 mount captures the game input to the menu (the M2-012 result
	// screen precedent: FInputModeUIOnly::SetWidgetToFocus requires a
	// focusable widget).
	bIsFocusable = true;
}

void UMapSelectWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildControls();
}

void UMapSelectWidget::BuildControls()
{
	if (bControlsBuilt || WidgetTree == nullptr)
	{
		return;
	}

	// Centered panel: canvas root -> dark border panel -> vertical stack (the
	// M2-012 code-built pattern; everything is a native control).
	RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
	PanelBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	PanelBorder->SetBrushColor(FLinearColor(0.02f, 0.03f, 0.05f, 0.88f));
	PanelBorder->SetPadding(FMargin(48.0f, 28.0f));
	UCanvasPanelSlot* BorderSlot = RootCanvas->AddChildToCanvas(PanelBorder);
	if (BorderSlot != nullptr)
	{
		BorderSlot->SetAnchors(FAnchors(0.5f, 0.5f));
		BorderSlot->SetAlignment(FVector2D(0.5f, 0.5f));
		BorderSlot->SetAutoSize(true);
	}

	Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	PanelBorder->SetContent(Stack);

	TitleBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	TitleBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 24));
	TitleBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.85f, 0.3f, 1.0f)));
	TitleBlock->SetJustification(ETextJustify::Center);
	TitleBlock->SetText(FText::FromString(GM3_017_TitleText));

	// M6-002: the free-select list container - the rows are rebuilt from the
	// level catalog on every BindMenu presentation (the catalog and the clear
	// records may have changed).
	CatalogListBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());

	EnterButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	EnterLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	EnterLabel->SetText(FText::FromString(GM3_017_EnterLabelText));
	EnterButton->AddChild(EnterLabel);
	EnterButton->OnClicked.AddDynamic(this, &UMapSelectWidget::HandleEnterButtonClicked);

	SystemTestRoomRow = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	SystemTestRoomRow->SetBrushColor(FLinearColor(0.08f, 0.12f, 0.18f, 0.9f));
	SystemTestRoomRow->SetPadding(FMargin(20.0f, 10.0f));
	SystemTestRoomBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	SystemTestRoomBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.95f, 1.0f, 1.0f)));
	SystemTestRoomBlock->SetJustification(ETextJustify::Center);
	SystemTestRoomBlock->SetText(FText::FromString(GM5_018A_SystemTestRoomText));
	SystemTestRoomRow->SetContent(SystemTestRoomBlock);

	SystemTestRoomEnterButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	SystemTestRoomEnterLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	SystemTestRoomEnterLabel->SetText(FText::FromString(GM3_017_EnterLabelText));
	SystemTestRoomEnterButton->AddChild(SystemTestRoomEnterLabel);
	SystemTestRoomEnterButton->OnClicked.AddDynamic(this, &UMapSelectWidget::HandleSystemTestRoomClicked);

	ErrorBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	ErrorBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.3f, 1.0f)));
	ErrorBlock->SetJustification(ETextJustify::Center);
	// Hidden until a failure fills it (a clean menu shows no error line).
	ErrorBlock->SetVisibility(ESlateVisibility::Collapsed);

	// The card's simple text placeholder: no settings/volume implementation
	// in this card.
	SettingsPlaceholderBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	SettingsPlaceholderBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.65f, 0.7f, 1.0f)));
	SettingsPlaceholderBlock->SetJustification(ETextJustify::Center);
	SettingsPlaceholderBlock->SetText(FText::FromString(GM3_017_SettingsPlaceholderText));

	Stack->AddChildToVerticalBox(TitleBlock);
	Stack->AddChildToVerticalBox(CatalogListBox);
	Stack->AddChildToVerticalBox(EnterButton);
	Stack->AddChildToVerticalBox(SystemTestRoomRow);
	Stack->AddChildToVerticalBox(SystemTestRoomEnterButton);
	Stack->AddChildToVerticalBox(ErrorBlock);
	Stack->AddChildToVerticalBox(SettingsPlaceholderBlock);

	WidgetTree->RootWidget = RootCanvas;
	bControlsBuilt = true;
}

void UMapSelectWidget::BindMenu()
{
	// Engine 5.8 runs NativeOnInitialized on CreateWidget only with a valid
	// player context; a world-created (headless test / plain-game) widget
	// builds here - the same safety net as M2-012's BindResult.
	BuildControls();
	SetErrorText(FString());
	SetEnterButtonEnabled(true);
	if (SystemTestRoomEnterButton != nullptr)
	{
		SystemTestRoomEnterButton->SetIsEnabled(true);
	}
	// M6-002: every presentation mirrors the current catalog and the current
	// clear records (a cleared room re-arms with its new "cleared x N" line).
	RebuildCatalogRows();
}

UGameFlowSubsystem* UMapSelectWidget::GetGameFlowSubsystem() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
}

UProfileSubsystem* UMapSelectWidget::GetProfileSubsystem() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UProfileSubsystem>() : nullptr;
}

void UMapSelectWidget::RebuildCatalogRows()
{
	if (!bControlsBuilt || CatalogListBox == nullptr)
	{
		return;
	}

	// Drop the previous presentation's rows (the holders own no UObject the
	// tree needs; the widgets die with the list box's children).
	CatalogRows.Reset();
	while (CatalogListBox->GetChildrenCount() > 0)
	{
		CatalogListBox->RemoveChildAt(CatalogListBox->GetChildrenCount() - 1);
	}

	UGameFlowSubsystem* Flow = GetGameFlowSubsystem();
	if (Flow == nullptr || WidgetTree == nullptr)
	{
		// Without a flow the free-select surface stays empty (a bare world
		// has no catalog); the legacy faces keep their own behavior.
		return;
	}
	UProfileSubsystem* Profile = GetProfileSubsystem();
	const TArray<URoomDefinition*>& Catalog = Flow->GetSelectableRoomDefinitions();

	for (int32 Index = 0; Index < Catalog.Num(); ++Index)
	{
		const URoomDefinition* Definition = Catalog[Index];
		if (Definition == nullptr)
		{
			continue;
		}
		// The row panel: the same border style as the appended system test
		// row, with the status text left and this row's own Enter right.
		UMapSelectCatalogRow* Row = NewObject<UMapSelectCatalogRow>(this);
		UBorder* RowPanel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		RowPanel->SetBrushColor(FLinearColor(0.08f, 0.12f, 0.18f, 0.9f));
		RowPanel->SetPadding(FMargin(20.0f, 10.0f));
		UHorizontalBox* RowBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		UTextBlock* RowText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		RowText->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.95f, 1.0f, 1.0f)));
		RowText->SetText(FText::FromString(GM6_002_BuildRowText(*Definition, Profile)));
		UButton* RowButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		UTextBlock* RowButtonLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		RowButtonLabel->SetText(FText::FromString(GM3_017_EnterLabelText));
		RowButton->AddChild(RowButtonLabel);
		UHorizontalBoxSlot* TextSlot = RowBox->AddChildToHorizontalBox(RowText);
		TextSlot->SetPadding(FMargin(0.0f, 0.0f, 12.0f, 0.0f));
		RowBox->AddChildToHorizontalBox(RowButton);
		RowPanel->SetContent(RowBox);

		Row->SetupRow(Definition, Index, RowPanel, RowText, RowButton, RowButtonLabel);
		Row->RowEnterRequested.AddDynamic(this, &UMapSelectWidget::HandleCatalogRowEnterRequested);
		CatalogListBox->AddChildToVerticalBox(RowPanel);
		CatalogRows.Add(Row);
	}
}

void UMapSelectWidget::HandleCatalogRowEnterRequested(int32 RowIndex)
{
	UMapSelectCatalogRow* Row = CatalogRows.IsValidIndex(RowIndex) ? CatalogRows[RowIndex].Get() : nullptr;
	if (Row == nullptr)
	{
		return;
	}
	UGameFlowSubsystem* Flow = GetGameFlowSubsystem();
	if (Flow == nullptr)
	{
		SetErrorText(GM6_002_FlowUnavailableText);
		return;
	}
	const URoomDefinition* TargetDefinition = Row->GetRowDefinition();
	if (TargetDefinition == nullptr)
	{
		SetErrorText(GM6_002_RowDefinitionUnavailableText);
		return;
	}

	if (Flow->GetState() != EGameFlowState::Menu || Flow->IsLoading())
	{
		// The card's loading guard (the flow's state machine half): while a
		// load is in flight - or a room is already active - no further world
		// switch is requested. The visible half parks this row's button.
		Row->SetRowButtonEnabled(false);
		return;
	}

	// The visible anti-double-click half: EVERY enter button disables for the
	// attempt (the flow's bLoading guard is the machine half; one accepted
	// click means the room world takes over from the whole menu).
	SetEnterButtonEnabled(false);
	if (SystemTestRoomEnterButton != nullptr)
	{
		SystemTestRoomEnterButton->SetIsEnabled(false);
	}
	for (UMapSelectCatalogRow* OtherRow : CatalogRows)
	{
		if (OtherRow != nullptr)
		{
			OtherRow->SetRowButtonEnabled(false);
		}
	}

	if (Flow->EnterRoom(TargetDefinition))
	{
		// Accepted: the room world takes over; the buttons stay disabled for
		// this presentation and a stale error line clears. A catalog-row
		// click is a REAL menu click into gameplay, so the M5-056 gameplay
		// loadout bridge runs exactly like the M3-030 enter button's path.
		SetErrorText(FString());
		GameplayEnterAccepted.Broadcast();
		EnterAccepted.Broadcast();
		return;
	}

	// Failed open: the flow already fell back to the Menu state; show its
	// recorded reason and return every button to the player (the failure
	// stays retryable - including the OTHER rows, which stay operable).
	SetErrorText(Flow->GetLastError());
	SetEnterButtonEnabled(true);
	if (SystemTestRoomEnterButton != nullptr)
	{
		SystemTestRoomEnterButton->SetIsEnabled(true);
	}
	for (UMapSelectCatalogRow* OtherRow : CatalogRows)
	{
		if (OtherRow != nullptr)
		{
			OtherRow->SetRowButtonEnabled(true);
		}
	}
}

void UMapSelectWidget::HandleEnterClicked()
{
	UGameFlowSubsystem* Flow = GetGameFlowSubsystem();
	if (Flow == nullptr)
	{
		SetEnterButtonEnabled(false);
		SetErrorText(TEXT("M3-017: the game flow subsystem is unavailable - cannot enter the room."));
		SetEnterButtonEnabled(true);
		return;
	}

	if (Flow->GetState() != EGameFlowState::Menu || Flow->IsLoading())
	{
		// The card's loading guard (the flow's state machine half): while a
		// load is in flight - or a room is already active - the entry stays
		// disabled and no further world switch is requested.
		SetEnterButtonEnabled(false);
		return;
	}

	// The visible anti-double-click half: the button disables for the whole
	// enter attempt (the flow's bLoading guard is the machine half).
	SetEnterButtonEnabled(false);

	// The enter target: the presentation-level override wins when the mount
	// set one (the combat room the boot world's trigger chain owns); without
	// it the card's single selectable entry is requested (the M3-017 form).
	// M5-018A: the wave-room enter keeps the M3-017/M3-030 chain (the boot
	// world's override first, the selectable definition otherwise); the
	// system test room and the M6-002 catalog rows have their own handlers.
	const URoomDefinition* TargetDefinition = EnterRoomDefinitionOverride.Get();
	if (TargetDefinition == nullptr)
	{
		TargetDefinition = Flow->GetSelectableRoomDefinition();
	}
	if (TargetDefinition == nullptr)
	{
		SetErrorText(TEXT("M3-017: the selectable map entry is unavailable."));
		SetEnterButtonEnabled(true);
		return;
	}

	if (Flow->EnterRoom(TargetDefinition))
	{
		// Accepted: the room world takes over; the button stays disabled for
		// this presentation and a stale error line clears. The M3-030 mount
		// listens on EnterAccepted to dismiss the overlay.
		SetErrorText(FString());
		// M5-056: production-only bridge (the real click path) - grants the
		// gameplay boot loadout before the dismissal broadcast below. The
		// tests broadcast EnterAccepted directly and never reach this line.
		GameplayEnterAccepted.Broadcast();
		EnterAccepted.Broadcast();
		return;
	}

	// Failed open: the flow already fell back to the Menu state; show its
	// recorded reason and return the button to the player (the card: the
	// failure must stay retryable).
	SetErrorText(Flow->GetLastError());
	SetEnterButtonEnabled(true);
}

void UMapSelectWidget::HandleSystemTestRoomClicked()
{
	// The same guard chain as the wave-room enter (the state machine half
	// lives in the flow; the visible half disables both buttons for the
	// attempt so a double click cannot request two worlds).
	UGameFlowSubsystem* Flow = GetGameFlowSubsystem();
	if (Flow == nullptr)
	{
		SetErrorText(TEXT("M5-018A: the game flow subsystem is unavailable - cannot enter the system test room."));
		return;
	}
	if (Flow->GetState() != EGameFlowState::Menu || Flow->IsLoading())
	{
		SystemTestRoomEnterButton->SetIsEnabled(false);
		return;
	}
	SystemTestRoomEnterButton->SetIsEnabled(false);
	EnterButton->SetIsEnabled(false);

	URoomDefinition* TargetDefinition = Flow->GetSystemTestRoomDefinition();
	if (TargetDefinition == nullptr)
	{
		SetErrorText(TEXT("M5-018A: the system test room entry is unavailable."));
		SystemTestRoomEnterButton->SetIsEnabled(true);
		EnterButton->SetIsEnabled(true);
		return;
	}
	if (Flow->EnterRoom(TargetDefinition))
	{
		SetErrorText(FString());
		EnterAccepted.Broadcast();
		return;
	}
	SetErrorText(Flow->GetLastError());
	SystemTestRoomEnterButton->SetIsEnabled(true);
	EnterButton->SetIsEnabled(true);
}

void UMapSelectWidget::SetEnterRoomDefinition(const URoomDefinition* Definition)
{
	// The presentation-level enter target (the M3-030 HUD mount hands the
	// combat-room definition); a null restores the M3-017 selectable entry.
	EnterRoomDefinitionOverride = Definition;
}

void UMapSelectWidget::SetErrorText(const FString& Message)
{
	if (!bControlsBuilt || ErrorBlock == nullptr)
	{
		return;
	}
	if (Message.IsEmpty())
	{
		ErrorBlock->SetVisibility(ESlateVisibility::Collapsed);
		ErrorBlock->SetText(FText::GetEmpty());
	}
	else
	{
		ErrorBlock->SetText(FText::FromString(Message));
		ErrorBlock->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
}

void UMapSelectWidget::SetEnterButtonEnabled(bool bNewEnabled)
{
	if (!bControlsBuilt || EnterButton == nullptr)
	{
		return;
	}
	EnterButton->SetIsEnabled(bNewEnabled);
}

void UMapSelectWidget::HandleEnterButtonClicked()
{
	HandleEnterClicked();
}
