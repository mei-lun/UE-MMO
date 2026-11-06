// M6-021: the DNFM character picture data asset - manifest deserialization
// (the frozen M6-020 contract) plus the world-independent frame sampling math.
// The parsing here runs only inside the M6-022 editor import helper (or the
// tests); the game loop never reads JSON (plan section 9: no per-tick parse).
#include "DNFMCharacterVisualSet.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    // Strict field readers for the frozen M6-020 manifest schema: a missing
    // field or a wrong type fails the whole build (no silent defaults, plan
    // section 10). UE 5.8's Json module exposes GetFieldUntyped/TryGetString
    // (no Find/AsString(FString&) overloads on these types).
    bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& Out)
    {
        const TSharedPtr<FJsonValue> Value = Object->GetFieldUntyped(Field);
        return Value.IsValid() && Value->Type == EJson::String && Value->TryGetString(Out);
    }

    bool ReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool& Out)
    {
        const TSharedPtr<FJsonValue> Value = Object->GetFieldUntyped(Field);
        if (!Value.IsValid() || Value->Type != EJson::Boolean)
        {
            return false;
        }
        Out = Value->AsBool();
        return true;
    }

    // Integral-number read shared by the object-field and array-element
    // readers. The contract carries integers only; a fractional number is a
    // schema violation, not something to floor away.
    bool ReadNumberInt(double Number, int32& Out)
    {
        if (!FMath::IsFinite(Number) || FMath::Abs(Number - FMath::RoundToDouble(Number)) > UE_DOUBLE_SMALL_NUMBER)
        {
            return false;
        }
        Out = static_cast<int32>(FMath::RoundToDouble(Number));
        return true;
    }

    bool ReadInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32& Out)
    {
        const TSharedPtr<FJsonValue> Value = Object->GetFieldUntyped(Field);
        if (!Value.IsValid() || Value->Type != EJson::Number)
        {
            return false;
        }
        return ReadNumberInt(Value->AsNumber(), Out);
    }

    /**
     * Numeric array reader: "size"/"canvas_size_px" take [w, h], "uv_rect_px"
     * takes [x, y, w, h]. OutB is only written by the four-value form.
     */
    bool ReadIntArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
        FIntPoint& OutA, FIntPoint& OutB, bool bFourValues)
    {
        const TSharedPtr<FJsonValue> Value = Object->GetFieldUntyped(Field);
        if (!Value.IsValid() || Value->Type != EJson::Array)
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
        if (Array.Num() != (bFourValues ? 4 : 2))
        {
            return false;
        }
        int32 Numbers[4] = {0, 0, 0, 0};
        for (int32 Index = 0; Index < Array.Num(); ++Index)
        {
            if (!Array[Index].IsValid() || Array[Index]->Type != EJson::Number
                || !ReadNumberInt(Array[Index]->AsNumber(), Numbers[Index]))
            {
                return false;
            }
        }
        OutA = FIntPoint(Numbers[0], Numbers[1]);
        OutB = bFourValues ? FIntPoint(Numbers[2], Numbers[3]) : OutA;
        return true;
    }

    FString DescribeExpected(const FString& Where, const TCHAR* Problem)
    {
        return FString::Printf(TEXT("manifest %s: %s"), *Where, Problem);
    }
}

int32 UDNFMCharacterVisualSet::GetActionIndex(FName ActionId) const
{
    for (int32 Index = 0; Index < Actions.Num(); ++Index)
    {
        if (Actions[Index].ActionId == ActionId)
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

const FDNFMActionClip* UDNFMCharacterVisualSet::FindAction(FName ActionId) const
{
    const int32 Index = GetActionIndex(ActionId);
    return Actions.IsValidIndex(Index) ? &Actions[Index] : nullptr;
}

UTexture2D* UDNFMCharacterVisualSet::LoadPageTexture(int32 PageIndex) const
{
    return PageTextures.IsValidIndex(PageIndex) ? PageTextures[PageIndex].LoadSynchronous() : nullptr;
}

bool UDNFMCharacterVisualSet::BuildFromManifestJson(const FString& ManifestJson,
    const TArray<UTexture2D*>& ResolvedPageTextures,
    UDNFMCharacterVisualSet& OutSet, FString& OutError)
{
    OutError.Reset();
    if (!ParseManifestInto(ManifestJson, ResolvedPageTextures, OutSet, OutError))
    {
        // Validation-first: a refused build leaves NOTHING half-populated
        // (no partially added actions survive a mid-parse refusal).
        OutSet.CharacterId.Reset();
        OutSet.OutfitId.Reset();
        OutSet.Generator.Reset();
        OutSet.PageTextures.Reset();
        OutSet.PageFiles.Reset();
        OutSet.PageSha256s.Reset();
        OutSet.PageSizesPx.Reset();
        OutSet.Actions.Reset();
        return false;
    }
    return true;
}

FString UDNFMCharacterVisualSet::BuildManifestIntoSet(const FString& ManifestJson,
    const TArray<UTexture2D*>& ResolvedPageTextures,
    UDNFMCharacterVisualSet* OutSet)
{
    FString OutError;
    if (OutSet == nullptr)
    {
        OutError = TEXT("OutSet data asset is null");
        return OutError;
    }
    BuildFromManifestJson(ManifestJson, ResolvedPageTextures, *OutSet, OutError);
    return OutError;
}

bool UDNFMCharacterVisualSet::ParseManifestInto(const FString& ManifestJson,
    const TArray<UTexture2D*>& ResolvedPageTextures,
    UDNFMCharacterVisualSet& OutSet, FString& OutError)
{
    // Start from a clean slate so a reuse of the same object is deterministic.
    OutSet.CharacterId.Reset();
    OutSet.OutfitId.Reset();
    OutSet.Generator.Reset();
    OutSet.PageTextures.Reset();
    OutSet.PageFiles.Reset();
    OutSet.PageSha256s.Reset();
    OutSet.PageSizesPx.Reset();
    OutSet.Actions.Reset();

    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(ManifestJson);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        OutError = TEXT("manifest is not a JSON object");
        return false;
    }

    // Pages first (the frames reference them by index).
    const TSharedPtr<FJsonValue> PagesValue = Root->GetFieldUntyped(TEXT("pages"));
    if (!PagesValue.IsValid() || PagesValue->Type != EJson::Array || PagesValue->AsArray().Num() == 0)
    {
        OutError = TEXT("manifest pages: missing or empty array");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& Pages = PagesValue->AsArray();
    for (int32 PageIndex = 0; PageIndex < Pages.Num(); ++PageIndex)
    {
        const FString Where = FString::Printf(TEXT("pages[%d]"), PageIndex);
        if (!Pages[PageIndex].IsValid() || Pages[PageIndex]->Type != EJson::Object)
        {
            OutError = DescribeExpected(Where, TEXT("entry is not an object"));
            return false;
        }
        const TSharedPtr<FJsonObject>& Page = Pages[PageIndex]->AsObject();
        FString File;
        if (!ReadString(Page, TEXT("file"), File))
        {
            OutError = DescribeExpected(Where, TEXT("missing file string"));
            return false;
        }
        FString Sha256;
        if (!ReadString(Page, TEXT("sha256"), Sha256))
        {
            OutError = DescribeExpected(Where, TEXT("missing sha256 string"));
            return false;
        }
        FIntPoint Size;
        FIntPoint Ignored;
        if (!ReadIntArray(Page, TEXT("size"), Size, Ignored, false))
        {
            OutError = DescribeExpected(Where, TEXT("size must be [width, height] integers"));
            return false;
        }
        if (Size.X <= 0 || Size.Y <= 0)
        {
            OutError = DescribeExpected(Where, TEXT("size must be positive"));
            return false;
        }
        OutSet.PageFiles.Add(File);
        OutSet.PageSha256s.Add(Sha256);
        OutSet.PageSizesPx.Add(Size);
        OutSet.PageTextures.Add(ResolvedPageTextures.IsValidIndex(PageIndex)
            ? ResolvedPageTextures[PageIndex]
            : TSoftObjectPtr<UTexture2D>());
    }
    if (ResolvedPageTextures.Num() > 0 && ResolvedPageTextures.Num() != OutSet.PageFiles.Num())
    {
        OutError = FString::Printf(TEXT("resolved page texture count (%d) does not match the manifest page count (%d)"),
            ResolvedPageTextures.Num(), OutSet.PageFiles.Num());
        return false;
    }

    ReadString(Root, TEXT("character"), OutSet.CharacterId);
    ReadString(Root, TEXT("outfit"), OutSet.OutfitId);
    ReadString(Root, TEXT("generator"), OutSet.Generator);

    // Actions and their frame rows.
    const TSharedPtr<FJsonValue> ActionsValue = Root->GetFieldUntyped(TEXT("actions"));
    if (!ActionsValue.IsValid() || ActionsValue->Type != EJson::Array)
    {
        OutError = TEXT("manifest actions: missing array");
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>& Actions = ActionsValue->AsArray();
    if (Actions.Num() == 0)
    {
        OutError = TEXT("manifest actions: empty array (an empty set is not a usable visual)");
        return false;
    }
    for (int32 ActionIndex = 0; ActionIndex < Actions.Num(); ++ActionIndex)
    {
        const FString Where = FString::Printf(TEXT("actions[%d]"), ActionIndex);
        if (!Actions[ActionIndex].IsValid() || Actions[ActionIndex]->Type != EJson::Object)
        {
            OutError = DescribeExpected(Where, TEXT("entry is not an object"));
            return false;
        }
        const TSharedPtr<FJsonObject>& Action = Actions[ActionIndex]->AsObject();
        FDNFMActionClip Clip;
        FString ActionId;
        if (!ReadString(Action, TEXT("action_id"), ActionId) || ActionId.IsEmpty())
        {
            OutError = DescribeExpected(Where, TEXT("missing action_id string"));
            return false;
        }
        Clip.ActionId = FName(*ActionId);
        if (!ReadBool(Action, TEXT("loop"), Clip.bLoop))
        {
            OutError = DescribeExpected(Where, TEXT("missing loop bool"));
            return false;
        }
        // Traceability strings: the contract lists them; absent ones stay
        // empty (a present-but-not-string value is ignored for these two, the
        // identity fields above are the load-bearing ones).
        {
            const TSharedPtr<FJsonValue> AniValue = Action->GetFieldUntyped(TEXT("source_ani"));
            if (AniValue.IsValid() && AniValue->Type == EJson::String)
            {
                AniValue->TryGetString(Clip.SourceAniPath);
            }
            ReadString(Action, TEXT("source_sha256"), Clip.SourceSha256);
        }

        const TSharedPtr<FJsonValue> FramesValue = Action->GetFieldUntyped(TEXT("frames"));
        if (!FramesValue.IsValid() || FramesValue->Type != EJson::Array || FramesValue->AsArray().Num() == 0)
        {
            OutError = DescribeExpected(Where, TEXT("missing or empty frames array"));
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Frames = FramesValue->AsArray();
        for (int32 FrameIndex = 0; FrameIndex < Frames.Num(); ++FrameIndex)
        {
            const FString FrameWhere = FString::Printf(TEXT("actions[%d].frames[%d]"), ActionIndex, FrameIndex);
            if (!Frames[FrameIndex].IsValid() || Frames[FrameIndex]->Type != EJson::Object)
            {
                OutError = DescribeExpected(FrameWhere, TEXT("entry is not an object"));
                return false;
            }
            const TSharedPtr<FJsonObject>& Frame = Frames[FrameIndex]->AsObject();
            FDNFMVisualFrame Row;
            int32 DeclaredIndex = 0;
            if (!ReadInt(Frame, TEXT("index"), DeclaredIndex) || DeclaredIndex != FrameIndex)
            {
                OutError = DescribeExpected(FrameWhere, TEXT("index must be the contiguous frame position starting at 0"));
                return false;
            }
            if (!ReadInt(Frame, TEXT("duration_ms"), Row.DurationMs) || Row.DurationMs <= 0)
            {
                OutError = DescribeExpected(FrameWhere, TEXT("duration_ms must be a positive integer"));
                return false;
            }
            // M8-018: a no-draw logic frame keeps its timeline duration but
            // carries no pixels (page/uv/pivot absent in the manifest); the
            // display holds the previous drawn pose for the duration.
            Row.bDrawFrame = true;
            if (Frame->HasField(TEXT("draw_frame")))
            {
                const TSharedPtr<FJsonValue> DrawValue = Frame->GetFieldUntyped(TEXT("draw_frame"));
                Row.bDrawFrame = DrawValue.IsValid() && DrawValue->AsBool();
            }
            if (Row.bDrawFrame)
            {
            if (!ReadInt(Frame, TEXT("page"), Row.PageIndex) || !OutSet.PageSizesPx.IsValidIndex(Row.PageIndex))
            {
                OutError = DescribeExpected(FrameWhere, TEXT("page must index the manifest pages array"));
                return false;
            }
            FIntPoint RectPos;
            FIntPoint RectSize;
            if (!ReadIntArray(Frame, TEXT("uv_rect_px"), RectPos, RectSize, true))
            {
                OutError = DescribeExpected(FrameWhere, TEXT("uv_rect_px must be [x, y, w, h] integers"));
                return false;
            }
            if (RectSize.X <= 0 || RectSize.Y <= 0)
            {
                OutError = DescribeExpected(FrameWhere, TEXT("uv_rect_px size must be positive"));
                return false;
            }
            if (RectPos.X < 0 || RectPos.Y < 0
                || RectPos.X + RectSize.X > OutSet.PageSizesPx[Row.PageIndex].X
                || RectPos.Y + RectSize.Y > OutSet.PageSizesPx[Row.PageIndex].Y)
            {
                OutError = FString::Printf(TEXT("manifest %s: uv_rect_px (%d, %d, %d, %d) exceeds page %d bounds (%d, %d)"),
                    *FrameWhere, RectPos.X, RectPos.Y, RectSize.X, RectSize.Y,
                    Row.PageIndex, OutSet.PageSizesPx[Row.PageIndex].X, OutSet.PageSizesPx[Row.PageIndex].Y);
                return false;
            }
            Row.UvRectPosPx = RectPos;
            Row.UvRectSizePx = RectSize;
            FIntPoint Ignored;
            if (!ReadIntArray(Frame, TEXT("canvas_size_px"), Row.CanvasSizePx, Ignored, false)
                || Row.CanvasSizePx.X <= 0 || Row.CanvasSizePx.Y <= 0)
            {
                OutError = DescribeExpected(FrameWhere, TEXT("canvas_size_px must be [width, height] positive integers"));
                return false;
            }
            if (!ReadIntArray(Frame, TEXT("pivot_px"), Row.PivotPx, Ignored, false))
            {
                OutError = DescribeExpected(FrameWhere, TEXT("pivot_px must be [px, py] integers"));
                return false;
            }
            } // end draw-frame pixel block (M8-018 no-draw frames skip it)
            ReadString(Frame, TEXT("source_sha256"), Row.SourceSha256);

            const TSharedPtr<FJsonValue> EventsValue = Frame->GetFieldUntyped(TEXT("events"));
            if (EventsValue.IsValid() && EventsValue->Type == EJson::Array)
            {
                for (const TSharedPtr<FJsonValue>& EventValue : EventsValue->AsArray())
                {
                    if (!EventValue.IsValid() || EventValue->Type != EJson::Object)
                    {
                        OutError = DescribeExpected(FrameWhere, TEXT("events[] entry is not an object"));
                        return false;
                    }
                    const TSharedPtr<FJsonObject>& Event = EventValue->AsObject();
                    FDNFMVisualFrameEvent RowEvent;
                    FString Type;
                    if (!ReadString(Event, TEXT("type"), Type))
                    {
                        OutError = DescribeExpected(FrameWhere, TEXT("events[] missing type string"));
                        return false;
                    }
                    RowEvent.Type = FName(*Type);
                    FString Name;
                    if (ReadString(Event, TEXT("name"), Name))
                    {
                        RowEvent.Name = FName(*Name);
                    }
                    Row.Events.Add(RowEvent);
                }
            }
            Clip.Frames.Add(Row);
        }

        if (OutSet.GetActionIndex(Clip.ActionId) != INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("manifest actions: duplicate action_id '%s'"), *Clip.ActionId.ToString());
            return false;
        }
        OutSet.Actions.Add(Clip);
    }
    return true;
}

namespace UE::UEMMO::Tasks::M6_021
{
    int32 ComputeClipTotalDurationMs(const TArray<FDNFMVisualFrame>& Frames)
    {
        int32 Total = 0;
        for (const FDNFMVisualFrame& Frame : Frames)
        {
            Total += FMath::Max(Frame.DurationMs, 0);
        }
        return Total;
    }

    int32 SampleFrameIndexByElapsedMs(const TArray<FDNFMVisualFrame>& Frames, double ElapsedMs, bool bLoop)
    {
        if (Frames.Num() == 0)
        {
            return INDEX_NONE;
        }
        const int32 Total = ComputeClipTotalDurationMs(Frames);
        if (Total <= 0)
        {
            return Frames.Num() - 1; // degenerate clip deterministically holds its last frame
        }
        double Local = ElapsedMs;
        if (bLoop)
        {
            Local = FMath::Fmod(Local, static_cast<double>(Total));
            if (Local < 0.0)
            {
                Local += static_cast<double>(Total);
            }
        }
        else
        {
            // A finished once-clip holds its end frame (plan section 6.2).
            Local = FMath::Clamp(Local, 0.0, static_cast<double>(Total));
        }
        int32 Index = 0;
        double Accumulated = 0.0;
        for (; Index < Frames.Num(); ++Index)
        {
            const double Duration = static_cast<double>(FMath::Max(Frames[Index].DurationMs, 0));
            // The frame covering [Accumulated, Accumulated+Duration) owns the
            // instant; the last frame also absorbs any zero-duration tail or
            // rounding overshoot so the walk can never run past the table.
            if (Local < Accumulated + Duration || Index == Frames.Num() - 1)
            {
                break;
            }
            Accumulated += Duration;
        }
        return Index;
    }

    TArray<int32> ComputeCrossedFrameIndices(const TArray<FDNFMVisualFrame>& Frames,
        int32 PreviousIndex, int32 NewIndex, bool bLoop)
    {
        TArray<int32> Crossed;
        if (Frames.Num() == 0 || NewIndex == PreviousIndex)
        {
            return Crossed;
        }
        if (!Frames.IsValidIndex(NewIndex))
        {
            return Crossed;
        }
        if (!Frames.IsValidIndex(PreviousIndex))
        {
            // Playback start (or a fresh SetAction): frame 0 through NewIndex.
            for (int32 Index = 0; Index <= NewIndex; ++Index)
            {
                Crossed.Add(Index);
            }
            return Crossed;
        }
        if (!bLoop || NewIndex > PreviousIndex)
        {
            // A plain forward interval: non-loop playback only moves forward
            // (SampleFrameIndex clamps), and a loop playback that still moves
            // to a higher index has not wrapped in this advance.
            for (int32 Index = PreviousIndex + 1; Index <= NewIndex; ++Index)
            {
                Crossed.Add(Index);
            }
            return Crossed;
        }
        // Loop wrap (NewIndex < PreviousIndex): walk to the table end, then
        // wrap through 0 to NewIndex. The iteration count is bounded by the
        // table length by construction (a single advance never laps the
        // clip), so no zero-duration spin.
        for (int32 Index = PreviousIndex + 1; Index < Frames.Num(); ++Index)
        {
            Crossed.Add(Index);
        }
        for (int32 Index = 0; Index <= NewIndex; ++Index)
        {
            Crossed.Add(Index);
        }
        return Crossed;
    }
}
