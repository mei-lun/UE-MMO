#pragma once

#include "Components/ActorComponent.h"
#include "DNFMCharacterVisualSet.h"
#include "Delegates/Delegate.h"
#include "DNFMCharacterVisualComponent.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UTexture2D;

/**
 * M6-021: the loop policy override of SetAction. UseClipDefault follows the
 * clip's own manifest loop flag; ForceLoop/ForceOnce override it (the plan
 * section 6.2 mapping may need a once-clip to loop or vice versa until the
 * M6-023 presenter owns the mapping table).
 */
UENUM(BlueprintType)
enum class EDNFMVisualLoopPolicy : uint8
{
    UseClipDefault = 0,
    ForceLoop = 1,
    ForceOnce = 2
};

/**
 * M6-021: one frame event reported exactly when its frame BECOMES current
 * during playback (plan section 6.2: low-framerate crossings walk every
 * crossed frame in time order, so the M6-023 consumer sees the honest event
 * interval; dedup is the M6-023 contract, this component only reports).
 */
DECLARE_MULTICAST_DELEGATE_FiveParams(FDNFMVisualFrameEventBroadcast,
    uint64 /*ActorId*/, FName /*ActionName*/, int32 /*FrameIndex*/, FName /*EventTag*/, FName /*EventName*/);

/**
 * M6-021: the DNFM character picture component (plan section 3): it owns ONE
 * procedurally created camera-facing plane and drives frame selection plus
 * the section 5.1 placement. It is a pure display surface: its inputs are
 * the injected UDNFMCharacterVisualSet, an action id and the owner's current
 * capsule pose - it builds no second combat clock and never mutates gameplay
 * state (M6-023 wires the CombatPresentation facts into SetAction).
 *
 * Placement contract (plan section 5.1, exact):
 *   Foot anchor P = capsule center - current scaled half height (a jump moves
 *   P with the real Z). The plane's world orientation comes from the fixed
 *   side-view camera's Right/Up vectors (never from the actor yaw). The
 *   actor's 0/180 yaw only feeds Facing into the frozen formula
 *     WorldPoint = P + R * Facing * (x - PivotX) * s + U * (PivotY - y) * s
 *   implemented by the quad basis X = R * Facing, Y = U - the plane never
 *   inherits the actor yaw again (no double flip). The cm-per-pixel s is
 *   calibrated ONCE via SetWorldScaleFromStandingHeight and shared by every
 *   action; per-frame auto-scaling is deliberately absent (an attack would
 *   not grow, a crouch would not grow).
 *
 * Material contract (the M6-022 import builds the asset): Unlit with a
 * parameterized transparency mode (Masked first choice), Nearest/Clamp/mip
 * settings belong to the imported texture. The component writes the
 * parameters "PageTex" (the frame's page texture), "UVRectMin" (rect
 * top-left in page UV) and "UVRectSize" (rect size in page UV) on a dynamic
 * material instance; page switching is the texture parameter swap. The
 * material maps quad UV -> pageUV = UVRectMin + quadUV * UVRectSize, the
 * same linear structure the M6-015 flipbook used (whose upright rendering
 * was screenshot-verified), so orientation carries over unchanged.
 */
UCLASS(ClassGroup = (Character), meta = (BlueprintSpawnableComponent))
class UEMMO_API UDNFMCharacterVisualComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UDNFMCharacterVisualComponent();

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    /** Native frame-event feed (C++ consumers and the tests; M6-023 binds). */
    FDNFMVisualFrameEventBroadcast OnVisualFrameEvent;

    /**
     * Replaces the visual set and resets playback (the safe order when a
     * presenter switches sets). A null set is a ResetVisual.
     */
    void SetVisualSet(UDNFMCharacterVisualSet* NewSet);

    /**
     * Starts ActionId from its first frame. Returns false (unchanged state)
     * when the action id is unknown. LoopPolicy overrides the clip's manifest
     * loop flag. Frame-0 events fire immediately (the frame is current from
     * now on).
     */
    bool SetAction(FName ActionId, EDNFMVisualLoopPolicy LoopPolicy = EDNFMVisualLoopPolicy::UseClipDefault);

    /**
     * Stops playback and hides the plane: no current action, elapsed time 0,
     * page binding cleared, no further events. The visual set stays bound
     * (a presenter re-enters with SetAction); SetVisualSet(null) is the
     * full detach.
     */
    void ResetVisual();

    /**
     * Section 5.1 calibration, called once per visual set: s (cm per canvas
     * pixel) = TargetStandingHeightCm / StandingHeadTopPx, shared by every
     * action. Non-positive inputs refuse (s stays 0 = uncalibrated = the
     * plane stays hidden; an uncalibrated placement would be wrong, not
     * approximate).
     */
    void SetWorldScaleFromStandingHeight(float StandingHeadTopPx, float TargetStandingHeightCm);

    /** The plane's material source (null = logic-only runs, tests). */
    void SetMaterialInterface(UMaterialInterface* Material);

    /** Injection seams for the automation tests (production uses the owner). */
    void SetFacingProvider(TFunction<float()> Provider) { FacingProvider = MoveTemp(Provider); }
    void SetCameraBasisProvider(TFunction<void(FVector&, FVector&)> Provider) { CameraBasisProvider = MoveTemp(Provider); }
    void SetFootAnchorProvider(TFunction<FVector()> Provider) { FootAnchorProvider = MoveTemp(Provider); }

    /**
     * One playback step without the component tick (the M6-023 presenter can
     * disable the component tick and drive this from its own order).
     */
    void TickVisual(float DeltaSeconds);

    /** Recomputes the plane world transform for the current frame now. */
    void ApplyPlacementNow();

    /** True while a clip is loaded and playing (or holding its end frame). */
    bool IsVisualActive() const
    {
        return bActive && VisualSet != nullptr && VisualSet->Actions.IsValidIndex(CurrentActionIndex);
    }

    /** The set bound by SetVisualSet (null until then). */
    UDNFMCharacterVisualSet* GetVisualSet() const { return VisualSet; }

    /** The currently playing action id (None when idle). */
    FName GetCurrentActionId() const
    {
        return IsVisualActive() ? VisualSet->Actions[CurrentActionIndex].ActionId : FName(NAME_None);
    }

    /** The current display frame (INDEX_NONE when idle). */
    int32 GetCurrentFrameIndex() const { return CurrentFrameIndex; }

    /** The calibrated cm-per-pixel (0 = uncalibrated). */
    float GetCmPerPixel() const { return CmPerPixel; }

    /** The plane component (created lazily; null before the first activation). */
    UStaticMeshComponent* GetPlaneComponent() const { return PlaneComponent; }

    /** Whether the plane is currently visible (the calibrated playback state). */
    bool IsPlaneVisible() const;

    /**
     * Settable default visual set (BeginPlay loads it once). The production
     * asset arrives with M6-022; tests inject sets programmatically.
     */
    UPROPERTY(EditAnywhere, Category = "DNFM Visual")
    TSoftObjectPtr<UDNFMCharacterVisualSet> DefaultVisualSet;

private:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    void EnsurePlaneComponent();
    void ApplyCurrentFrame();
    FVector ResolveFootAnchor() const;
    void ResolveCameraBasis(FVector& OutRight, FVector& OutUp) const;
    float ResolveFacing() const;
    void FireFrameEvents(const FDNFMActionClip& Clip, int32 FrameIndex);

    /** The bound set (UPROPERTY: it lives as long as this component does). */
    UPROPERTY(Transient)
    TObjectPtr<UDNFMCharacterVisualSet> VisualSet;

    /** Loaded page texture cache (the LoadSynchronous results per page). */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UTexture2D>> LoadedPages;

    /** The camera-facing plane (created on the owner; absolute transform). */
    UPROPERTY(Transient)
    TObjectPtr<UStaticMeshComponent> PlaneComponent;

    /** The plane's dynamic material instance (from SetMaterialInterface). */
    UPROPERTY(Transient)
    TObjectPtr<UMaterialInstanceDynamic> VisualMaterial;

    /** The engine plane mesh soft path (no hard asset dependency in code). */
    UPROPERTY(Transient)
    TSoftObjectPtr<UStaticMesh> PlaneMeshPath;

    /** Current playback row index into VisualSet->Actions (INDEX_NONE = idle). */
    int32 CurrentActionIndex = INDEX_NONE;
    int32 CurrentFrameIndex = INDEX_NONE;
    double ElapsedMs = 0.0;
    bool bActive = false;
    bool bLoopCurrent = false;

    /** The section 5.1 calibration (0 = uncalibrated, plane stays hidden). */
    float CmPerPixel = 0.0f;

    /** The page currently bound to the material (INDEX_NONE = none). */
    int32 BoundPageIndex = INDEX_NONE;

    /** The warned-once flags keeping the log readable. */
    bool bWarnedUncalibrated = false;
    bool bWarnedMissingPage = false;

    /** Injection seams (empty = owner-derived defaults). */
    TFunction<float()> FacingProvider;
    TFunction<void(FVector&, FVector&)> CameraBasisProvider;
    TFunction<FVector()> FootAnchorProvider;
};

namespace UE::UEMMO::Tasks::M6_021
{
    // Material parameter names the M6-022 import builds against (the component
    // writes them; the material reads them).
    UEMMO_API extern const TCHAR* const MaterialParamPageTex;
    UEMMO_API extern const TCHAR* const MaterialParamUVRectMin;
    UEMMO_API extern const TCHAR* const MaterialParamUVRectSize;

    /**
     * Section 5.1 canvas-pixel mapping, exact:
     *   WorldPoint = P + R * Facing * (x - PivotX) * s + U * (PivotY - y) * s
     * CanvasPointPx and PivotCanvasPx are canvas coordinates (y down);
     * FacingSign is the actor-facing sign (+1 right, -1 left). Pure math
     * (unit-tested without a world).
     */
    UEMMO_API FVector ComputeCanvasPointWorld(const FVector& FootP, const FVector& RightAxis,
        const FVector& UpAxis, float FacingSign, const FIntPoint& CanvasPointPx,
        const FIntPoint& PivotCanvasPx, float CmPerPixel);

    /**
     * The one-time section 5.1 calibration: s = TargetStandingHeightCm /
     * StandingHeadTopPx; non-positive inputs yield 0 (uncalibrated).
     */
    UEMMO_API float ComputeCmPerPixel(float StandingHeadTopPx, float TargetStandingHeightCm);

    /**
     * The quad world transform for one display frame: basis X = R * Facing
     * (canvas x), Y = UpAxis (canvas up), center from the formula at the rect
     * center, scale = rect pixels * s over the engine plane's 100 local units.
     */
    UEMMO_API FTransform ComputeFrameQuadTransform(const FVector& FootP, const FVector& RightAxis,
        const FVector& UpAxis, float FacingSign, const FDNFMVisualFrame& Frame, float CmPerPixel);

    /**
     * The page UV rect of one frame (the material parameter values): the page
     * pixel rect normalized by the page size (y-down, the verified UE texture
     * convention). A degenerate page falls back to the unit rect.
     */
    UEMMO_API void ComputePageUVRect(const FDNFMVisualFrame& Frame, const FIntPoint& PageSizePx,
        FVector2D& OutMin, FVector2D& OutSize);
}
