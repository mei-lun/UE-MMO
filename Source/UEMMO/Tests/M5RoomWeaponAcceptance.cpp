// M5-060: a fresh rendered game process, real menu travel, real inventory
// buttons and viewport keys, against the room's own wave enemies. This is
// deliberately outside UEMMO.Tasks: shared-world unit suites cannot satisfy
// its startup preconditions. A missing precondition fails, never skips.
#include "Misc/AutomationTest.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../UI/MapSelectWidget.h"
#include "../Weapons/WeaponComponent.h"
#include "../Projectiles/CombatProjectile.h"
#include "Components/StaticMeshComponent.h"
#include "../Projectiles/HitscanTracer.h"
#include "Components/Button.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
class FRoomWeaponAcceptance final : public IAutomationLatentCommand
{
public:
	FRoomWeaponAcceptance(FAutomationTestBase& InTest, FString InWeapon, FString InOutput)
		: Test(InTest), Weapon(MoveTemp(InWeapon)), Output(MoveTemp(InOutput))
	{
		StageStart = FPlatformTime::Seconds();
	}

	bool Update() override
	{
		if (FPlatformTime::Seconds() - StageStart > 30.0) { return Fail(FString::Printf(TEXT("stage %d timed out"), Stage)); }
		UWorld* World = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr;
		const double Elapsed = World ? World->GetTimeSeconds() - StageGameStart : 0.0;
		APrototypeCharacter* Pawn = World ? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0)) : nullptr;
		APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->Controller) : nullptr;
		APrototypeHUD* HUD = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
		UWeaponComponent* Mount = Pawn ? Pawn->GetWeaponMount() : nullptr;
		if (Stage >= 6 && World && !bMelee)
		{
			for (TActorIterator<ACombatProjectile> It(World); It; ++It)
			{
				if (It->Context.WeaponInstanceId != FiredWeaponInstance) { continue; }
				const FGuid Id = It->Context.ProjectileInstanceId;
				const double Now = World->GetTimeSeconds();
				if (!CaptureBulletId.IsValid()) { CaptureBulletId = Id; }
				if (const FVector* Start = BulletPositions.Find(Id))
				{
					const double Distance = FVector::Dist(*Start, It->GetActorLocation());
					const double Delta = Now - BulletTimes.FindChecked(Id);
					if (Distance > 1.0)
					{
						MovingBulletIds.Add(Id);
						bObservedFlightBeforeDamage |= Target.IsValid() && Target->GetHealthComponent()->GetHealth() == HealthBefore;
						if (It->Context.ProjectileId == FName(TEXT("bullet_linear")))
						{
							const double Expected = It->LaunchSpeedCmS * Delta;
							if (Delta <= 0 || !FMath::IsNearlyEqual(Distance, Expected, FMath::Max(1.0, Expected * 0.05)))
							{
								return Fail(TEXT("measured linear bullet displacement does not match configured speed"));
							}
							MeasuredLinearSpeeds.Add(Distance / Delta);
						}
						if (Id == CaptureBulletId && FlightFrames.Num() < 3 && Now >= NextFlightFrameTime)
						{
							if (!Target.IsValid() || Target->GetHealthComponent()->GetHealth() != HealthBefore) { return Fail(TEXT("target was damaged before the three flight frames")); }
							FlightFrames.Add(It->GetActorLocation());
							FlightFrameTimes.Add(Now);
							const FString Name = FlightFrames.Num() == 1 ? TEXT("flight.png") : FString::Printf(TEXT("flight-%02d.png"), FlightFrames.Num());
							FScreenshotRequest::RequestScreenshot(Output / Name, true, false);
							NextFlightFrameTime = Now + 0.045;
						}
					}
				}
				BulletPositions.Add(Id, It->GetActorLocation());
				BulletTimes.Add(Id, Now);
				BulletSpeed = It->LaunchSpeedCmS;
			}
			if (TActorIterator<AHitscanTracer>(World)) { return Fail(TEXT("ordinary ammunition produced a hitscan beam")); }
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->GetClass()->GetName().Contains(TEXT("FlightTrail"))) { return Fail(TEXT("line-like trail actor exists")); }
			}
		}
		if (Stage == 0)
		{
			if (!World || World->WorldType != EWorldType::Game || !FApp::CanEverRender()
				|| !World->GetName().Contains(TEXT("L_CombatRoom01")) || !HUD || !Mount
				|| !HUD->PeekMenuWidget() || !HUD->PeekMenuWidget()->PeekEnterButton())
			{
				return Fail(TEXT("requires a fresh rendered L_CombatRoom01 with its boot menu"));
			}
			OldPawn = Pawn;
			// Invoke the actual button handler, including EnterRoom/OpenLevel.
			// Never broadcast the success delegates or grant/equip by hand.
			HUD->PeekMenuWidget()->PeekEnterButton()->OnClicked.Broadcast();
			Next();
			return false;
		}
		if (Stage == 1)
		{
			if (!Pawn || Pawn == OldPawn.Get() || !HUD || HUD->HasMenuScreen() || !Mount || !Mount->GetActiveBinding()) { return false; }
			bTravelled = true;
			Key(EKeys::I, IE_Pressed);
			Next();
			return false;
		}
		if (!Pawn || !HUD || !Mount) { return Fail(TEXT("the live player/HUD/mount disappeared after travel")); }
		if (Stage == 2)
		{
			if (Elapsed < 0.1) { return false; }
			Key(EKeys::I, IE_Released);
			UInventoryWidget* Inventory = HUD->PeekInventoryWidget();
			if (!Inventory) { return Fail(TEXT("viewport I did not open the inventory")); }
			UProfileSubsystem* Profile = World->GetGameInstance()->GetSubsystem<UProfileSubsystem>();
			TArray<FTestRoomItemRow> Rows;
			FString Error;
			if (!Profile || !ACombatTestRoomDriver::LoadProductionItems(Rows, Error)) { return Fail(TEXT("production items/profile unavailable")); }
			FName ItemId;
			for (const FTestRoomItemRow& Row : Rows)
			{
				if (Row.WeaponDefinitionId == FName(*Weapon)) { ItemId = Row.DefinitionId; }
			}
			int32 Selected = INDEX_NONE;
			const auto& Items = Profile->GetInventory().GetAll();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (Items[Index].DefinitionId == ItemId) { Selected = Index; break; }
			}
			if (Selected == INDEX_NONE || !Inventory->PeekRowButton(Selected)) { return Fail(TEXT("requested weapon is absent from the real inventory")); }
			Inventory->PeekRowButton(Selected)->OnClicked.Broadcast();
			if (Inventory->PeekEquipButton()->GetIsEnabled()) { Inventory->PeekEquipButton()->OnClicked.Broadcast(); }
			const auto* Binding = Mount->GetActiveBinding();
			if (!Binding || Binding->WeaponDefinitionId != FName(*Weapon)) { return Fail(TEXT("inventory equip did not bind the selected weapon")); }
			const auto* Definition = Mount->GetMountedWeaponCatalog()->FindWeapon(Binding->WeaponDefinitionId);
			if (!Definition) { return Fail(TEXT("the bound definition cannot be resolved")); }
			bMelee = Definition->FireMode == EWeaponFireMode::Melee;
			if (!bMelee && Definition->FireMode != EWeaponFireMode::Projectile) { return Fail(TEXT("ordinary guns must fire projectile entities")); }
			ExpectedPellets = Definition->PelletCount;
			ExpectedShots = bMelee ? 0 : Definition->BurstCount;
			MagazineSize = Definition->MagazineSize;
			AmmoId = Definition->AmmoId;
			ConfigRevision = Mount->GetMountedRevision();
			MenuShotCount = Mount->GetCommittedShotCount();
			Key(EKeys::Q, IE_Pressed);
			Next();
			return false;
		}
		if (Stage == 3)
		{
			if (Elapsed < 0.2) { return false; }
			Key(EKeys::Q, IE_Released);
			if (Mount->GetCommittedShotCount() != MenuShotCount) { return Fail(TEXT("inventory-open Q fired a background shot")); }
			HUD->PeekInventoryWidget()->PeekCloseButton()->OnClicked.Broadcast();
			if (HUD->HasInventoryScreen()) { return Fail(TEXT("inventory close button did not restore gameplay")); }
			Key(EKeys::Right, IE_Pressed);
			Next();
			return false;
		}
		URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
		if (Stage == 4)
		{
			if (!Session || Session->GetState() != ERoomSessionState::Running) { return false; }
			Key(EKeys::Right, IE_Released);
			RunId = Session->GetRunId();
			Next();
			return false;
		}
		if (Stage == 5)
		{
			// Wait for a naturally chasing wave pawn in the player's +X lane.
			// No teleports, controller replacement, synthetic targets or damage.
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				const FVector Delta = It->GetActorLocation() - Pawn->GetActorLocation();
				if (It->GetHealthComponent() && It->GetHealthComponent()->IsAlive()
					&& Delta.X > (bMelee ? 65.0 : 400.0) && Delta.X < (bMelee ? 180.0 : 550.0) && FMath::Abs(Delta.Y) < 25.0)
				{
					Target = *It;
					HealthBefore = It->GetHealthComponent()->GetHealth();
					MagazineBefore = Mount->GetActiveBinding()->LoadedRounds;
					ShotsBefore = Mount->GetCommittedShotCount();
					FiredWeaponInstance = Mount->GetActiveBinding()->InstanceId;
					Key(bMelee ? EKeys::X : EKeys::Q, IE_Pressed);
					Next();
					break;
				}
			}
			return false;
		}
		if (Stage == 6)
		{
			if (Elapsed < 0.06) { return false; }
			Key(bMelee ? EKeys::X : EKeys::Q, IE_Released);
			Next();
			return false;
		}
		if (Stage == 7)
		{
			if (!Target.IsValid() || !Target->GetHealthComponent()) { return Fail(TEXT("the measured target disappeared before health was observed")); }
			HealthAfter = Target->GetHealthComponent()->GetHealth();
			if (HealthAfter >= HealthBefore) { return false; }
			// Capture the first verified damage frame with the health bar.
			FScreenshotRequest::RequestScreenshot(Output / TEXT("gameplay.png"), true, false);
			Next();
			return false;
		}
		if (Stage == 8)
		{
			if (Elapsed < 0.8) { return false; }
			ShotsAfter = Mount->GetCommittedShotCount();
			if (!bMelee)
			{
				if (Pawn->GetLiveWeaponProjectileCount() > 0) { return false; }
				FScreenshotRequest::RequestScreenshot(Output / TEXT("projectiles-cleared.png"), true, false);
			}
			MagazineAfter = Mount->GetActiveBinding()->LoadedRounds;
			if (ShotsAfter - ShotsBefore != ExpectedShots) { return Fail(TEXT("release/burst shot count differs from the weapon definition")); }
			if (!bMelee && MagazineBefore - MagazineAfter != ExpectedShots) { return Fail(TEXT("magazine cost differs from the number of committed shots")); }
			ReserveBeforeReload = Mount->GetAmmoModel().GetReserveRounds(AmmoId);
			if (!bMelee) { Key(EKeys::T, IE_Pressed); }
			Next();
			return false;
		}
		if (Stage == 9)
		{
			if (Elapsed < 0.1) { return false; }
			Key(EKeys::T, IE_Released);
			// The current B trial explicitly starts with zero reserve (M5-021).
			// Prove the negative outcome without secretly seeding the test. If
			// production later supplies reserve, the same gate proves transfer.
			const int32 Transfer = bMelee ? 0 : FMath::Min(ReserveBeforeReload, MagazineSize - MagazineAfter);
			if (!bMelee && Mount->GetActiveBinding()->LoadedRounds != MagazineAfter + Transfer) { return false; }
			if (!bMelee && ReserveBeforeReload - Mount->GetAmmoModel().GetReserveRounds(AmmoId) != Transfer)
			{
				return Fail(TEXT("T reload did not transfer exactly the missing rounds from reserve"));
			}
			if (!bMelee && Transfer == 0 && Mount->GetRemainingReloadSeconds(World->GetTimeSeconds()) > 0)
			{
				return Fail(TEXT("zero-reserve T incorrectly opened a reload window"));
			}
			ReloadResult = bMelee ? TEXT("not_applicable_melee") : (Transfer > 0 ? TEXT("transferred_reserve") : TEXT("refused_no_reserve"));
			if (Mount->GetCommittedShotCount() != ShotsAfter) { return Fail(TEXT("weapon kept firing after release")); }
			if (!bMelee)
			{
				if (!bObservedFlightBeforeDamage || FlightFrames.Num() != 3 || BulletPositions.Num() != ExpectedShots * ExpectedPellets || MovingBulletIds.Num() != BulletPositions.Num())
				{
					return Fail(TEXT("did not observe all spawned bullets moving before impact"));
				}
				if (Pawn->GetLiveWeaponProjectileCount() != 0) { return Fail(TEXT("bullets survived after impacts")); }
			}
			Test.TestTrue(TEXT("this shot reduced the same real wave enemy's health"), HealthAfter < HealthBefore);
			return Save(true, FString());
		}
		return Fail(TEXT("unknown acceptance stage"));
	}

private:
	void Next()
	{
		++Stage; StageStart = FPlatformTime::Seconds();
		UWorld* World = GEngine && GEngine->GameViewport ? GEngine->GameViewport->GetWorld() : nullptr;
		StageGameStart = World ? World->GetTimeSeconds() : 0.0;
	}
	void Key(const FKey& Input, EInputEvent Event)
	{
		if (GEngine && GEngine->GameViewport)
		{
			UWorld* World = GEngine->GameViewport->GetWorld();
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			ULocalPlayer* LocalPlayer = PC ? PC->GetLocalPlayer() : nullptr;
			if (!LocalPlayer) { Test.AddError(TEXT("no local player for viewport input")); return; }
			const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetPrimaryInputDeviceForUser(LocalPlayer->GetPlatformUserId());
			if (!Device.IsValid() || GEngine->GetLocalPlayerFromInputDevice(GEngine->GameViewport, Device) != LocalPlayer)
			{
				Test.AddError(TEXT("no mapped input device for the active player"));
				return;
			}
			const bool bHandled = GEngine->GameViewport->InputKey(FInputKeyEventArgs::CreateSimulated(Input, Event,
				Event == IE_Released ? 0.0f : 1.0f, -1, Device, false, GEngine->GameViewport->Viewport));
			// Inventory intentionally captures Q. Gameplay presses must route.
			if (!bHandled && Event == IE_Pressed && Stage != 2) { Test.AddError(TEXT("gameplay viewport key was not handled")); }
		}
	}
	bool Fail(const FString& Error)
	{
		Test.AddError(FString::Printf(TEXT("M5-060 %s: %s"), *Weapon, *Error));
		for (const FKey& Input : {EKeys::Right, EKeys::I, EKeys::Q, EKeys::X, EKeys::T}) { Key(Input, IE_Released); }
		return Save(false, Error);
	}
	bool Save(bool bSuccess, const FString& Error)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("success"), bSuccess);
		Json->SetStringField(TEXT("weapon"), Weapon);
		Json->SetStringField(TEXT("config_revision"), ConfigRevision);
		Json->SetStringField(TEXT("error"), Error);
		Json->SetNumberField(TEXT("stage"), Stage);
		Json->SetBoolField(TEXT("menu_travel_completed"), bTravelled);
		Json->SetStringField(TEXT("input_path"), TEXT("UMG button callbacks + viewport I/Right/X/Q/T key events; no physical keyboard claim"));
		Json->SetNumberField(TEXT("run_id"), static_cast<double>(RunId));
		Json->SetNumberField(TEXT("health_before"), HealthBefore);
		Json->SetNumberField(TEXT("health_after"), HealthAfter);
		Json->SetNumberField(TEXT("shots_committed"), ShotsAfter - ShotsBefore);
		Json->SetNumberField(TEXT("loaded_before"), MagazineBefore);
		Json->SetNumberField(TEXT("loaded_after_fire"), MagazineAfter);
		Json->SetNumberField(TEXT("reserve_before_reload"), ReserveBeforeReload);
		Json->SetStringField(TEXT("reload_result"), ReloadResult);
		Json->SetNumberField(TEXT("projectile_entities_observed"), BulletPositions.Num());
		Json->SetNumberField(TEXT("projectile_speed_cm_s"), BulletSpeed);
		Json->SetBoolField(TEXT("flight_observed_before_damage"), bObservedFlightBeforeDamage);
		Json->SetNumberField(TEXT("moving_projectile_entities"), MovingBulletIds.Num());
		Json->SetStringField(TEXT("captured_projectile_id"), CaptureBulletId.ToString());
		TArray<TSharedPtr<FJsonValue>> Frames;
		for (int32 Index = 0; Index < FlightFrames.Num(); ++Index)
		{
			auto Frame = MakeShared<FJsonObject>();
			Frame->SetNumberField(TEXT("time"), FlightFrameTimes[Index]);
			Frame->SetNumberField(TEXT("x"), FlightFrames[Index].X);
			Frame->SetNumberField(TEXT("y"), FlightFrames[Index].Y);
			Frame->SetNumberField(TEXT("z"), FlightFrames[Index].Z);
			Frames.Add(MakeShared<FJsonValueObject>(Frame));
		}
		Json->SetArrayField(TEXT("same_projectile_flight_frames"), Frames);
		TArray<TSharedPtr<FJsonValue>> SpeedSamples;
		for (const double Speed : MeasuredLinearSpeeds) { SpeedSamples.Add(MakeShared<FJsonValueNumber>(Speed)); }
		Json->SetArrayField(TEXT("measured_linear_speed_cm_s"), SpeedSamples);
		FString Serialized;
		FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Serialized));
		if (!FFileHelper::SaveStringToFile(Serialized, *(Output / TEXT("gameplay.json")))) { Test.AddError(TEXT("cannot save current gameplay evidence")); }
		Test.AddInfo(Serialized);
		return true;
	}
	FAutomationTestBase& Test;
	FString Weapon, Output, ConfigRevision, ReloadResult;
	int32 Stage = 0;
	double StageStart = 0;
	double StageGameStart = 0;
	TWeakObjectPtr<APrototypeCharacter> OldPawn;
	TWeakObjectPtr<AMeleeEnemy> Target;
	bool bTravelled = false, bMelee = false;
	uint64 RunId = 0;
	float HealthBefore = -1, HealthAfter = -1;
	int32 MenuShotCount = 0, ShotsBefore = 0, ShotsAfter = 0, ExpectedShots = 0;
	int32 MagazineBefore = 0, MagazineAfter = 0, MagazineSize = 0, ReserveBeforeReload = 0;
	FName AmmoId;
	FGuid FiredWeaponInstance;
	TMap<FGuid, FVector> BulletPositions;
	TMap<FGuid, double> BulletTimes;
	TSet<FGuid> MovingBulletIds;
	TArray<double> MeasuredLinearSpeeds;
	int32 ExpectedPellets = 0;
	float BulletSpeed = 0;
	FGuid CaptureBulletId;
	TArray<FVector> FlightFrames;
	TArray<double> FlightFrameTimes;
	double NextFlightFrameTime = 0;
	bool bObservedFlightBeforeDamage = false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM5_060RoomWeaponGameplay,
	"UEMMO.Acceptance.M5_060.RoomWeaponGameplay",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FM5_060RoomWeaponGameplay::RunTest(const FString& Parameters)
{
	FString Weapon, Output;
	if (!FParse::Value(FCommandLine::Get(), TEXT("UEMMOAcceptanceWeapon="), Weapon)
		|| !FParse::Value(FCommandLine::Get(), TEXT("UEMMOAcceptanceOutput="), Output))
	{
		AddError(TEXT("explicit UEMMOAcceptanceWeapon and fresh UEMMOAcceptanceOutput are required"));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FRoomWeaponAcceptance(*this, Weapon, Output));
	return true;
}
#endif
