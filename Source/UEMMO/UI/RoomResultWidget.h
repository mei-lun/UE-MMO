#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "RoomResultWidget.generated.h"

struct FRoomResult;
struct FPendingReward;
struct FRewardClaimAtomicOutcome;
struct FItemDefinitionCatalog;

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
	 * Reward line; ALWAYS empty in M2/M3-018: the M2 view model carries no
	 * reward data (nothing is invented). The M3-018 settlement reward area is
	 * a SEPARATE view model (FRoomRewardViewModel below) filled only from the
	 * profile's pending draft; while that one is invalid the reward row stays
	 * hidden.
	 */
	FString RewardText;
};

UEMMO_API FRoomResultViewModel MakeRoomResultViewModel(const FRoomResult& Result);

// ----- M3-018: settlement reward area (pure view model) ----------------------

/**
 * M3-018: one display line of a pending draft item. The texts are built from
 * the INSTANCE's owned data (rolled stats) plus its definition display name;
 * a missing/unknown definition degrades to the readable "<unknown item>"
 * placeholder (the M3-011 rule) and never crashes.
 */
struct FRoomRewardItemLine
{
	/** Definition display name; "<unknown item>" when no definition resolves. */
	FString DisplayName;

	/** Instance stats line "Atk+<a> Def+<d> HP+<h>" (zeros included). */
	FString StatsText;
};

/**
 * M3-018: the five display states of the settlement reward area. Pure value
 * enum derived by DeriveRoomRewardClaimState from the reward draft plus one
 * URewardService claim outcome - never from UI-internal assumptions.
 */
enum class ERoomRewardClaimState : uint8
{
	/** A draft snapshot is bound and no claim was attempted yet. */
	Unclaimed,

	/** The claim request is in flight (the save may take frames). */
	Saving,

	/** The claim committed (items entered the inventory) or was claimed earlier. */
	Claimed,

	/** Nothing fit (full inventory): the draft stays pending, retry stays possible. */
	InventoryFull,

	/** Save failure / rejection: a readable error, NO fake completion, retry stays possible. */
	Failed
};

/**
 * M3-018: display state of the reward area of one finished CLEARED run. Built
 * by MakeRoomRewardViewModelFromDraft from the profile's pending draft
 * snapshot - the SAME draft every re-presentation shows (the roll happened
 * once at BeginReward time, the display never re-rolls). The claim state and
 * the status text are updated by ApplyRoomRewardClaimOutcome from the real
 * URewardService claim result (the UI reflects the actual save outcome).
 */
struct FRoomRewardViewModel
{
	/** True only when bound from a draft snapshot (the area stays hidden otherwise). */
	bool bValid = false;

	/** Business identity of the settlement this draft belongs to. */
	uint64 SettlementId = 0;

	/** XP granted by the settlement (design: 50 per cleared room). */
	int32 XP = 0;

	/** One display line per draft item (draft order; no invented entries). */
	TArray<FRoomRewardItemLine> Items;

	/** "Reward: XP <x>    <name> (<stats>)..."; the row text while pending. */
	FString RewardText;

	/** The five-state claim display state (Unclaimed right after a bind). */
	ERoomRewardClaimState ClaimState = ERoomRewardClaimState::Unclaimed;

	/** Readable claim feedback per state; empty while Unclaimed. */
	FString StatusText;
};

/**
 * Builds the reward display from one pending draft snapshot (pure function;
 * no world, no clock, no re-roll). Catalog may be null or may not know a
 * DefinitionId - the line degrades to the M3-011 placeholders while the
 * instance's own stats still display. The state starts Unclaimed.
 */
UEMMO_API FRoomRewardViewModel MakeRoomRewardViewModelFromDraft(
	const FPendingReward& Draft, const FItemDefinitionCatalog* Catalog);

/**
 * Pure mapping of one atomic claim outcome to the five display states:
 * Claimed/PartiallyClaimed/AlreadyClaimed -> Claimed (an AlreadyClaimed
 * answer is the persisted truth of an earlier claim, not a fake completion),
 * InventoryFull -> InventoryFull, every failure/rejection value -> Failed.
 */
UEMMO_API ERoomRewardClaimState DeriveRoomRewardClaimState(const FRewardClaimAtomicOutcome& Claim);

/**
 * Returns a copy of Base with one claim outcome applied (pure function): the
 * claim state derives via DeriveRoomRewardClaimState and the status text is
 * the readable per-state feedback (the full-bag prompt, the saved-claim line
 * or the failure reason). The item lines are the draft snapshot and never
 * change here - a failure keeps the ORIGINAL reward visible and retryable.
 */
UEMMO_API FRoomRewardViewModel ApplyRoomRewardClaimOutcome(
	const FRoomRewardViewModel& Base, const FRewardClaimAtomicOutcome& Claim);

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

	/**
	 * M3-018: fired by the Claim button (the HUD executes the real
	 * URewardService::ClaimPendingAtomic; the widget never claims and never
	 * re-rolls anything itself).
	 */
	FRoomResultActionRequested RewardClaimRequested;

	/** Enables/disables both buttons together (the visible anti-double-click half). */
	void SetActionButtonsEnabled(bool bNewEnabled);

	// ----- M3-018: settlement reward area ------------------------------------

	/**
	 * Fills the reward area from one draft snapshot view model (pure fill;
	 * works before the widget ever reaches a viewport). An invalid view model
	 * (defeat, no draft) collapses the whole area - no invented reward.
	 */
	void BindReward(const FRoomRewardViewModel& Reward);

	/** Flips the reward area to the Saving state (claim in flight; button disabled). */
	void SetRewardClaimSaving();

	/**
	 * Applies one real claim outcome: derives the state via the pure
	 * ApplyRoomRewardClaimOutcome and re-arms the Claim button exactly on the
	 * retryable failures (InventoryFull/Failed) - a committed claim keeps the
	 * button disabled (nothing left to claim).
	 */
	void ApplyRewardClaimOutcome(const FRewardClaimAtomicOutcome& Claim);

	/**
	 * M3-018: the Claim button's click path (public UFUNCTION so headless
	 * tests can drive the exact click entry, the UInventoryRowButton::
	 * HandleRowClicked precedent): the first click disables the button (the
	 * visible anti-double-click half) and broadcasts once; a click on the
	 * disabled button is dropped.
	 */
	UFUNCTION()
	void HandleClaimButtonClicked();

	// -- Read seams (tests and the HUD) ----------------------------------------

	const FRoomResultViewModel& PeekViewModel() const { return ViewModel; }
	UTextBlock* PeekHeadlineBlock() const { return HeadlineBlock; }
	UTextBlock* PeekSummaryBlock() const { return SummaryBlock; }
	UTextBlock* PeekRewardBlock() const { return RewardBlock; }
	UButton* PeekRetryButton() const { return RetryButton; }
	UButton* PeekReturnButton() const { return ReturnButton; }
	UTextBlock* PeekRetryLabel() const { return RetryLabel; }
	UTextBlock* PeekReturnLabel() const { return ReturnLabel; }

	// -- M3-018 read seams (tests and the HUD) ----------------------------------

	const FRoomRewardViewModel& PeekRewardViewModel() const { return RewardViewModel; }
	UButton* PeekClaimButton() const { return ClaimButton; }
	UTextBlock* PeekClaimLabel() const { return ClaimLabel; }
	UTextBlock* PeekRewardStatusBlock() const { return RewardStatusBlock; }

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	UFUNCTION()
	void HandleRetryButtonClicked();

	UFUNCTION()
	void HandleReturnButtonClicked();

	/** Re-fills every text from the current view model (reward row included). */
	void ApplyViewModelToControls();

	/** M3-018: applies the reward view model to the reward controls (texts, visibility, claim button). */
	void ApplyRewardToControls();

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

	// ----- M3-018: reward area controls ----------------------------------------

	UPROPERTY(Transient)
	TObjectPtr<UButton> ClaimButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ClaimLabel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> RewardStatusBlock;

	FRoomResultViewModel ViewModel;
	FRoomRewardViewModel RewardViewModel;
	bool bControlsBuilt = false;
};
