#include "PrototypeHUD.h"
#include "Engine/Canvas.h"

#include "Combat/AttackDefinition.h"
#include "Combat/CombatComponent.h"
#include "Combat/CombatGeometry.h"
#include "Combat/HealthComponent.h"
#include "Combat/System/TestRoomConfigDriver.h"
#include "Enemy/EnemyDefinition.h"
#include "Enemy/TrainingEnemy.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "PrototypeCharacter.h"
#include "Room/RoomDefinition.h"
#include "Room/RoomRetryService.h"
#include "Room/RoomSessionSubsystem.h"
#include "UI/DamageNumberModel.h"
#include "UI/InventoryWidget.h"
#include "UI/MapSelectWidget.h"
#include "UI/PendingRewardsWidget.h"
#include "Items/ItemDefinition.h"
#include "Items/ItemInstance.h"
#include "Items/InventoryModel.h"
#include "Persistence/ProfileSaveService.h"
#include "Profile/GameFlowSubsystem.h"
#include "Profile/ProfileSubsystem.h"
#include "Profile/RewardService.h"

namespace
{
    // M1-028: coarse action-state text for the debug panel; the order mirrors
    // the append-only ECombatActionState enum.
    const TCHAR* M1_028_ActionStateLabel(ECombatActionState State)
    {
        switch (State)
        {
        case ECombatActionState::Free: return TEXT("Free");
        case ECombatActionState::Attacking: return TEXT("Attacking");
        case ECombatActionState::HitStun: return TEXT("HitStun");
        case ECombatActionState::Knockdown: return TEXT("Knockdown");
        case ECombatActionState::Recovering: return TEXT("Recovering");
        default: return TEXT("Unknown");
        }
    }

    // M1-028: debug palette and panel layout (pure display constants).
    const FLinearColor M1_028_PanelColor(0.02f, 0.03f, 0.05f, 0.86f);
    const FLinearColor M1_028_HeaderColor(1.0f, 0.85f, 0.3f, 1.0f);
    const FLinearColor M1_028_TextColor(0.85f, 0.95f, 1.0f, 1.0f);
    const FLinearColor M1_028_BoxColor(0.2f, 1.0f, 0.3f, 1.0f);

    constexpr float M1_028_PanelX = 18.0f;   // below the M0 panel (y 18..102)
    constexpr float M1_028_PanelY = 108.0f;
    constexpr float M1_028_PanelW = 620.0f;
    constexpr float M1_028_PanelH = 104.0f;
    constexpr float M1_028_RowStep = 19.0f;

    // M1-035: palette and layout for the hit-feedback display. The HP bars
    // anchor at the top-right screen edge (away from the top-left M0/M1-028
    // panels at any rendering resolution) and the combo counter at the
    // bottom-left edge; neither ever covers the input hints.
    const FLinearColor M1_035_BarFrameColor(0.02f, 0.03f, 0.05f, 0.86f);
    const FLinearColor M1_035_BarFillColor(0.85f, 0.2f, 0.2f, 1.0f);
    const FLinearColor M1_035_NumberColor(1.0f, 0.9f, 0.25f, 1.0f);
    const FLinearColor M1_035_ComboColor(1.0f, 0.65f, 0.2f, 1.0f);
    const FLinearColor M1_035_MutedColor(0.6f, 0.65f, 0.7f, 1.0f);

    constexpr float M1_035_BarWidth = 240.0f;
    constexpr float M1_035_BarHeight = 14.0f;
    constexpr float M1_035_BarMargin = 24.0f;
    constexpr float M1_035_BarRowStep = 38.0f;
    constexpr double M1_035_NumberRiseSpeed = 60.0;   // screen px per second
    constexpr float M1_035_ComboMargin = 24.0f;
    constexpr float M1_035_ComboBottomOffset = 64.0f;

    // M3-026: world-anchored enemy health bars (production HUD, independent of
    // the debug overlay). The palette reuses the M1-035 bar colors (dark frame
    // + red fill, the M2-012 enemy HP bar style); the bar anchors above the
    // enemy's head (the training/melee capsule half height is 88). M3-027: the
    // bar grew from 90x8 to 120x10 - the fourth playtest round read the small
    // bar as absent at full-screen viewing distance.
    constexpr int32 M3_026_EnemyBarCap = 3;    // a room never spawns more than 3 enemies
    constexpr float M3_026_BarWidth = 120.0f;
    constexpr float M3_026_BarHeight = 10.0f;
    constexpr float M3_026_BarLiftZ = 120.0f;
}

void APrototypeHUD::DrawHUD()
{
    Super::DrawHUD();
    DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.86f), 18, 18, 630, 110);
    // M2-017: stage copy refreshed for the M2 room combat prototype (pending
    // playtest); the M1-only line is obsolete now that M2 rooms are in.
    DrawText(TEXT("UE-MMO | M3: single-player prototype (pending playtest)"), FLinearColor::White, 32, 28, nullptr, 1.2f);
    // M1-040: DNF-style keymap hints. Arrows move (X/Y incl. depth), X attack,
    // Z launcher, C jump (Space stays a jump alias), F2 resets, QWERASDF are
    // the reserved skill slots (no skill effect yet), F1 the debug overlay.
    DrawText(TEXT("Arrows: move  X: attack  Z: launcher  C: jump  F2: reset  QWERASDF: skills (reserved)  F1: debug"), FLinearColor(0.6f, 0.85f, 1.f), 32, 53);
    // M2-017: the stage note reflects the implemented M1 combat + M2 wave
    // room instead of the stale "M2 planned" text.
    DrawText(TEXT("Single-player. M1 combat + M2 wave room + M3 progression implemented; M4 server integration not started."), FLinearColor(0.9f, 0.8f, 0.45f), 32, 77);

    // M3-018: the pending-reward badge while the menu flow state owns the
    // screen (the card's minimal HUD hook point: canvas text of the prototype
    // HUD; a count of zero shows nothing). The pure badge text and the count
    // itself are testable without a HUD.
    if (const UGameFlowSubsystem* Flow = ResolveGameFlowSubsystem())
    {
        if (Flow->GetState() == EGameFlowState::Menu)
        {
            if (const UProfileSubsystem* Profile = ResolveProfileSubsystem())
            {
                const FString Badge = MakePendingRewardsBadgeText(Profile->GetPendingRewards().Num());
                if (!Badge.IsEmpty())
                {
                    DrawText(Badge, FLinearColor(1.0f, 0.85f, 0.3f, 1.0f), 32, 101);
                }
            }
        }
    }

    // M1-028: the debug overlay is a pure display layer gated by the F1 flag.
    // With it off nothing below runs: no references resolved, no text, no box,
    // and combat queries/damage are untouched by any of this code. The M1-035
    // debug HP bar rows stay behind the flag with the panel.
    if (bCombatDebugOverlayEnabled)
    {
        RefreshDebugReferences();
        DrawCombatDebugOverlay();
        DrawHealthBars();
    }

    // M3-026: the enemy health bars are production HUD. They share the M1-035
    // OnHitConfirmed feed (one subscription feeds numbers, combo and the bar
    // tracking), so the binding refresh runs every drawn frame independent of
    // the debug flag; with the overlay off it resolves the pawn through the
    // owning controller (O(1), no world scan).
    RefreshDamageFeedBinding();
    DrawEnemyHealthBars();
    // M3-027: the damage numbers and the combo counter are production hit
    // feedback too (the fourth playtest round read their absence as
    // "attacking a monster gives no feedback"): they draw every frame like
    // the enemy bars, fed by the same OnHitConfirmed subscription. The pool
    // is event-driven, so the per-frame cost is the draw itself.
    DrawDamageNumbers();
    DrawComboCounter();
}

void APrototypeHUD::DrawCombatDebugOverlay()
{
    DrawRect(M1_028_PanelColor, M1_028_PanelX, M1_028_PanelY, M1_028_PanelW, M1_028_PanelH);
    float RowY = M1_028_PanelY + 10.0f;
    DrawText(TEXT("DEBUG OVERLAY (M1) | placeholder combat debug view, not final UI"),
        M1_028_HeaderColor, M1_028_PanelX + 14.0f, RowY, nullptr, 1.0f);
    RowY += M1_028_RowStep;

    const APrototypeCharacter* Player = DebugPlayer.Get();
    if (Player == nullptr)
    {
        DrawText(TEXT("Player: none"), M1_028_TextColor, M1_028_PanelX + 14.0f, RowY);
        RowY += M1_028_RowStep;
    }
    else if (const UCombatComponent* Combat = Player->GetCombat())
    {
        const FCombatSnapshot Snapshot = Combat->GetSnapshot();
        const FString PlayerLine = FString::Printf(
            TEXT("Player: AttackId=%s Frame=%d Facing=%d State=%s Buffer=%d Dead=%s"),
            Snapshot.AttackId == NAME_None ? TEXT("none") : *Snapshot.AttackId.ToString(),
            Snapshot.Frame,
            Snapshot.Facing,
            M1_028_ActionStateLabel(Snapshot.ActionState),
            Snapshot.BufferSize,
            Combat->IsDead() ? TEXT("yes") : TEXT("no"));
        DrawText(PlayerLine, M1_028_TextColor, M1_028_PanelX + 14.0f, RowY);
        RowY += M1_028_RowStep;

        // The debug text names the current placeholder action explicitly, so
        // the overlay is never mistaken for final UI.
        const FString ActionLine = Snapshot.AttackId == NAME_None
            ? FString(TEXT("Action: none (idle)"))
            : FString::Printf(TEXT("Action: %s (placeholder animation)"), *Snapshot.AttackId.ToString());
        DrawText(ActionLine, M1_028_TextColor, M1_028_PanelX + 14.0f, RowY);
        RowY += M1_028_RowStep;

        // The drawn box shares the real query exactly: same definition
        // (GetCurrentDefinition), same feet origin (GetDebugFeetLocation),
        // same facing (snapshot). Drawn only while an instance runs.
        const UAttackDefinition* Definition = Combat->GetCurrentDefinition();
        if (Snapshot.ActionState == ECombatActionState::Attacking && Definition != nullptr)
        {
            DrawText(TEXT("HitBox: shared ComputeHitBox query path, drawn while attacking"),
                M1_028_BoxColor, M1_028_PanelX + 14.0f, RowY);
            RowY += M1_028_RowStep;
            DrawSharedHitBox(Combat->GetDebugFeetLocation(), Snapshot.Facing, *Definition);
        }
    }

    const ATrainingEnemy* Target = DebugTarget.Get();
    if (Target == nullptr)
    {
        DrawText(TEXT("Target: none"), M1_028_TextColor, M1_028_PanelX + 14.0f, RowY);
    }
    else if (const UHealthComponent* Health = Target->GetHealthComponent())
    {
        const FString TargetLine = FString::Printf(TEXT("Target HP: %.1f / %.1f%s"),
            Health->GetHealth(),
            Health->GetMaxHealth(),
            Health->IsAlive() ? TEXT("") : TEXT(" (dead)"));
        DrawText(TargetLine, M1_028_TextColor, M1_028_PanelX + 14.0f, RowY);
    }
}

void APrototypeHUD::DrawSharedHitBox(const FVector& FeetLocation, int32 Facing, const UAttackDefinition& Definition)
{
    if (Canvas == nullptr)
    {
        return;
    }
    const FCombatHitBox Box = ComputeHitBox(FeetLocation, Facing, Definition);

    // Behind-camera guard: Project mirrors coordinates of points behind the
    // view, so skip the stroke when the box center sits behind the camera.
    if (const APlayerController* PC = PlayerOwner)
    {
        FVector ViewLocation;
        FRotator ViewRotation;
        PC->GetPlayerViewPoint(ViewLocation, ViewRotation);
        if (FVector::DotProduct(Box.Center - ViewLocation, ViewRotation.Vector()) <= 0.0f)
        {
            return;
        }
    }

    const FVector Min = Box.Center - Box.Extent;
    const FVector Max = Box.Center + Box.Extent;
    const FVector M1_028_Corners[8] = {
        FVector(Min.X, Min.Y, Min.Z),
        FVector(Max.X, Min.Y, Min.Z),
        FVector(Max.X, Max.Y, Min.Z),
        FVector(Min.X, Max.Y, Min.Z),
        FVector(Min.X, Min.Y, Max.Z),
        FVector(Max.X, Min.Y, Max.Z),
        FVector(Max.X, Max.Y, Max.Z),
        FVector(Min.X, Max.Y, Max.Z)
    };
    static constexpr int32 M1_028_EdgeIndices[12][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    FVector Screen[8];
    for (int32 Corner = 0; Corner < 8; ++Corner)
    {
        Screen[Corner] = Canvas->Project(M1_028_Corners[Corner]);
    }
    for (const auto& Edge : M1_028_EdgeIndices)
    {
        DrawLine(Screen[Edge[0]].X, Screen[Edge[0]].Y, Screen[Edge[1]].X, Screen[Edge[1]].Y, M1_028_BoxColor, 2.0f);
    }
}

void APrototypeHUD::RefreshDebugReferences()
{
    // Player: the owning local player's pawn first (O(1), no world scan); only
    // when that yields nothing, one class-filtered iterator pass. Runs only
    // while the cached weak reference is invalid.
    if (DebugPlayer.Get() == nullptr)
    {
        APrototypeCharacter* Resolved = nullptr;
        if (APlayerController* PC = PlayerOwner)
        {
            Resolved = Cast<APrototypeCharacter>(PC->GetCharacter());
        }
        if (Resolved == nullptr && GetWorld() != nullptr)
        {
            for (TActorIterator<APrototypeCharacter> It(GetWorld()); It; ++It)
            {
                Resolved = *It;
                break;
            }
        }
        DebugPlayer = Resolved;
    }

    // Target: the first ATrainingEnemy. One class-filtered iterator pass only
    // while the cached weak reference is invalid; a destroyed target reads
    // null ("no target" display) and is re-resolved on the next drawn frame.
    if (DebugTarget.Get() == nullptr && GetWorld() != nullptr)
    {
        ATrainingEnemy* Resolved = nullptr;
        for (TActorIterator<ATrainingEnemy> It(GetWorld()); It; ++It)
        {
            Resolved = *It;
            break;
        }
        DebugTarget = Resolved;
    }
}

void APrototypeHUD::UEMMODebugCombatOverlay(int32 Mode)
{
    UE_LOG(LogTemp, Display, TEXT("UEMMO M1-028: debug overlay exec mode %d"), Mode);
    if (Mode == 0)
    {
        SetCombatDebugOverlayEnabled(false);
        return;
    }
    SetCombatDebugOverlayEnabled(true);
    if (Mode == 2)
    {
        // Offscreen-evidence choreography (never gameplay): wait until the smoke
        // runner's movement window (2..4.3 s) is over, start light_01 toward the
        // first training enemy, then freeze the combat clock inside the active
        // window (light_01 hits on frames [7,11); 0.15 s after the start is frame
        // 8-9) so the ~8 s screenshot catches Frame>=0 with the shared box drawn.
        if (GetWorld() != nullptr)
        {
            GetWorldTimerManager().SetTimer(DebugStrikeStartTimerHandle,
                FTimerDelegate::CreateUObject(this, &APrototypeHUD::StartDebugDemoStrike), 6.0f, false);
        }
        return;
    }
    if (Mode == 3)
    {
        // M1-035 render staging (debug only, never gameplay): repeated REAL
        // light_01 strikes from 6.6 s every 0.6 s. Each hit lands ~0.13 s into
        // its strike, so at least one 0.6 s damage number is alive when the
        // smoke runner captures its ~8 s screenshot, and the combo/HP bar
        // carry the real consecutive-hit values. Nothing here fakes data: the
        // numbers only arrive through the real OnHitConfirmed event.
        if (GetWorld() == nullptr)
        {
            return;
        }
        TWeakObjectPtr<APrototypeHUD> WeakHUD(this);
        GetWorldTimerManager().SetTimer(DebugStrikeRepeatTimerHandle,
            FTimerDelegate::CreateLambda([WeakHUD]()
            {
                if (WeakHUD.IsValid())
                {
                    WeakHUD->StartDebugRepeatStrike();
                }
            }), 0.6f, /*bLoop*/ true, /*firstDelay*/ 6.6f);
    }
}

void APrototypeHUD::StartDebugDemoStrike()
{
    RefreshDebugReferences();
    const APrototypeCharacter* Player = DebugPlayer.Get();
    UCombatComponent* Combat = Player ? Player->GetCombat() : nullptr;
    if (Combat == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M1-028: demo strike skipped (no player or combat component)"));
        return;
    }

    int32 Facing = 1;
    if (const ATrainingEnemy* Target = DebugTarget.Get())
    {
        Facing = (Target->GetActorLocation().X >= Player->GetActorLocation().X) ? 1 : -1;
    }
    if (!Combat->TryStartAttack(FName(TEXT("light_01")), Facing))
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M1-028: demo strike skipped (TryStartAttack refused)"));
        return;
    }

    // Freeze the clock ~0.15 s later (frame 8-9 of light_01's [7,11) window)
    // so the state survives until the screenshot; weak captures keep the
    // lambda safe if the HUD or component is torn down first.
    TWeakObjectPtr<UCombatComponent> WeakCombat(Combat);
    TWeakObjectPtr<APrototypeHUD> WeakHUD(this);
    GetWorldTimerManager().SetTimer(DebugStrikeFreezeTimerHandle, FTimerDelegate::CreateLambda([WeakCombat, WeakHUD]()
    {
        if (WeakCombat.IsValid())
        {
            WeakCombat->SetClockFrozen(true);
        }
        if (WeakHUD.IsValid())
        {
            UE_LOG(LogTemp, Display, TEXT("UEMMO M1-028: demo strike clock frozen inside the active window"));
        }
    }), 0.15f, false);
    UE_LOG(LogTemp, Display, TEXT("UEMMO M1-028: demo strike started (light_01, facing %d)"), Facing);
}


// ----- M1-035: health bars, damage numbers, combo counter (display model) -----

void APrototypeHUD::BindDamageFeed(UCombatComponent* Source)
{
    if (Source == DamageFeedSource.Get())
    {
        return;
    }
    UnbindDamageFeed();
    if (Source != nullptr)
    {
        DamageFeedHandle = Source->OnHitConfirmed.AddUObject(this, &APrototypeHUD::HandleHitConfirmed);
        DamageFeedSource = Source;
    }
}

void APrototypeHUD::RefreshDamageFeedBinding()
{
    // The feed binds to the local player's combat component (the attacker
    // whose OnHitConfirmed the debug HUD visualizes and the M3-026 enemy
    // health bars track). Resolution goes through the cached weak reference
    // M1-028 maintains (no scan in the steady state); M3-026: with the debug
    // overlay off (production) the cached reference is never refreshed, so the
    // owning controller's pawn resolves instead - O(1), no world scan. A
    // destroyed pawn reads null and simply unbinds the feed.
    UCombatComponent* Desired = nullptr;
    APrototypeCharacter* Player = DebugPlayer.Get();
    if (Player == nullptr && PlayerOwner != nullptr)
    {
        Player = Cast<APrototypeCharacter>(PlayerOwner->GetCharacter());
    }
    if (Player != nullptr)
    {
        Desired = Player->GetCombat();
    }
    BindDamageFeed(Desired);
}

void APrototypeHUD::UnbindDamageFeed()
{
    // Stale-source safety: only a still-valid source is touched. A destroyed
    // component reads null here and is dropped without dereferencing it; the
    // delegate handle dies with the object, so no explicit removal is needed.
    if (UCombatComponent* Bound = DamageFeedSource.Get())
    {
        Bound->OnHitConfirmed.Remove(DamageFeedHandle);
    }
    DamageFeedSource = nullptr;
    DamageFeedHandle.Reset();
}

void APrototypeHUD::HandleHitConfirmed(const FCombatHit& Hit)
{
    // Stale-source guard: a destroyed attacker reads as an invalid weak
    // reference and the event is ignored (no crash, no stale dereference).
    if (!DamageFeedSource.IsValid() || Hit.Damage <= 0.0f)
    {
        return;
    }
    const double Now = ResolveDisplayClockSeconds();
    DamageNumbers.SetNowSeconds(Now);
    // Hit.Damage is the ApplyDamage return value - the health the target
    // actually lost, never the configured base damage. The number anchors at
    // the reported world hit location and is projected at draw time.
    DamageNumbers.Add(Hit.Damage, Hit.WorldHitLocation, Hit.AttackId);
    ComboCounter.NotifyHit(Now);
    // M3-026: the same accepted hit upserts the target as a tracked enemy
    // health bar entry (the production HUD's hit feedback).
    TrackEnemyBarTarget(Hit);
}

void APrototypeHUD::DrawHealthBars()
{
    if (Canvas == nullptr)
    {
        return;
    }
    const float BarX = static_cast<float>(Canvas->SizeX) - M1_035_BarWidth - M1_035_BarMargin;
    float RowY = M1_035_BarMargin;

    // Player row: M1-016 grants a HealthComponent to enemies only, so the
    // player side shows the explicit placeholder - no invented HP numbers.
    DrawText(TEXT("Player HP: n/a (no health component yet)"), M1_035_MutedColor, BarX, RowY);
    RowY += M1_035_BarRowStep;

    // Enemy row: the real HealthComponent truth, read per drawn frame from the
    // cached weak target (a plain component query on the M1-028 reference -
    // never a world scan).
    const ATrainingEnemy* Target = DebugTarget.Get();
    UHealthComponent* Health = (Target != nullptr) ? Target->GetHealthComponent() : nullptr;
    if (Health == nullptr)
    {
        DrawText(TEXT("Enemy HP: no target"), M1_035_MutedColor, BarX, RowY);
        LastObservedHealth = nullptr;
        LastObservedTargetHP = -1.0f;
        return;
    }

    // Session-reset signature (the card's reset step within this file scope):
    // damage can only lower the observed pool, so a rising value is a reset
    // (ResetEnemy restores full HP) and clears the combo and the numbers. A
    // target switch just re-baselines the observation.
    if (LastObservedHealth.Get() != Health || Health->GetHealth() < LastObservedTargetHP
        || LastObservedTargetHP < 0.0f)
    {
        LastObservedHealth = Health;
        LastObservedTargetHP = Health->GetHealth();
    }
    else if (Health->GetHealth() > LastObservedTargetHP)
    {
        DamageNumbers.Clear();
        ComboCounter.Reset();
        LastObservedTargetHP = Health->GetHealth();
    }

    FHealthBarData Data;
    Data.Set(Health->GetHealth(), Health->GetMaxHealth(), Health->IsAlive());
    DrawRect(M1_035_BarFrameColor, BarX, RowY, M1_035_BarWidth, M1_035_BarHeight);
    const float FillWidth = (M1_035_BarWidth - 2.0f) * Data.GetRatio();
    if (FillWidth > 0.0f)
    {
        DrawRect(M1_035_BarFillColor, BarX + 1.0f, RowY + 1.0f, FillWidth, M1_035_BarHeight - 2.0f);
    }
    const FString EnemyLine = FString::Printf(TEXT("Enemy HP: %.0f / %.0f%s"),
        Data.CurrentHP, Data.MaxHP, Data.bAlive ? TEXT("") : TEXT(" (dead)"));
    DrawText(EnemyLine, M1_028_TextColor, BarX, RowY + M1_035_BarHeight + 2.0f);
}

void APrototypeHUD::DrawDamageNumbers()
{
    if (Canvas == nullptr)
    {
        return;
    }
    const double Now = ResolveDisplayClockSeconds();
    DamageNumbers.SetNowSeconds(Now);
    DamageNumbers.PruneExpired();

    // Behind-camera guard shared with the M1-028 box stroke: Project mirrors
    // points behind the view, so their screen position is meaningless.
    FVector ViewLocation = FVector::ZeroVector;
    FRotator ViewRotation = FRotator::ZeroRotator;
    const bool bHasView = (PlayerOwner != nullptr);
    if (bHasView)
    {
        PlayerOwner->GetPlayerViewPoint(ViewLocation, ViewRotation);
    }

    for (const FDamageNumberEntry& Entry : DamageNumbers.GetEntries())
    {
        const double Age = FMath::Max(0.0, Now - Entry.SpawnTimeSeconds);
        if (bHasView
            && FVector::DotProduct(Entry.Location - ViewLocation, ViewRotation.Vector()) <= 0.0)
        {
            continue;
        }
        const FVector Screen = Canvas->Project(Entry.Location);
        const float Alpha = FMath::Clamp(
            1.0f - static_cast<float>(Age / FDamageNumberPool::LifeTimeSeconds), 0.0f, 1.0f);
        const FLinearColor Color(M1_035_NumberColor.R, M1_035_NumberColor.G, M1_035_NumberColor.B, Alpha);
        const float ScreenY = Screen.Y - static_cast<float>(Age * M1_035_NumberRiseSpeed);
        DrawText(FString::Printf(TEXT("%.0f"), Entry.Damage), Color, Screen.X, ScreenY, nullptr, 1.4f);
    }
}

void APrototypeHUD::DrawComboCounter()
{
    if (Canvas == nullptr)
    {
        return;
    }
    const int32 CurrentCombo = ComboCounter.EvaluateCombo(ResolveDisplayClockSeconds());
    if (CurrentCombo <= 0)
    {
        return;
    }
    const float ScreenY = static_cast<float>(Canvas->SizeY) - M1_035_ComboBottomOffset;
    DrawText(FString::Printf(TEXT("Combo x%d"), CurrentCombo),
        M1_035_ComboColor, M1_035_ComboMargin, ScreenY, nullptr, 1.5f);
}

double APrototypeHUD::ResolveDisplayClockSeconds() const
{
    return (GetWorld() != nullptr) ? GetWorld()->GetTimeSeconds() : 0.0;
}

// ----- M3-026: enemy health bar tracking (production HUD) ----------------------

void APrototypeHUD::TrackEnemyBarTarget(const FCombatHit& Hit)
{
    // The tracked entry is the hit VICTIM: the event's weak target reads null
    // when it was destroyed in the meantime (no crash, no entry). M3-027: the
    // body moved to TrackEnemyBarActor so the spawn-time registration shares
    // the exact upsert/cap rules.
    TrackEnemyBarActor(Hit.Target.Get());
}

void APrototypeHUD::TrackEnemyBarActor(AActor* Target)
{
    // A victim without a health pool has no bar to draw (the M1-016 grant
    // covers the combatants only); a null actor reads as a stale weak
    // reference from the hit path and is ignored.
    if (Target == nullptr)
    {
        return;
    }
    UHealthComponent* Health = Target->FindComponentByClass<UHealthComponent>();
    if (Health == nullptr)
    {
        return;
    }
    // Upsert first: a repeat hit on an already tracked enemy only refreshes
    // its (possibly re-resolved) pool - never a duplicate bar.
    for (FEnemyBarEntry& Entry : EnemyBars)
    {
        if (Entry.Enemy.Get() == Target)
        {
            Entry.Health = Health;
            return;
        }
    }
    // The room never spawns more than 3 enemies at once; the cap refuses
    // further entries instead of silently evicting an already tracked one.
    if (EnemyBars.Num() >= M3_026_EnemyBarCap)
    {
        return;
    }
    FEnemyBarEntry Entry;
    Entry.Enemy = Target;
    Entry.Health = Health;
    EnemyBars.Add(Entry);
}

void APrototypeHUD::PruneEnemyBarEntries()
{
    // Weak-reference contract: a destroyed enemy/component reads null here
    // (never a stale dereference). A dead pool (HP<=0 or not alive) drops its
    // entry - the card's death rule; no OnDied binding is needed because every
    // read path (count seam and draw) runs this prune first.
    for (int32 Index = EnemyBars.Num() - 1; Index >= 0; --Index)
    {
        const AActor* Enemy = EnemyBars[Index].Enemy.Get();
        const UHealthComponent* Health = EnemyBars[Index].Health.Get();
        if (Enemy == nullptr || Health == nullptr || !Health->IsAlive()
            || Health->GetHealth() <= 0.0f)
        {
            EnemyBars.RemoveAt(Index);
        }
    }
}

void APrototypeHUD::DrawEnemyHealthBars()
{
    if (Canvas == nullptr)
    {
        return;
    }
    PruneEnemyBarEntries();
    if (EnemyBars.Num() == 0)
    {
        return;
    }
    // Behind-camera guard shared with the M1-035 numbers: Project mirrors
    // points behind the view, so their screen position is meaningless.
    FVector ViewLocation = FVector::ZeroVector;
    FRotator ViewRotation = FRotator::ZeroRotator;
    const bool bHasView = (PlayerOwner != nullptr);
    if (bHasView)
    {
        PlayerOwner->GetPlayerViewPoint(ViewLocation, ViewRotation);
    }
    for (const FEnemyBarEntry& Entry : EnemyBars)
    {
        const AActor* Enemy = Entry.Enemy.Get();
        const UHealthComponent* Health = Entry.Health.Get();
        if (Enemy == nullptr || Health == nullptr)
        {
            continue; // defensive: the prune pass above already dropped these
        }
        const FVector Anchor = Enemy->GetActorLocation() + FVector(0.0, 0.0, M3_026_BarLiftZ);
        if (bHasView
            && FVector::DotProduct(Anchor - ViewLocation, ViewRotation.Vector()) <= 0.0)
        {
            continue;
        }
        const FVector Screen = Canvas->Project(Anchor);
        const float BarX = Screen.X - M3_026_BarWidth * 0.5f;
        // The M2-012/M1-035 enemy HP bar style: dark frame + red fill, the
        // fill ratio read from the enemy's real HealthComponent per frame.
        DrawRect(M1_035_BarFrameColor, BarX, Screen.Y, M3_026_BarWidth, M3_026_BarHeight);
        FHealthBarData Data;
        Data.Set(Health->GetHealth(), Health->GetMaxHealth(), Health->IsAlive());
        const float FillWidth = (M3_026_BarWidth - 2.0f) * Data.GetRatio();
        if (FillWidth > 0.0f)
        {
            DrawRect(M1_035_BarFillColor, BarX + 1.0f, Screen.Y + 1.0f, FillWidth, M3_026_BarHeight - 2.0f);
        }
        DrawText(FString::Printf(TEXT("%.0f / %.0f"), Data.CurrentHP, Data.MaxHP),
            M1_028_TextColor, BarX, Screen.Y + M3_026_BarHeight + 2.0f, nullptr, 0.9f);
    }
}

int32 APrototypeHUD::GetTrackedEnemyCount()
{
    PruneEnemyBarEntries();
    return EnemyBars.Num();
}

const AActor* APrototypeHUD::PeekTrackedEnemy(int32 Index)
{
    return EnemyBars.IsValidIndex(Index) ? EnemyBars[Index].Enemy.Get() : nullptr;
}

const UHealthComponent* APrototypeHUD::PeekTrackedHealth(int32 Index)
{
    return EnemyBars.IsValidIndex(Index) ? EnemyBars[Index].Health.Get() : nullptr;
}

void APrototypeHUD::StartDebugRepeatStrike()
{
    RefreshDebugReferences();
    APrototypeCharacter* Player = DebugPlayer.Get();
    ATrainingEnemy* Target = DebugTarget.Get();
    UCombatComponent* Combat = Player ? Player->GetCombat() : nullptr;
    if (Player == nullptr || Target == nullptr || Combat == nullptr)
    {
        return;
    }

    // Debug-only repositioning: keep the enemy exactly one strike's reach in
    // front of the player (player-relative +150 X) so every staged strike
    // lands despite the knockback of the previous one.
    Target->SetActorLocation(Player->GetActorLocation() + FVector(150.0, 0.0, 0.0));
    const int32 Facing = (Target->GetActorLocation().X >= Player->GetActorLocation().X) ? 1 : -1;
    if (Combat->TryStartAttack(FName(TEXT("light_01")), Facing))
    {
        UE_LOG(LogTemp, Display, TEXT("UEMMO M1-035: staged real strike (light_01, facing %d)"), Facing);
    }
}

// ----- M2-012: room result screen --------------------------------------------

void APrototypeHUD::BeginPlay()
{
    Super::BeginPlay();
    BindRoomSessionEvents();
    // M3-030: the game-flow surface presentation. The flow starts in the menu
    // state (the GameInstance-level machine initializes before any world
    // exists), so the BOOT world presents the map select overlay; room worlds
    // (flow Room/Result) present nothing. The presentation is headless-safe
    // (prepared without a viewport in automation worlds).
    RefreshMenuPresentation();
}

void APrototypeHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnbindRoomSessionEvents();
    Super::EndPlay(EndPlayReason);
}

void APrototypeHUD::BindRoomSessionEvents()
{
    if (bRoomSessionBound || GetWorld() == nullptr)
    {
        return;
    }
    URoomSessionSubsystem* Session = GetWorld()->GetSubsystem<URoomSessionSubsystem>();
    if (Session == nullptr)
    {
        return;
    }
    RoomSessionPtr = Session;
    RoomRunEndedHandle = Session->OnRunEnded().AddUObject(this, &APrototypeHUD::HandleRunEnded);
    // M3-012: a run starting while the inventory screen is open must disable
    // the equip actions immediately (the visible half of the two-layer gate).
    RoomRunStartedHandle = Session->OnRunStarted().AddUObject(this, &APrototypeHUD::HandleRoomRunStartedForInventory);
    bRoomSessionBound = true;
}

void APrototypeHUD::UnbindRoomSessionEvents()
{
    if (!bRoomSessionBound)
    {
        return;
    }
    if (URoomSessionSubsystem* Session = RoomSessionPtr.Get())
    {
        Session->OnRunEnded().Remove(RoomRunEndedHandle);
        Session->OnRunStarted().Remove(RoomRunStartedHandle);
    }
    RoomSessionPtr = nullptr;
    RoomRunEndedHandle.Reset();
    RoomRunStartedHandle.Reset();
    bRoomSessionBound = false;
}

void APrototypeHUD::SetRoomRetryContext(const URoomDefinition* RoomDef, UEnemyDefinition* EnemyDef)
{
    RetryRoomDefPtr = RoomDef;
    RetryEnemyDefPtr = EnemyDef;
    UE_LOG(LogTemp, Display, TEXT("UEMMO M2-012: retry context registered (room %s)"),
        RoomDef != nullptr ? *RoomDef->RoomId.ToString() : TEXT("none"));
}

bool APrototypeHUD::EnsureResultWidget()
{
    if (ResultWidgetPtr.IsValid())
    {
        return true;
    }
    if (GetWorld() == nullptr)
    {
        return false;
    }
    // Native C++ widget: no UMG asset is involved anywhere on this path. The
    // delegates bind weakly, so a torn-down HUD can never be touched by a
    // leftover widget.
    URoomResultWidget* Widget = CreateWidget<URoomResultWidget>(GetWorld(), URoomResultWidget::StaticClass());
    if (Widget == nullptr)
    {
        UE_LOG(LogTemp, Error, TEXT("UEMMO M2-012: the result widget could not be created."));
        return false;
    }
    TWeakObjectPtr<APrototypeHUD> WeakHUD(this);
    Widget->RetryRequested.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            HUD->HandleRetryRequested();
        }
    });
    Widget->ReturnRequested.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            HUD->HandleReturnRequested();
        }
    });
    // M3-018: the Claim button forwards through the same delegate pattern; the
    // HUD owns the real atomic claim (the widget never claims itself).
    Widget->RewardClaimRequested.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            HUD->HandleRewardClaimRequested();
        }
    });
    ResultWidgetPtr = Widget;
    return true;
}

void APrototypeHUD::HandleRunEnded(const FRoomResult& Result)
{
    // Pure display fill from the session's real terminal result; the action
    // guards re-arm with the fresh screen and the input-focus tracker moves
    // exactly once. The M1 debug overlay and the M1-035 damage feed are
    // untouched (pure additive wiring behind their own flags).
    RoomResultViewModel = MakeRoomResultViewModel(Result);
    RetryGuard.ReArm();
    ReturnGuard.ReArm();

    // M3-018: a CLEARED run settles its reward draft (the idempotent
    // BeginReward answers a repeat of the same SettlementId with the ORIGINAL
    // draft, so a re-presentation never re-rolls); a failed run binds no
    // reward area at all (the card's Failed rule).
    RoomRewardViewModel = FRoomRewardViewModel();
    RewardClaimGuard.ReArm();
    if (Result.bCleared)
    {
        PrepareRoomRewardDraft(Result);
    }

    if (!EnsureResultWidget())
    {
        return;
    }
    if (URoomResultWidget* Widget = ResultWidgetPtr.Get())
    {
        Widget->BindResult(Result);
        Widget->BindReward(RoomRewardViewModel);
    }
    ShowRoomResultScreen();
    // M3-012: a finished run lifts the equip block; if the inventory screen is
    // also open, its action buttons must come back immediately.
    UpdateInventoryEquipContext();

    // M3-030: the terminal-run notification on the game flow (Room -> Result,
    // the result-surface state). Refused outside the room state - the debug
    // staging worlds keep their menu-state flow and never fake a result.
    if (UGameFlowSubsystem* Flow = ResolveGameFlowSubsystem())
    {
        if (Flow->NotifyRunEnded())
        {
            UE_LOG(LogTemp, Display, TEXT("UEMMO M3-030: the flow moved to the result state (%s)."),
                *RoomResultViewModel.HeadlineText);
        }
    }
    // The menu overlay stays hidden in the result state (idempotent no-op).
    RefreshMenuPresentation();

    UE_LOG(LogTemp, Display, TEXT("UEMMO M2-012: result screen prepared (%s, %.1f s, %d kills)"),
        *RoomResultViewModel.HeadlineText, RoomResultViewModel.ElapsedSeconds, RoomResultViewModel.KilledCount);
}

void APrototypeHUD::ShowRoomResultScreen()
{
    URoomResultWidget* Widget = ResultWidgetPtr.Get();
    if (Widget == nullptr || GetWorld() == nullptr)
    {
        return;
    }
    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    const bool bCanPresent = GEngine != nullptr && GEngine->GameViewport != nullptr && PC != nullptr;
    if (bCanPresent && !Widget->IsInViewport())
    {
        Widget->AddToViewport();
    }
    // The input-focus decision is recorded regardless of the presentation
    // ability (the pure tracker is the card's state flag); the real engine
    // input switch happens inside, only where a player controller exists.
    ApplyResultScreenInputCapture();
    if (!bCanPresent)
    {
        // Headless context (world-less automation): the screen stays prepared
        // (widget created and bound) without a viewport or an input switch.
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M2-012: result screen prepared without presentation (no game viewport/player controller)."));
    }
}

void APrototypeHUD::ApplyResultScreenInputCapture()
{
    // The tracker records the decision first; the real engine input switch
    // happens only on an actual phase transition (once per presentation).
    if (!ResultInputFocus.CaptureToUI())
    {
        return;
    }
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    URoomResultWidget* Widget = ResultWidgetPtr.Get();
    if (PC != nullptr && Widget != nullptr)
    {
        // The game input pauses to the UI exactly once per presentation.
        FInputModeUIOnly Mode;
        Mode.SetWidgetToFocus(Widget->TakeWidget());
        PC->SetInputMode(Mode);
        PC->bShowMouseCursor = true;
        UE_LOG(LogTemp, Display, TEXT("UEMMO M2-012: input captured to the result screen (UI focus, cursor shown)."));
    }
}

void APrototypeHUD::HideRoomResultScreen()
{
    if (URoomResultWidget* Widget = ResultWidgetPtr.Get())
    {
        if (Widget->IsInViewport())
        {
            Widget->RemoveFromParent();
        }
    }
    ResultWidgetPtr = nullptr;
    ApplyResultScreenInputRestore();
}

void APrototypeHUD::ApplyResultScreenInputRestore()
{
    if (!ResultInputFocus.RestoreToGame())
    {
        return;
    }
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (PC != nullptr)
    {
        // Back to the game: keyboard and mouse return to the character once.
        PC->SetInputMode(FInputModeGameOnly());
        PC->bShowMouseCursor = false;
        UE_LOG(LogTemp, Display, TEXT("UEMMO M2-012: game input focus restored after the result screen."));
    }
}

APrototypeCharacter* APrototypeHUD::ResolveLocalPlayer()
{
    APrototypeCharacter* Resolved = nullptr;
    if (APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
    {
        Resolved = Cast<APrototypeCharacter>(PC->GetCharacter());
    }
    if (Resolved == nullptr && GetWorld() != nullptr)
    {
        for (TActorIterator<APrototypeCharacter> It(GetWorld()); It; ++It)
        {
            Resolved = *It;
            break;
        }
    }
    return Resolved;
}

void APrototypeHUD::HandleRetryRequested()
{
    // Anti-double-click: the first request consumes the one-shot guard; the
    // duplicate of a fast double click is dropped here.
    if (!RetryGuard.TryAccept())
    {
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M2-012: duplicate retry request dropped (anti double-click)."));
        return;
    }
    const URoomDefinition* RoomDef = RetryRoomDefPtr.Get();
    UEnemyDefinition* EnemyDef = RetryEnemyDefPtr.Get();
    APrototypeCharacter* Player = ResolveLocalPlayer();
    if (GetWorld() == nullptr || RoomDef == nullptr || EnemyDef == nullptr || Player == nullptr)
    {
        // Not a double click but a refused attempt: re-arm so a later press
        // can retry once the context exists; the screen stays up.
        RetryGuard.ReArm();
        if (URoomResultWidget* Widget = ResultWidgetPtr.Get())
        {
            Widget->SetActionButtonsEnabled(true);
        }
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M2-012: retry unavailable (missing retry context or player) - the result screen stays up."));
        return;
    }
    // Dismiss first: the input focus returns to the game before the restart.
    HideRoomResultScreen();
    URoomRetryService::RetryRoom(GetWorld(), RoomDef, EnemyDef, Player);
}

void APrototypeHUD::HandleReturnRequested()
{
    if (!ReturnGuard.TryAccept())
    {
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M2-012: duplicate return request dropped (anti double-click)."));
        return;
    }
    HideRoomResultScreen();
    // M3-030: the return goes through the game flow (Result/Room -> Menu; the
    // M2-011 LeaveRoom semantics run inside and the GameInstance-level profile
    // survives untouched). When the flow cannot accept the return (e.g. the
    // debug staging never entered a room), the direct M2-012 leave keeps the
    // original contract.
    bool bReturnedThroughFlow = false;
    if (UGameFlowSubsystem* Flow = ResolveGameFlowSubsystem())
    {
        bReturnedThroughFlow = Flow->ReturnToMenu();
    }
    if (!bReturnedThroughFlow)
    {
        if (URoomSessionSubsystem* Session = RoomSessionPtr.Get())
        {
            Session->LeaveRoom();
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("UEMMO M2-012: return could not find the room session."));
        }
    }
    // M3-030: back in the menu state the overlay re-presents (the loop
    // closes; the next enter reloads the room world fresh).
    RefreshMenuPresentation();
}

// ----- M3-018: settlement reward claim ----------------------------------------

void APrototypeHUD::PrepareRoomRewardDraft(const FRoomResult& Result)
{
    // The draft lives in the GameInstance-level profile; without it (or the
    // service) there is nowhere to settle, so the reward area stays hidden.
    RoomRewardViewModel = FRoomRewardViewModel();
    UProfileSubsystem* Profile = ResolveProfileSubsystem();
    URewardService* Reward = EnsureRewardService();
    if (Profile == nullptr || Reward == nullptr || !Profile->HasProfile())
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-018: no profile or reward service - the victory screen shows no reward area."));
        return;
    }
    if (!EnsureRewardSettlementCatalog())
    {
        return;
    }

    // Idempotent settlement entry: Applied creates the draft once, a repeat of
    // the same SettlementId answers AlreadyApplied with the ORIGINAL draft (the
    // display data source is that snapshot - same items on every re-open); an
    // already-claimed settlement answers with a default draft and the area
    // stays hidden (nothing pending).
    const FRewardBeginOutcome Begin = Reward->BeginReward(Result, RewardSettlementCatalog);
    if ((Begin.Result == ERewardBeginResult::Applied || Begin.Result == ERewardBeginResult::AlreadyApplied)
        && Begin.Draft.SettlementId == Result.SettlementId)
    {
        RoomRewardViewModel = MakeRoomRewardViewModelFromDraft(Begin.Draft, &RewardSettlementCatalog);
        UE_LOG(LogTemp, Display, TEXT("UEMMO M3-018: reward area bound (settlement %llu, XP %d, %d item(s), begin=%d)."),
            RoomRewardViewModel.SettlementId, RoomRewardViewModel.XP, RoomRewardViewModel.Items.Num(),
            static_cast<int32>(Begin.Result));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-018: the settlement did not produce a pending draft (result %d: %s) - no reward area."),
            static_cast<int32>(Begin.Result), *Begin.Error);
    }
}

URewardService* APrototypeHUD::EnsureRewardService()
{
    URewardService* Reward = RewardServicePtr.Get();
    if (Reward == nullptr)
    {
        Reward = NewObject<URewardService>(this);
        if (Reward == nullptr)
        {
            return nullptr;
        }
        RewardServicePtr = Reward;
    }
    // (Re)bind per use: a fresh GameInstance profile is picked up cheaply and
    // a destroyed subsystem is detected on the service side (weak reference).
    if (UProfileSubsystem* Profile = ResolveProfileSubsystem())
    {
        Reward->BindProfile(Profile);
    }
    return Reward;
}

UProfileSaveService* APrototypeHUD::EnsureRewardSaveService()
{
    if (RewardSaveService != nullptr && !RewardSaveService->GetSlotPrefix().IsEmpty())
    {
        return RewardSaveService;
    }
    // The production prefix of the M3-015/M3-017 chain: the claim's atomic
    // snapshot travels through the same A/B slot pair the startup pass reads.
    UProfileSaveService* Service = NewObject<UProfileSaveService>(this);
    if (Service == nullptr || !Service->Initialize(TEXT("Profile_")))
    {
        // Graceful degradation: the claim answers RejectedNoSaveService, the
        // UI shows a readable failure and the draft stays pending/retryable.
        UE_LOG(LogTemp, Error, TEXT("UEMMO M3-018: the save service rejected the production prefix - the atomic claim will refuse and stay retryable."));
        return nullptr;
    }
    RewardSaveService = Service;
    return Service;
}

bool APrototypeHUD::EnsureRewardSettlementCatalog()
{
    if (bRewardCatalogReady)
    {
        return true;
    }
    // The legacy staging values (the fallback when the production source
    // table cannot be read); the display degrades to the readable
    // "<unknown item>" placeholder for any unresolved id.
    auto StageDefinition = [](FName Id, const FString& DisplayName, EItemSlot Slot,
        float Attack, float Defense, float MaxHP)
    {
        FItemDefinition Definition;
        Definition.DefinitionId = Id;
        Definition.DisplayName = DisplayName;
        Definition.Slot = Slot;
        Definition.BaseStats.Attack = Attack;
        Definition.BaseStats.Defense = Defense;
        Definition.BaseStats.MaxHP = MaxHP;
        Definition.Rarity = EItemRarity::Normal;
        return Definition;
    };
    // M5-020A: the definitions come from the production source table
    // (Data/items.json); a missing/unreadable source (the packaged build
    // carries no Data/ directory) falls back to the legacy staging values
    // with a log - the runtime fallback the contract keeps legal.
    FString CatalogError;
    TArray<FTestRoomItemRow> Rows;
    FString SourceError;
    if (ACombatTestRoomDriver::LoadProductionItems(Rows, SourceError))
    {
        for (const FTestRoomItemRow& Row : Rows)
        {
            FItemDefinition Definition;
            Definition.DefinitionId = Row.DefinitionId;
            Definition.DisplayName = Row.DisplayName;
            Definition.Slot = Row.Slot == TEXT("Weapon") ? EItemSlot::Weapon
                : Row.Slot == TEXT("Armor") ? EItemSlot::Armor
                : Row.Slot == TEXT("Accessory") ? EItemSlot::Accessory
                : EItemSlot::Weapon;
            Definition.BaseStats.Attack = Row.Attack;
            Definition.BaseStats.Defense = Row.Defense;
            Definition.BaseStats.MaxHP = Row.MaxHP;
            Definition.Rarity = static_cast<EItemRarity>(FMath::Clamp(Row.Rarity, 1, 3));
            RewardSettlementCatalog.AddDefinition(Definition, &CatalogError);
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("APrototypeHUD: the items source is unavailable (%s); the legacy staging values apply."),
            *SourceError);
        RewardSettlementCatalog.AddDefinition(
            StageDefinition(TEXT("weapon_training"), TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f), &CatalogError);
        RewardSettlementCatalog.AddDefinition(
            StageDefinition(TEXT("armor_training"), TEXT("Training Armor"), EItemSlot::Armor, 0.0f, 3.0f, 0.0f), &CatalogError);
        RewardSettlementCatalog.AddDefinition(
            StageDefinition(TEXT("charm_training"), TEXT("Training Charm"), EItemSlot::Accessory, 0.0f, 0.0f, 20.0f), &CatalogError);
    }
    bRewardCatalogReady = true;
    return true;
}

void APrototypeHUD::HandleRewardClaimRequested()
{
    // Anti-double-click: the first request consumes the one-shot guard; the
    // duplicate of a fast double click is dropped here (the widget's disabled
    // button is the visible half).
    if (!RewardClaimGuard.TryAccept())
    {
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M3-018: duplicate claim request dropped (anti double-click)."));
        return;
    }
    URoomResultWidget* Widget = ResultWidgetPtr.Get();
    UProfileSubsystem* Profile = ResolveProfileSubsystem();
    URewardService* Reward = EnsureRewardService();
    if (Widget == nullptr || Profile == nullptr || Reward == nullptr || !RoomRewardViewModel.bValid)
    {
        // Not a double click but a refused attempt: re-arm so a later press
        // can retry once the context exists; the screen stays up.
        RewardClaimGuard.ReArm();
        if (Widget != nullptr)
        {
            FRewardClaimAtomicOutcome Refused;
            Refused.Result = ERewardClaimAtomicResult::RejectedNoProfile;
            Refused.Error = TEXT("no profile or reward context is available for the claim");
            Widget->ApplyRewardClaimOutcome(Refused);
        }
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-018: claim unavailable (missing profile/context) - the reward stays pending."));
        return;
    }

    // The claim is in flight: the visible Saving state (the save may take
    // frames; the returned outcome below is the only confirmation gate).
    Widget->SetRewardClaimSaving();

    // The M3-016 atomic commit: XP + items + PendingRewards + applied ids as
    // ONE snapshot; without a save service it refuses and the UI stays
    // retryable (graceful degradation, never a fake completion).
    UProfileSaveService* SaveService = EnsureRewardSaveService();
    const FRewardClaimAtomicOutcome Outcome = Reward->ClaimPendingAtomic(
        RoomRewardViewModel.SettlementId, Profile, SaveService);
    Widget->ApplyRewardClaimOutcome(Outcome);
    RoomRewardViewModel = Widget->PeekRewardViewModel();

    UE_LOG(LogTemp, Display, TEXT("UEMMO M3-018: claim outcome %d (stored %d, retained %d, xp %d, save %d): %s"),
        static_cast<int32>(Outcome.Result), Outcome.ClaimedItemCount, Outcome.RetainedItemCount,
        Outcome.bGrantedXP ? 1 : 0, Outcome.bSaveCommitted ? 1 : 0, *Outcome.Error);
}

const UGameFlowSubsystem* APrototypeHUD::ResolveGameFlowSubsystem() const
{
    const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
    return GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
}

UGameFlowSubsystem* APrototypeHUD::ResolveGameFlowSubsystem()
{
    UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
    return GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
}

// ----- M3-030: boot menu mount (the full game-flow loop) -----------------------

bool APrototypeHUD::EnsureMenuWidget()
{
    if (MenuWidgetPtr.IsValid())
    {
        return true;
    }
    if (GetWorld() == nullptr)
    {
        return false;
    }
    // Native C++ widget: no UMG asset is involved anywhere on this path. The
    // enter-accepted delegate binds weakly, so a torn-down HUD can never be
    // touched by a leftover widget.
    UMapSelectWidget* Widget = CreateWidget<UMapSelectWidget>(GetWorld(), UMapSelectWidget::StaticClass());
    if (Widget == nullptr)
    {
        UE_LOG(LogTemp, Error, TEXT("UEMMO M3-030: the menu widget could not be created."));
        return false;
    }
    TWeakObjectPtr<APrototypeHUD> WeakHUD(this);
    Widget->EnterAccepted.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            // The flow accepted the enter: the room world owns the screen now
            // (in production the travel reloads the boot map and this HUD
            // dies; the dismissal restores the game input for the travel).
            HUD->HideMenuOverlay();
        }
    });
    MenuWidgetPtr = Widget;
    return true;
}

void APrototypeHUD::ShowMenuOverlay()
{
    if (!EnsureMenuWidget() || GetWorld() == nullptr)
    {
        return;
    }
    UMapSelectWidget* Widget = MenuWidgetPtr.Get();

    // The mount's enter target: the wave-combat room of the boot world (its
    // trigger chain starts the run there), NOT the M3-017 selectable entry's
    // training-map identity. Without a flow the widget keeps the selectable
    // default (the M3-017 form).
    if (UGameFlowSubsystem* Flow = ResolveGameFlowSubsystem())
    {
        Widget->SetEnterRoomDefinition(Flow->GetCombatRoomDefinition());
    }
    // A fresh presentation re-arms the enter button and clears a stale error
    // line (the M3-017 BindMenu re-presentation contract).
    Widget->BindMenu();

    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    const bool bCanPresent = GEngine != nullptr && GEngine->GameViewport != nullptr && PC != nullptr;
    if (bCanPresent && !Widget->IsInViewport())
    {
        Widget->AddToViewport();
    }
    ApplyMenuInputCapture();
    if (!bCanPresent)
    {
        // Headless context (world-less automation): the overlay stays prepared
        // (widget created and armed) without a viewport or an input switch.
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M3-030: menu overlay prepared without presentation (no game viewport/player controller)."));
    }
    UE_LOG(LogTemp, Display, TEXT("UEMMO M3-030: menu overlay presented (the flow menu state owns the screen)."));
}

void APrototypeHUD::HideMenuOverlay()
{
    if (UMapSelectWidget* Widget = MenuWidgetPtr.Get())
    {
        if (Widget->IsInViewport())
        {
            Widget->RemoveFromParent();
        }
    }
    MenuWidgetPtr = nullptr;
    ApplyMenuInputRestore();
}

void APrototypeHUD::ApplyMenuInputCapture()
{
    // The tracker records the decision first; the real engine input switch
    // happens only on an actual phase transition (once per presentation).
    if (!MenuInputFocus.CaptureToUI())
    {
        return;
    }
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    UMapSelectWidget* Widget = MenuWidgetPtr.Get();
    if (PC != nullptr && Widget != nullptr)
    {
        // The menu owns the input: keyboard/mouse leave the character and the
        // cursor appears for the enter button (the M2-012 capture pattern).
        FInputModeUIOnly Mode;
        Mode.SetWidgetToFocus(Widget->TakeWidget());
        PC->SetInputMode(Mode);
        PC->bShowMouseCursor = true;
        UE_LOG(LogTemp, Display, TEXT("UEMMO M3-030: input captured to the menu overlay (UI focus, cursor shown)."));
    }
}

void APrototypeHUD::ApplyMenuInputRestore()
{
    if (!MenuInputFocus.RestoreToGame())
    {
        return;
    }
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (PC != nullptr)
    {
        // Back to the game: keyboard and mouse return to the character once.
        PC->SetInputMode(FInputModeGameOnly());
        PC->bShowMouseCursor = false;
        UE_LOG(LogTemp, Display, TEXT("UEMMO M3-030: game input focus restored after the menu overlay."));
    }
}

void APrototypeHUD::RefreshMenuPresentation()
{
    // The menu overlay is exactly the flow's menu-state surface: presented
    // while the flow is in Menu (the boot world, and the world after a
    // result-screen return) and dismissed in every other state (Loading /
    // Room / Result). Idempotent: the show/hide halves are tracker- and
    // weak-pointer-gated, so calling this at any change point is safe.
    const UGameFlowSubsystem* Flow = ResolveGameFlowSubsystem();
    const bool bMenuState = Flow != nullptr && Flow->GetState() == EGameFlowState::Menu;
    if (bMenuState)
    {
        ShowMenuOverlay();
    }
    else
    {
        HideMenuOverlay();
    }
}

// ----- M3-011: read-only inventory list screen --------------------------------

void APrototypeHUD::UEMMODebugInventory(int32 Mode)
{
    UE_LOG(LogTemp, Display, TEXT("UEMMO M3-011: inventory exec mode %d"), Mode);
    if (Mode == 0)
    {
        // The real close path (input focus restored to the game).
        HandleInventoryCloseRequested();
        return;
    }
    if (Mode == 2)
    {
        // Debug-only fill-to-capacity staging (never gameplay): tops the staged
        // inventory up to the 30-slot capacity through the production TryAdd,
        // then presents (or throttled-refreshes) the list - the scroll and
        // rebuild evidence.
        if (!EnsureInventoryStaging())
        {
            return;
        }
        if (UProfileSubsystem* Profile = ResolveProfileSubsystem())
        {
            FInventoryModel& Inventory = Profile->GetInventory();
            const FItemDefinition* WeaponDef = InventoryStagingCatalog.Find(TEXT("weapon_training"));
            int64 Seed = 100;
            while (WeaponDef != nullptr && Inventory.Count() < FInventoryModel::Capacity)
            {
                if (Inventory.TryAdd(MakeItemInstance(*WeaponDef, Seed++)) != EInventoryAddResult::Added)
                {
                    break;
                }
            }
            UE_LOG(LogTemp, Display, TEXT("UEMMO M3-011: staged inventory filled to %d/%d slots"),
                Inventory.Count(), FInventoryModel::Capacity);
        }
        if (InventoryWidgetPtr.IsValid())
        {
            RefreshInventoryScreen();
        }
        else
        {
            ShowInventoryScreen();
        }
        return;
    }

    // Mode 1 (default): stage the starter inventory once, then present.
    EnsureInventoryStaging();
    ShowInventoryScreen();
}

void APrototypeHUD::RefreshInventoryScreen()
{
    // The HUD-side change-point entry (reward claim / equip UI call this later;
    // the debug exec uses it today). No-op while no screen is presented.
    UInventoryWidget* Widget = InventoryWidgetPtr.Get();
    if (Widget == nullptr)
    {
        return;
    }
    UProfileSubsystem* Profile = ResolveProfileSubsystem();
    TArray<FItemInstance> EmptyItems;
    const TArray<FItemInstance>& Items = (Profile != nullptr) ? Profile->GetInventory().GetAll() : EmptyItems;
    // Throttled: the widget's fingerprint gate skips identical snapshots, so
    // calling this at every change point never rebuilds unchanged rows.
    Widget->RefreshIfChanged(Items, InventoryDisplayCatalog, EquippedInventoryIds);
    InventoryViewModel = Widget->PeekViewModel();
    // M3-012: the context push recomputes the preview and the button states
    // (the running flag and the base stats may have changed independently of
    // the list fingerprint).
    UpdateInventoryEquipContext();
}

void APrototypeHUD::HandleInventoryCloseRequested()
{
    HideInventoryScreen();
}

void APrototypeHUD::SetEquippedInventoryIds(const TSet<FGuid>& Ids)
{
    // Debug/test seam: no production equipment mapping reaches the HUD yet
    // (M3-010 wired stat rows only), so the empty default means "no markers".
    EquippedInventoryIds = Ids;
}

bool APrototypeHUD::EnsureInventoryWidget()
{
    if (InventoryWidgetPtr.IsValid())
    {
        return true;
    }
    if (GetWorld() == nullptr)
    {
        return false;
    }
    // Native C++ widget: no UMG asset is involved anywhere on this path. The
    // close delegate binds weakly, so a torn-down HUD can never be touched by
    // a leftover widget.
    UInventoryWidget* Widget = CreateWidget<UInventoryWidget>(GetWorld(), UInventoryWidget::StaticClass());
    if (Widget == nullptr)
    {
        UE_LOG(LogTemp, Error, TEXT("UEMMO M3-011: the inventory widget could not be created."));
        return false;
    }
    TWeakObjectPtr<APrototypeHUD> WeakHUD(this);
    Widget->CloseRequested.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            HUD->HandleInventoryCloseRequested();
        }
    });
    // M3-012: the equip panel's intents route through the HUD (the M2-012
    // button-callback pattern): every row click re-arms the one-shot action
    // guard, the buttons execute the real request paths.
    Widget->SelectionChanged.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            HUD->InventoryActionGuard.ReArm();
        }
    });
    Widget->EquipRequested.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            HUD->HandleInventoryEquipRequested();
        }
    });
    Widget->UnequipRequested.AddLambda([WeakHUD]()
    {
        if (APrototypeHUD* HUD = WeakHUD.Get())
        {
            HUD->HandleInventoryUnequipRequested();
        }
    });
    InventoryWidgetPtr = Widget;
    return true;
}

void APrototypeHUD::ShowInventoryScreen()
{
    if (!EnsureInventoryWidget() || GetWorld() == nullptr)
    {
        return;
    }
    UInventoryWidget* Widget = InventoryWidgetPtr.Get();

    // Data source: the real profile inventory (a missing profile degrades to
    // the empty state - the read-only screen never invents items) plus the
    // equipped-id set and the display catalog (null -> placeholder rows).
    UProfileSubsystem* Profile = ResolveProfileSubsystem();
    TArray<FItemInstance> EmptyItems;
    const TArray<FItemInstance>& Items = (Profile != nullptr) ? Profile->GetInventory().GetAll() : EmptyItems;
    Widget->BindInventory(Items, InventoryDisplayCatalog, EquippedInventoryIds);
    InventoryViewModel = Widget->PeekViewModel();

    // M3-012: bind the equipment model to the (possibly new) profile inventory
    // and push the fresh context; a fresh presentation re-arms the one-shot
    // action guard (one action token per presentation, like the M2-012 screen).
    EnsureInventoryEquipmentWiring();
    InventoryActionGuard.ReArm();
    UpdateInventoryEquipContext();

    APlayerController* PC = GetWorld()->GetFirstPlayerController();
    const bool bCanPresent = GEngine != nullptr && GEngine->GameViewport != nullptr && PC != nullptr;
    if (bCanPresent && !Widget->IsInViewport())
    {
        Widget->AddToViewport();
    }
    ApplyInventoryInputCapture();
    if (!bCanPresent)
    {
        // Headless context (world-less automation): the screen stays prepared
        // (widget created and bound) without a viewport or an input switch.
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M3-011: inventory screen prepared without presentation (no game viewport/player controller)."));
    }
    UE_LOG(LogTemp, Display, TEXT("UEMMO M3-011: inventory screen presented (%d items, %d equipped ids)"),
        InventoryViewModel.Rows.Num(), EquippedInventoryIds.Num());
}

void APrototypeHUD::HideInventoryScreen()
{
    if (UInventoryWidget* Widget = InventoryWidgetPtr.Get())
    {
        if (Widget->IsInViewport())
        {
            Widget->RemoveFromParent();
        }
    }
    InventoryWidgetPtr = nullptr;
    ApplyInventoryInputRestore();
}

void APrototypeHUD::ApplyInventoryInputCapture()
{
    // The tracker records the decision first; the real engine input switch
    // happens only on an actual phase transition (once per presentation).
    if (!InventoryInputFocus.CaptureToUI())
    {
        return;
    }
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    UInventoryWidget* Widget = InventoryWidgetPtr.Get();
    if (PC != nullptr && Widget != nullptr)
    {
        // The game input pauses to the UI exactly once per presentation.
        FInputModeUIOnly Mode;
        Mode.SetWidgetToFocus(Widget->TakeWidget());
        PC->SetInputMode(Mode);
        PC->bShowMouseCursor = true;
        UE_LOG(LogTemp, Display, TEXT("UEMMO M3-011: input captured to the inventory screen (UI focus, cursor shown)."));
    }
}

void APrototypeHUD::ApplyInventoryInputRestore()
{
    if (!InventoryInputFocus.RestoreToGame())
    {
        return;
    }
    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (PC != nullptr)
    {
        // Back to the game: keyboard and mouse return to the character once.
        PC->SetInputMode(FInputModeGameOnly());
        PC->bShowMouseCursor = false;
        UE_LOG(LogTemp, Display, TEXT("UEMMO M3-011: game input focus restored after the inventory screen."));
    }
}

UProfileSubsystem* APrototypeHUD::ResolveProfileSubsystem()
{
    UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
    return GameInstance ? GameInstance->GetSubsystem<UProfileSubsystem>() : nullptr;
}

bool APrototypeHUD::EnsureInventoryStaging()
{
    // Debug-exec-only staging (headless render evidence and manual browsing;
    // gameplay never calls this): a local profile through the production
    // NewProfile when the world has none, transient definition doubles (the
    // M2-007+ precedent - production display data arrives with a later task)
    // and three starter items through the production TryAdd.
    UProfileSubsystem* Profile = ResolveProfileSubsystem();
    if (Profile == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-011: staging skipped (no profile subsystem in this world)."));
        return false;
    }
    if (!Profile->HasProfile())
    {
        Profile->NewProfile();
        UE_LOG(LogTemp, Display, TEXT("UEMMO M3-011: debug staging minted a local profile (debug-only)."));
    }
    if (bInventoryStaged)
    {
        return true;
    }

    auto StageDefinition = [](FName Id, const FString& DisplayName, EItemSlot Slot,
        float Attack, float Defense, float MaxHP)
    {
        FItemDefinition Definition;
        Definition.DefinitionId = Id;
        Definition.DisplayName = DisplayName;
        Definition.Slot = Slot;
        Definition.BaseStats.Attack = Attack;
        Definition.BaseStats.Defense = Defense;
        Definition.BaseStats.MaxHP = MaxHP;
        Definition.Rarity = EItemRarity::Normal;
        return Definition;
    };

    // M5-020A: the staging definitions come from the production source table
    // (the M3-011 debug divergence - accessory_training 1/0/5, armor max_hp
    // 20 - is eliminated here); a missing source falls back to the legacy
    // values with a log. The charm registers under the authority id
    // charm_training; the legacy accessory_training instances resolve through
    // the frozen alias at lookup (their InstanceIds never change).
    TArray<FTestRoomItemRow> StagingRows;
    FString StagingSourceError;
    const bool bProductionRows = ACombatTestRoomDriver::LoadProductionItems(StagingRows, StagingSourceError);
    if (!bProductionRows)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("APrototypeHUD: the items source is unavailable (%s); the legacy staging values apply."),
            *StagingSourceError);
    }
    auto RowToDefinition = [](const FTestRoomItemRow& Row)
    {
        FItemDefinition Definition;
        Definition.DefinitionId = Row.DefinitionId;
        Definition.DisplayName = Row.DisplayName;
        Definition.Slot = Row.Slot == TEXT("Weapon") ? EItemSlot::Weapon
            : Row.Slot == TEXT("Armor") ? EItemSlot::Armor
            : Row.Slot == TEXT("Accessory") ? EItemSlot::Accessory
            : EItemSlot::Weapon;
        Definition.BaseStats.Attack = Row.Attack;
        Definition.BaseStats.Defense = Row.Defense;
        Definition.BaseStats.MaxHP = Row.MaxHP;
        Definition.Rarity = static_cast<EItemRarity>(FMath::Clamp(Row.Rarity, 1, 3));
        return Definition;
    };
    const FItemDefinition Weapon = bProductionRows
        ? RowToDefinition(StagingRows[0])
        : StageDefinition(TEXT("weapon_training"), TEXT("Training Sword"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f);
    const FItemDefinition Armor = bProductionRows
        ? RowToDefinition(StagingRows[1])
        : StageDefinition(TEXT("armor_training"), TEXT("Training Armor"), EItemSlot::Armor, 0.0f, 3.0f, 20.0f);
    const FItemDefinition Charm = bProductionRows
        ? RowToDefinition(StagingRows[2])
        : StageDefinition(TEXT("accessory_training"), TEXT("Training Charm"), EItemSlot::Accessory, 1.0f, 0.0f, 5.0f);
    FString CatalogError;
    InventoryStagingCatalog.AddDefinition(Weapon, &CatalogError);
    InventoryStagingCatalog.AddDefinition(Armor, &CatalogError);
    InventoryStagingCatalog.AddDefinition(Charm, &CatalogError);
    InventoryDisplayCatalog = &InventoryStagingCatalog;

    FInventoryModel& Inventory = Profile->GetInventory();
    const FItemDefinition* StageDefs[3] = {
        InventoryStagingCatalog.Find(ACombatTestRoomDriver::ResolveItemAlias(TEXT("weapon_training"))),
        InventoryStagingCatalog.Find(ACombatTestRoomDriver::ResolveItemAlias(TEXT("armor_training"))),
        InventoryStagingCatalog.Find(ACombatTestRoomDriver::ResolveItemAlias(TEXT("accessory_training")))
    };
    TOptional<FGuid> FirstStagedWeaponId;
    for (int32 Index = 0; Index < 3; ++Index)
    {
        if (StageDefs[Index] == nullptr)
        {
            continue;
        }
        const FItemInstance Item = MakeItemInstance(*StageDefs[Index], Index + 1);
        if (Inventory.TryAdd(Item) == EInventoryAddResult::Added)
        {
            if (Index == 0)
            {
                FirstStagedWeaponId = Item.InstanceId;
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-011: staging item %d was rejected by the inventory."), Index);
        }
    }

    // M3-012: the first staged weapon equips through the real equipment model
    // (the marker comes from the model now, not a hand-made id set) and the
    // matching bonus row is pushed so the staged "current" stats match the
    // staged mapping. Debug staging pushes the row directly (headless-safe,
    // no pawn exists in the render-evidence worlds); the PRODUCTION equip flow
    // always goes through the pawn's TryEquipStatBonus gate.
    if (FirstStagedWeaponId.IsSet() && EnsureInventoryEquipmentWiring())
    {
        const EEquipmentEquipResult EquipResult = InventoryEquipment.Equip(
            EItemSlot::Weapon, FirstStagedWeaponId.GetValue(), Profile->GetInventory());
        if (EquipResult == EEquipmentEquipResult::Equipped)
        {
            RebuildEquippedInventoryIds();
            Profile->SetEquippedStatBonus(ComputeEquippedBonusFromModel());
            UE_LOG(LogTemp, Display, TEXT("UEMMO M3-012: staged the first weapon equipped through the real model."));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-012: staging equip refused (result %d)."),
                static_cast<int32>(EquipResult));
        }
    }

    bInventoryStaged = true;
    UE_LOG(LogTemp, Display, TEXT("UEMMO M3-011: staged 3 starter items (first marked equipped)."));
    return true;
}

// ----- M3-012: inventory equip actions ----------------------------------------

void APrototypeHUD::HandleInventoryEquipRequested()
{
    HandleInventoryEquipAction(/*bEquip*/ true);
}

void APrototypeHUD::HandleInventoryUnequipRequested()
{
    HandleInventoryEquipAction(/*bEquip*/ false);
}

void APrototypeHUD::HandleInventoryEquipAction(bool bEquip)
{
    UInventoryWidget* Widget = InventoryWidgetPtr.Get();
    UProfileSubsystem* Profile = ResolveProfileSubsystem();

    // One-shot guard first (the M2-012 pattern): the fast duplicate of a
    // double click is dropped here; the click-disabled buttons are re-derived
    // so they cannot stay stuck when nothing was processed.
    if (!InventoryActionGuard.TryAccept())
    {
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M3-012: duplicate %s request dropped (anti double-click)."),
            bEquip ? TEXT("equip") : TEXT("unequip"));
        if (Widget != nullptr)
        {
            Widget->RefreshEquipPreviewAndActions();
        }
        return;
    }

    // Context pre-checks BEFORE any mapping write: the same gate the pawn's
    // TryEquipStatBonus enforces (profile exists, not Running, player entry
    // available), so a blocked request cannot even half-apply to the model.
    const bool bMissingProfile = (Profile == nullptr || !Profile->HasProfile());
    const bool bRunning = IsRoomSessionRunning();
    APrototypeCharacter* Player = ResolveLocalPlayer();
    const FString BlockReason = MakeInventoryEquipBlockText(bMissingProfile, bRunning, Player == nullptr);
    if (!BlockReason.IsEmpty())
    {
        // A refused attempt re-arms (the M2-012 refused-retry precedent) so a
        // later request can proceed once the context allows it.
        InventoryActionGuard.ReArm();
        if (Widget != nullptr)
        {
            Widget->SetInventoryStatusText(BlockReason);
        }
        UpdateInventoryEquipContext();
        return;
    }

    // The selection decides the target instance; the widget owns it. The
    // selection is re-resolved against the LIVE inventory (a reward claim or
    // another flow may have changed it since the row was clicked).
    const FGuid SelectedId = (Widget != nullptr && Widget->HasSelection())
        ? Widget->PeekSelectedInstanceId() : FGuid();
    const FItemInstance* Selected = nullptr;
    if (SelectedId.IsValid())
    {
        for (const FItemInstance& Instance : Profile->GetInventory().GetAll())
        {
            if (Instance.InstanceId == SelectedId)
            {
                Selected = &Instance;
                break;
            }
        }
    }
    if (!SelectedId.IsValid() || Selected == nullptr)
    {
        InventoryActionGuard.ReArm();
        if (Widget != nullptr)
        {
            Widget->SetInventoryStatusText(TEXT("Select an item first"));
        }
        UpdateInventoryEquipContext();
        return;
    }

    // The slot of the selection; the model re-verifies the slot match itself
    // (an unknown definition cannot confirm any slot and Equip rejects it).
    EItemSlot Slot = EItemSlot::Weapon;
    if (const FItemDefinition* Definition = (InventoryDisplayCatalog != nullptr)
        ? InventoryDisplayCatalog->Find(Selected->DefinitionId) : nullptr)
    {
        Slot = Definition->Slot;
    }

    // The pre-action mapping and bonus row, for the defensive rollback if the
    // bottom entry refuses AFTER the mapping moved (both sides are
    // pre-checked above, so this is unreachable in practice - kept honest).
    const FGuid* PreviousIdPtr = InventoryEquipment.GetEquippedId(Slot);
    const FGuid PreviousId = (PreviousIdPtr != nullptr) ? *PreviousIdPtr : FGuid();
    const FItemStats PreviousBonus = ComputeEquippedBonusFromModel();
    bool bSucceeded = false;
    FString ResultText;

    if (bEquip)
    {
        const EEquipmentEquipResult Result = InventoryEquipment.Equip(Slot, SelectedId, Profile->GetInventory());
        if (Result == EEquipmentEquipResult::Equipped || Result == EEquipmentEquipResult::AlreadyEquipped)
        {
            // The production push: the fresh bonus row goes through the pawn's
            // TryEquipStatBonus (the bottom Running gate + the no-heal clamp).
            if (Player->TryEquipStatBonus(ComputeEquippedBonusFromModel()))
            {
                bSucceeded = true;
                ResultText = MakeEquipResultText(Result);
            }
            else
            {
                // Roll the mapping and the stored row back (defensive).
                if (PreviousId.IsValid())
                {
                    InventoryEquipment.Equip(Slot, PreviousId, Profile->GetInventory());
                }
                else
                {
                    InventoryEquipment.Unequip(Slot);
                }
                Player->TryEquipStatBonus(PreviousBonus);
                ResultText = TEXT("The equip entry refused the request");
            }
        }
        else
        {
            // Pure model rejection: no mapping changed; show the reason.
            ResultText = MakeEquipResultText(Result);
        }
    }
    else
    {
        // Unequip acts on the slot that holds the selection (no definition
        // needed): a selection that equips nothing is the NotEquipped no-op.
        EItemSlot HeldSlot = EItemSlot::Weapon;
        bool bHeld = false;
        for (int32 SlotIndex = 0; SlotIndex < 3; ++SlotIndex)
        {
            const EItemSlot Candidate = static_cast<EItemSlot>(SlotIndex);
            const FGuid* HeldId = InventoryEquipment.GetEquippedId(Candidate);
            if (HeldId != nullptr && *HeldId == SelectedId)
            {
                HeldSlot = Candidate;
                bHeld = true;
                break;
            }
        }
        if (bHeld)
        {
            const EEquipmentUnequipResult Result = InventoryEquipment.Unequip(HeldSlot);
            if (Result == EEquipmentUnequipResult::Unequipped)
            {
                if (Player->TryEquipStatBonus(ComputeEquippedBonusFromModel()))
                {
                    bSucceeded = true;
                    ResultText = MakeUnequipResultText(Result);
                }
                else
                {
                    // Roll the mapping and the stored row back (defensive).
                    InventoryEquipment.Equip(HeldSlot, SelectedId, Profile->GetInventory());
                    Player->TryEquipStatBonus(PreviousBonus);
                    ResultText = TEXT("The equip entry refused the request");
                }
            }
            else
            {
                ResultText = MakeUnequipResultText(Result);
            }
        }
        else
        {
            ResultText = MakeUnequipResultText(EEquipmentUnequipResult::NotEquipped);
        }
    }

    if (bSucceeded)
    {
        // Success tail: the markers rebuild from the model and the throttled
        // M3-011 refresh entry rebuilds the list (the fingerprint changed).
        // The guard stays CONSUMED: acting again on the same selection needs
        // an explicit re-select (the anti-double-click rule; SelectionChanged
        // re-arms, as does any failed attempt or a fresh presentation).
        RebuildEquippedInventoryIds();
        RefreshInventoryScreen();
    }
    else
    {
        InventoryActionGuard.ReArm();
    }
    if (Widget != nullptr)
    {
        Widget->SetInventoryStatusText(ResultText);
    }
    UpdateInventoryEquipContext();
}

bool APrototypeHUD::EnsureInventoryEquipmentWiring()
{
    UProfileSubsystem* Profile = ResolveProfileSubsystem();
    if (Profile == nullptr || !Profile->HasProfile())
    {
        return false;
    }
    // The display catalog doubles as the equip slot-match source today (the
    // debug staging registers the transient training definitions; a
    // production catalog asset arrives with a later task). A missing catalog
    // makes every Equip fail with MissingDefinitions - never silently.
    InventoryEquipment.SetDefinitionCatalog(InventoryDisplayCatalog);
    // Re-attach on every call: NewProfile reassigns the backing inventory and
    // FInventoryModel drops equip guards on copy/assignment, so the guard must
    // be (re-)registered against the CURRENT inventory object.
    InventoryEquipment.AttachToInventory(Profile->GetInventory());
    return true;
}

void APrototypeHUD::RebuildEquippedInventoryIds()
{
    // The M3-011 equipped-id set is DERIVED from the model mapping now (one
    // id per occupied slot), so the list markers can never disagree with it.
    EquippedInventoryIds.Reset();
    for (int32 SlotIndex = 0; SlotIndex < 3; ++SlotIndex)
    {
        const EItemSlot Slot = static_cast<EItemSlot>(SlotIndex);
        if (const FGuid* EquippedId = InventoryEquipment.GetEquippedId(Slot))
        {
            EquippedInventoryIds.Add(*EquippedId);
        }
    }
}

FItemStats APrototypeHUD::ComputeEquippedBonusFromModel()
{
    // The equipment STAT SUM pushed into the profile (the M3-005 contract:
    // "Base + Sigma of every equipped instance's rolled stats" - this is the
    // Sigma side). This row is NOT a final row, so the final-only MaxHP floor
    // of FStatCalculator::Recalculate must NOT apply here (a no-HP loadout is
    // a zero row, never a +1; the floor belongs to the profile's FINAL
    // recalculation, which runs on the level base plus this row). The
    // per-entry hardening mirrors the M3-005 rules: a non-finite entry is
    // skipped as a whole (no trustworthy magnitude) and a negative field
    // clamps to 0 (corrupted data of that field, never a stat drain).
    FItemStats Bonus;
    UProfileSubsystem* Profile = ResolveProfileSubsystem();
    if (Profile != nullptr)
    {
        for (int32 SlotIndex = 0; SlotIndex < 3; ++SlotIndex)
        {
            const FGuid* EquippedId = InventoryEquipment.GetEquippedId(static_cast<EItemSlot>(SlotIndex));
            if (EquippedId == nullptr)
            {
                continue;
            }
            for (const FItemInstance& Instance : Profile->GetInventory().GetAll())
            {
                if (Instance.InstanceId != *EquippedId)
                {
                    continue;
                }
                const FItemStats& Stats = Instance.RolledStats;
                if (FMath::IsFinite(Stats.Attack) && FMath::IsFinite(Stats.Defense) && FMath::IsFinite(Stats.MaxHP))
                {
                    Bonus.Attack += FMath::Max(Stats.Attack, 0.0f);
                    Bonus.Defense += FMath::Max(Stats.Defense, 0.0f);
                    Bonus.MaxHP += FMath::Max(Stats.MaxHP, 0.0f);
                }
                break;
            }
        }
    }
    return Bonus;
}

void APrototypeHUD::UpdateInventoryEquipContext()
{
    UInventoryWidget* Widget = InventoryWidgetPtr.Get();
    if (Widget == nullptr)
    {
        return;
    }
    // The level base row comes from the shared static formulas (never from a
    // World's HealthComponent - the snapshot rule, interface contract 8).
    bool bHasProfile = false;
    FItemStats Base;
    if (UProfileSubsystem* Profile = ResolveProfileSubsystem())
    {
        if (Profile->HasProfile())
        {
            bHasProfile = true;
            const int32 Level = Profile->GetLevel();
            Base.MaxHP = static_cast<float>(UProfileSubsystem::GetMaxHPForLevel(Level));
            Base.Attack = static_cast<float>(UProfileSubsystem::GetAttackForLevel(Level));
            Base.Defense = static_cast<float>(UProfileSubsystem::GetDefenseForLevel(Level));
        }
    }
    Widget->SetEquipContext(bHasProfile, Base, IsRoomSessionRunning());
    InventoryPreview = Widget->PeekPreview();
}

bool APrototypeHUD::IsRoomSessionRunning() const
{
    const URoomSessionSubsystem* Session = RoomSessionPtr.Get();
    return Session != nullptr && Session->GetState() == ERoomSessionState::Running;
}

void APrototypeHUD::HandleRoomRunStartedForInventory()
{
    // A run started while the inventory screen may be open: refresh the
    // context so the equip actions disable immediately (the visible half of
    // the two-layer Running gate; the pawn's TryEquipStatBonus is the other).
    UpdateInventoryEquipContext();
}

bool APrototypeHUD::EnsureStageDefinitions()
{
    if (StageRoomDefinition != nullptr && StageEnemyDefinition != nullptr)
    {
        SetRoomRetryContext(StageRoomDefinition, StageEnemyDefinition);
        return true;
    }
    APrototypeCharacter* Player = ResolveLocalPlayer();
    if (Player == nullptr || GetWorld() == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M2-012: staging skipped (no player to anchor the stage room)."));
        return false;
    }
    // Transient definition double (the M2-007+ test precedent: runtime
    // definitions; the JSON-to-asset catalog belongs to a later task).
    UEnemyDefinition* Enemy = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
    Enemy->EnemyId = FName(TEXT("melee_grunt"));
    Enemy->MaxHP = 60.0f;
    Enemy->AttackPower = 0.0f;
    Enemy->MoveSpeed = 220.0f;
    Enemy->AttackRangeX = 160.0f;
    Enemy->AlignYTolerance = 35.0f;
    Enemy->TelegraphSeconds = 0.35f;
    Enemy->SpawnGraceSeconds = 0.5f;
    Enemy->MeleeAttackId = FName(TEXT("light_01"));

    URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
    Room->RoomId = FName(TEXT("room_m2_012_stage"));
    Room->RewardTableId = FName(TEXT("starter"));
    FRoomWaveDefinition Wave;
    Wave.EnemyId = FName(TEXT("melee_grunt"));
    Wave.Count = 1;
    Wave.SpawnLocations.Add(Player->GetActorLocation() + FVector(420.0, 0.0, 0.0));
    Room->Waves.Add(Wave);

    StageRoomDefinition = Room;
    StageEnemyDefinition = Enemy;
    SetRoomRetryContext(Room, Enemy);
    return true;
}

void APrototypeHUD::UEMMODebugRoomResult(int32 Mode)
{
    UE_LOG(LogTemp, Display, TEXT("UEMMO M2-012: result screen exec mode %d"), Mode);
    if (Mode == 0)
    {
        HideRoomResultScreen();
        return;
    }
    if (Mode == 3)
    {
        HandleRetryRequested(); // the real request path (a context must exist)
        return;
    }
    if (Mode == 4)
    {
        HandleReturnRequested(); // the real request path
        return;
    }
    if (Mode != 1 && Mode != 2)
    {
        return;
    }
    URoomSessionSubsystem* Session = GetWorld() ? GetWorld()->GetSubsystem<URoomSessionSubsystem>() : nullptr;
    if (Session == nullptr || !EnsureStageDefinitions())
    {
        return;
    }
    if (Session->GetState() == ERoomSessionState::Running)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M2-012: staging refused - a run is already active."));
        return;
    }
    // Debug-only session clock injection through the real SetSessionClockSeconds
    // entry (the game frame driver is a later task, so no other injection
    // exists in-game); the elapsed display is the session's own measured truth.
    Session->SetSessionClockSeconds(GetWorld()->GetTimeSeconds());
    if (!Session->StartRoom(StageRoomDefinition) || !Session->BeginWaves(StageRoomDefinition, StageEnemyDefinition))
    {
        return;
    }
    Session->SetSessionClockSeconds(GetWorld()->GetTimeSeconds());
    // The session's own production terminal entries drive the real broadcast:
    // mode 1 -> MarkCleared (the wave progression's clearing entry), mode 2 ->
    // FailRun (the player-death failure entry). The OnRunEnded handler then
    // presents the screen through the exact gameplay path.
    if (Mode == 1)
    {
        Session->MarkCleared();
    }
    else
    {
        Session->FailRun();
    }
}
