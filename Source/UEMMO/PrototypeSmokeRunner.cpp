#include "PrototypeSmokeRunner.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UnrealClient.h"

APrototypeSmokeRunner::APrototypeSmokeRunner() { PrimaryActorTick.bCanEverTick = true; }
void APrototypeSmokeRunner::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bFinished) return;
    Elapsed += DeltaSeconds;
    ACharacter* Player = UGameplayStatics::GetPlayerCharacter(this, 0);
    if (!Player)
    {
        if (Elapsed > 20.f)
        {
            bFinished = true;
            const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Artifacts"));
            IFileManager::Get().MakeDirectory(*Directory, true);
            FFileHelper::SaveStringToFile(TEXT("{\"success\":false,\"error\":\"Player did not spawn within 20 simulation seconds\"}"), *(Directory / TEXT("runtime-smoke.json")));
            FPlatformMisc::RequestExitWithStatus(false, 1);
        }
        return;
    }
    if (Elapsed < 2.f) return;
    if (!bStarted) { Origin = Player->GetActorLocation(); bStarted = true; }
    if (Elapsed < 3.f) Player->AddMovementInput(FVector::ForwardVector, 1.f);
    else if (Elapsed < 4.f) Player->AddMovementInput(FVector::RightVector, -1.f);
    else if (!bJumped) { Player->Jump(); bJumped = true; }
    MaximumZ = FMath::Max(MaximumZ, Player->GetActorLocation().Z);
    if (Elapsed > 4.3f) Player->StopJumping();
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Artifacts"));
    if (Elapsed > 8.f && !bCaptured)
    {
        IFileManager::Get().MakeDirectory(*Directory, true);
        if (FApp::CanEverRender()) FScreenshotRequest::RequestScreenshot(Directory / TEXT("prototype-smoke.png"), true, false);
        bCaptured = true;
    }
    if (Elapsed > 11.f)
    {
        bFinished = true;
        const FVector Delta = Player->GetActorLocation() - Origin;
        const float Height = MaximumZ - Origin.Z;
        const bool bSuccess = Delta.X > 100.f && Delta.Y < -100.f && Height > 60.f;
        const FString Json = FString::Printf(TEXT("{\"success\":%s,\"delta_x_cm\":%.3f,\"delta_y_cm\":%.3f,\"jump_height_cm\":%.3f,\"rendering\":%s,\"coverage\":\"movement commands, collision, jump, map load; physical keyboard input not simulated\"}"),
            bSuccess ? TEXT("true") : TEXT("false"), Delta.X, Delta.Y, Height, FApp::CanEverRender() ? TEXT("true") : TEXT("false"));
        const bool bSaved = FFileHelper::SaveStringToFile(Json, *(Directory / TEXT("runtime-smoke.json")));
        UE_LOG(LogTemp, Display, TEXT("UEMMO_RUNTIME_SMOKE %s"), *Json);
        FPlatformMisc::RequestExitWithStatus(false, bSuccess && bSaved ? 0 : 1);
    }
}
