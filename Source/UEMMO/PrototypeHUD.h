#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "PrototypeHUD.generated.h"

class APrototypeCharacter;
class ATrainingEnemy;
class UAttackDefinition;

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

private:
    /** Draws the debug panel: snapshot line, placeholder action, target HP. */
    void DrawCombatDebugOverlay();

    /**
     * M1-028: computes the box through the shared ComputeHitBox path
     * (definition via GetCurrentDefinition, feet via GetDebugFeetLocation,
     * facing from the snapshot) and strokes its 12 edges in canvas space.
     */
    void DrawSharedHitBox(const FVector& FeetLocation, int32 Facing, const UAttackDefinition& Definition);

    /** M1-028: demo strike for offscreen evidence (console mode 2 only). */
    void StartDebugDemoStrike();

    /** M1-028: debug overlay switch; pure display state, default off. */
    bool bCombatDebugOverlayEnabled = false;

    /** M1-028: cached references (weak; stale entries read null and re-resolve). */
    TWeakObjectPtr<APrototypeCharacter> DebugPlayer;
    TWeakObjectPtr<ATrainingEnemy> DebugTarget;

    /** M1-028: demo strike timers (console mode 2 only; never set by gameplay). */
    FTimerHandle DebugStrikeStartTimerHandle;
    FTimerHandle DebugStrikeFreezeTimerHandle;
};
