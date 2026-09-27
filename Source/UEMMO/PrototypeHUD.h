#pragma once
#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "GameFramework/HUD.h"
#include "UI/DamageNumberModel.h"
#include "UI/RoomResultWidget.h"
// M3-011: FInventoryListViewModel is a by-value member below, so the widget
// header (its owner) must be complete here; same pattern as RoomResultWidget.
#include "UI/InventoryWidget.h"
// M3-011: the debug display catalog is also a by-value member (plain struct).
#include "Items/ItemDefinition.h"
#include "PrototypeHUD.generated.h"

class APrototypeCharacter;
class ATrainingEnemy;
class UAttackDefinition;
class UCombatComponent;
class UEnemyDefinition;
class UHealthComponent;
class UProfileSubsystem;
class URoomDefinition;
class URoomSessionSubsystem;
struct FCombatHit;
struct FRoomResult;

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

    /** M3-011: the presented inventory screen (weak; recreated per presentation). */
    TWeakObjectPtr<UInventoryWidget> InventoryWidgetPtr;

    /** M3-011: display state filled by the last presentation (test seam copy). */
    FInventoryListViewModel InventoryViewModel;

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
    bool bRoomSessionBound = false;

private:
    /** Draws the debug panel: snapshot line, placeholder action, target HP. */
    void DrawCombatDebugOverlay();

    /**
     * M1-035: per-drawn-frame feed wiring while the debug HUD is on. Resolves
     * the player's combat component through the cached weak reference (no scan
     * in the steady state) and binds/unbinds only when the source changed.
     */
    void RefreshDamageFeedBinding();

    /** M1-035: removes the current feed binding; stale-source safe. */
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
};
