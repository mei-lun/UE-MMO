#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DNFMCharacterVisualSet.generated.h"

class UTexture2D;

/**
 * M6-021: one frame event row of a DNFM action clip. Values mirror the
 * M6-020 manifest contract (Docs/Tasks/M6-020.md) events[] entries verbatim;
 * the runtime never invents event semantics (dedup and dispatch are M6-023).
 */
USTRUCT(BlueprintType)
struct UEMMO_API FDNFMVisualFrameEvent
{
    GENERATED_BODY()

    /** manifest events[].type (e.g. "audio"). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FName Type;

    /** manifest events[].name (the original resource name of the event). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FName Name;
};

/**
 * M6-021: one display frame of a DNFM action clip. Every field mirrors the
 * frozen M6-020 manifest schema (frame rows); the y axis follows the manifest
 * convention (y down, measured from the top-left of the referenced rect).
 */
USTRUCT(BlueprintType)
struct UEMMO_API FDNFMVisualFrame
{
    GENERATED_BODY()

    /** manifest frames[].duration_ms (> 0; a zero-duration frame is refused at import). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    int32 DurationMs = 0;

    /** manifest frames[].page - the atlas page index holding this frame's pixels. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    int32 PageIndex = 0;

    /** manifest frames[].uv_rect_px top-left corner (page pixels). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FIntPoint UvRectPosPx = FIntPoint::ZeroValue;

    /** manifest frames[].uv_rect_px width/height (page pixels). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FIntPoint UvRectSizePx = FIntPoint::ZeroValue;

    /** manifest frames[].canvas_size_px (the frame's composed canvas, pixels). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FIntPoint CanvasSizePx = FIntPoint::ZeroValue;

    /**
     * manifest frames[].pivot_px - the foot/action origin inside the uv rect,
     * relative to the rect top-left, y down (the pivot_definition of M6-020).
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FIntPoint PivotPx = FIntPoint::ZeroValue;

    /** manifest frames[].events (reported as-is when the frame becomes current). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    TArray<FDNFMVisualFrameEvent> Events;

    /** Optional per-frame source hash when the manifest carries one (traceability). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FString SourceSha256;
};

/**
 * M6-021: one action clip (the manifest's actions[] row): the display-frame
 * sequence of one action id with its loop flag and source identity. The
 * frames are stored in manifest order (index 0..N-1 contiguous, enforced at
 * import); the per-frame fields carry the frozen contract values.
 */
USTRUCT(BlueprintType)
struct UEMMO_API FDNFMActionClip
{
    GENERATED_BODY()

    /** manifest actions[].action_id (e.g. "rest", "attack1"). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FName ActionId;

    /** manifest actions[].loop (the default playback wrap policy). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    bool bLoop = false;

    /** manifest actions[].source_ani (traceability of the original resource). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FString SourceAniPath;

    /** manifest actions[].source_sha256 (the source identity hash). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FString SourceSha256;

    /** Display frames in manifest order (the sampling walks them in order). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    TArray<FDNFMVisualFrame> Frames;
};

/**
 * M6-021: the DNFM character picture data asset (the M6-020 offline pipeline
 * output shape): page textures plus per-action frame tables. The asset is a
 * pure data carrier - it owns no playback state; the sampling math lives in
 * the UE::UEMMO::Tasks::M6_021 namespace below and the playback/positioning
 * lives in UDNFMCharacterVisualComponent (the plan section 3 split).
 *
 * Page textures are SOFT references (source isolation: the imported page
 * assets stay under their own content root; the set never hard-pins them).
 * The M6-022 editor import script fills the soft references (either by
 * passing the resolved textures to BuildFromManifestJson or by writing the
 * property afterwards).
 */
UCLASS(BlueprintType)
class UEMMO_API UDNFMCharacterVisualSet : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    /** manifest character (e.g. "fighter"). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FString CharacterId;

    /** manifest outfit (e.g. "0000a"). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FString OutfitId;

    /** manifest generator (the producing script path, traceability). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    FString Generator;

    /** Page textures in manifest page order (soft refs, source isolation). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    TArray<TSoftObjectPtr<UTexture2D>> PageTextures;

    /** manifest pages[].file (traceability of the page pngs). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    TArray<FString> PageFiles;

    /** manifest pages[].sha256 (traceability of the page content). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    TArray<FString> PageSha256s;

    /** manifest pages[].size (page pixels; the uv math denominator). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    TArray<FIntPoint> PageSizesPx;

    /** Action clips (one row per manifest action; action ids unique). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM Visual")
    TArray<FDNFMActionClip> Actions;

    /** The clip row index for ActionId, or INDEX_NONE. */
    int32 GetActionIndex(FName ActionId) const;

    /** The clip row for ActionId, or nullptr. */
    const FDNFMActionClip* FindAction(FName ActionId) const;

    /**
     * Synchronous page-texture load helper (the component caches the result
     * per page; callers that poll every frame should keep the returned
     * pointer themselves). Null for an out-of-range page or an unset soft
     * reference (the honest "no texture imported yet" state).
     */
    UTexture2D* LoadPageTexture(int32 PageIndex) const;

    /**
     * M6-020 manifest JSON -> asset fields (the editor import helper M6-022
     * calls; runtime code never parses JSON in Tick). Validation-first per
     * plan section 10: any contract violation refuses the WHOLE build (OutSet
     * is cleared) with a concrete OutError - no silent row dropping.
     *
     * ResolvedPageTextures (optional) binds page textures by manifest page
     * order: an empty array builds a data-only set (tests, offline review);
     * a non-empty array must match pages one-to-one or the build refuses.
     */
    static bool BuildFromManifestJson(const FString& ManifestJson,
        const TArray<UTexture2D*>& ResolvedPageTextures,
        UDNFMCharacterVisualSet& OutSet, FString& OutError);

    /**
     * M6-022: scripting entry for the editor import script
     * (Scripts/Editor/import_dnfm_character_visuals.py) - Python can only
     * call UFUNCTIONs, and UHT forbids non-const object-reference params on
     * BlueprintCallable functions (hence the pointer here and the exact
     * delegate into the validated builder above; the M6-021 reference API
     * stays untouched for its tests). The refusal reason travels as the
     * RETURN value (empty = success): the probed 5.8.3 python binding drops
     * plain bool returns and mis-maps a FString& out param on this
     * closure-marshal path (bool return + FString& out + TArray in), so the
     * error text must not depend on either. OutSet is mutated in place;
     * Python passes the freshly created data asset and reads it back. A null
     * OutSet refuses with the error text.
     */
    UFUNCTION(BlueprintCallable, Category = "DNFM Visual",
        meta = (DisplayName = "Build Manifest Into Set"))
    static FString BuildManifestIntoSet(const FString& ManifestJson,
        const TArray<UTexture2D*>& ResolvedPageTextures,
        UDNFMCharacterVisualSet* OutSet);

private:
    /** The raw parser behind BuildFromManifestJson (leaves rows on refusal). */
    static bool ParseManifestInto(const FString& ManifestJson,
        const TArray<UTexture2D*>& ResolvedPageTextures,
        UDNFMCharacterVisualSet& OutSet, FString& OutError);

public:
};

namespace UE::UEMMO::Tasks::M6_021
{
    /**
     * Total playback duration of one clip (the sum of the frame durations,
     * milliseconds). Negative durations count as 0.
     */
    UEMMO_API int32 ComputeClipTotalDurationMs(const TArray<FDNFMVisualFrame>& Frames);

    /**
     * The display frame current at ElapsedMs. Loop clips wrap modulo the
     * total duration; non-loop clips clamp to the last frame (plan section
     * 6.2: a finished once-clip holds its end frame). An empty clip returns
     * INDEX_NONE; a degenerate (<= 0 total) clip holds the last frame.
     * Deterministic: the frame covering [Acc, Acc+Duration) owns the instant;
     * a boundary instant belongs to the frame that starts there.
     */
    UEMMO_API int32 SampleFrameIndexByElapsedMs(const TArray<FDNFMVisualFrame>& Frames,
        double ElapsedMs, bool bLoop);

    /**
     * The frame indices newly crossed when playback moves from PreviousIndex
     * to NewIndex (exclusive of Previous, inclusive of New), walking frames in
     * time order. Loop clips wrap: e.g. previous N-1 -> new 0 walks the wrap.
     * Event reporting consumes this in order (one report per newly reached
     * frame), which gives the M6-023 dedup an honest crossed-interval feed on
     * low framerates. Empty for an unchanged index or an empty clip.
     */
    UEMMO_API TArray<int32> ComputeCrossedFrameIndices(const TArray<FDNFMVisualFrame>& Frames,
        int32 PreviousIndex, int32 NewIndex, bool bLoop);
}
