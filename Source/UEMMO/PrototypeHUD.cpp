#include "PrototypeHUD.h"
#include "Engine/Canvas.h"
void APrototypeHUD::DrawHUD()
{
    Super::DrawHUD();
    DrawRect(FLinearColor(0.02f, 0.03f, 0.05f, 0.86f), 18, 18, 630, 84);
    DrawText(TEXT("UE-MMO | M0: movement and asset foundation"), FLinearColor::White, 32, 28, nullptr, 1.2f);
    DrawText(TEXT("A/D: left/right    W/S: depth    Space: jump    R: reset"), FLinearColor(0.6f, 0.85f, 1.f), 32, 53);
    DrawText(TEXT("Single-player. Combat is specified, not implemented in M0."), FLinearColor(0.9f, 0.8f, 0.45f), 32, 77);
}
