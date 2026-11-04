#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DNFAnimationRepairLibrary.generated.h"
class UAnimationAsset;

UCLASS()
class UEMMO_API UDNFAnimationRepairLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    /** Editor-only migration of the independently imported hero action skeletons. */
    UFUNCTION(BlueprintCallable, Category="Editor Scripting")
    static bool UseCanonicalHeroSkeleton(UAnimationAsset* Asset);
};
