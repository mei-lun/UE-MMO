#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Blueprint/UserWidget.h"
// M3-012: FItemStats is a by-value member below and the equip/unequip result
// enums appear in the pure helpers' signatures; the Items headers are plain
// value types (no UObject, no engine coupling).
#include "../Items/ItemDefinition.h"
#include "../Items/EquipmentModel.h"
#include "Components/Button.h"
#include "InventoryWidget.generated.h"

struct FItemInstance;
struct FItemDefinitionCatalog;

class UBorder;
class UCanvasPanel;
class UImage;
class UScrollBox;
class UTextBlock;
class UTexture2D;
class UVerticalBox;

/** Fired by the Close button and the Esc key; the HUD executes the real dismiss. */
DECLARE_MULTICAST_DELEGATE(FInventoryCloseRequested);

/** M3-012: fired on every row click (the HUD re-arms its one-shot action guard). */
DECLARE_MULTICAST_DELEGATE(FInventorySelectionChanged);

/** M3-012: fired by the Equip button; the HUD executes the real equip path. */
DECLARE_MULTICAST_DELEGATE(FInventoryEquipRequested);

/** M3-012: fired by the Unequip button; the HUD executes the real unequip path. */
DECLARE_MULTICAST_DELEGATE(FInventoryUnequipRequested);

/** M3-012: native select event of one inventory row (carries the row's instance id). */
DECLARE_DELEGATE_OneParam(FOnInventoryRowSelected, const FGuid& /*InstanceId*/);

/**
 * M3-019: resolved icon display configuration of one equipment slot (the ONE
 * shared source the inventory rows AND the settlement reward lines read).
 * All textures are ENGINE BUILT-IN placeholder icons (no external asset, no
 * download; the registration and the placeholder status live in
 * SourceAssets/manifest.json and Docs/03); a formal icon pass belongs to
 * M3-H01.
 */
struct FInventorySlotIconConfig
{
	/** Soft object path of the configured placeholder texture; empty = no icon. */
	FString TexturePath;

	/** Short fallback label ("WPN"/"ARM"/"ACC"); "-" for an unknown slot. */
	FString Tag;
};

/**
 * M3-019: resolves the shared icon config of one slot. Only the three closed
 * enum values resolve a texture + tag; every other value degrades to an empty
 * path and the readable "-" tag (no slot is invented).
 */
UEMMO_API FInventorySlotIconConfig MakeInventorySlotIconConfig(EItemSlot Slot);

/**
 * M3-019: loads one configured row icon texture synchronously; nullptr on an
 * empty path or any load failure (the tag fallback trigger). Synchronous on
 * purpose: the config points at tiny engine built-in textures and a missing
 * texture must degrade to the tag within the same row build (no pop-in).
 */
UEMMO_API UTexture2D* LoadInventoryRowIconTexture(const FString& SoftObjectPath);

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
	 * M3-019: short slot tag of the row ("WPN"/"ARM"/"ACC"); "-" when the
	 * definition is missing or carries an out-of-enum slot (no slot invented).
	 * The SAME shared config resolves it for the settlement reward lines, so
	 * both surfaces read one icon source.
	 */
	FString SlotTag;

	/**
	 * M3-019: soft object path of the row's configured placeholder icon (the
	 * shared slot config); empty when no icon resolves. The row render loads
	 * it synchronously and falls back to the visible short tag on any failure.
	 */
	FString IconPath;

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
 * M3-012: pure display state of the equip stat-comparison preview. The
 * CURRENT row is the complete recalculation over the level base plus every
 * currently equipped instance; the AFTER row re-runs the SAME complete
 * recalculation with the selected item swapped into its slot. Both rows come
 * from FStatCalculator::Recalculate (the interface contract section 8 rule:
 * never incremental add/remove bookkeeping) - the preview is a pure display
 * value and never touches the real profile.
 */
struct FInventoryStatPreviewViewModel
{
	/** False for a default-constructed (never built) view model. */
	bool bValid = false;

	/** True when an item is selected; false keeps every line empty. */
	bool bHasSelection = false;

	/** "Current: Atk+<a> Def+<d> HP+<h>"; empty without a selection. */
	FString CurrentLine;

	/** "After equip: ..." (equals Current when the selection changes nothing). */
	FString AfterLine;

	/** "Delta: Atk+<d> Def+<d> HP+<h>" (signed) or "Delta: No change". */
	FString DeltaLine;
};

/**
 * M3-012: builds the comparison preview (pure function; no world, no profile
 * access - the caller passes copies). EquippedStats holds one stats row per
 * currently equipped instance. When the selection is already equipped the
 * AFTER row keeps EquippedStats unchanged (an equip of the same item is the
 * idempotent no-op, so nothing may change). Otherwise a slot occupant's row
 * (bSlotOccupied + ReplacedStats) is removed once and the selection's row is
 * added - mirroring exactly what the real Equip does to the mapping.
 */
UEMMO_API FInventoryStatPreviewViewModel MakeInventoryStatPreviewViewModel(
	const FItemStats& BaseStats, const TArray<FItemStats>& EquippedStats,
	bool bHasSelection, const FItemStats& SelectedStats, bool bSelectedAlreadyEquipped,
	bool bSlotOccupied, const FItemStats& ReplacedStats);

/** M3-012: readable text of an Equip result (every enum value names itself). */
UEMMO_API FString MakeEquipResultText(EEquipmentEquipResult Result);

/** M3-012: readable text of an Unequip result. */
UEMMO_API FString MakeUnequipResultText(EEquipmentUnequipResult Result);

/**
 * M3-012: readable refusal reason for a context-blocked equip request
 * (priority: missing profile > Running room > missing player); empty when
 * none blocks. The HUD shows this instead of silently dropping the request.
 */
UEMMO_API FString MakeInventoryEquipBlockText(bool bMissingProfile, bool bSessionRunning,
	bool bMissingPlayer);

/**
 * M3-012: one clickable inventory row. The M3-011 rows were plain text blocks
 * (read-only list); selecting an item for the stat preview needs a clickable
 * row, so each row is now a UButton carrying the instance id it displays. The
 * dynamic OnClicked cannot bind a per-row lambda (UE dynamic delegates bind
 * UFUNCTIONs only), so the button bridges its own click into the NATIVE
 * OnRowSelected event, which the list widget binds per row with the id.
 */
UCLASS()
class UEMMO_API UInventoryRowButton : public UButton
{
	GENERATED_BODY()

public:
	UInventoryRowButton();

	/** The stored instance this row displays (set at row build time). */
	FGuid InstanceId;

	/** Native select event; executed with InstanceId when the row is clicked. */
	FOnInventoryRowSelected OnRowSelected;

	/** Bridges the dynamic button click into the native select event. */
	UFUNCTION()
	void HandleRowClicked();
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
 *
 * M3-012 additions: rows are clickable (UInventoryRowButton) and select the
 * instance for the stat-comparison preview (Current vs After equip, computed
 * purely from the bound snapshot copies); Equip/Unequip buttons forward the
 * actions through plain delegates to the HUD, which owns the equipment model,
 * the one-shot anti-double-click guard and the Running gate. The widget never
 * touches the profile itself - it renders copies and broadcasts intents.
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

	// ----- M3-012: equip actions, selection and context ------------------------

	/** Fired on every row click (the HUD re-arms its one-shot action guard). */
	FInventorySelectionChanged SelectionChanged;

	/** Fired by the Equip button (the HUD executes the real equip path). */
	FInventoryEquipRequested EquipRequested;

	/** Fired by the Unequip button (the HUD executes the real unequip path). */
	FInventoryUnequipRequested UnequipRequested;

	/**
	 * M3-012: pushes the equip context the HUD owns (copies only): whether a
	 * profile exists, the level base stats row and whether a room run is
	 * currently Running (buttons disabled while it is - the visible half of
	 * the two-layer gate; the pawn's TryEquipStatBonus is the bottom half).
	 * Recomputes the preview and the button states.
	 */
	void SetEquipContext(bool bHasProfile, const FItemStats& BaseStats, bool bSessionRunning);

	/** M3-012: sets the status line (result text or refusal reason). */
	void SetInventoryStatusText(const FString& Text);

	/**
	 * M3-012: recomputes the preview from the bound snapshot copies plus the
	 * current selection/context and re-applies the button states. Public so
	 * the HUD can force a refresh after its guard refused a duplicate request
	 * (the disabled-by-click buttons must not stay stuck).
	 */
	void RefreshEquipPreviewAndActions();

	// -- Read seams (tests and the HUD) ----------------------------------------

	const FInventoryListViewModel& PeekViewModel() const { return ViewModel; }
	UTextBlock* PeekTitleBlock() const { return TitleBlock; }
	UTextBlock* PeekEmptyBlock() const { return EmptyBlock; }
	UScrollBox* PeekListScrollBox() const { return ListScrollBox; }
	UButton* PeekCloseButton() const { return CloseButton; }
	const FInventoryRefreshGuard& PeekRefreshGuard() const { return RefreshGuard; }
	UButton* PeekEquipButton() const { return EquipButton; }
	UButton* PeekUnequipButton() const { return UnequipButton; }
	UTextBlock* PeekPreviewBlock() const { return PreviewBlock; }
	UTextBlock* PeekStatusBlock() const { return StatusBlock; }
	const FInventoryStatPreviewViewModel& PeekPreview() const { return Preview; }
	const FGuid& PeekSelectedInstanceId() const { return SelectedInstanceId; }
	bool HasSelection() const { return bHasSelection; }

	/** Number of row controls actually built in the list (0 before a build). */
	int32 PeekRowCount() const;

	/** The row button at the given insertion index (nullptr out of range). */
	UButton* PeekRowButton(int32 Index) const;

	/**
	 * M3-019: the icon image of the row at the given insertion index (nullptr
	 * out of range or before a build). The brush carries the loaded placeholder
	 * texture while one resolves; the control stays built but COLLAPSED on the
	 * tag fallback (the paired tag block is the visible half then).
	 */
	UImage* PeekRowIconImage(int32 Index) const;

	/**
	 * M3-019: the short-tag text of the row at the given insertion index
	 * (nullptr out of range). Visible ONLY on the fallback (no configured icon
	 * resolved); collapsed while the icon image shows.
	 */
	UTextBlock* PeekRowIconTagText(int32 Index) const;

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	/** Applies the current view model to the controls (rows + empty state). */
	void ApplyViewModelToControls();

	/** M3-012: row click bridge (the row's own id arrives via OnRowSelected). */
	void HandleRowSelected(const FGuid& InstanceId);

	/** M3-012: re-applies the action-button enabled states from the context. */
	void UpdateEquipActionButtons();

	UFUNCTION()
	void HandleCloseClicked();

	UFUNCTION()
	void HandleEquipClicked();

	UFUNCTION()
	void HandleUnequipClicked();

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

	// ----- M3-012: equip panel controls ---------------------------------------

	UPROPERTY(Transient)
	TObjectPtr<UButton> EquipButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> EquipLabel;

	UPROPERTY(Transient)
	TObjectPtr<UButton> UnequipButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> UnequipLabel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> PreviewBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusBlock;

	// ----- M3-019: per-row icon controls (parallel to the row order) ----------

	UPROPERTY(Transient)
	TArray<TObjectPtr<UImage>> RowIconImages;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextBlock>> RowIconTags;

	FInventoryListViewModel ViewModel;
	FInventoryRefreshGuard RefreshGuard;
	bool bControlsBuilt = false;

	// ----- M3-012: selection and equip context (copies; no profile access) ----

	/** The snapshot the last bind/refresh presented (preview data source). */
	TArray<FItemInstance> BoundInstances;

	/** The definition source handed to the last bind/refresh (non-owning). */
	const FItemDefinitionCatalog* BoundCatalog = nullptr;

	/** The equipped-id set handed to the last bind/refresh. */
	TSet<FGuid> BoundEquippedIds;

	/** The row the user clicked (invalid until the first row click). */
	FGuid SelectedInstanceId;

	/** True once a row was clicked (the selection persists across refreshes). */
	bool bHasSelection = false;

	/** Level base stats row pushed by the HUD (zero until a context arrives). */
	FItemStats EquipBaseStats;

	/** True once a context with an existing profile was pushed. */
	bool bHasEquipContext = false;

	/** The HUD's Running push (buttons disabled while true). */
	bool bSessionRunning = false;

	/** The last built preview (empty until the first selection + context). */
	FInventoryStatPreviewViewModel Preview;
};
