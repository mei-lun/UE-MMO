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
#include "../Profile/RewardService.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "InventoryWidget.h"
#include "PendingRewardsWidget.h"

namespace
{
	// M3-018: display constants of the settlement reward area (ASCII only; the
	// full-bag prompt comes from PendingRewardsWidget.h so both surfaces show
	// the identical line).
	const TCHAR* M3_018_ClaimButtonText = TEXT("Claim");
	const TCHAR* M3_018_SavingStatusText = TEXT("Saving reward...");
	const TCHAR* M3_018_AlreadyClaimedStatusText = TEXT("Reward was already claimed.");
}

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

// ----- M3-018: settlement reward area (pure view model) ----------------------

FRoomRewardViewModel MakeRoomRewardViewModelFromDraft(const FPendingReward& Draft, const FItemDefinitionCatalog* Catalog)
{
	FRoomRewardViewModel ViewModel;
	ViewModel.bValid = true;
	ViewModel.SettlementId = Draft.SettlementId;
	ViewModel.XP = Draft.XP;

	// Item lines from the draft's own pre-generated instances: the display is
	// a projection of the SNAPSHOT (the roll happened once at BeginReward
	// time), so every re-presentation of the same settlement shows the same
	// items - the view model never re-rolls and never invents an entry.
	TArray<FString> ItemTexts;
	for (int32 Index = 0; Index < Draft.Items.Num(); ++Index)
	{
		const FItemInstance& Item = Draft.Items[Index];
		const FItemDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(Item.DefinitionId) : nullptr;
		const FInventoryRowViewModel Row = MakeInventoryRowViewModel(Item, Definition, /*bEquipped*/ false);
		FRoomRewardItemLine Line;
		Line.DisplayName = Row.DisplayName;
		Line.StatsText = Row.StatsText;
		ViewModel.Items.Add(Line);
		ItemTexts.Add(FString::Printf(TEXT("%s (%s)"), *Row.DisplayName, *Row.StatsText));
	}

	ViewModel.RewardText = FString::Printf(TEXT("Reward: XP %d"), Draft.XP);
	if (ItemTexts.Num() > 0)
	{
		ViewModel.RewardText += FString::Printf(TEXT("    %s"), *FString::Join(ItemTexts, TEXT("    ")));
	}
	ViewModel.ClaimState = ERoomRewardClaimState::Unclaimed;
	ViewModel.StatusText.Reset();
	return ViewModel;
}

ERoomRewardClaimState DeriveRoomRewardClaimState(const FRewardClaimAtomicOutcome& Claim)
{
	switch (Claim.Result)
	{
	case ERewardClaimAtomicResult::Claimed:
	case ERewardClaimAtomicResult::PartiallyClaimed:
	case ERewardClaimAtomicResult::AlreadyClaimed:
		// AlreadyClaimed is the persisted truth of an EARLIER claim - showing
		// "already claimed" is honest, not a fake completion.
		return ERoomRewardClaimState::Claimed;
	case ERewardClaimAtomicResult::InventoryFull:
		return ERoomRewardClaimState::InventoryFull;
	default:
		// SaveFailed, UnknownSettlement, RejectedNoProfile, RejectedNoSaveService
		// and RejectedUnreadableSave: every failure keeps the reward pending and
		// retryable - the UI may never confirm a claim from these.
		return ERoomRewardClaimState::Failed;
	}
}

FRoomRewardViewModel ApplyRoomRewardClaimOutcome(const FRoomRewardViewModel& Base, const FRewardClaimAtomicOutcome& Claim)
{
	FRoomRewardViewModel ViewModel = Base;
	ViewModel.ClaimState = DeriveRoomRewardClaimState(Claim);
	// The item lines stay the draft snapshot on EVERY path - a failure keeps
	// the ORIGINAL reward visible and retryable (no re-roll, no fake empty).
	switch (ViewModel.ClaimState)
	{
	case ERoomRewardClaimState::Claimed:
		if (Claim.Result == ERewardClaimAtomicResult::AlreadyClaimed)
		{
			ViewModel.StatusText = M3_018_AlreadyClaimedStatusText;
		}
		else
		{
			ViewModel.StatusText = FString::Printf(TEXT("Reward claimed and saved: %d item(s) entered the inventory%s"),
				Claim.ClaimedItemCount,
				Claim.bGrantedXP ? TEXT(" (XP granted once)") : TEXT(""));
		}
		break;
	case ERoomRewardClaimState::InventoryFull:
		ViewModel.StatusText = MakeRewardInventoryFullText();
		break;
	case ERoomRewardClaimState::Failed:
		ViewModel.StatusText = FString::Printf(TEXT("Claim failed - the reward stays pending for retry. Reason: %s"),
			*Claim.Error);
		break;
	case ERoomRewardClaimState::Saving:
		ViewModel.StatusText = M3_018_SavingStatusText;
		break;
	case ERoomRewardClaimState::Unclaimed:
	default:
		ViewModel.StatusText.Reset();
		break;
	}
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
	// No reward draft is bound yet: the row stays hidden until BindReward
	// fills the M3-018 reward view model (nothing is invented meanwhile).
	RewardBlock->SetVisibility(ESlateVisibility::Collapsed);

	// M3-018: the claim feedback line (Saving / claimed / full bag / failure).
	RewardStatusBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	RewardStatusBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 13));
	RewardStatusBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.35f, 1.0f)));
	RewardStatusBlock->SetJustification(ETextJustify::Center);
	RewardStatusBlock->SetVisibility(ESlateVisibility::Collapsed);

	// M3-018: the claim button (the HUD executes the real atomic claim; the
	// widget only broadcasts the request and owns the visible guard half).
	ClaimButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	ClaimLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	ClaimLabel->SetText(FText::FromString(M3_018_ClaimButtonText));
	ClaimButton->AddChild(ClaimLabel);
	ClaimButton->OnClicked.AddDynamic(this, &URoomResultWidget::HandleClaimButtonClicked);

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
	if (UHorizontalBoxSlot* ClaimSlot = Buttons->AddChildToHorizontalBox(ClaimButton))
	{
		// The claim belongs to the reward area; it sits in front of the
		// retry/return pair and is hidden when no reward area is bound.
		ClaimSlot->SetPadding(FMargin(14.0f, 8.0f));
	}
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
	Stack->AddChildToVerticalBox(RewardStatusBlock);
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
	ApplyRewardToControls();
}

void URoomResultWidget::ApplyRewardToControls()
{
	if (!bControlsBuilt)
	{
		return;
	}

	// The M3-018 reward area is visible only for a valid draft snapshot
	// binding (victory with a pending draft); every other shape stays hidden.
	const bool bShowReward = RewardViewModel.bValid && !RewardViewModel.RewardText.IsEmpty();
	if (bShowReward)
	{
		RewardBlock->SetText(FText::FromString(RewardViewModel.RewardText));
		RewardBlock->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	}
	else
	{
		RewardBlock->SetVisibility(ESlateVisibility::Collapsed);
	}

	if (RewardStatusBlock != nullptr)
	{
		if (bShowReward && !RewardViewModel.StatusText.IsEmpty())
		{
			RewardStatusBlock->SetText(FText::FromString(RewardViewModel.StatusText));
			RewardStatusBlock->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
		}
		else
		{
			RewardStatusBlock->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	if (ClaimButton != nullptr)
	{
		// The claim affordance exists only with a pending reward and is
		// clickable exactly while a claim may still happen: Unclaimed and the
		// two retryable failures (the card's retry rule). Saving and a
		// committed claim disable it.
		const bool bClaimEnabled = bShowReward &&
			(RewardViewModel.ClaimState == ERoomRewardClaimState::Unclaimed ||
			 RewardViewModel.ClaimState == ERoomRewardClaimState::InventoryFull ||
			 RewardViewModel.ClaimState == ERoomRewardClaimState::Failed);
		ClaimButton->SetVisibility(bShowReward
			? ESlateVisibility::Visible
			: ESlateVisibility::Collapsed);
		ClaimButton->SetIsEnabled(bClaimEnabled);
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

// ----- M3-018: settlement reward area of the result screen --------------------

void URoomResultWidget::BindReward(const FRoomRewardViewModel& Reward)
{
	// A pure fill from the caller's draft snapshot: the widget renders the
	// pending reward and starts Unclaimed (or whatever state the caller's view
	// model carries on a re-presentation); it never claims and never re-rolls.
	RewardViewModel = Reward;
	BuildControls(); // safety net for a fill before Initialize built the tree
	ApplyRewardToControls();
}

void URoomResultWidget::SetRewardClaimSaving()
{
	if (!RewardViewModel.bValid)
	{
		return;
	}
	// The claim request was accepted and the save is in flight: the feedback
	// line shows and the claim button is disabled until the outcome arrives.
	RewardViewModel.ClaimState = ERoomRewardClaimState::Saving;
	RewardViewModel.StatusText = M3_018_SavingStatusText;
	ApplyRewardToControls();
}

void URoomResultWidget::ApplyRewardClaimOutcome(const FRewardClaimAtomicOutcome& Claim)
{
	if (!RewardViewModel.bValid)
	{
		return;
	}
	// The UI truth is the returned claim outcome (never memory state): the
	// pure apply derives the five-state display and the readable feedback.
	RewardViewModel = ApplyRoomRewardClaimOutcome(RewardViewModel, Claim);
	ApplyRewardToControls();
}

void URoomResultWidget::HandleClaimButtonClicked()
{
	// A click on the disabled button is dropped (the visible anti-double-click
	// half); the HUD's one-shot FRoomResultActionGuard is the machine half.
	if (ClaimButton == nullptr || !ClaimButton->GetIsEnabled())
	{
		return;
	}
	ClaimButton->SetIsEnabled(false);
	RewardClaimRequested.Broadcast();
}
