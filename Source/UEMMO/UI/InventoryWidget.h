#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Blueprint/UserWidget.h"
#include "InventoryWidget.generated.h"

struct FItemInstance;
struct FItemDefinition;
struct FItemDefinitionCatalog;

class UBorder;
class UButton;
class UCanvasPanel;
class UScrollBox;
class UTextBlock;
class UVerticalBox;

/** Fired by the Close button and the Esc key; the HUD executes the real dismiss. */
DECLARE_MULTICAST_DELEGATE(FInventoryCloseRequested);

/**
 * M3-011: pure display state of one inventory row. Built by MakeInventoryRowViewModel
 * from the owned FItemInstance plus its (optional) shared FItemDefinition; the
 * definition may be missing (stale DefinitionId, no catalog bound) - the row then
 * shows readable placeholders and never crashes.
 */
struct FInventoryRowViewModel
{
	/** Business identity of the stored instance (copied verbatim). */
	FGuid InstanceId;

	/** Definition display name; "<unknown item>" when no definition resolves. */
	FString DisplayName;

	/**
	 * Slot label "Weapon"/"Armor"/"Accessory"; "-" when the definition is
	 * missing or carries an out-of-enum slot (no slot can be invented).
	 */
	FString SlotName;

	/**
	 * Instance stats line "Atk+<a> Def+<d> HP+<h>". All three attributes are
	 * always shown (zeros included) so a missing grant reads as an explicit 0;
	 * the values come from the INSTANCE's rolled stats (owned data), never
	 * from the definition's base row.
	 */
	FString StatsText;

	/** True when the instance id is in the equipped-id set the caller passed in. */
	bool bEquipped = false;
};

/**
 * M3-011: pure display state of the read-only inventory list. Rows follow the
 * inventory's insertion order and are capped at 30 (the design capacity);
 * an empty inventory keeps Rows empty (the widget shows the empty-state line).
 */
struct FInventoryListViewModel
{
	/** False for a default-constructed (never filled) view model. */
	bool bValid = false;

	/** One row per stored instance (insertion order, max 30). */
	TArray<FInventoryRowViewModel> Rows;

	/** True when the inventory had no items (the empty-state condition). */
	bool IsEmpty() const { return Rows.Num() == 0; }
};

/**
 * Builds one row from an instance and its definition (pure function; no world,
 * no clock). Definition may be nullptr: the row degrades to placeholders
 * ("<unknown item>" / slot "-") while the instance's own stats still display.
 */
UEMMO_API FInventoryRowViewModel MakeInventoryRowViewModel(
	const FItemInstance& Instance, const FItemDefinition* Definition, bool bEquipped);

/**
 * Builds the whole list display state (pure function). Catalog may be nullptr
 * or simply not know a DefinitionId - every unresolved row uses the placeholder
 * path of MakeInventoryRowViewModel. More than 30 instances are truncated to
 * the first 30 (insertion order).
 */
UEMMO_API FInventoryListViewModel MakeInventoryListViewModel(
	const TArray<FItemInstance>& Instances, const FItemDefinitionCatalog* Catalog,
	const TSet<FGuid>& EquippedInstanceIds);

/**
 * Pure fingerprint of one inventory snapshot (count, per-instance id,
 * definition id, level, stats and equipped flag). Two snapshots with the same
 * fingerprint render identically, so the widget can skip a rebuild (the
 * throttle of the refresh strategy; InventoryModel itself has no change event).
 */
UEMMO_API FString MakeInventorySnapshotFingerprint(
	const TArray<FItemInstance>& Instances, const TSet<FGuid>& EquippedInstanceIds);

/**
 * M3-011: the refresh throttle. AcceptSnapshot compares one fingerprint
 * against the last accepted one: the first call always rebuilds (no baseline
 * yet), a call with a DIFFERENT fingerprint rebuilds, and a call with the SAME
 * fingerprint is skipped and counted - the "no per-Tick full rebuild" rule as
 * a pure, testable state.
 */
class UEMMO_API FInventoryRefreshGuard
{
public:
	/** True = rebuild due (first or changed snapshot); false = identical (skip). */
	bool AcceptSnapshot(const FString& SnapshotFingerprint);

	/** Rebuilds performed so far (first bind included). */
	int32 GetPerformedCount() const { return PerformedCount; }

	/** Skipped identical refreshes so far. */
	int32 GetSkippedCount() const { return SkippedCount; }

private:
	FString LastFingerprint;
	bool bHasLastSnapshot = false;
	int32 PerformedCount = 0;
	int32 SkippedCount = 0;
};

/**
 * M3-011: the read-only inventory list screen (names, slots, stats, equipped
 * markers of up to 30 items). Native-only UUserWidget built in code inside
 * NativeOnInitialized (the M2-012 RoomResultWidget precedent - no UMG asset):
 * a bordered panel with a title, an explicit empty-state line, a scroll box
 * holding the row texts and a Close button. BindInventory fills the display
 * from one snapshot; RefreshIfChanged rebuilds the rows ONLY when the snapshot
 * fingerprint changed (no per-Tick rebuild). Esc (the widget is focusable) and
 * the Close button broadcast CloseRequested; the HUD owns the real dismissal
 * and the one-time input-focus switch.
 */
UCLASS()
class UEMMO_API UInventoryWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UInventoryWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;

	/**
	 * Esc while the screen owns the focus: broadcasts CloseRequested (the HUD
	 * restores the game input focus) and consumes the key.
	 */
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	/**
	 * Fills the display from one snapshot (pure fill; works before the widget
	 * ever reaches a viewport). Also records the refresh baseline: a following
	 * identical RefreshIfChanged is skipped.
	 */
	void BindInventory(const TArray<FItemInstance>& Instances, const FItemDefinitionCatalog* Catalog,
		const TSet<FGuid>& EquippedInstanceIds);

	/**
	 * Throttled refresh: rebuilds the rows only when the snapshot fingerprint
	 * differs from the last accepted one. True when rows were rebuilt.
	 */
	bool RefreshIfChanged(const TArray<FItemInstance>& Instances, const FItemDefinitionCatalog* Catalog,
		const TSet<FGuid>& EquippedInstanceIds);

	/** Fired by the Close button and the Esc key (the HUD executes the dismiss). */
	FInventoryCloseRequested CloseRequested;

	// -- Read seams (tests and the HUD) ----------------------------------------

	const FInventoryListViewModel& PeekViewModel() const { return ViewModel; }
	UTextBlock* PeekTitleBlock() const { return TitleBlock; }
	UTextBlock* PeekEmptyBlock() const { return EmptyBlock; }
	UScrollBox* PeekListScrollBox() const { return ListScrollBox; }
	UButton* PeekCloseButton() const { return CloseButton; }
	const FInventoryRefreshGuard& PeekRefreshGuard() const { return RefreshGuard; }

	/** Number of row controls actually built in the list (0 before a build). */
	int32 PeekRowCount() const;

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	/** Applies the current view model to the controls (rows + empty state). */
	void ApplyViewModelToControls();

	UFUNCTION()
	void HandleCloseClicked();

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
	TObjectPtr<UButton> CloseButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> CloseLabel;

	FInventoryListViewModel ViewModel;
	FInventoryRefreshGuard RefreshGuard;
	bool bControlsBuilt = false;
};
