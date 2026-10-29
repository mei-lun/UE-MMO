#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MapSelectWidget.generated.h"

class UBorder;
class UButton;
class UCanvasPanel;
class UGameFlowSubsystem;
class URoomDefinition;
class UTextBlock;
class UVerticalBox;

/** Fired after the flow ACCEPTED the enter request (the M3-030 HUD mount
 * dismisses the overlay; nothing is broadcast for a refused request). */
DECLARE_MULTICAST_DELEGATE(FMapMenuEnterAccepted);

/** M5-056: fired ONLY from the real enter-click handler (after the flow
 * accepted). The automation tests broadcast EnterAccepted directly and never
 * cross this bridge, so the gameplay boot loadout stays a production-only
 * side effect. */
DECLARE_MULTICAST_DELEGATE(FMapGameplayEnterAccepted);

/** M6-002: fired when a catalog row's own Enter button was clicked; RowIndex
 * addresses the menu's CatalogRows array (the free-select list order). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMapCatalogRowEnter, int32, RowIndex);

/**
 * M6-002: one dynamic free-select row of the map select menu. The row owns
 * its widgets (built by the menu's WidgetTree) and its own Enter button
 * binding: a UButton's OnClicked is a DYNAMIC delegate, so the click binds
 * this row instance's own UFUNCTION and forwards the request with the row
 * index through RowEnterRequested - the per-row identity travels with the
 * click, no per-row handler boilerplate on the menu.
 *
 * The row is a plain UObject holder (not a UUserWidget): the menu constructs
 * every widget through its own WidgetTree and the holder only wires identity
 * and the click routing. RowDefinition is the exact catalog entry the row
 * enters (the M6-001 catalog order); RecordText carries the M6-003 clear
 * record line ("cleared x N" / "not cleared").
 */
UCLASS()
class UEMMO_API UMapSelectCatalogRow : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Wires one built row: the definition this row enters, its index in the
	 * menu's row array, and the widgets the menu constructed for it. Binds the
	 * button's click to this instance (per-instance AddDynamic - the standard
	 * dynamic-delegate per-row identity pattern).
	 */
	void SetupRow(const URoomDefinition* Definition, int32 InRowIndex,
		UBorder* InRowPanel, UTextBlock* InRowText, UButton* InRowButton, UTextBlock* InRowButtonLabel);

	/** The catalog entry this row enters (null only before SetupRow). */
	const URoomDefinition* GetRowDefinition() const { return RowDefinition.Get(); }

	/** The row's index in the owning menu's CatalogRows array. */
	int32 GetRowIndex() const { return RowIndex; }

	/** The visible guard half: enables/disables this row's Enter button. */
	void SetRowButtonEnabled(bool bNewEnabled) const;

	FOnMapCatalogRowEnter RowEnterRequested;

	// -- Read seams (tests) ------------------------------------------------------

	UBorder* PeekRowPanel() const { return RowPanel.Get(); }
	UTextBlock* PeekRowText() const { return RowText.Get(); }
	UButton* PeekRowButton() const { return RowButton.Get(); }

private:
	UFUNCTION()
	void HandleRowClicked();

	/** The catalog entry this row enters (weak: the catalog owns the asset). */
	TWeakObjectPtr<const URoomDefinition> RowDefinition;

	int32 RowIndex = -1;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> RowPanel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> RowText;

	UPROPERTY(Transient)
	TObjectPtr<UButton> RowButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> RowButtonLabel;
};

/**
 * M3-017: the map select menu (native-only UUserWidget, same code-built
 * pattern as M2-012's URoomResultWidget - no UMG asset, no external UI
 * package). M6-002 (the user's free-select decision, 2026-10-29): the menu's
 * level list is the M6-001 catalog - one dynamic row per entry (DisplayName
 * with the RoomId fallback, plus the M6-003 clear record as a read-only
 * status line; NOTHING is locked or gated), each row entering through the
 * same EnterRoom chain (the bLoading state guard is the machine half, the
 * disabled buttons the visible half; a failed open shows the flow's error
 * line and re-arms the rows so the entry stays retryable). The row list
 * rebuilds on every BindMenu presentation (the catalog and the records may
 * have changed). The M5-018A SystemTestRoom row stays the appended extra
 * destination, and the M3-030 enter button (the HUD mount's override-or-
 * selectable face with the M5-056 gameplay loadout bridge) stays unchanged
 * above the list. The settings/volume row is a plain text placeholder.
 *
 * The widget resolves the flow subsystem through its owning game instance, so
 * it works headless (automation creates it in a plain game world) and inside
 * the menu HUD wiring without extra plumbing.
 */
UCLASS()
class UEMMO_API UMapSelectWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UMapSelectWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;

	/**
	 * Arms one fresh menu presentation (the BindResult precedent of the M2-012
	 * result screen): builds the control tree when NativeOnInitialized has not
	 * run yet (engine 5.8 runs it on CreateWidget only with a valid player
	 * context), clears the error line, enables the enter buttons and REBUILDS
	 * the catalog row list (M6-002: the free-select surface always mirrors the
	 * current catalog and clear records). The menu wiring calls this on every
	 * presentation.
	 */
	void BindMenu();

	/**
	 * The enter button's click path: while the flow cannot accept an entry
	 * (a load in flight or a room already active) the button stays disabled
	 * and nothing is requested; otherwise the flow's EnterRoom runs for the
	 * override target (M3-030) or the selectable definition. Success keeps
	 * the button disabled and clears the error line (the room world takes
	 * over); failure shows the flow's recorded error and re-enables the
	 * button (retry stays possible).
	 */
	void HandleEnterClicked();

	/**
	 * M3-030: the presentation-level enter target override. The production HUD
	 * mount presents this widget over the boot/wave-combat world and sets the
	 * flow's combat-room definition here, so the enter button opens the room
	 * the world's trigger chain owns (NOT the raw selectable entry's
	 * training-map identity). A null definition (the default) restores the
	 * M3-017 selectable-entry behavior unchanged.
	 */
	void SetEnterRoomDefinition(const URoomDefinition* Definition);

	/**
	 * Shows Message on the error line; an empty Message collapses the line.
	 */
	void SetErrorText(const FString& Message);

	/** Enables/disables the enter button (the visible guard half). */
	void SetEnterButtonEnabled(bool bNewEnabled);

	/** M3-030: broadcast exactly once per ACCEPTED enter request. */
	FMapMenuEnterAccepted EnterAccepted;

	/** M5-056: real-click-only bridge for the gameplay boot loadout. */
	FMapGameplayEnterAccepted GameplayEnterAccepted;

	// -- Read seams (tests and the future menu HUD wiring) -----------------------

	UTextBlock* PeekTitleBlock() const { return TitleBlock.Get(); }
	UTextBlock* PeekSettingsPlaceholderBlock() const { return SettingsPlaceholderBlock.Get(); }
	UButton* PeekEnterButton() const { return EnterButton.Get(); }
	UTextBlock* PeekEnterLabel() const { return EnterLabel.Get(); }
	UTextBlock* PeekErrorBlock() const { return ErrorBlock.Get(); }

	/** M6-002: how many free-select rows the current presentation carries. */
	int32 PeekCatalogRowCount() const { return CatalogRows.Num(); }

	/** M6-002: row Index of the current presentation (null outside [0, count)). */
	UMapSelectCatalogRow* PeekCatalogRow(int32 Index) const
	{
		return CatalogRows.IsValidIndex(Index) ? CatalogRows[Index].Get() : nullptr;
	}

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	/** Null-safe flow resolution through the owning game instance. */
	UGameFlowSubsystem* GetGameFlowSubsystem() const;

	/** Null-safe profile resolution through the owning game instance. */
	class UProfileSubsystem* GetProfileSubsystem() const;

	/**
	 * M6-002: rebuilds the free-select row list from the level catalog (one
	 * row per entry, catalog order, each text carrying the DisplayName/RoomId
	 * and the read-only clear record). Called by BindMenu on every
	 * presentation; without a flow the list stays empty (the legacy faces
	 * keep their own behavior).
	 */
	void RebuildCatalogRows();

	/**
	 * M6-002: the per-row click path (the row forwarded its index). The same
	 * guard chain as the system test room's handler: the flow must be in the
	 * Menu state with no load in flight (the machine half), all enter buttons
	 * disable for the attempt (the visible half), the row's definition enters
	 * through the flow, and success broadcasts the accepted chain (including
	 * the M5-056 gameplay bridge - a catalog-row click is a real menu click
	 * into gameplay); failure shows the flow's error and re-arms the rows.
	 */
	UFUNCTION()
	void HandleCatalogRowEnterRequested(int32 RowIndex);

	UFUNCTION()
	void HandleEnterButtonClicked();

	/** M5-018A: enters the system test room (the appended extra destination). */
	UFUNCTION()
	void HandleSystemTestRoomClicked();

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> RootCanvas;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> PanelBorder;

	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> Stack;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleBlock;

	/** M6-002: the free-select list container (rows rebuilt per presentation). */
	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> CatalogListBox;

	/** M6-002: the current presentation's row holders (widget owners). */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMapSelectCatalogRow>> CatalogRows;

	/** M5-018A: the appended system test destination row. */
	UPROPERTY(Transient)
	TObjectPtr<UBorder> SystemTestRoomRow;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> SystemTestRoomBlock;

	/** M5-018A: the system test room's enter button. */
	UPROPERTY(Transient)
	TObjectPtr<UButton> SystemTestRoomEnterButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> SystemTestRoomEnterLabel;

	UPROPERTY(Transient)
	TObjectPtr<UButton> EnterButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> EnterLabel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ErrorBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> SettingsPlaceholderBlock;

	/** M3-030: the presentation-level enter target (weak; null = selectable). */
	TWeakObjectPtr<const URoomDefinition> EnterRoomDefinitionOverride;

	bool bControlsBuilt = false;
};
