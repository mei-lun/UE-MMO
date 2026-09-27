#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RoomResultWidget.generated.h"

struct FRoomResult;

class UBorder;
class UButton;
class UCanvasPanel;
class UTextBlock;

/** Fired when the user presses the Retry/Return button of the result screen. */
DECLARE_MULTICAST_DELEGATE(FRoomResultActionRequested);

/**
 * M2-012: pure display state of the room result screen, filled from the
 * session's FRoomResult (interface contract section 7). Pure value type on
 * purpose: every string is built by MakeRoomResultViewModel, which needs no
 * world and no clock, so the whole display contract is testable headless.
 */
struct FRoomResultViewModel
{
	/** False for a default-constructed (never filled) view model. */
	bool bValid = false;

	/** True = Victory, false = Defeat (copied from FRoomResult::bCleared). */
	bool bCleared = false;

	/** Run duration in seconds, copied from FRoomResult::ElapsedSeconds. */
	double ElapsedSeconds = 0.0;

	/** Kills the session recorded for the run, copied from FRoomResult. */
	int32 KilledCount = 0;

	/** "Victory" / "Defeat"; empty before the first fill. */
	FString HeadlineText;

	/** One-line summary "Time <x> s    Kills <n>"; empty before the first fill. */
	FString SummaryText;

	/**
	 * Reward line; ALWAYS empty in M2: this card has no reward/equipment
	 * data, so nothing may be invented here (no fake counts, the card's
	 * explicit rule). A later settlement task owns the real content; while
	 * the line is empty the widget keeps the reward row hidden.
	 */
	FString RewardText;
};

/**
 * Builds the display state of one finished run from the session's result.
 * Pure function (no world, no clock): the headline derives from bCleared,
 * the summary from ElapsedSeconds/KilledCount, and the reward line stays
 * empty because no reward data exists in M2 (never invented).
 */
UEMMO_API FRoomResultViewModel MakeRoomResultViewModel(const FRoomResult& Result);

/**
 * M2-012: one-shot request guard of the result screen's action paths. The
 * guard is disarmed by default (an un-shown screen accepts nothing); every
 * presentation re-arms it exactly once, so of two fast clicks only the FIRST
 * request is processed and the duplicate is dropped (and counted).
 */
class UEMMO_API FRoomResultActionGuard
{
public:
	/** Re-arms the guard (called every time the result screen is presented). */
	void ReArm();

	/**
	 * Consumes one request: true exactly once per arm; afterwards false until
	 * the next ReArm. A refused request is counted as rejected.
	 */
	bool TryAccept();

	int32 GetAcceptedCount() const { return AcceptedCount; }
	int32 GetRejectedCount() const { return RejectedCount; }

private:
	bool bArmed = false;
	int32 AcceptedCount = 0;
	int32 RejectedCount = 0;
};

/** Input-focus phase of the result screen lifecycle (pure state flag). */
enum class ERoomResultInputPhase : uint8
{
	/** The game owns the input (before the first presentation and after a restore). */
	InGame,
	/** The result screen captured the input (UI focus, once per presentation). */
	CapturedToUI,
	/** The screen was dismissed and the game input focus was restored. */
	RestoredToGame
};

/**
 * M2-012: pure tracker of the one-time input switch contract ("the game
 * input <-> UI focus switch happens exactly once per presentation"). The HUD
 * records every capture/restore decision here and performs the real
 * SetInputMode call only when the tracker reports an actual phase transition,
 * so duplicates (a second capture while captured, a restore without a
 * capture) can never re-switch the engine input mode.
 */
class UEMMO_API FRoomResultInputFocusTracker
{
public:
	/**
	 * Records the capture; true only on the real InGame/RestoredToGame ->
	 * CapturedToUI transition (a duplicate capture returns false).
	 */
	bool CaptureToUI();

	/**
	 * Records the restore; true only on the real CapturedToUI ->
	 * RestoredToGame transition (a restore without a capture returns false).
	 */
	bool RestoreToGame();

	ERoomResultInputPhase GetPhase() const { return Phase; }
	int32 GetCaptureCount() const { return CaptureCount; }
	int32 GetRestoreCount() const { return RestoreCount; }

private:
	ERoomResultInputPhase Phase = ERoomResultInputPhase::InGame;
	int32 CaptureCount = 0;
	int32 RestoreCount = 0;
};

/**
 * M2-012: the room result screen (Victory/Defeat, time, kills, Retry/Return).
 * Native-only UUserWidget: the control tree (canvas root, dark border panel,
 * text blocks and the two labeled buttons) is built in code inside
 * NativeOnInitialized - no UMG asset and no external UI package. BindResult
 * fills the texts from the session's FRoomResult; the reward row exists but
 * stays collapsed because M2 has no reward data (nothing is invented). The
 * buttons forward through the two plain delegates; the HUD owns the real
 * retry/leave execution and the anti-double-click guard.
 */
UCLASS()
class UEMMO_API URoomResultWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	URoomResultWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;

	/**
	 * Fills the display from one finished run (pure fill; works before the
	 * widget ever reaches a viewport). Also re-enables the action buttons: a
	 * fresh presentation starts with a fresh, enabled button pair.
	 */
	void BindResult(const FRoomResult& Result);

	/** Fired by the Retry button (the HUD executes the real retry path). */
	FRoomResultActionRequested RetryRequested;

	/** Fired by the Return button (the HUD executes the real leave path). */
	FRoomResultActionRequested ReturnRequested;

	/** Enables/disables both buttons together (the visible anti-double-click half). */
	void SetActionButtonsEnabled(bool bNewEnabled);

	// -- Read seams (tests and the HUD) ----------------------------------------

	const FRoomResultViewModel& PeekViewModel() const { return ViewModel; }
	UTextBlock* PeekHeadlineBlock() const { return HeadlineBlock; }
	UTextBlock* PeekSummaryBlock() const { return SummaryBlock; }
	UTextBlock* PeekRewardBlock() const { return RewardBlock; }
	UButton* PeekRetryButton() const { return RetryButton; }
	UButton* PeekReturnButton() const { return ReturnButton; }
	UTextBlock* PeekRetryLabel() const { return RetryLabel; }
	UTextBlock* PeekReturnLabel() const { return ReturnLabel; }

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	UFUNCTION()
	void HandleRetryButtonClicked();

	UFUNCTION()
	void HandleReturnButtonClicked();

	/** Re-fills every text from the current view model (reward row included). */
	void ApplyViewModelToControls();

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> RootCanvas;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> PanelBorder;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> HeadlineBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> SummaryBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> RewardBlock;

	UPROPERTY(Transient)
	TObjectPtr<UButton> RetryButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> ReturnButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> RetryLabel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ReturnLabel;

	FRoomResultViewModel ViewModel;
	bool bControlsBuilt = false;
};
