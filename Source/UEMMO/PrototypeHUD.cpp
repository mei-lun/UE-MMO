#include "PrototypeHUD.h"
#include "Engine/Canvas.h"

#include "Combat/AttackDefinition.h"
#include "Combat/CombatComponent.h"
#include "Combat/CombatGeometry.h"
#include "Combat/HealthComponent.h"
#include "Enemy/TrainingEnemy.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "PrototypeCharacter.h"

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
}

void APrototypeHUD::DrawHUD()
{
    Super::DrawHUD();
    DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.86f), 18, 18, 630, 84);
    DrawText(TEXT("UE-MMO | M0: movement and asset foundation"), FLinearColor::White, 32, 28, nullptr, 1.2f);
    DrawText(TEXT("A/D: left/right    W/S: depth    Space: jump    R: reset"), FLinearColor(0.6f, 0.85f, 1.f), 32, 53);
    DrawText(TEXT("Single-player. Combat is specified, not implemented in M0."), FLinearColor(0.9f, 0.8f, 0.45f), 32, 77);

    // M1-028: the debug overlay is a pure display layer gated by the F1 flag.
    // With it off nothing below runs: no references resolved, no text, no box,
    // and combat queries/damage are untouched by any of this code.
    if (bCombatDebugOverlayEnabled)
    {
        RefreshDebugReferences();
        DrawCombatDebugOverlay();
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
    if (Mode != 2)
    {
        return;
    }
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
