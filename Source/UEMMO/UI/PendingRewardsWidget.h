#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PendingRewardsWidget.generated.h"

struct FPendingReward;
struct FItemDefinitionCatalog;

class UBorder;
class UCanvasPanel;
class UScrollBox;
class UTextBlock;
class UVerticalBox;

/**
 * M3-018: the readable full-inventory prompt of the reward flow (the card's
 * "inventory full" line). One shared source so the result screen's claim
 * feedback and the pending list show the exact same text.
 */
UEMMO_API FString MakeRewardInventoryFullText();

/**
 * M3-018: the menu's pending-reward badge line: "Pending rewards: N" for
 * N > 0, empty for N <= 0 (no noise in a clean menu). The HUD draws this
 * line as canvas text while the game flow state is Menu (the minimal hook
 * point of this card; a future menu wiring task may move it into the menu
 * widget itself).
 */
UEMMO_API FString MakePendingRewardsBadgeText(int32 PendingCount);

/**
 * M3-018: pure display state of one pending reward draft row (the M3-011 row
 * rule: a missing/unknown definition degrades to readable placeholders and
 * never crashes; an item-less draft shows an explicit "(no items)" line).
 */
struct FPendingRewardRowViewModel
{
	/** Business identity of the settlement this draft belongs to (copied verbatim). */
	uint64 SettlementId = 0;

	/** "Settlement <id>" row identity text. */
	FString SettlementText;

	/** "XP <x>" pending XP text (the draft's stored XP, design 50). */
	FString XPText;

	/** "<name> (<stats>)" per pending item, joined; "(no items)" when empty. */
	FString ItemsText;
};

/**
 * M3-018: pure display state of the pending reward list (rows in the
 * profile's PendingRewards insertion order). StatusText carries the
 * full-inventory prompt while the caller reports a full bag, else empty.
 */
struct FPendingRewardListViewModel
{
	/** False for a default-constructed (never filled) view model. */
	bool bValid = false;

	/** One row per pending draft (insertion order). */
	TArray<FPendingRewardRowViewModel> Rows;

	/** True when no pending draft exists (the empty-state condition). */
	bool IsEmpty() const { return Rows.Num() == 0; }

	/** The full-inventory prompt while the bag is full, else empty. */
	FString StatusText;
};

/**
 * Builds one row from a draft snapshot (pure function; no world, no clock).
 * Catalog may be nullptr or simply not know a DefinitionId - every unresolved
 * item uses the "<unknown item>" placeholder while the instance's own stats
 * still display (the M3-011 rule).
 */
UEMMO_API FPendingRewardRowViewModel MakePendingRewardRowViewModel(
	const FPendingReward& Draft, const FItemDefinitionCatalog* Catalog);

/**
 * Builds the whole list display state (pure function). bInventoryFull is the
 * caller's read of the profile inventory (Count() >= Capacity) and only feeds
 * the StatusText prompt - the rows always mirror the passed drafts verbatim.
 */
UEMMO_API FPendingRewardListViewModel MakePendingRewardListViewModel(
	const TArray<FPendingReward>& Drafts, const FItemDefinitionCatalog* Catalog,
	bool bInventoryFull);

/**
 * M3-018: the pending reward list screen (settlement ids, pending XP and the
 * pre-generated item names of every unclaimed draft). Native-only UUserWidget
 * built in code inside NativeOnInitialized (the M2-012 RoomResultWidget /
 * M3-011 InventoryWidget precedent - no UMG asset, no external UI package).
 * BindPendingRewards fills the display from one snapshot of the profile's
 * PendingRewards; the full-bag state shows the shared
 * MakeRewardInventoryFullText() prompt so the player knows to free a slot and
 * claim again. The widget is a pure display surface: it never claims, never
 * mutates the profile and never re-rolls anything.
 */
UCLASS()
class UEMMO_API UPendingRewardsWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UPendingRewardsWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;

	/**
	 * Fills the list from one snapshot of the profile's pending drafts (pure
	 * fill; works before the widget ever reaches a viewport).
	 */
	void BindPendingRewards(const TArray<FPendingReward>& Drafts,
		const FItemDefinitionCatalog* Catalog, bool bInventoryFull);

	// -- Read seams (tests and the future menu wiring) --------------------------

	const FPendingRewardListViewModel& PeekViewModel() const { return ViewModel; }
	UTextBlock* PeekTitleBlock() const { return TitleBlock; }
	UTextBlock* PeekEmptyBlock() const { return EmptyBlock; }
	UTextBlock* PeekStatusBlock() const { return StatusBlock; }
	UScrollBox* PeekListScrollBox() const { return ListScrollBox; }

	/** Number of row text blocks actually built in the list (0 before a build). */
	int32 PeekRowCount() const;

	/** The row text block at the given insertion index (nullptr out of range). */
	UTextBlock* PeekRowText(int32 Index) const;

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	/** Applies the current view model to the controls (rows + empty + status). */
	void ApplyViewModelToControls();

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> RootCanvas;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> PanelBorder;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> EmptyBlock;

	UPROPERTY(Transient)
	TObjectPtr<UScrollBox> ListScrollBox;

	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> RowsBox;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusBlock;

	/** Row text blocks of the last applied view model (rebuilt per bind). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextBlock>> RowBlocks;

	FPendingRewardListViewModel ViewModel;
	bool bControlsBuilt = false;
};
