#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "UI/DamageNumberModel.h"
#include "PrototypeHUD.generated.h"

class APrototypeCharacter;
class ATrainingEnemy;
class UAttackDefinition;
class UCombatComponent;
class UHealthComponent;
struct FCombatHit;

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
