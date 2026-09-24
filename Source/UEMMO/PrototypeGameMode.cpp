#include "PrototypeGameMode.h"
#include "PrototypeCharacter.h"
#include "PrototypeHUD.h"
#include "PrototypeSmokeRunner.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
APrototypeGameMode::APrototypeGameMode()
{
    DefaultPawnClass = APrototypeCharacter::StaticClass();
    HUDClass = APrototypeHUD::StaticClass();
}
void APrototypeGameMode::StartPlay()
{
    Super::StartPlay();
#if !UE_BUILD_SHIPPING
    if (FParse::Param(FCommandLine::Get(), TEXT("UEMMOSmoke")))
        GetWorld()->SpawnActor<APrototypeSmokeRunner>();
#endif
}
