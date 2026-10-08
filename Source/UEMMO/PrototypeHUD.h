#pragma once
#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "GameFramework/HUD.h"
#include "UI/DamageNumberModel.h"
#include "UI/RoomResultWidget.h"
// M3-011: FInventoryListViewModel is a by-value member below, so the widget
// header (its owner) must be complete here; same pattern as RoomResultWidget.
#include "UI/InventoryWidget.h"
// M3-012: the HUD-owned equipment model and the one-shot action guard are
// by-value members (plain structs).
#include "Items/EquipmentModel.h"
// M3-011: the debug display catalog is also a by-value member (plain struct).
#include "Items/ItemDefinition.h"
#include "PrototypeHUD.generated.h"

class AActor;
class APrototypeCharacter;
class ATrainingEnemy;
class UAttackDefinition;
class UCombatComponent;
class UEnemyDefinition;
class UGameFlowSubsystem;
class UHealthComponent;
class UMapSelectWidget;
class UProfileSaveService;
class UProfileSubsystem;
class URoomDefinition;
class URoomSessionSubsystem;
class URewardService;
struct FCombatHit;
struct FRoomResult;

/**
 * M3-026: one tracked enemy health bar entry. Weak references only: a
 * destroyed enemy or component reads null and the entry is dropped by the
 * prune pass before every read (never a stale dereference, the M1-035
 * weak-reference contract).
 */
struct FEnemyBarEntry
{
	/** The enemy actor the bar anchors to (weak). */
	TWeakObjectPtr<AActor> Enemy;

	/** The enemy's health pool the bar reads per drawn frame (weak). */
	TWeakObjectPtr<UHealthComponent> Health;
};

UCLASS()
class UEMMO_API APrototypeHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;

    /**
     * M1-028: F1 press entry (APrototypeCharacter binds F1 -> here through the
     * possessing player controller's HUD). Flips the debug overlay flag only;
     * combat state, queries and damage are untouched.
     */
    void ToggleCombatDebugOverlay() { bCombatDebugOverlayEnabled = !bCombatDebugOverlayEnabled; }

    /** M1-028: explicit overlay switch (console entry / tests). */
    void SetCombatDebugOverlayEnabled(bool bNewEnabled) { bCombatDebugOverlayEnabled = bNewEnabled; }

    /** M1-028: current overlay switch state (default off). */
    bool IsCombatDebugOverlayEnabled() const { return bCombatDebugOverlayEnabled; }

    /**
     * M1-028 console entry for headless verification (the UPlayer::Exec chain
     * reaches the HUD; gameplay never calls this). Mode 0 turns the overlay
     * off, mode 1 on, mode 2 turns it on and stages a frozen demo strike
     * (start light_01, freeze the combat clock inside its active window) so an
     * offscreen-rendered screenshot can catch a running frame plus the shared
     * hit box. The strike timers fire only in this debug path.
     */
    UFUNCTION(Exec)
    void UEMMODebugCombatOverlay(int32 Mode = 1);

    /**
     * M1-028: resolves the cached player/target references used by the debug
     * overlay (called per drawn frame while the overlay is on; public so the
     * world-less automation tests can drive it). Weak-ref contract: a stale
     * cache entry reads null, is dropped and re-resolved; both iterators run
     * only while the cache is invalid and are filtered to the exact classes,
     * so the steady state performs no world scan at all.
     */
    void RefreshDebugReferences();

    /** Read seams (DrawHUD text lines + tests): stale weak refs read null. */
    const APrototypeCharacter* PeekDebugPlayer() const { return DebugPlayer.Get(); }
    const ATrainingEnemy* PeekDebugTarget() const { return DebugTarget.Get(); }

    /**
     * M1-035: (re)binds the damage-number / combo feed to Source's
     * OnHitConfirmed; null unbinds. Unbinding a stale source is safe (the old
     * component is only touched while its weak reference still reads valid).
     * Public so world-less tests can drive the exact subscription the per-frame
     * refresh uses.
     */
    void BindDamageFeed(UCombatComponent* Source);

    /**
     * M1-035: per-drawn-frame feed wiring. M3-026: the enemy health bars are
     * production HUD and share this feed, so the refresh is now public - the
     * exact entry DrawHUD calls every drawn frame (tests drive the same
     * subscription).
     */
    void RefreshDamageFeedBinding();

    /** M1-035 test seam: live damage-number count of the HUD pool. */
    int32 PeekDamageNumberCount() { return DamageNumbers.Num(); }

    /** M1-035 test seam: read access to the HUD pool (prune first for the live set). */
    const FDamageNumberPool& PeekDamageNumbers() const { return DamageNumbers; }

    /** M1-035 test seam: combo value at an explicitly injected clock. */
    int32 PeekComboCount(double NowSeconds) const { return ComboCounter.EvaluateCombo(NowSeconds); }

    // ----- M2-012: room result screen ---------------------------------------

    /**
     * M2-012: registers the definitions the Retry button hands to
     * URoomRetryService::RetryRoom. The future room-catalog task will wire
     * the real assets; until then only explicit registration (tests / the
     * debug staging) provides a context - a missing context refuses the
     * retry with a diagnostic and keeps the screen up.
     */
    void SetRoomRetryContext(const URoomDefinition* RoomDef, UEnemyDefinition* EnemyDef);

    /**
     * M2-012: the retry path behind the Retry button (and the debug/test
     * driver): the one-shot guard drops the duplicate of a fast double
     * click, the screen is dismissed first (input focus restored) and the
     * real URoomRetryService::RetryRoom restarts the failed run.
     */
    void HandleRetryRequested();

    /** M2-012: the return path behind the Return button: guard, dismiss, LeaveRoom. */
    void HandleReturnRequested();

    /**
     * M2-012: OnRunEnded handler (bound in BeginPlay through the world's
     * session): fills the view model from the session's real terminal result,
     * re-arms the action guards and presents the result screen. The M1 debug
     * overlay and the M1-035 damage feed are untouched by all of this.
     */
    void HandleRunEnded(const FRoomResult& Result);

    /**
     * M2-012 debug staging console entry (headless render evidence only;
     * gameplay never calls this). Mode 0 dismisses the screen, mode 1 stages
     * one real run to Cleared (StartRoom + BeginWaves + the session's own
     * MarkCleared), mode 2 to Failed (FailRun), mode 3 drives the real retry
     * request path and mode 4 the real return path. The staged run uses a
     * transient definition double (the M2-007+ test precedent) and registers
     * it as the retry context, so the staged retry is fully real. The session
     * clock is injected on the real SetSessionClockSeconds entry (the game
     * frame driver is a later task, so no other injection exists in-game).
     */
    UFUNCTION(Exec)
    void UEMMODebugRoomResult(int32 Mode = 1);

    /** M2-012 test seam: a result widget is created and bound (headless-safe). */
    bool HasRoomResultScreen() const { return ResultWidgetPtr.IsValid(); }

    /** M2-012 test seam: the view model filled by the last HandleRunEnded. */
    const FRoomResultViewModel& PeekRoomResultViewModel() const { return RoomResultViewModel; }

    /** M2-012 test seam: pure input-focus state of the result screen flow. */
    const FRoomResultInputFocusTracker& PeekRoomResultInputFocus() const { return ResultInputFocus; }

    // ----- M3-011: read-only inventory list screen ---------------------------

    /**
     * M3-011 debug staging console entry (headless render evidence and manual
     * browsing only; gameplay never calls this). Mode 0 dismisses the screen
     * and restores the game input focus, mode 1 stages a small starter
     * inventory once (debug-only; production entries NewProfile/TryAdd) and
     * opens the list, mode 2 fills the staged inventory up to the 30-slot
     * capacity and opens/refreshes the list (the scroll + throttle evidence).
     */
    UFUNCTION(Exec)
    void UEMMODebugInventory(int32 Mode = 1);

    /**
     * M3-011: the HUD-side refresh entry the later change points (reward
     * claim, equip UI) call when the profile inventory changed. No-op while
     * the screen is not presented; throttled (fingerprint-gated) otherwise,
     * so identical snapshots never rebuild the rows.
     */
    void RefreshInventoryScreen();

    /** M3-011: the real close path behind the Close button and the Esc key. */
    void HandleInventoryCloseRequested();

    /** M3-011 debug-only seam: replaces the equipped-id set the list marks. */
    void SetEquippedInventoryIds(const TSet<FGuid>& Ids);

    /** M3-011 test seam: the inventory widget is created and presented. */
    bool HasInventoryScreen() const { return InventoryWidgetPtr.IsValid(); }

    /** M3-011 test seam: the view model filled by the last presentation. */
    const FInventoryListViewModel& PeekInventoryViewModel() const { return InventoryViewModel; }

    /** M3-011 test seam: pure input-focus state of the inventory flow. */
    const FRoomResultInputFocusTracker& PeekInventoryInputFocus() const { return InventoryInputFocus; }

    // ----- M3-012: inventory equip actions ------------------------------------

    /**
     * M3-012: the real equip request path behind the inventory screen's Equip
     * button (and the tests' direct driver, the M2-012 HandleRetryRequested
     * precedent): the one-shot guard drops a fast double click, the shared
     * pre-checks refuse a blocked context with a readable reason, then the
     * equipment model equips the selected instance and the fresh bonus row is
     * pushed through the pawn's TryEquipStatBonus (the bottom Running gate).
     */
    void HandleInventoryEquipRequested();

    /** M3-012: the real unequip request path behind the Unequip button. */
    void HandleInventoryUnequipRequested();

    /** M3-012 test seam: the HUD-owned equipment slot mapping. */
    const FEquipmentModel& PeekInventoryEquipment() const { return InventoryEquipment; }

    /** M3-012 test seam: the one-shot guard behind both action buttons. */
    const FRoomResultActionGuard& PeekInventoryActionGuard() const { return InventoryActionGuard; }

    /** M3-012 test seam: the preview state of the presented screen. */
    const FInventoryStatPreviewViewModel& PeekInventoryPreview() const { return InventoryPreview; }

    /** M3-012 test seam: the presented inventory widget itself (weak-safe; null when dismissed). */
    UInventoryWidget* PeekInventoryWidget() const { return InventoryWidgetPtr.Get(); }

    // ----- M5-034: weapon status presentation -----------------------------------

    /**
     * M5-034: the production open/close entry behind the pawn's inventory-
     * toggle key (I): an open screen dismisses through the REAL close path
     * (input focus restored), a closed one presents. No fire, no ammo, no
     * combat state touched here - the weapon mount's own menu-open gate
     * refuses fire while the screen holds the input.
     */
    void ToggleInventoryScreen();

    /**
     * M5-034: the refresh entry the presented screen's status panel reads
     * from: resolves the local pawn's REAL weapon mount (binding, magazine,
     * reserve, reload window, catalog error) and pushes the fingerprint-
     * gated snapshot into the embedded panel. No-op while the screen is not
     * presented; DrawHUD calls this per drawn frame (the reload countdown
     * advances) and the equip paths call it after the mount refresh.
     */
    void RefreshWeaponStatusScreen();

    // ----- M3-018: settlement reward claim ------------------------------------

    /**
     * M3-018: the claim request path behind the result screen's Claim button
     * (and the tests' direct driver, the M2-012 HandleRetryRequested
     * precedent): the one-shot guard drops the duplicate of a fast double
     * click, the claim runs through URewardService::ClaimPendingAtomic (the
     * M3-016 atomic commit; the UI reflects the RETURNED outcome only), and a
     * retryable failure re-arms the Claim button with a readable error.
     */
    void HandleRewardClaimRequested();

    /** M3-018 test seam: the reward area is bound from a pending draft. */
    bool HasRoomRewardDraft() const { return RoomRewardViewModel.bValid; }

    /** M3-018 test seam: the reward view model of the current presentation. */
    const FRoomRewardViewModel& PeekRoomRewardViewModel() const { return RoomRewardViewModel; }

    /** M3-018 test seam: the one-shot guard behind the Claim request path. */
    const FRoomResultActionGuard& PeekRewardClaimGuard() const { return RewardClaimGuard; }

    // ----- M3-026: enemy health bar tracking ------------------------------------

    /**
     * M3-026 test seam and the draw path's read entry: the tracked enemy bar
     * count AFTER the prune pass dropped destroyed enemies and dead (HP<=0)
     * pools, so every read reports the live set.
     */
    int32 GetTrackedEnemyCount();

    /** M3-026 test seam: the tracked enemy actor at Index (null when out of range or stale). */
    const AActor* PeekTrackedEnemy(int32 Index);

    /** M3-026 test seam: the tracked health pool at Index (null when out of range or stale). */
    const UHealthComponent* PeekTrackedHealth(int32 Index);

    /**
     * M3-027: registers/upserts one enemy as a tracked bar entry outside the
     * hit path - AMeleeEnemy::BeginPlay registers every spawned room enemy,
     * so a bar exists from spawn instead of from the first accepted hit (the
     * fourth playtest round read the hit-only appearance as "no health bar").
     * Same upsert/cap rules as the hit-driven path: an already tracked enemy
     * only refreshes its pool, the 3-entry room cap refuses further entries,
     * a target without a health pool is ignored, and every read prunes
     * destroyed/dead entries first.
     */
    void TrackEnemyBarActor(AActor* Enemy);

    // ----- M3-030: boot menu mount (the full game-flow loop) -------------------

    /**
     * M3-030: re-presents the game-flow surface for the CURRENT flow state:
     * the map select overlay is presented exactly while the flow is in the
     * menu state (the boot surface, and the surface after a result-screen
     * return) and dismissed otherwise. Idempotent; the presentation follows
     * the M2-012 result-screen pattern (headless-safe prepared-without-
     * presentation when no viewport/player controller exists).
     */
    void RefreshMenuPresentation();

    /** M3-030 test seam: the menu overlay is presented (created and armed). */
    bool HasMenuScreen() const { return MenuWidgetPtr.IsValid(); }

    /** M3-030 test seam: the presented menu widget itself (null when dismissed). */
    UMapSelectWidget* PeekMenuWidget() const { return MenuWidgetPtr.Get(); }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    /** M2-012: binds the world session's OnRunEnded (BeginPlay; once). */
    void BindRoomSessionEvents();

    /** M2-012: drops the OnRunEnded binding (EndPlay; idempotent). */
    void UnbindRoomSessionEvents();

    /** M2-012: creates the native result widget once and wires its delegates. */
    bool EnsureResultWidget();

    /** M2-012: presents the bound screen (viewport + one-time input capture). */
    void ShowRoomResultScreen();

    /** M2-012: dismisses the screen and restores the game input focus. */
    void HideRoomResultScreen();

    /** M2-012: the capture half of the one-time input switch (tracker-gated). */
    void ApplyResultScreenInputCapture();

    /** M2-012: the restore half of the one-time input switch (tracker-gated). */
    void ApplyResultScreenInputRestore();

    /** M2-012: local player pawn (controller character first, then one iterator pass). */
    APrototypeCharacter* ResolveLocalPlayer();

    /** M2-012: builds the transient debug staging definitions (exec path only). */
    bool EnsureStageDefinitions();

    // ----- M3-011: inventory screen internals ---------------------------------

    /** M3-011: creates the native inventory widget once (headless-safe). */
    bool EnsureInventoryWidget();

    /** M3-011: fills from the profile inventory and presents the screen. */
    void ShowInventoryScreen();

    /** M3-011: dismisses the screen and restores the game input focus. */
    void HideInventoryScreen();

    /** M3-011: the capture half of the one-time input switch (tracker-gated). */
    void ApplyInventoryInputCapture();

    /** M3-011: the restore half of the one-time input switch (tracker-gated). */
    void ApplyInventoryInputRestore();

    /** M3-011: the profile subsystem of the world's game instance (weak-safe). */
    UProfileSubsystem* ResolveProfileSubsystem();

    /** M3-011: debug-only staging (profile + starter items + display catalog). */
    bool EnsureInventoryStaging();

    // ----- M3-012: inventory equip internals -----------------------------------

    /**
     * M3-012: shared body of the two request paths (bEquip decides the verb):
     * one-shot guard, context pre-checks (profile / Running / player), the
     * equipment model mutation, the bonus push through the pawn entry and the
     * success tail (markers + throttled list refresh + context push).
     */
    void HandleInventoryEquipAction(bool bEquip);

    /** M3-012: binds the model's catalog + inventory guard (idempotent). */
    bool EnsureInventoryEquipmentWiring();

    /** M3-012: the slot mapping rebuilt into the M3-011 equipped-id set. */
    void RebuildEquippedInventoryIds();

    /** M3-012: the hardened sum of every model-equipped instance's stats. */
    FItemStats ComputeEquippedBonusFromModel();

    /** M3-012: pushes base stats + Running state into the widget (if open). */
    void UpdateInventoryEquipContext();

    /** M3-012: the session's Running state (false without a session). */
    bool IsRoomSessionRunning() const;

    // ----- M3-018: settlement reward internals ----------------------------------

    /** M3-018: binds the reward view model from the idempotent BeginReward draft (cleared runs only). */
    void PrepareRoomRewardDraft(const FRoomResult& Result);

    /** M3-018: the HUD-owned reward service (lazy; bound to the world's profile). */
    URewardService* EnsureRewardService();

    /** M3-018: the lazily initialized production save service (null when unavailable: the claim then refuses and stays retryable). */
    UProfileSaveService* EnsureRewardSaveService();

    /** M3-018: builds the interim settlement catalog (the code-built double of Data/items.json). */
    bool EnsureRewardSettlementCatalog();

    /** M3-018: the game-flow subsystem of the world's game instance (null-safe). */
    const UGameFlowSubsystem* ResolveGameFlowSubsystem() const;

    /** M3-030: the mutable game-flow resolution for the mount's request paths. */
    UGameFlowSubsystem* ResolveGameFlowSubsystem();

    // ----- M3-030: boot menu mount internals -----------------------------------

    /** M3-030: creates the native menu widget once and wires the enter path. */
    bool EnsureMenuWidget();

    /** M3-030: presents the menu overlay (viewport + one-time input capture). */
    void ShowMenuOverlay();

    /** M3-030: dismisses the overlay and restores the game input focus. */
    void HideMenuOverlay();

    /** M3-030: the capture half of the one-time input switch (tracker-gated). */
    void ApplyMenuInputCapture();

    /** M3-030: the restore half of the one-time input switch (tracker-gated). */
    void ApplyMenuInputRestore();

    /** M3-012: OnRunStarted handler - disables the equip actions live. */
    void HandleRoomRunStartedForInventory();

    /** M3-011: the presented inventory screen (weak; recreated per presentation). */
    TWeakObjectPtr<UInventoryWidget> InventoryWidgetPtr;

    /** M3-011: display state filled by the last presentation (test seam copy). */
    FInventoryListViewModel InventoryViewModel;

    /** M3-012: preview display state copied from the widget (test seam). */
    FInventoryStatPreviewViewModel InventoryPreview;

    /** M3-012: the HUD-owned equipment slot mapping (references the profile inventory). */
    FEquipmentModel InventoryEquipment;

    /** M3-012: one-shot guard of the Equip/Unequip request paths. */
    FRoomResultActionGuard InventoryActionGuard;

    /** M3-011: pure record of the one-time input focus switch contract. */
    FRoomResultInputFocusTracker InventoryInputFocus;

    /** M3-011: equipped-id set the list marks (empty until a wiring task fills it). */
    TSet<FGuid> EquippedInventoryIds;

    /** M3-011: debug-only display catalog (transient doubles; never gameplay). */
    FItemDefinitionCatalog InventoryStagingCatalog;

    /** M3-011: catalog pointer handed to the view model (null = placeholders). */
    const FItemDefinitionCatalog* InventoryDisplayCatalog = nullptr;

    /** M3-011: the debug staging ran once for this HUD (items added exactly once). */
    bool bInventoryStaged = false;

    // ----- M3-018: settlement reward state --------------------------------------

    /** M3-018: the save service of the atomic claim (lazy; production prefix). */
    UPROPERTY(Transient)
    TObjectPtr<UProfileSaveService> RewardSaveService;

    /** M3-018: the HUD-owned reward service (lazy; weak like the profile). */
    TWeakObjectPtr<URewardService> RewardServicePtr;

    /** M3-018: one-shot guard of the Claim request path. */
    FRoomResultActionGuard RewardClaimGuard;

    /** M3-018: the reward view model of the current presentation (invalid = no reward area). */
    FRoomRewardViewModel RoomRewardViewModel;

    /** M3-018: the interim settlement catalog (code-built Data/items.json double). */
    FItemDefinitionCatalog RewardSettlementCatalog;

    /** M3-018: the interim catalog was built once for this HUD. */
    bool bRewardCatalogReady = false;

    /** M2-012: the presented result screen (weak; recreated per presentation). */
    TWeakObjectPtr<URoomResultWidget> ResultWidgetPtr;

    /** M2-012: display state filled from the session's terminal result. */
    FRoomResultViewModel RoomResultViewModel;

    /** M2-012: one-shot guards of the retry/return request paths. */
    FRoomResultActionGuard RetryGuard;
    FRoomResultActionGuard ReturnGuard;

    /** M2-012: pure record of the one-time input focus switch contract. */
    FRoomResultInputFocusTracker ResultInputFocus;

    /** M2-012: definitions the Retry button hands to URoomRetryService (weak). */
    TWeakObjectPtr<const URoomDefinition> RetryRoomDefPtr;
    TWeakObjectPtr<UEnemyDefinition> RetryEnemyDefPtr;

    /** M2-012: transient staging doubles of the debug exec (never gameplay). */
    UPROPERTY(Transient)
    TObjectPtr<URoomDefinition> StageRoomDefinition;

    UPROPERTY(Transient)
    TObjectPtr<UEnemyDefinition> StageEnemyDefinition;

    /** M2-012: session resolved at BeginPlay (weak) and its end-event handle. */
    TWeakObjectPtr<URoomSessionSubsystem> RoomSessionPtr;
    FDelegateHandle RoomRunEndedHandle;
    /** M3-012: handle of the run-started binding (the equip actions' disable push). */
    FDelegateHandle RoomRunStartedHandle;
    bool bRoomSessionBound = false;

private:
    /** Draws the debug panel: snapshot line, placeholder action, target HP. */
    void DrawCombatDebugOverlay();

    /**
     * M1-035: removes the current feed binding; stale-source safe.
     */
    void UnbindDamageFeed();

    /**
     * M1-035: OnHitConfirmed handler. A stale (destroyed) source reads as an
     * invalid weak reference and the event is ignored - no crash, no stale
     * dereference. Pool + combo advance on the world clock (0.0 world-less).
     */
    void HandleHitConfirmed(const FCombatHit& Hit);

    /**
     * M1-035: draws the two HP bar rows at the top-right screen edge (away
     * from the M0/M1-028 top-left panels): the player placeholder (no health
     * component in M1) and the target's real HealthComponent truth.
     */
    void DrawHealthBars();

    /**
     * M1-035: projects each live damage number's world location to the screen
     * (behind-camera guarded) and draws it rising and fading inside its 0.6 s
     * life.
     */
    void DrawDamageNumbers();

    /** M1-035: draws the combo counter at the bottom-left edge (0 = hidden). */
    void DrawComboCounter();

    /** M1-035: world-clock value the HUD feeds into the model (0.0 world-less). */
    double ResolveDisplayClockSeconds() const;

    // ----- M3-026: enemy health bar internals -------------------------------------

    /**
     * M3-026: OnHitConfirmed companion of the M1-035 feed (the bar data source
     * is the same subscription the damage numbers use): upserts Hit.Target as
     * a tracked bar entry (weak enemy + weak HealthComponent). A target
     * without a health pool is ignored, the 3-entry room cap refuses further
     * entries and a repeat hit on a tracked enemy only refreshes its pool.
     */
    void TrackEnemyBarTarget(const FCombatHit& Hit);

    /** M3-026: drops destroyed enemies and dead (HP<=0) pools; idempotent. */
    void PruneEnemyBarEntries();

    /**
     * M3-026: projects each tracked enemy's bar above its head (world anchor,
     * behind-camera guarded like the M1-035 numbers) and draws the shared
     * M1-035 palette frame + red fill with an optional HP readout.
     */
    void DrawEnemyHealthBars();

    /**
     * M1-028: computes the box through the shared ComputeHitBox path
     * (definition via GetCurrentDefinition, feet via GetDebugFeetLocation,
     * facing from the snapshot) and strokes its 12 edges in canvas space.
     */
    void DrawSharedHitBox(const FVector& FeetLocation, int32 Facing, const UAttackDefinition& Definition);

    /** M1-028: demo strike for offscreen evidence (console mode 2 only). */
    void StartDebugDemoStrike();

    /**
     * M1-035: one repeated real strike for the mode-3 render staging (debug
     * only, never gameplay): snaps the training enemy exactly one strike's
     * reach in front of the player (knockback compensation) and starts the
     * real light_01 through the real combat path, so the hit lands through
     * OnHitConfirmed with no synthetic values.
     */
    void StartDebugRepeatStrike();

    /** M1-028: debug overlay switch; pure display state, default off. */
    bool bCombatDebugOverlayEnabled = false;

    /** M1-028: cached references (weak; stale entries read null and re-resolve). */
    TWeakObjectPtr<APrototypeCharacter> DebugPlayer;
    TWeakObjectPtr<ATrainingEnemy> DebugTarget;

    /** M1-028: demo strike timers (console mode 2 only; never set by gameplay). */
    FTimerHandle DebugStrikeStartTimerHandle;
    FTimerHandle DebugStrikeFreezeTimerHandle;

    /** M1-035: repeated real-strike timer for the mode-3 render staging (debug only). */
    FTimerHandle DebugStrikeRepeatTimerHandle;

    /** M1-035: damage-number pool feeding the floating numbers (pure data copies). */
    FDamageNumberPool DamageNumbers;

    /** M1-035: consecutive-hit counter of the bound attacker (default 1.5 s window). */
    FComboCounter ComboCounter;

    /** M1-035: component whose OnHitConfirmed the feed is bound to (weak; stale reads null). */
    TWeakObjectPtr<UCombatComponent> DamageFeedSource;

    /** M1-035: handle of the active OnHitConfirmed binding. */
    FDelegateHandle DamageFeedHandle;

    /**
     * M1-035: last target health the HP bar observed. Damage can only lower
     * it; a rise is the session-reset signature (no reset event reaches the
     * HUD within the card's file scope), so a rise clears the combo and the
     * damage numbers (the card's reset step). Reset when the target changes.
     */
    float LastObservedTargetHP = -1.0f;

    /** M1-035: health pool the last observation came from (reset on switch). */
    TWeakObjectPtr<UHealthComponent> LastObservedHealth;

    /** M3-026: tracked enemy bar entries (weak; capped at the 3-enemy room max). */
    TArray<FEnemyBarEntry> EnemyBars;

    // ----- M3-030: boot menu mount state ----------------------------------------

    /** M3-030: the presented menu overlay (weak; recreated per presentation). */
    TWeakObjectPtr<UMapSelectWidget> MenuWidgetPtr;

    /** M3-030: pure record of the one-time input focus switch contract. */
    FRoomResultInputFocusTracker MenuInputFocus;
};
