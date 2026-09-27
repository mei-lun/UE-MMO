#include "RoomResultWidget.h"

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

#include "../Room/RoomSessionSubsystem.h"

// ----- M2-012: pure view model ----------------------------------------------

FRoomResultViewModel MakeRoomResultViewModel(const FRoomResult& Result)
{
	FRoomResultViewModel ViewModel;
	ViewModel.bValid = true;
	ViewModel.bCleared = Result.bCleared;
	ViewModel.ElapsedSeconds = Result.ElapsedSeconds;
	ViewModel.KilledCount = Result.KilledCount;
	ViewModel.HeadlineText = Result.bCleared ? TEXT("Victory") : TEXT("Defeat");
	ViewModel.SummaryText = FString::Printf(TEXT("Time %.1f s    Kills %d"),
		Result.ElapsedSeconds, Result.KilledCount);
	// M2 has no reward/equipment data: the line stays empty for BOTH outcomes
	// (the widget hides the row) - never an invented count (the card's rule).
	ViewModel.RewardText.Empty();
	return ViewModel;
}

// ----- M2-012: one-shot action guard ----------------------------------------

void FRoomResultActionGuard::ReArm()
{
	bArmed = true;
}

bool FRoomResultActionGuard::TryAccept()
{
	if (!bArmed)
	{
		// Nothing presented (or this presentation's request was already
		// consumed): the duplicate is dropped and counted.
		++RejectedCount;
		return false;
	}
	bArmed = false;
	++AcceptedCount;
	return true;
}

// ----- M2-012: input-focus phase tracker ------------------------------------

bool FRoomResultInputFocusTracker::CaptureToUI()
{
	if (Phase == ERoomResultInputPhase::CapturedToUI)
	{
		// Duplicate capture: the switch already happened once for this
		// presentation and must not happen again (the card's one-time rule).
		return false;
	}
	Phase = ERoomResultInputPhase::CapturedToUI;
	++CaptureCount;
	return true;
}

bool FRoomResultInputFocusTracker::RestoreToGame()
{
	if (Phase != ERoomResultInputPhase::CapturedToUI)
	{
		// Nothing captured: a restore without a capture is inert.
		return false;
	}
	Phase = ERoomResultInputPhase::RestoredToGame;
	++RestoreCount;
	return true;
}

// ----- M2-012: the native result screen widget ------------------------------

URoomResultWidget::URoomResultWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// The result screen takes the UI focus when the HUD captures the input
	// (FInputModeUIOnly::SetWidgetToFocus requires a focusable widget).
	bIsFocusable = true;
}

void URoomResultWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildControls();
}

void URoomResultWidget::BuildControls()
{
	if (bControlsBuilt || WidgetTree == nullptr)
	{
		return;
	}

	// Centered panel: canvas root -> dark border panel -> vertical stack.
	// Everything is a code-built native control: no UMG asset is involved.
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

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	PanelBorder->SetContent(Stack);

	HeadlineBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	HeadlineBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 26));
	HeadlineBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.85f, 0.3f, 1.0f)));
	HeadlineBlock->SetJustification(ETextJustify::Center);

	SummaryBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	SummaryBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.95f, 1.0f, 1.0f)));
	SummaryBlock->SetJustification(ETextJustify::Center);

	RewardBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	RewardBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.65f, 0.7f, 1.0f)));
	RewardBlock->SetJustification(ETextJustify::Center);
	// No reward data exists in M2: the row stays hidden until a settlement
	// task fills RewardText with real content (nothing invented meanwhile).
	RewardBlock->SetVisibility(ESlateVisibility::Collapsed);

	RetryButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	RetryLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	RetryLabel->SetText(FText::FromString(TEXT("Retry")));
	RetryButton->AddChild(RetryLabel);
	RetryButton->OnClicked.AddDynamic(this, &URoomResultWidget::HandleRetryButtonClicked);

	ReturnButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	ReturnLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	ReturnLabel->SetText(FText::FromString(TEXT("Return")));
	ReturnButton->AddChild(ReturnLabel);
	ReturnButton->OnClicked.AddDynamic(this, &URoomResultWidget::HandleReturnButtonClicked);

	UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	if (UHorizontalBoxSlot* RetrySlot = Buttons->AddChildToHorizontalBox(RetryButton))
	{
		RetrySlot->SetPadding(FMargin(14.0f, 8.0f));
	}
	if (UHorizontalBoxSlot* ReturnSlot = Buttons->AddChildToHorizontalBox(ReturnButton))
	{
		ReturnSlot->SetPadding(FMargin(14.0f, 8.0f));
	}

	Stack->AddChildToVerticalBox(HeadlineBlock);
	Stack->AddChildToVerticalBox(SummaryBlock);
	Stack->AddChildToVerticalBox(RewardBlock);
	Stack->AddChildToVerticalBox(Buttons);

	WidgetTree->RootWidget = RootCanvas;
	bControlsBuilt = true;
	ApplyViewModelToControls();
}

void URoomResultWidget::BindResult(const FRoomResult& Result)
{
	ViewModel = MakeRoomResultViewModel(Result);
	BuildControls(); // safety net for a fill before Initialize built the tree
	ApplyViewModelToControls();
	// A fresh presentation starts with a fresh, enabled button pair; the
	// one-shot request guard re-arms on the HUD side with the same lifecycle.
	SetActionButtonsEnabled(true);
}

void URoomResultWidget::ApplyViewModelToControls()
{
	if (!bControlsBuilt)
	{
		return;
	}
	HeadlineBlock->SetText(FText::FromString(ViewModel.HeadlineText));
	SummaryBlock->SetText(FText::FromString(ViewModel.SummaryText));
	if (ViewModel.RewardText.IsEmpty())
	{
		RewardBlock->SetVisibility(ESlateVisibility::Collapsed);
	}
	else
	{
		RewardBlock->SetText(FText::FromString(ViewModel.RewardText));
		RewardBlock->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
}

void URoomResultWidget::SetActionButtonsEnabled(bool bNewEnabled)
{
	if (!bControlsBuilt)
	{
		return;
	}
	RetryButton->SetIsEnabled(bNewEnabled);
	ReturnButton->SetIsEnabled(bNewEnabled);
}

void URoomResultWidget::HandleRetryButtonClicked()
{
	// The first click disables both buttons (the visible anti-double-click
	// half) and forwards once; the HUD's one-shot guard is the second half.
	SetActionButtonsEnabled(false);
	RetryRequested.Broadcast();
}

void URoomResultWidget::HandleReturnButtonClicked()
{
	SetActionButtonsEnabled(false);
	ReturnRequested.Broadcast();
}
