// M6-021: the DNFM character picture component (plan section 3): frame
// selection plus the frozen section 5.1 placement on one camera-facing
// plane. Gameplay state stays untouched: the component reads the owner's
// capsule pose and the injected visual set, and reports frame events; it
// never moves the actor, changes combat state or spawns a second clock.
#include "DNFMCharacterVisualComponent.h"

#include "SideViewCameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Math/Axis.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace UE::UEMMO::Tasks::M6_021
{
    // The engine plane is 100x100 local units (its UV layout is a quad [0,1]^2
    // mapped linearly by the material - see the component header contract).
    constexpr float EnginePlaneLocalSize = 100.0f;

    /** Material parameter names (the M6-022 import builds against them). */
    const TCHAR* const MaterialParamPageTex = TEXT("PageTex");
    const TCHAR* const MaterialParamUVRectMin = TEXT("UVRectMin");
    const TCHAR* const MaterialParamUVRectSize = TEXT("UVRectSize");

    /**
     * Section 5.1 canvas-pixel mapping, exact:
     *   WorldPoint = P + R * Facing * (x - PivotX) * s + U * (PivotY - y) * s
     * CanvasPointPx and PivotCanvasPx are canvas coordinates (y down);
     * Facing is the actor-facing sign (+1 right, -1 left). The formula is the
     * ONLY consumer of Facing - the plane never rotates with the actor yaw
     * again (no double flip, no side-edge disappearance).
     */
    FVector ComputeCanvasPointWorld(const FVector& FootP, const FVector& RightAxis, const FVector& UpAxis,
        float FacingSign, const FIntPoint& CanvasPointPx, const FIntPoint& PivotCanvasPx, float CmPerPixel)
    {
        const float SafeFacing = FMath::IsFinite(FacingSign) ? FMath::Sign(FacingSign) : 1.0f;
        const float SafeScale = (FMath::IsFinite(CmPerPixel) && CmPerPixel > 0.0f) ? CmPerPixel : 0.0f;
        const float Dx = static_cast<float>(CanvasPointPx.X - PivotCanvasPx.X);
        const float Dy = static_cast<float>(CanvasPointPx.Y - PivotCanvasPx.Y);
        // (PivotY - y) is the formula's vertical term; with Dy = y - PivotY it
        // becomes -Dy (canvas y grows downward, world Up grows upward).
        return FootP
            + RightAxis * (SafeFacing * Dx * SafeScale)
            + UpAxis * (-Dy * SafeScale);
    }

    /**
     * The one-time section 5.1 calibration: s = target height cm / standing
     * head-top pixels (measured foot-to-head on the standing frame). Invalid
     * inputs keep s uncalibrated (0) rather than guessing.
     */
    float ComputeCmPerPixel(float StandingHeadTopPx, float TargetStandingHeightCm)
    {
        if (!FMath::IsFinite(StandingHeadTopPx) || !FMath::IsFinite(TargetStandingHeightCm)
            || StandingHeadTopPx <= 0.0f || TargetStandingHeightCm <= 0.0f)
        {
            return 0.0f;
        }
        return TargetStandingHeightCm / StandingHeadTopPx;
    }

    /**
     * The quad world transform for one display frame: the quad center canvas
     * point (rect center) mapped through ComputeCanvasPointWorld, with the
     * quad basis X = R * Facing (canvas x), Y = UpAxis (canvas up) and size
     * = rect pixels * s. The plane normal follows from the basis (X x Y); the
     * M6-022 material is two-sided, so both facings render.
     */
    FTransform ComputeFrameQuadTransform(const FVector& FootP, const FVector& RightAxis, const FVector& UpAxis,
        float FacingSign, const FDNFMVisualFrame& Frame, float CmPerPixel)
    {
        const FIntPoint PivotCanvas(Frame.UvRectPosPx + Frame.PivotPx);
        const FIntPoint CenterCanvas(Frame.UvRectPosPx + Frame.UvRectSizePx / 2);
        const FVector Center = ComputeCanvasPointWorld(FootP, RightAxis, UpAxis,
            FacingSign, CenterCanvas, PivotCanvas, CmPerPixel);
        const float SafeFacing = FMath::IsFinite(FacingSign) ? FMath::Sign(FacingSign) : 1.0f;
        const FVector XAxis = RightAxis * SafeFacing; // canvas x direction in world
        const FVector YAxis = UpAxis;                 // canvas up (image top) in world
        const FQuat Rotation = FRotationMatrix::MakeFromXY(XAxis, YAxis).ToQuat();
        const float Width = static_cast<float>(Frame.UvRectSizePx.X) * FMath::Max(CmPerPixel, 0.0f);
        const float Height = static_cast<float>(Frame.UvRectSizePx.Y) * FMath::Max(CmPerPixel, 0.0f);
        const FVector Scale(FMath::Max(Width, KINDA_SMALL_NUMBER) / EnginePlaneLocalSize,
            FMath::Max(Height, KINDA_SMALL_NUMBER) / EnginePlaneLocalSize, 1.0f);
        return FTransform(Rotation, Center, Scale);
    }

    /**
     * The page UV rect of one frame (the material parameter values): the page
     * pixel rect normalized by the page size. UE texture UVs put (0,0) at the
     * image top-left (the M6-015 flipbook's verified convention), so the
     * manifest's y-down rect maps directly.
     */
    void ComputePageUVRect(const FDNFMVisualFrame& Frame, const FIntPoint& PageSizePx,
        FVector2D& OutMin, FVector2D& OutSize)
    {
        if (PageSizePx.X <= 0 || PageSizePx.Y <= 0)
        {
            OutMin = FVector2D::ZeroVector;
            OutSize = FVector2D::UnitVector;
            return;
        }
        OutMin = FVector2D(static_cast<float>(Frame.UvRectPosPx.X) / static_cast<float>(PageSizePx.X),
            static_cast<float>(Frame.UvRectPosPx.Y) / static_cast<float>(PageSizePx.Y));
        OutSize = FVector2D(static_cast<float>(Frame.UvRectSizePx.X) / static_cast<float>(PageSizePx.X),
            static_cast<float>(Frame.UvRectSizePx.Y) / static_cast<float>(PageSizePx.Y));
    }
}

using namespace UE::UEMMO::Tasks::M6_021;

UDNFMCharacterVisualComponent::UDNFMCharacterVisualComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    // The pawn's gate decides when the picture runs (gate 0 keeps the whole
    // component inert); presenters/tests enable the tick or drive TickVisual.
    PrimaryComponentTick.bStartWithTickEnabled = false;
    // PostPhysics: the pawn's movement (PrePhysics) and the jump physics step
    // have settled, so the foot anchor reads this frame's real Z.
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
    // The engine plane (no hard asset dependency; a missing mesh only keeps
    // the plane invisible, the documented honest fallback).
    PlaneMeshPath = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Plane.Plane")));
}

void UDNFMCharacterVisualComponent::BeginPlay()
{
    Super::BeginPlay();
    if (DefaultVisualSet.ToSoftObjectPath().IsValid())
    {
        if (UDNFMCharacterVisualSet* Loaded = DefaultVisualSet.LoadSynchronous())
        {
            SetVisualSet(Loaded);
        }
    }
}

void UDNFMCharacterVisualComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // The clean teardown: no dangling playback state, no further events.
    ResetVisual();
    SetVisualSet(nullptr);
    Super::EndPlay(EndPlayReason);
}

void UDNFMCharacterVisualComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    TickVisual(DeltaTime);
}

void UDNFMCharacterVisualComponent::SetVisualSet(UDNFMCharacterVisualSet* NewSet)
{
    if (VisualSet == NewSet)
    {
        return;
    }
    ResetVisual();
    VisualSet = NewSet;
    LoadedPages.Reset();
    BoundPageIndex = INDEX_NONE;
}

bool UDNFMCharacterVisualComponent::SetAction(FName ActionId, EDNFMVisualLoopPolicy LoopPolicy)
{
    if (VisualSet == nullptr)
    {
        return false;
    }
    const int32 ActionIndex = VisualSet->GetActionIndex(ActionId);
    if (!VisualSet->Actions.IsValidIndex(ActionIndex))
    {
        return false;
    }
    const FDNFMActionClip& Clip = VisualSet->Actions[ActionIndex];
    CurrentActionIndex = ActionIndex;
    CurrentFrameIndex = 0;
    ElapsedMs = 0.0;
    bActive = true;
    bLoopCurrent = (LoopPolicy == EDNFMVisualLoopPolicy::ForceLoop)
        || (LoopPolicy == EDNFMVisualLoopPolicy::UseClipDefault && Clip.bLoop);
    BoundPageIndex = INDEX_NONE;
    ApplyCurrentFrame();
    ApplyPlacementNow();
    // Frame 0 is current from now on; its events fire once, here.
    FireFrameEvents(Clip, 0);
    return true;
}

void UDNFMCharacterVisualComponent::ResetVisual()
{
    bActive = false;
    CurrentActionIndex = INDEX_NONE;
    CurrentFrameIndex = INDEX_NONE;
    ElapsedMs = 0.0;
    bLoopCurrent = false;
    BoundPageIndex = INDEX_NONE;
    if (PlaneComponent != nullptr)
    {
        PlaneComponent->SetVisibility(false);
    }
}

void UDNFMCharacterVisualComponent::SetWorldScaleFromStandingHeight(float StandingHeadTopPx,
    float TargetStandingHeightCm)
{
    const float NewScale = ComputeCmPerPixel(StandingHeadTopPx, TargetStandingHeightCm);
    if (NewScale <= 0.0f)
    {
        return; // refuse: keep the previous calibration (or uncalibrated)
    }
    CmPerPixel = NewScale;
    bWarnedUncalibrated = false;
    if (IsVisualActive())
    {
        ApplyPlacementNow();
    }
}

void UDNFMCharacterVisualComponent::SetMaterialInterface(UMaterialInterface* Material)
{
    if (Material == nullptr)
    {
        VisualMaterial = nullptr;
        BoundPageIndex = INDEX_NONE;
        return;
    }
    VisualMaterial = UMaterialInstanceDynamic::Create(Material, GetOwner());
    BoundPageIndex = INDEX_NONE;
    EnsurePlaneComponent();
    if (PlaneComponent != nullptr)
    {
        PlaneComponent->SetMaterial(0, VisualMaterial);
    }
}

bool UDNFMCharacterVisualComponent::IsPlaneVisible() const
{
    return PlaneComponent != nullptr && PlaneComponent->IsVisible();
}

void UDNFMCharacterVisualComponent::TickVisual(float DeltaSeconds)
{
    if (!IsVisualActive())
    {
        return;
    }
    const FDNFMActionClip& Clip = VisualSet->Actions[CurrentActionIndex];
    const int32 Total = ComputeClipTotalDurationMs(Clip.Frames);
    if (Total <= 0)
    {
        return; // degenerate clip: the hold-last-frame state needs no time
    }
    // M6-023: a frozen playback advances no time, crosses no frames and fires
    // no events; the placement below still refreshes (the anchor follows the
    // real capsule through the freeze).
    if (!bPlaybackFrozen)
    {
        ElapsedMs += FMath::Max(DeltaSeconds, 0.0f) * 1000.0;
        const int32 NewIndex = SampleFrameIndexByElapsedMs(Clip.Frames, ElapsedMs, bLoopCurrent);
        if (NewIndex != CurrentFrameIndex)
        {
            // Low-framerate crossings walk every newly reached frame in time
            // order and report each one's events (the M6-023 dedup consumes the
            // honest interval; nothing here dedups on its own).
            const TArray<int32> Crossed = ComputeCrossedFrameIndices(Clip.Frames, CurrentFrameIndex, NewIndex, bLoopCurrent);
            for (const int32 CrossedIndex : Crossed)
            {
                FireFrameEvents(Clip, CrossedIndex);
            }
            CurrentFrameIndex = NewIndex;
            ApplyCurrentFrame();
        }
    }
    // Placement runs EVERY tick: the foot anchor follows the real capsule
    // (walking, jumping), so the plane pose is refreshed per frame even when
    // the display frame did not change (plan section 5.1).
    ApplyPlacementNow();
}

void UDNFMCharacterVisualComponent::SetPlaybackFrozen(bool bNewFrozen)
{
    bPlaybackFrozen = bNewFrozen;
}

bool UDNFMCharacterVisualComponent::SeekToDisplayFrame(int32 DisplayFrameIndex)
{
    if (!IsVisualActive())
    {
        return false;
    }
    const FDNFMActionClip& Clip = VisualSet->Actions[CurrentActionIndex];
    if (!Clip.Frames.IsValidIndex(DisplayFrameIndex))
    {
        return false;
    }
    if (DisplayFrameIndex == CurrentFrameIndex)
    {
        return true; // already there (idempotent)
    }
    // The honest crossing interval: every newly reached frame in time order
    // reports its events (a low-framerate logic jump may cross several). A
    // backward seek on a non-loop clip walks nothing (no fake re-fires); a
    // loop clip walks the wrap the long way (the M6-021 frozen walk rule).
    const TArray<int32> Crossed = ComputeCrossedFrameIndices(Clip.Frames, CurrentFrameIndex, DisplayFrameIndex, bLoopCurrent);
    for (const int32 CrossedIndex : Crossed)
    {
        FireFrameEvents(Clip, CrossedIndex);
    }
    // Pin the elapsed time to the target frame's START, so a later real-time
    // advance continues from the seeked pose (never from a stale position).
    double StartMs = 0.0;
    for (int32 Index = 0; Index < DisplayFrameIndex; ++Index)
    {
        StartMs += FMath::Max(Clip.Frames[Index].DurationMs, 0);
    }
    ElapsedMs = StartMs;
    CurrentFrameIndex = DisplayFrameIndex;
    ApplyCurrentFrame();
    ApplyPlacementNow();
    return true;
}

void UDNFMCharacterVisualComponent::ApplyPlacementNow()
{
    if (!IsVisualActive())
    {
        return;
    }
    if (CmPerPixel <= 0.0f)
    {
        if (!bWarnedUncalibrated)
        {
            bWarnedUncalibrated = true;
            UE_LOG(LogTemp, Warning,
                TEXT("UEMMO M6-021: DNFM visual has no cm/pixel calibration (SetWorldScaleFromStandingHeight); the plane stays hidden."));
        }
        return;
    }
    const FDNFMVisualFrame& Frame = VisualSet->Actions[CurrentActionIndex].Frames[CurrentFrameIndex];
    EnsurePlaneComponent();
    if (PlaneComponent == nullptr)
    {
        return;
    }
    const FVector Foot = ResolveFootAnchor();
    FVector Right;
    FVector Up;
    ResolveCameraBasis(Right, Up);
    const float Facing = ResolveFacing();
    // Teleport: the quad jumps between poses/pages without sweeping (the
    // plane has no collision anyway; this only skips needless sweep work).
    PlaneComponent->SetWorldTransform(ComputeFrameQuadTransform(Foot, Right, Up, Facing, Frame, CmPerPixel),
        /*bSweep*/ false, /*OutSweepHitResult*/ nullptr, ETeleportType::TeleportPhysics);
    PlaneComponent->SetVisibility(true);
}

void UDNFMCharacterVisualComponent::ApplyCurrentFrame()
{
    if (!IsVisualActive())
    {
        return;
    }
    const FDNFMVisualFrame& Frame = VisualSet->Actions[CurrentActionIndex].Frames[CurrentFrameIndex];
    // M8-018: a no-draw logic frame keeps the previously drawn pose on
    // screen for its duration (charge holds etc.) - skip the material and
    // quad updates entirely, events still fire through the seek walk.
    if (!Frame.bDrawFrame)
    {
        return;
    }
    // Page + UV material parameters (no-op without an imported material).
    if (VisualMaterial != nullptr)
    {
        UTexture2D* PageTexture = nullptr;
        if (LoadedPages.IsValidIndex(Frame.PageIndex))
        {
            PageTexture = LoadedPages[Frame.PageIndex];
        }
        else if (VisualSet != nullptr)
        {
            PageTexture = VisualSet->LoadPageTexture(Frame.PageIndex);
            if (PageTexture != nullptr)
            {
                LoadedPages.SetNumZeroed(Frame.PageIndex + 1);
                LoadedPages[Frame.PageIndex] = PageTexture;
            }
        }
        if (PageTexture != nullptr)
        {
            if (BoundPageIndex != Frame.PageIndex)
            {
                // The page switch rides the texture parameter (the plan's
                // section 5.2 material entry; no material instance swap per
                // frame).
                VisualMaterial->SetTextureParameterValue(MaterialParamPageTex, PageTexture);
                BoundPageIndex = Frame.PageIndex;
            }
            FIntPoint PageSize(0, 0);
            if (VisualSet->PageSizesPx.IsValidIndex(Frame.PageIndex))
            {
                PageSize = VisualSet->PageSizesPx[Frame.PageIndex];
            }
            else
            {
                PageSize = FIntPoint(PageTexture->GetSizeX(), PageTexture->GetSizeY());
            }
            FVector2D UvMin;
            FVector2D UvSize;
            ComputePageUVRect(Frame, PageSize, UvMin, UvSize);
            VisualMaterial->SetVectorParameterValue(MaterialParamUVRectMin,
                FLinearColor(UvMin.X, UvMin.Y, 0.0f, 0.0f));
            VisualMaterial->SetVectorParameterValue(MaterialParamUVRectSize,
                FLinearColor(UvSize.X, UvSize.Y, 0.0f, 0.0f));
        }
        else if (!bWarnedMissingPage)
        {
            bWarnedMissingPage = true;
            UE_LOG(LogTemp, Warning,
                TEXT("UEMMO M6-021: DNFM visual page %d has no texture (import pending); the material keeps its previous page."),
                Frame.PageIndex);
        }
    }
    // Placement is the caller's duty (it runs every tick, not only on frame
    // changes, because the foot anchor follows the real capsule).
}

FVector UDNFMCharacterVisualComponent::ResolveFootAnchor() const
{
    if (FootAnchorProvider)
    {
        return FootAnchorProvider();
    }
    const AActor* Owner = GetOwner();
    if (Owner == nullptr)
    {
        return FVector::ZeroVector;
    }
    // Section 5.1: P = capsule center - current scaled half height, so a
    // jump moves the picture with the real Z (the anchor is the true capsule
    // bottom, not a stored ground Z).
    const UCapsuleComponent* Capsule = Owner->FindComponentByClass<UCapsuleComponent>();
    if (Capsule != nullptr)
    {
        return Capsule->GetComponentLocation()
            - FVector(0.0f, 0.0f, Capsule->GetScaledCapsuleHalfHeight());
    }
    return Owner->GetActorLocation();
}

void UDNFMCharacterVisualComponent::ResolveCameraBasis(FVector& OutRight, FVector& OutUp) const
{
    if (CameraBasisProvider)
    {
        CameraBasisProvider(OutRight, OutUp);
        if (OutRight.IsNormalized() && OutUp.IsNormalized())
        {
            return;
        }
    }
    const AActor* Owner = GetOwner();
    const UWorld* World = Owner ? Owner->GetWorld() : nullptr;
    // 1) The owner's fixed side-view rig (its world rotation is set once and
    //    never orbits; the section 5.1 source of R/U).
    const USideViewCameraComponent* Rig = Owner
        ? Owner->FindComponentByClass<USideViewCameraComponent>()
        : nullptr;
    if (Rig != nullptr)
    {
        const FTransform Transform = Rig->GetComponentTransform();
        OutRight = Transform.GetUnitAxis(EAxis::Y); // camera right
        OutUp = Transform.GetUnitAxis(EAxis::Z);    // camera up
        if (OutRight.IsNormalized() && OutUp.IsNormalized())
        {
            return;
        }
    }
    // 2) The possessing controller's actual camera (any custom rig still
    //    gives the real picture plane).
    const APlayerController* Controller = World
        ? World->GetFirstPlayerController()
        : nullptr;
    if (Controller != nullptr && Controller->PlayerCameraManager != nullptr)
    {
        const FRotator Rotation = Controller->PlayerCameraManager->GetCameraRotation();
        const FRotationMatrix Matrix(Rotation);
        OutRight = Matrix.GetUnitAxis(EAxis::Y);
        OutUp = Matrix.GetUnitAxis(EAxis::Z);
        if (OutRight.IsNormalized() && OutUp.IsNormalized())
        {
            return;
        }
    }
    // 3) The documented fixed rig values (no camera anywhere: headless tests).
    const FRotationMatrix Fixed(FRotator(UE::UEMMO::Tasks::M1_030::SideViewCameraPitch,
        UE::UEMMO::Tasks::M1_030::SideViewCameraYaw, 0.0f));
    OutRight = Fixed.GetUnitAxis(EAxis::Y);
    OutUp = Fixed.GetUnitAxis(EAxis::Z);
}

float UDNFMCharacterVisualComponent::ResolveFacing() const
{
    if (FacingProvider)
    {
        return FacingProvider();
    }
    const AActor* Owner = GetOwner();
    if (Owner == nullptr)
    {
        return 1.0f;
    }
    // The M1-029 facing convention: yaw 0 = right (+1), yaw 180 = left (-1).
    const float Yaw = Owner->GetActorRotation().Yaw;
    return (Yaw > -90.0f && Yaw < 90.0f) ? 1.0f : -1.0f;
}

void UDNFMCharacterVisualComponent::FireFrameEvents(const FDNFMActionClip& Clip, int32 FrameIndex)
{
    if (!Clip.Frames.IsValidIndex(FrameIndex))
    {
        return;
    }
    const FDNFMVisualFrame& Frame = Clip.Frames[FrameIndex];
    if (Frame.Events.Num() == 0)
    {
        return;
    }
    const AActor* Owner = GetOwner();
    const uint64 ActorId = Owner ? static_cast<uint64>(Owner->GetUniqueID()) : 0;
    for (const FDNFMVisualFrameEvent& Event : Frame.Events)
    {
        // As-is reporting only: (ActorId, ActionName, FrameIndex, EventTag,
        // EventName). The M6-023 contract owns dedup and dispatch.
        OnVisualFrameEvent.Broadcast(ActorId, Clip.ActionId, FrameIndex, Event.Type, Event.Name);
    }
}

void UDNFMCharacterVisualComponent::EnsurePlaneComponent()
{
    if (PlaneComponent != nullptr)
    {
        return;
    }
    AActor* Owner = GetOwner();
    if (Owner == nullptr)
    {
        return;
    }
    PlaneComponent = NewObject<UStaticMeshComponent>(Owner, TEXT("DNFMCharacterVisualPlane"));
    // One plane per character (plan section 5.2); no gameplay body - the
    // capsule owns collision.
    PlaneComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    PlaneComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
    PlaneComponent->SetCastShadow(false);
    PlaneComponent->SetGenerateOverlapEvents(false);
    // The plane is placed in WORLD coordinates every frame: absolute flags
    // detach it from the actor's transform so the 0/180 yaw never rotates it
    // again (the section 5.1 no-double-flip rule).
    PlaneComponent->SetUsingAbsoluteLocation(true);
    PlaneComponent->SetUsingAbsoluteRotation(true);
    PlaneComponent->SetUsingAbsoluteScale(true);
    PlaneComponent->SetVisibility(false);
    PlaneComponent->SetupAttachment(Owner->GetRootComponent());
    if (UStaticMesh* PlaneMesh = PlaneMeshPath.LoadSynchronous())
    {
        PlaneComponent->SetStaticMesh(PlaneMesh);
    }
    if (VisualMaterial != nullptr)
    {
        PlaneComponent->SetMaterial(0, VisualMaterial);
    }
    if (Owner->GetWorld() != nullptr)
    {
        // Register when a world exists (the M5-041 precedent); a world-less
        // owner keeps the component for pure-logic use.
        PlaneComponent->RegisterComponent();
    }
}
