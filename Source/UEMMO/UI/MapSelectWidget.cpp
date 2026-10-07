// M3-017: the map select menu widget. GREEN implementation of the stubbed
// contract: the code-built control tree (the M2-012 result screen's native
// pattern, no UMG asset), exactly one selectable TrainingArena row, the
// settings/volume text placeholder, and the click path with the loading
// guard plus the failure error line and button recovery.

#include "MapSelectWidget.h"

#include "../Profile/GameFlowSubsystem.h"
#include "../Room/RoomDefinition.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"

namespace
{
	// M3-017 widget copy (ASCII; the real localization belongs to a later UI
	// task): the title, the single map row, the enter label and the settings
	// placeholder. Unique M3_017 names per the per-file constant rule.
	const TCHAR* GM3_017_TitleText = TEXT("Map Select");
	const TCHAR* GM3_017_MapEntryText = TEXT("TrainingArena / CombatRoom01 (waves)");
	const TCHAR* GM5_018A_SystemTestRoomText = TEXT("SystemTestRoom (config-driven)");
	const TCHAR* GM3_017_EnterLabelText = TEXT("Enter");
	const TCHAR* GM3_017_SettingsPlaceholderText = TEXT("Settings / Volume: text placeholder (not in the M3-017 scope)");
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

	// The card's single selectable map row: the list holds exactly one entry
	// (no multi-map store).
	// M5-018A: two legal entries (the wave room keeps the M3-030 override
	// chain; the system test room is the new config-driven destination).
	// Both rows are buttons so the selection is a real click, not a console
	// command; the selected row highlights.
	MapEntryRow = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
	MapEntryRow->SetBrushColor(FLinearColor(0.08f, 0.12f, 0.18f, 0.9f));
	MapEntryRow->SetPadding(FMargin(20.0f, 10.0f));
	MapEntryBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	MapEntryBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.95f, 1.0f, 1.0f)));
	MapEntryBlock->SetJustification(ETextJustify::Center);
	MapEntryBlock->SetText(FText::FromString(GM3_017_MapEntryText));
	MapEntryRow->SetContent(MapEntryBlock);

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

	EnterButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	EnterLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	EnterLabel->SetText(FText::FromString(GM3_017_EnterLabelText));
	EnterButton->AddChild(EnterLabel);
	EnterButton->OnClicked.AddDynamic(this, &UMapSelectWidget::HandleEnterButtonClicked);

	ErrorBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	ErrorBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.35f, 0.3f, 1.0f)));
	ErrorBlock->SetJustification(ETextJustify::Center);
	// Hidden until a failure fills it (a clean menu shows no error line).
	ErrorBlock->SetVisibility(ESlateVisibility::Collapsed);

	// The card's simple text placeholder: no settings/volume implementation
	// and no multi-map store in this card.
	SettingsPlaceholderBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	SettingsPlaceholderBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.65f, 0.7f, 1.0f)));
	SettingsPlaceholderBlock->SetJustification(ETextJustify::Center);
	SettingsPlaceholderBlock->SetText(FText::FromString(GM3_017_SettingsPlaceholderText));

	Stack->AddChildToVerticalBox(TitleBlock);
	Stack->AddChildToVerticalBox(MapEntryRow);
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
}

UGameFlowSubsystem* UMapSelectWidget::GetGameFlowSubsystem() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
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
	// system test room has its own enter button and handler below.
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
