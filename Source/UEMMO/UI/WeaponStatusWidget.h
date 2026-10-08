#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WeaponStatusWidget.generated.h"

// M5-034: plain value types used through pointers in the signatures below;
// the complete headers are included by the .cpp only (lean UI header).
struct FWeaponBindingRecord;
struct FWeaponDefinition;

class UBorder;
class UCanvasPanel;
class UTextBlock;
class UVerticalBox;

/**
 * M5-034: the closed display-state vocabulary of the weapon status panel.
 * Every state names one visible outcome of the REAL mount (never invented);
 * ConfigFailure covers the explicit illegal-configuration shapes (catalog
 * unavailable, unresolved definition, an equipped weapon item that no weapon
 * behavior is bound to). Only grows, never renumbers.
 */
enum class EWeaponStatusState : uint8
{
	/** No active binding (and no unbound weapon item expected). */
	NoWeapon = 0,
	/** A weapon is bound and can act (melee ready, or magazine holds rounds). */
	Ready = 1,
	/** Ranged weapon bound, magazine empty, reserve still available (T reloads). */
	EmptyMagazine = 2,
	/** A reload window is open on the active instance. */
	Reloading = 3,
	/** Ranged weapon bound, magazine empty AND the reserve pool is empty. */
	ReserveEmpty = 4,
	/** Explicit illegal-configuration state (catalog error / unresolved definition / unbound weapon item). */
	ConfigFailure = 5,
};

/** M5-034: readable text of one state (every enum value names itself). */
UEMMO_API FString MakeWeaponStatusStateText(EWeaponStatusState State);

/**
 * M5-034: the read-only snapshot of the REAL weapon mount, pushed by the HUD
 * (never by the widget itself). Every pointer is non-owning and may be null:
 * the pure view model turns nulls into the explicit display states, never a
 * crash and never invented data. The UI layer never writes through this
 * struct - no ammo grant, no binding change (the card's display-only rule).
 */
struct FWeaponStatusInputs
{
	/** The active binding record (null = no weapon bound). Its LoadedRounds /
		MagazineCapacity pair is the REAL ammo readout - the M5-019 fire book
		the shots consume (the M5-021 model slot stays the reload ledger and
		reads 0 between refills). */
	const FWeaponBindingRecord* Binding = nullptr;

	/** The definition resolved from the mounted catalog (null with a binding = config failure). */
	const FWeaponDefinition* Definition = nullptr;

	/** Reserve rounds of the bound weapon's ammo kind; -1 = no pool registered. */
	int32 ReserveRounds = -1;

	/** Remaining seconds of an open reload window on the caller's clock (0 = none). */
	double RemainingReloadSeconds = 0.0;

	/** The mount's explicit catalog-error latch (refused mount / catalog unavailable). */
	bool bCatalogError = false;

	/**
	 * True when the equipment model has a weapon-slot occupant but the mount
	 * holds no active binding - the production shape of a weapon item without
	 * a weapon mapping (the illegal-configuration state must be visible).
	 */
	bool bEquippedItemUnbound = false;
};

/**
 * M5-034: pure display state of the weapon status panel (no world, no clock,
 * no mount access - the HUD copies everything in). StatusLine carries the
 * readable outcome; WeaponLine/AmmoLine/AbilitiesLine carry the definition
 * name, the type, the loaded/reserve rounds and the available abilities.
 */
struct FWeaponStatusViewModel
{
	/** False for a default-constructed (never built) view model. */
	bool bValid = false;

	/** The closed display state (drives the status color). */
	EWeaponStatusState State = EWeaponStatusState::NoWeapon;

	/** "<weapon id> | <type>" (type text from the fire mode; "-" without a definition). */
	FString WeaponLine;

	/** "Loaded <l>/<cap> | Reserve <r>" for ranged weapons; "-" for melee/none. */
	FString AmmoLine;

	/** Abilities of the bound weapon (RPM/pellets/burst/spread or the melee chain); "-" when none. */
	FString AbilitiesLine;

	/** Readable status text (the State's outcome, reload seconds included). */
	FString StatusLine;
};

/**
 * M5-034: builds the display state from the real-mount snapshot (pure
 * function). State precedence: catalog error > unbound weapon item > no
 * binding > unresolved definition > reloading > reserve empty > empty
 * magazine > ready. Melee bindings with a resolved definition are Ready.
 */
UEMMO_API FWeaponStatusViewModel MakeWeaponStatusViewModel(const FWeaponStatusInputs& Inputs);

/**
 * M5-034: deterministic fingerprint of everything the render depends on
 * (state inputs, ids, rounds, quantized reload remainder, ability fields).
 * Two snapshots with the same fingerprint render identically - the refresh
 * gate skips the rebuild.
 */
UEMMO_API FString MakeWeaponStatusSnapshotFingerprint(const FWeaponStatusInputs& Inputs);

/**
 * M5-034: the refresh throttle (the FInventoryRefreshGuard pattern): the
 * first call always rebuilds (no baseline yet), a call with a DIFFERENT
 * fingerprint rebuilds, a call with the SAME fingerprint is skipped and
 * counted - "no per-frame full rebuild" as a pure, testable state.
 */
class UEMMO_API FWeaponStatusRefreshGuard
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
 * M5-034: the native weapon status panel embedded in the inventory screen
 * (the M3-011 code-built widget precedent - no UMG asset). Display-only: it
 * renders the REAL mount state the HUD pushes through BindWeaponStatus /
 * RefreshIfChanged and owns no actions, no close button and no key handling
 * (the inventory screen's Close/Esc path dismisses the whole screen - one
 * input-focus switch, one dismissal). The HUD is the only data source: the
 * panel never reads the mount itself and never initializes or grants ammo.
 */
UCLASS()
class UEMMO_API UWeaponStatusWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UWeaponStatusWidget(const FObjectInitializer& ObjectInitializer);

	virtual void NativeOnInitialized() override;

	/**
	 * Fills the display from one snapshot (pure fill; works before the widget
	 * reaches a viewport). Also records the refresh baseline: a following
	 * identical RefreshIfChanged is skipped.
	 */
	void BindWeaponStatus(const FWeaponStatusInputs& Inputs);

	/**
	 * Throttled refresh: rebuilds the display only when the snapshot
	 * fingerprint differs from the last accepted one. True when rebuilt.
	 */
	bool RefreshIfChanged(const FWeaponStatusInputs& Inputs);

	// ----- Read seams (tests and the HUD) --------------------------------------

	const FWeaponStatusViewModel& PeekViewModel() const { return ViewModel; }
	UTextBlock* PeekWeaponBlock() const { return WeaponBlock; }
	UTextBlock* PeekAmmoBlock() const { return AmmoBlock; }
	UTextBlock* PeekAbilitiesBlock() const { return AbilitiesBlock; }
	UTextBlock* PeekStatusBlock() const { return StatusBlock; }
	const FWeaponStatusRefreshGuard& PeekRefreshGuard() const { return RefreshGuard; }

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	/** Applies the current view model to the controls (texts + status color). */
	void ApplyViewModelToControls();

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> RootCanvas;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> PanelBorder;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> WeaponBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> AmmoBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> AbilitiesBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StatusBlock;

	FWeaponStatusViewModel ViewModel;
	FWeaponStatusRefreshGuard RefreshGuard;
	bool bControlsBuilt = false;
};
