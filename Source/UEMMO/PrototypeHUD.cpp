#include "PrototypeHUD.h"
#include "Engine/Canvas.h"

#include "Combat/AttackDefinition.h"
#include "Combat/CombatComponent.h"
#include "Combat/CombatGeometry.h"
#include "Combat/HealthComponent.h"
#include "Enemy/EnemyDefinition.h"
#include "Enemy/TrainingEnemy.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "PrototypeCharacter.h"
#include "Room/RoomDefinition.h"
#include "Room/RoomRetryService.h"
#include "Room/RoomSessionSubsystem.h"
#include "UI/DamageNumberModel.h"

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
}

void APrototypeHUD::DrawHUD()
{
    Super::DrawHUD();
    DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.86f), 18, 18, 630, 84);
    // M1-042: stage copy refreshed for the M1 combat prototype (pending
    // playtest); the M0 foundation line is obsolete now that M1 combat is in.
    DrawText(TEXT("UE-MMO | M1: combat prototype (pending playtest)"), FLinearColor::White, 32, 28, nullptr, 1.2f);
    // M1-040: DNF-style keymap hints. Arrows move (X/Y incl. depth), X attack,
    // Z launcher, C jump (Space stays a jump alias), F2 resets, QWERASDF are
    // the reserved skill slots (no skill effect yet), F1 the debug overlay.
    DrawText(TEXT("Arrows: move  X: attack  Z: launcher  C: jump  F2: reset  QWERASDF: skills (reserved)  F1: debug"), FLinearColor(0.6f, 0.85f, 1.f), 32, 53);
    // M1-042: the stage note reflects the implemented M1 combat and the
    // planned M2 scope instead of the stale "not implemented in M0" text.
    DrawText(TEXT("Single-player. M1 combat implemented; arrows/X/Z/C controls; M2 enemies & rooms planned."), FLinearColor(0.9f, 0.8f, 0.45f), 32, 77);

    // M1-028: the debug overlay is a pure display layer gated by the F1 flag.
    // With it off nothing below runs: no references resolved, no text, no box,
    // and combat queries/damage are untouched by any of this code.
    if (bCombatDebugOverlayEnabled)
    {
        RefreshDebugReferences();
        DrawCombatDebugOverlay();

        // M1-035: the hit feedback (HP bars, damage numbers, combo counter)
        // lives on the same debug HUD behind the same flag, so the default
        // view and the input hints above stay untouched. The feed binds only
        // here (event-driven; no per-frame world scan).
        RefreshDamageFeedBinding();
        DrawHealthBars();
        DrawDamageNumbers();
        DrawComboCounter();
    }
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
    // whose OnHitConfirmed the debug HUD visualizes). Resolution goes through
    // the cached weak reference M1-028 maintains (no scan in the steady
    // state); a destroyed pawn reads null and simply unbinds the feed.
    UCombatComponent* Desired = nullptr;
    if (APrototypeCharacter* Player = DebugPlayer.Get())
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
    }
    RoomSessionPtr = nullptr;
    RoomRunEndedHandle.Reset();
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
    if (!EnsureResultWidget())
    {
        return;
    }
    if (URoomResultWidget* Widget = ResultWidgetPtr.Get())
    {
        Widget->BindResult(Result);
    }
    ShowRoomResultScreen();
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
    if (URoomSessionSubsystem* Session = RoomSessionPtr.Get())
    {
        Session->LeaveRoom();
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M2-012: return could not find the room session."));
    }
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
