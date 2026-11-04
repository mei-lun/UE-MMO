#include "DNFAnimationRepairLibrary.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"

bool UDNFAnimationRepairLibrary::UseCanonicalHeroSkeleton(UAnimationAsset* Asset)
{
#if WITH_EDITOR
    if (!Asset || !(Asset->GetPathName().StartsWith(TEXT("/Game/UEMMO/ArtValidation/DNFArt/Hero/"))
        || Asset->GetPathName().StartsWith(TEXT("/Game/UEMMO/Animation/Montages/MNT_")))) return false;
    USkeleton* Canonical=LoadObject<USkeleton>(nullptr,
        TEXT("/Game/UEMMO/ArtValidation/DNFArt/Hero/hero_skm_Skeleton.hero_skm_Skeleton"));
    USkeleton* Source=Asset->GetSkeleton();
    if (!Canonical || !Source || !Source->GetPathName().StartsWith(TEXT("/Game/UEMMO/ArtValidation/DNFArt/Hero/"))) return false;
    if(Source==Canonical) return true;
    const FReferenceSkeleton& A=Source->GetReferenceSkeleton();
    const FReferenceSkeleton& B=Canonical->GetReferenceSkeleton();
    if(A.GetNum()!=B.GetNum()) return false;
    for(int32 i=0;i<A.GetNum();++i)
        if(A.GetBoneName(i)!=B.GetBoneName(i) || A.GetParentIndex(i)!=B.GetParentIndex(i)) return false;
    Asset->Modify();
    const bool Ok=Asset->ReplaceSkeleton(Canonical,false);
    if(Ok) Asset->MarkPackageDirty();
    return Ok && Asset->GetSkeleton()==Canonical;
#else
    return false;
#endif
}
