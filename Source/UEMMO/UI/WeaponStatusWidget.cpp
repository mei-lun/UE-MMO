#include "WeaponStatusWidget.h"

#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateColor.h"

#include "../Weapons/AmmoModel.h"
#include "../Weapons/WeaponBinding.h"
#include "../Weapons/WeaponTypes.h"

namespace
{
    // M5-034: display constants of the weapon status panel (the M3-011
    // inventory palette is reused for coherence; plain display values).
    const TCHAR* M5_034_TitleText = TEXT("Weapon Status");
    const TCHAR* M5_034_DashText = TEXT("-");

    // Type text of one fire mode (the closed three-value vocabulary).
    const TCHAR* M5_034_MeleeType = TEXT("Melee");
    const TCHAR* M5_034_HitscanType = TEXT("Hitscan");
    const TCHAR* M5_034_ProjectileType = TEXT("Projectile");

    // Status texts (every state names its outcome; the caller-visible rules
    // live in MakeWeaponStatusViewModel's documented precedence).
    const TCHAR* M5_034_NoWeaponStatus = TEXT("No weapon equipped");
    const TCHAR* M5_034_ReadyStatus = TEXT("Ready");
    const TCHAR* M5_034_MeleeReadyStatus = TEXT("Melee ready");
    const TCHAR* M5_034_EmptyMagazineStatus = TEXT("Magazine empty - press T to reload");
    const TCHAR* M5_034_ReserveEmptyStatus = TEXT("Reserve empty - reload refused");
    const TCHAR* M5_034_CatalogErrorStatus = TEXT("Weapon catalog unavailable - firing disabled");
    const TCHAR* M5_034_MappingMissingStatus = TEXT("Equipped weapon item has no weapon mapping");
    const TCHAR* M5_034_UnresolvedStatus = TEXT("Bound weapon definition is unresolved");
}

FString MakeWeaponStatusStateText(EWeaponStatusState State)
{
    switch (State)
    {
    case EWeaponStatusState::NoWeapon: return M5_034_NoWeaponStatus;
    case EWeaponStatusState::Ready: return M5_034_ReadyStatus;
    case EWeaponStatusState::EmptyMagazine: return M5_034_EmptyMagazineStatus;
    case EWeaponStatusState::Reloading: return TEXT("Reloading");
    case EWeaponStatusState::ReserveEmpty: return M5_034_ReserveEmptyStatus;
    case EWeaponStatusState::ConfigFailure: return TEXT("Configuration failure");
    default: return TEXT("Unknown weapon status");
    }
}

namespace
{
    // Type text of one fire mode; the closed enum keeps the default arm
    // unreachable for validated definitions.
    const TCHAR* M5_034_FireModeText(EWeaponFireMode Mode)
    {
        switch (Mode)
        {
        case EWeaponFireMode::Melee: return M5_034_MeleeType;
        case EWeaponFireMode::Hitscan: return M5_034_HitscanType;
        case EWeaponFireMode::Projectile: return M5_034_ProjectileType;
        default: return M5_034_DashText;
        }
    }
}

FWeaponStatusViewModel MakeWeaponStatusViewModel(const FWeaponStatusInputs& Inputs)
{
    // Every input shape resolves to an explicit display state - the pure
    // function never invents a weapon, rounds or an ability.
    FWeaponStatusViewModel ViewModel;
    ViewModel.bValid = true;

    // State precedence (the documented order; the first matching rule wins):
    // catalog error > unbound weapon item > no binding > unresolved
    // definition > reloading > reserve empty > empty magazine > ready.
    if (Inputs.bCatalogError)
    {
        ViewModel.State = EWeaponStatusState::ConfigFailure;
        ViewModel.StatusLine = M5_034_CatalogErrorStatus;
        ViewModel.WeaponLine = M5_034_DashText;
        ViewModel.AmmoLine = M5_034_DashText;
        ViewModel.AbilitiesLine = M5_034_DashText;
        return ViewModel;
    }
    if (Inputs.Binding == nullptr)
    {
        if (Inputs.bEquippedItemUnbound)
        {
            // The illegal-configuration shape: a weapon-slot occupant exists
            // in the equipment model but the real bind refused (the shipped
            // source carries no weapon mappings yet - never papered over).
            ViewModel.State = EWeaponStatusState::ConfigFailure;
            ViewModel.StatusLine = M5_034_MappingMissingStatus;
            ViewModel.WeaponLine = M5_034_DashText;
            ViewModel.AmmoLine = M5_034_DashText;
            ViewModel.AbilitiesLine = M5_034_DashText;
            return ViewModel;
        }
        ViewModel.State = EWeaponStatusState::NoWeapon;
        ViewModel.StatusLine = M5_034_NoWeaponStatus;
        ViewModel.WeaponLine = M5_034_DashText;
        ViewModel.AmmoLine = M5_034_DashText;
        ViewModel.AbilitiesLine = M5_034_DashText;
        return ViewModel;
    }

    // A binding exists: resolve the display identity through the definition.
    const bool bHasDefinition = Inputs.Definition != nullptr;
    if (bHasDefinition)
    {
        ViewModel.WeaponLine = FString::Printf(TEXT("%s | %s"),
            *Inputs.Definition->WeaponId.ToString(), M5_034_FireModeText(Inputs.Definition->FireMode));
    }
    else
    {
        ViewModel.State = EWeaponStatusState::ConfigFailure;
        ViewModel.StatusLine = M5_034_UnresolvedStatus;
        ViewModel.WeaponLine = FString::Printf(TEXT("%s | %s"),
            *Inputs.Binding->WeaponDefinitionId.ToString(), M5_034_DashText);
        ViewModel.AmmoLine = M5_034_DashText;
        ViewModel.AbilitiesLine = M5_034_DashText;
        return ViewModel;
    }

    const bool bMelee = Inputs.Definition->FireMode == EWeaponFireMode::Melee;
    if (bMelee)
    {
        // Melee: no magazine, no reserve, no ranged ability row - the legacy
        // attack chain stays the ability surface.
        ViewModel.AmmoLine = M5_034_DashText;
        ViewModel.AbilitiesLine = TEXT("Melee chain: 4 attacks");
        ViewModel.State = EWeaponStatusState::Ready;
        ViewModel.StatusLine = M5_034_MeleeReadyStatus;
        return ViewModel;
    }

    // Ranged: the REAL rounds readout. The loaded count is the M5-019 fire
    // book on the binding record (the rounds the shots consume); the shared
    // pool owns the reserve; a missing pool reads as the explicit "-".
    const int32 Loaded = Inputs.Binding->LoadedRounds;
    const int32 Capacity = Inputs.Binding->MagazineCapacity;
    FString ReserveText = M5_034_DashText;
    if (Inputs.ReserveRounds >= 0)
    {
        ReserveText = FString::Printf(TEXT("%d"), Inputs.ReserveRounds);
    }
    ViewModel.AmmoLine = FString::Printf(TEXT("Loaded %d/%d | Reserve %s"), Loaded, Capacity, *ReserveText);

    // Abilities: only the meaningful fields (a 1-burst/1-pellet/0-spread row
    // is noise).
    ViewModel.AbilitiesLine = FString::Printf(TEXT("RPM %.0f"), static_cast<double>(Inputs.Definition->FireRateRpm));
    if (Inputs.Definition->BurstCount > 1)
    {
        ViewModel.AbilitiesLine += FString::Printf(TEXT(" | Burst x%d"), Inputs.Definition->BurstCount);
    }
    if (Inputs.Definition->PelletCount > 1)
    {
        ViewModel.AbilitiesLine += FString::Printf(TEXT(" | Pellets x%d"), Inputs.Definition->PelletCount);
    }
    if (Inputs.Definition->SpreadDegrees > 0.0f)
    {
        ViewModel.AbilitiesLine += FString::Printf(TEXT(" | Spread %.1f deg"), static_cast<double>(Inputs.Definition->SpreadDegrees));
    }

    // Status: reloading > reserve empty > empty magazine > ready. The reload
    // remainder is a query on the caller's clock (never a scheduler here).
    if (Inputs.RemainingReloadSeconds > 0.0)
    {
        ViewModel.State = EWeaponStatusState::Reloading;
        ViewModel.StatusLine = FString::Printf(TEXT("Reloading - %.1fs left"), Inputs.RemainingReloadSeconds);
        return ViewModel;
    }
    if (Loaded <= 0)
    {
        if (Inputs.ReserveRounds <= 0)
        {
            ViewModel.State = EWeaponStatusState::ReserveEmpty;
            ViewModel.StatusLine = M5_034_ReserveEmptyStatus;
            return ViewModel;
        }
        ViewModel.State = EWeaponStatusState::EmptyMagazine;
        ViewModel.StatusLine = M5_034_EmptyMagazineStatus;
        return ViewModel;
    }
    ViewModel.State = EWeaponStatusState::Ready;
    ViewModel.StatusLine = M5_034_ReadyStatus;
    return ViewModel;
}

FString MakeWeaponStatusSnapshotFingerprint(const FWeaponStatusInputs& Inputs)
{
    // Deterministic text of everything the render depends on. The reload
    // remainder quantizes to 0.1 s (the countdown text's own resolution), so
    // a sub-tick clock jitter cannot force a rebuild every drawn frame.
    const int32 bCatalogError = Inputs.bCatalogError ? 1 : 0;
    const int32 bUnbound = Inputs.bEquippedItemUnbound ? 1 : 0;
    if (Inputs.Binding == nullptr)
    {
        return FString::Printf(TEXT("n=0;c=%d;u=%d"), bCatalogError, bUnbound);
    }
    const int32 bHasDefinition = (Inputs.Definition != nullptr) ? 1 : 0;
    if (!bHasDefinition)
    {
        return FString::Printf(TEXT("u=1;c=%d;w=%s"), bCatalogError, *Inputs.Binding->WeaponDefinitionId.ToString());
    }
    const double ReloadTenth = FMath::RoundToFloat(Inputs.RemainingReloadSeconds * 10.0) / 10.0;
    return FString::Printf(TEXT("b=1;c=%d;w=%s;m=%d;f=%d;l=%d;cap=%d;r=%d;t=%.1f;rpm=%s;bu=%d;pe=%d;sp=%s"),
        bCatalogError,
        *Inputs.Binding->WeaponDefinitionId.ToString(),
        Inputs.Definition->FireMode == EWeaponFireMode::Melee ? 1 : 0,
        static_cast<int32>(Inputs.Definition->FireMode),
        Inputs.Binding->LoadedRounds,
        Inputs.Binding->MagazineCapacity,
        Inputs.ReserveRounds,
        ReloadTenth,
        *FString::SanitizeFloat(Inputs.Definition->FireRateRpm),
        Inputs.Definition->BurstCount,
        Inputs.Definition->PelletCount,
        *FString::SanitizeFloat(Inputs.Definition->SpreadDegrees));
}

bool FWeaponStatusRefreshGuard::AcceptSnapshot(const FString& SnapshotFingerprint)
{
    if (bHasLastSnapshot && LastFingerprint.Equals(SnapshotFingerprint))
    {
        // Identical snapshot: the render would not change, so the rebuild is
        // skipped (the "no per-frame full rebuild" rule).
        ++SkippedCount;
        return false;
    }
    LastFingerprint = SnapshotFingerprint;
    bHasLastSnapshot = true;
    ++PerformedCount;
    return true;
}

// ----- the native weapon status panel -----------------------------------------

UWeaponStatusWidget::UWeaponStatusWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Display-only section: not focusable (the default), no key handling -
    // the inventory screen owns Esc and the close path.
}

void UWeaponStatusWidget::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    BuildControls();
}

void UWeaponStatusWidget::BindWeaponStatus(const FWeaponStatusInputs& Inputs)
{
    // A bind is an unconditional (re)fill; it also records the refresh
    // baseline, so a following identical RefreshIfChanged is skipped.
    RefreshGuard.AcceptSnapshot(MakeWeaponStatusSnapshotFingerprint(Inputs));
    ViewModel = MakeWeaponStatusViewModel(Inputs);
    BuildControls(); // safety net for a fill before Initialize built the tree
    ApplyViewModelToControls();
}

bool UWeaponStatusWidget::RefreshIfChanged(const FWeaponStatusInputs& Inputs)
{
    // Throttled refresh: the fingerprint gate decides; an identical snapshot
    // leaves the controls completely untouched.
    if (!RefreshGuard.AcceptSnapshot(MakeWeaponStatusSnapshotFingerprint(Inputs)))
    {
        return false;
    }
    ViewModel = MakeWeaponStatusViewModel(Inputs);
    BuildControls();
    ApplyViewModelToControls();
    return true;
}

void UWeaponStatusWidget::BuildControls()
{
    if (bControlsBuilt || WidgetTree == nullptr)
    {
        return;
    }

    // Bordered sub-panel: canvas root -> dark border -> vertical stack of the
    // title and the four display rows. Everything is a code-built native
    // control: no UMG asset is involved (the M3-011 precedent).
    RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
    PanelBorder = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
    PanelBorder->SetBrushColor(FLinearColor(0.02f, 0.04f, 0.06f, 0.85f));
    PanelBorder->SetPadding(FMargin(12.0f, 8.0f));
    UCanvasPanelSlot* BorderSlot = RootCanvas->AddChildToCanvas(PanelBorder);
    if (BorderSlot != nullptr)
    {
        BorderSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
        BorderSlot->SetAutoSize(false);
    }

    UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
    PanelBorder->SetContent(Stack);

    TitleBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    TitleBlock->SetText(FText::FromString(M5_034_TitleText));
    TitleBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 14));
    TitleBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.85f, 0.3f, 1.0f)));

    WeaponBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    WeaponBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12));
    WeaponBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.95f, 1.0f, 1.0f)));

    AmmoBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    AmmoBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 12));
    AmmoBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.75f, 0.9f, 1.0f, 1.0f)));

    AbilitiesBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    AbilitiesBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 11));
    AbilitiesBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.7f, 0.8f, 1.0f)));

    StatusBlock = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
    StatusBlock->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 12));

    Stack->AddChildToVerticalBox(TitleBlock);
    if (UVerticalBoxSlot* WeaponSlot = Stack->AddChildToVerticalBox(WeaponBlock))
    {
        WeaponSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));
    }
    if (UVerticalBoxSlot* AmmoSlot = Stack->AddChildToVerticalBox(AmmoBlock))
    {
        AmmoSlot->SetPadding(FMargin(0.0f, 2.0f));
    }
    if (UVerticalBoxSlot* AbilitySlot = Stack->AddChildToVerticalBox(AbilitiesBlock))
    {
        AbilitySlot->SetPadding(FMargin(0.0f, 2.0f));
    }
    if (UVerticalBoxSlot* StatusSlot = Stack->AddChildToVerticalBox(StatusBlock))
    {
        StatusSlot->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 0.0f));
    }

    WidgetTree->RootWidget = RootCanvas;
    bControlsBuilt = true;
    ApplyViewModelToControls();
}

void UWeaponStatusWidget::ApplyViewModelToControls()
{
    if (!bControlsBuilt)
    {
        return;
    }

    WeaponBlock->SetText(FText::FromString(ViewModel.WeaponLine));
    AmmoBlock->SetText(FText::FromString(ViewModel.AmmoLine));
    AbilitiesBlock->SetText(FText::FromString(ViewModel.AbilitiesLine));
    StatusBlock->SetText(FText::FromString(ViewModel.StatusLine));

    // The status color carries the state (failure red, reloading amber,
    // ready/empty greens and greys) so the outcome reads at a glance.
    switch (ViewModel.State)
    {
    case EWeaponStatusState::ConfigFailure:
        StatusBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.4f, 0.35f, 1.0f)));
        break;
    case EWeaponStatusState::Reloading:
        StatusBlock->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.8f, 0.3f, 1.0f)));
        break;
    case EWeaponStatusState::Ready:
        StatusBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.95f, 0.6f, 1.0f)));
        break;
    default:
        StatusBlock->SetColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.65f, 0.7f, 1.0f)));
        break;
    }
}
