// M5-035: the multi-type weapon production scenario. The suites run the
// WHOLE production chain in a real ticked world - catalog mount -> real item
// bind -> input-authorized fire -> the delivery bridge -> the M5-027 spawn
// transaction -> the M5-028..032 motion policies (registered into the
// production dispatch this card) -> the unified hit entry - and never write
// health directly (the pool drop is the proof). One suite per weapon family
// (melee, hitscan, straight, parabolic, homing, explosion, pierce) plus the
// burst/reload, death/retry and config-change production scenarios; every
// suite exports its observed event sequence as JSON evidence under
// Artifacts/Tasks/M5-035/scenario-json (the M5-018 export precedent). The
// rendered capture companion stages the same production chain in the real
// game world and takes screenshots; headless runs skip silently.

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "../PrototypeCharacter.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/HealthComponent.h"
#include "../Items/ItemDefinition.h"
#include "../Projectiles/CombatProjectile.h"
#include "../PrototypeHUD.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_035
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M5_035_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention). X horizontal, Y
	// depth, Z height; the floor top sits exactly at the base Z.
	const FVector M5_035_SceneBase(180000.0, 52000.0, 600.0);

	const float M5_035_FloorHalfThickness = 100.0f;
	const float M5_035_FloorHalfExtentXY = 6000.0f;
	const float M5_035_PlayerSpawnHeight = 120.0f;

	// The standard target stands 400 cm ahead of the pawn (facing +X); the
	// shot origin rides the feet offset (30, 0, 50) and the shot flies
	// straight +X, so a box centered at the same height catches every pellet.
	const FVector M5_035_TargetOffset(400.0, 0.0, 50.0);
	const FVector M5_035_TargetBoxHalfExtent(50.0, 50.0, 60.0);
	// The deep target (the homing curve) and the far target (the parabolic
	// arc that drops short) variants.
	const FVector M5_035_DeepTargetOffset(400.0, 300.0, 50.0);
	const FVector M5_035_FarTargetOffset(1200.0, 0.0, 50.0);
	const FVector M5_035_SecondTargetOffset(700.0, 0.0, 50.0);

	// The world-static blocking wall between the pawn and the target lane:
	// a wide box the shot line always meets before the target.
	const FVector M5_035_WallCenter(200.0, 0.0, 100.0);
	const FVector M5_035_WallHalfExtent(20.0, 3000.0, 250.0);

	// Weapon fixture values: a straight 3000 cm/s linear projectile, the
	// slowed config-change round, and a 0.5 s flight life (1500 cm) so whiffs
	// expire inside the tick budget. The hitscan range bounds the ray.
	constexpr float M5_035_SpeedCmS = 3000.0f;
	constexpr float M5_035_SlowSpeedCmS = 750.0f;
	constexpr float M5_035_ProjectileLifetimeS = 0.5f;
	constexpr float M5_035_HitscanRangeCm = 5000.0f;
	constexpr float M5_035_ExplosionRadiusCm = 250.0f;
	constexpr float M5_035_HomingTurnRateDegS = 360.0f;
	// The burst fixture: 450 rpm = 0.1333 s between burst sub-shots, 3-round
	// burst, 15-round magazine; the kill loop uses 600 rpm = 6 frames.
	constexpr float M5_035_BurstRpm = 450.0f;
	constexpr float M5_035_KillRpm = 600.0f;
	constexpr double M5_035_ReloadDurationSeconds = 2.0;

	// Damage profiles: the 10-damage standard round, the 25-damage adjusted
	// round (the config-change measurement) and the 30-damage blast.
	constexpr float M5_035_Damage = 10.0f;
	constexpr float M5_035_AdjustedDamage = 25.0f;
	constexpr float M5_035_BlastDamage = 30.0f;

	/** World-static blocking floor box the pawn stands on (the M5-033 pattern). */
	static AActor* M5_035_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M5_035_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_035_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M5_035_FloorHalfExtentXY, M5_035_FloorHalfExtentXY, M5_035_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M5_035_FloorHalfThickness));
		return Actor;
	}

	/** A world-static blocking wall box (the unregistered environment). */
	static AActor* M5_035_SpawnWall(UWorld& World)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), M5_035_SceneBase + M5_035_WallCenter, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_035_Wall"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_035_WallHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(M5_035_SceneBase + M5_035_WallCenter);
		return Actor;
	}

	/**
	 * A hostile scenario target: a world-static box plus a real health pool
	 * (the unified entry ignores bodies without one). Heavy targets carry a
	 * bigger pool (300) so one standard round never kills them - the pool
	 * drop measures the damage, the survival keeps the scenario repeatable.
	 */
	static AActor* M5_035_SpawnTarget(UWorld& World, const FVector& Base, const FVector& Offset,
		float MaxHealth, UHealthComponent** OutHealth)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base + Offset, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_035_TargetBody"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_035_TargetBoxHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(Base + Offset);
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_035_TargetHealth"));
		Health->RegisterComponent();
		Health->SetMaxHealth(MaxHealth);
		Health->ResetHealth();
		if (OutHealth != nullptr)
		{
			*OutHealth = Health;
		}
		return Actor;
	}

	/** The configured control a shot carries (from its resolved profile). */
	static FString M5_035_ControlText(const FDamageProfile& Profile)
	{
		return FString::Printf(TEXT("stun=%.2fs kb=%.0fcm/s launch=%.0fcm/s hitstop=%.2fs"),
			Profile.HitStunSeconds, Profile.KnockbackCmPerSecond, Profile.LaunchCmPerSecond, Profile.HitStopSeconds);
	}

	/** One recorded weapon-chain event (the JSON evidence row). */
	struct FM5_035_WeaponEvent
	{
		FString Kind;
		FString WeaponId;
		FString ProjectileId;
		int32 MagazineRounds = -1;
		int32 ReserveRounds = -1;
		FString TargetId;
		float Health = -1.0f;
		float Damage = 0.0f;
		FString Control;
		FString Failure;
		FString ConfigRevision;
		double AtSeconds = 0.0;

		FString ToJson() const
		{
			auto Escape = [](const FString& In)
			{
				return In.Replace(TEXT("\\"), TEXT("/")).Replace(TEXT("\""), TEXT("'"));
			};
			return FString::Printf(
				TEXT("{\"kind\":\"%s\",\"weapon\":\"%s\",\"projectile\":\"%s\",\"magazine\":%d,\"reserve\":%d,")
				TEXT("\"target\":\"%s\",\"health\":%.1f,\"damage\":%.1f,\"control\":\"%s\",\"failure\":\"%s\",")
				TEXT("\"config_revision\":\"%s\",\"at\":%.3f}"),
				*Escape(Kind), *Escape(WeaponId), *Escape(ProjectileId), MagazineRounds, ReserveRounds,
				*Escape(TargetId), Health, Damage, *Escape(Control), *Escape(Failure),
				*Escape(ConfigRevision), AtSeconds);
		}
	};

	// One full rig: the wrapper owns the manually ticked temp world, the rig
	// owns the catalog VALUES mounted into the real pawn's weapon component,
	// and the shared test clock feeds both the pawn's input layer and the
	// mount's fire clock (deterministic pacing). Each case builds a fresh rig
	// so no state bleeds between the per-family scenarios.
	struct FM5_035_Rig
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;
		AActor* ExtraTarget = nullptr;
		UHealthComponent* ExtraTargetHealth = nullptr;
		FCombatCatalog CatalogValue;
		FItemDefinitionCatalog ItemsValue;
		FGuid BoundInstanceId;
		double ClockSeconds = 200.0;
		FName WeaponIdValue;
		FName ProjectileIdValue;
		FName AmmoIdValue;
		FString ControlText;
		TArray<FM5_035_WeaponEvent> Events;

		bool Build(FAutomationTestBase& Test, bool bWithTarget, bool bWithWall = false)
		{
			if (!Test.TestTrue(TEXT("the manually ticked test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			if (M5_035_SpawnFloor(*World, M5_035_SceneBase) == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			if (bWithWall && M5_035_SpawnWall(*World) == nullptr)
			{
				Test.AddError(TEXT("the wall failed to spawn"));
				return false;
			}
			World->EnsureCollisionTreeIsBuilt();
			FActorSpawnParameters PlayerParams;
			Player = World->SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(),
				M5_035_SceneBase + FVector(0.0, 0.0, M5_035_PlayerSpawnHeight), FRotator::ZeroRotator, PlayerParams);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
			{
				Movement->bRunPhysicsWithNoController = true;
				if (!Movement->IsActive())
				{
					Movement->Activate(/*bReset*/ true);
				}
				if (Movement->MovementMode == MOVE_None)
				{
					Movement->SetDefaultMovementMode();
				}
			}
			if (bWithTarget)
			{
				Target = M5_035_SpawnTarget(*World, M5_035_SceneBase, M5_035_TargetOffset, 100.0f, &TargetHealth);
				if (!Test.TestNotNull(TEXT("the target actor spawns"), Target))
				{
					return false;
				}
			}
			// The shared rig clock feeds the pawn's input layer AND the mount's
			// fire clock (the M5-022 injection surface), so pacing is exact.
			Player->SetWeaponInputClockProvider([this]() { return ClockSeconds; });
			Player->GetWeaponMount()->SetFireClockProvider([this]() { return ClockSeconds; });
			return true;
		}

		/** Ticks the world until the pawn settled onto the floor (the M3-031 pattern). */
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
			for (int32 Frame = 0; Frame < 300; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks (settle)"), Wrapper.TickTestWorld(M5_035_FrameSeconds)))
				{
					return false;
				}
				ClockSeconds += M5_035_FrameSeconds;
				if (Movement != nullptr && Movement->MovementMode == MOVE_Walking && Movement->Velocity.Size() < 1.0f)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the pawn never settled onto the floor"));
			return false;
		}

		/**
		 * Builds a production-shaped catalog (the standard damage profile plus
		 * any extras the family needs, the ammo, every referenced projectile
		 * and the weapon), mounts it into the pawn's real weapon component,
		 * pre-seeds the shared reserve and binds a real item instance through
		 * the production bind entry. The pawn's input-layer wiring applies
		 * exactly as the production refresh tail applies it.
		 */
		bool BindWeapon(FAutomationTestBase& Test, const FWeaponDefinition& WeaponDef,
			const TArray<FProjectileDefinition>& ProjectileDefs,
			const TArray<FDamageProfile>& ExtraProfiles)
		{
			FParsedCombatConfig Parsed;
			Parsed.SchemaVersion = 1;
			FDamageProfile Profile;
			Profile.DamageProfileId = WeaponDef.DamageProfileId;
			Profile.BaseDamage = M5_035_Damage;
			Parsed.DamageProfiles.Add(Profile);
			for (const FDamageProfile& Extra : ExtraProfiles)
			{
				Parsed.DamageProfiles.Add(Extra);
			}
			if (!WeaponDef.AmmoId.IsNone())
			{
				FAmmoType Ammo;
				Ammo.AmmoId = WeaponDef.AmmoId;
				Ammo.MaxReserve = 120;
				Ammo.MagazineSize = WeaponDef.MagazineSize;
				Parsed.AmmoTypes.Add(Ammo);
			}
			for (const FProjectileDefinition& ProjectileDef : ProjectileDefs)
			{
				Parsed.Projectiles.Add(ProjectileDef);
			}
			FWeaponDefinition Weapon = WeaponDef;
			Parsed.Weapons.Add(Weapon);
			FString BuildErrors;
			if (!Test.TestTrue(TEXT("the rig catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, CatalogValue, BuildErrors)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog build refused: %s"), *BuildErrors));
				return false;
			}
			FItemDefinition Item;
			Item.DefinitionId = FName(TEXT("item_rig_scenario"));
			Item.DisplayName = TEXT("Rig Scenario Weapon");
			Item.Slot = EItemSlot::Weapon;
			Item.Rarity = EItemRarity::Normal;
			Item.WeaponDefinitionId = WeaponDef.WeaponId;
			FString ItemError;
			if (!Test.TestTrue(TEXT("the rig item definition registers"), ItemsValue.AddDefinition(Item, &ItemError)))
			{
				Test.AddError(FString::Printf(TEXT("the rig item definition refused: %s"), *ItemError));
				return false;
			}
			UWeaponComponent* Mount = Player->GetWeaponMount();
			FString MountError;
			if (!Test.TestTrue(TEXT("the rig catalogs mount into the pawn's component"),
				Mount->MountCatalogs(&CatalogValue, &ItemsValue, &MountError)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog mount refused: %s"), *MountError));
				return false;
			}
			if (!WeaponDef.AmmoId.IsNone())
			{
				// The reserve is pre-seeded so the bind's own zero-reserve
				// registration is a refused duplicate (the M5-033 fixture
				// pattern): the shared pool keeps its 60 rounds.
				if (!Test.TestTrue(TEXT("the rig reserve ammo registers"),
					Mount->GetAmmoModel().RegisterAmmoType(WeaponDef.AmmoId, 120, 60)))
				{
					return false;
				}
			}
			FItemInstance Instance;
			Instance.InstanceId = FGuid(0x0350A000u, 0x0BECu, 0x5335u, 1u);
			Instance.DefinitionId = Item.DefinitionId;
			Instance.RollSeed = 0x52035;
			Instance.Level = 1;
			const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
			if (!Test.TestTrue(TEXT("the rig weapon binds"), Outcome.bSucceeded && Outcome.bWeaponBound))
			{
				Test.AddError(FString::Printf(TEXT("the rig bind refused (%d): %s"),
					static_cast<int32>(Outcome.Reject), *Outcome.RejectDetail));
				return false;
			}
			BoundInstanceId = Instance.InstanceId;
			// The pawn's input-layer wiring (policy selection, gates, the
			// delivery bridge) applies exactly as the production refresh tail
			// applies it.
			Player->ApplyWeaponBindWiring();
			// The shot-control record comes from the resolved profile.
			if (const FDamageProfile* Resolved = CatalogValue.FindDamageProfile(WeaponDef.DamageProfileId))
			{
				ControlText = M5_035_ControlText(*Resolved);
			}
			WeaponIdValue = WeaponDef.WeaponId;
			ProjectileIdValue = WeaponDef.ProjectileId;
			AmmoIdValue = WeaponDef.AmmoId;
			return Test.TestTrue(TEXT("the bound mount authorizes fire"), Mount->IsFireAuthorized());
		}

		// Advances the world and the shared rig clock one frame at a time.
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Wrapper.TickTestWorld(M5_035_FrameSeconds))
				{
					break;
				}
				ClockSeconds += M5_035_FrameSeconds;
			}
		}

		int32 CommittedShots() const
		{
			return Player != nullptr && Player->GetWeaponMount() != nullptr
				? Player->GetWeaponMount()->GetCommittedShotCount()
				: 0;
		}

		int32 MagazineRounds() const
		{
			const UWeaponComponent* Mount = Player != nullptr ? Player->GetWeaponMount() : nullptr;
			const FWeaponBindingRecord* Binding = Mount != nullptr ? Mount->GetActiveBinding() : nullptr;
			return Binding != nullptr ? Binding->LoadedRounds : -1;
		}

		int32 ReserveRounds() const
		{
			const UWeaponComponent* Mount = Player != nullptr ? Player->GetWeaponMount() : nullptr;
			return Mount != nullptr && !AmmoIdValue.IsNone()
				? Mount->GetAmmoModel().GetReserveRounds(AmmoIdValue)
				: -1;
		}

		/** Records one observed event row (the JSON evidence). */
		void Record(const FString& Kind, const FString& TargetName, float Health, float Damage, const FString& Failure)
		{
			FM5_035_WeaponEvent Event;
			Event.Kind = Kind;
			Event.WeaponId = WeaponIdValue.ToString();
			Event.ProjectileId = ProjectileIdValue.ToString();
			Event.MagazineRounds = MagazineRounds();
			Event.ReserveRounds = ReserveRounds();
			Event.TargetId = TargetName;
			Event.Health = Health;
			Event.Damage = Damage;
			Event.Control = ControlText;
			Event.Failure = Failure;
			Event.ConfigRevision = CatalogValue.GetConfigRevision();
			Event.AtSeconds = ClockSeconds;
			Events.Add(Event);
		}

		/** Exports the recorded event sequence as the scenario JSON evidence. */
		bool ExportJson(FAutomationTestBase& Test, const FString& FileName)
		{
			const FString Directory = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-035") / TEXT("scenario-json"));
			IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
			FString Payload = TEXT("{\n  \"section\": \"Weapons\",\n  \"events\": [\n");
			for (int32 Index = 0; Index < Events.Num(); ++Index)
			{
				Payload += TEXT("    ") + Events[Index].ToJson()
					+ (Index + 1 < Events.Num() ? TEXT(",") : TEXT(""));
				Payload += TEXT("\n");
			}
			Payload += TEXT("  ]\n}\n");
			const FString Absolute = Directory / FileName;
			if (!FFileHelper::SaveStringToFile(Payload, *Absolute))
			{
				Test.AddError(FString::Printf(TEXT("the scenario JSON could not be written: %s"), *Absolute));
				return false;
			}
			return true;
		}
	};

	// The scenario weapon families (production-shaped definitions).
	static FWeaponDefinition M5_035_MakeMeleeDefinition()
	{
		FWeaponDefinition Weapon;
		Weapon.WeaponId = FName(TEXT("weapon_rig_melee"));
		Weapon.FireMode = EWeaponFireMode::Melee;
		Weapon.DamageProfileId = FName(TEXT("dmg_rig_melee"));
		Weapon.AmmoId = FName(NAME_None);
		Weapon.MagazineSize = 0;
		Weapon.FireRateRpm = 0.0f;
		Weapon.BurstCount = 1;
		Weapon.PelletCount = 1;
		Weapon.SpreadDegrees = 0.0f;
		Weapon.RangeCm = 0.0f;
		return Weapon;
	}

	static FWeaponDefinition M5_035_MakeHitscanDefinition()
	{
		FWeaponDefinition Weapon;
		Weapon.WeaponId = FName(TEXT("weapon_rig_hitscan"));
		Weapon.FireMode = EWeaponFireMode::Hitscan;
		Weapon.DamageProfileId = FName(TEXT("dmg_rig_round"));
		Weapon.AmmoId = FName(TEXT("ammo_rig_cell"));
		Weapon.MagazineSize = 12;
		Weapon.FireRateRpm = 120.0f;
		Weapon.BurstCount = 1;
		Weapon.PelletCount = 1;
		Weapon.SpreadDegrees = 0.0f;
		Weapon.RangeCm = M5_035_HitscanRangeCm;
		return Weapon;
	}

	static FProjectileDefinition M5_035_MakeProjectileDefinition(FName ProjectileId, EProjectileMotion Motion)
	{
		FProjectileDefinition Projectile;
		Projectile.ProjectileId = ProjectileId;
		Projectile.Motion = Motion;
		Projectile.SpeedCmS = M5_035_SpeedCmS;
		Projectile.LifetimeS = M5_035_ProjectileLifetimeS;
		Projectile.DamageProfileId = FName(TEXT("dmg_rig_round"));
		Projectile.PierceCount = 0;
		return Projectile;
	}

	static FWeaponDefinition M5_035_MakeProjectileWeaponDefinition(FName WeaponId, FName ProjectileId, float Rpm, int32 BurstCount)
	{
		FWeaponDefinition Weapon;
		Weapon.WeaponId = WeaponId;
		Weapon.FireMode = EWeaponFireMode::Projectile;
		Weapon.DamageProfileId = FName(TEXT("dmg_rig_round"));
		Weapon.AmmoId = FName(TEXT("ammo_rig_cell"));
		Weapon.MagazineSize = 15;
		Weapon.FireRateRpm = Rpm;
		Weapon.BurstCount = BurstCount;
		Weapon.PelletCount = 1;
		Weapon.SpreadDegrees = 0.0f;
		Weapon.RangeCm = 0.0f;
		Weapon.ProjectileId = ProjectileId;
		return Weapon;
	}
}

using namespace UE::UEMMO::Tasks::M5_035;

// ---------------------------------------------------------------------------
// MeleeBindKeepsLegacySkillIntent
// ---------------------------------------------------------------------------

// A bound melee weapon never authorizes the weapon-fire path (the legacy
// attack chain owns melee delivery): the Q press lands as the M1-040
// skill-slot 1 intent, the mount commits nothing and no ammo bookkeeping
// appears. Recorded as the melee family's explicit chain refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035MeleeBindKeepsLegacySkillIntent,
	"UEMMO.Tasks.M5_035.MeleeBindKeepsLegacySkillIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035MeleeBindKeepsLegacySkillIntent::RunTest(const FString& Parameters)
{
	FM5_035_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_035_MakeMeleeDefinition(), {}, {}))
	{
		return true;
	}

	TestFalse(TEXT("the melee bind never authorizes weapon fire"), Rig.Player->GetWeaponMount()->IsFireAuthorized());
	const int32 PressesBefore = Rig.Player->GetSkillSlotPressCount(1);

	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);

	TestEqual(TEXT("the Q press lands as the skill-slot 1 intent"),
		Rig.Player->GetSkillSlotPressCount(1), PressesBefore + 1);
	TestEqual(TEXT("the melee bind commits no shot"), Rig.CommittedShots(), 0);
	TestEqual(TEXT("the melee target's pool is untouched"),
		Rig.TargetHealth->GetHealth(), 100.0f);
	Rig.Record(TEXT("melee_press"), TEXT("dummy_normal"), Rig.TargetHealth->GetHealth(), 0.0f,
		TEXT("melee_fire_not_authorized"));
	Rig.ExportJson(*this, TEXT("weapons-melee.json"));
	return true;
}

// ---------------------------------------------------------------------------
// HitscanShotHitsWhiffsAndWallBlocks
// ---------------------------------------------------------------------------

// The hitscan family through the real chain: the positive shot damages the
// normal target through the unified entry, the whiff (an empty lane) commits
// the shot but delivers nothing, and the wall stops the ray before the
// target - the pool stays untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035HitscanShotHitsWhiffsAndWallBlocks,
	"UEMMO.Tasks.M5_035.HitscanShotHitsWhiffsAndWallBlocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035HitscanShotHitsWhiffsAndWallBlocks::RunTest(const FString& Parameters)
{
	// Positive: the ray reaches the target and applies the 10-damage profile.
	FM5_035_Rig HitRig;
	if (!HitRig.Build(*this, /*bWithTarget*/ true) || !HitRig.Settle(*this))
	{
		return true;
	}
	if (!HitRig.BindWeapon(*this, M5_035_MakeHitscanDefinition(), {}, {}))
	{
		return true;
	}
	const float HitBefore = HitRig.TargetHealth->GetHealth();
	HitRig.Player->OnFireInputPressed();
	HitRig.Player->OnFireInputReleased();
	HitRig.TickFrames(2);
	TestEqual(TEXT("the hitscan shot commits exactly one shot"), HitRig.CommittedShots(), 1);
	TestEqual(TEXT("the hitscan hit removes exactly the configured 10"), HitRig.TargetHealth->GetHealth(), HitBefore - M5_035_Damage);
	HitRig.Record(TEXT("hit"), TEXT("dummy_normal"), HitRig.TargetHealth->GetHealth(), M5_035_Damage, TEXT(""));
	HitRig.ExportJson(*this, TEXT("weapons-hitscan.json"));

	// Whiff: the same shot into an empty lane commits but delivers nothing.
	FM5_035_Rig WhiffRig;
	if (!WhiffRig.Build(*this, /*bWithTarget*/ false) || !WhiffRig.Settle(*this))
	{
		return true;
	}
	if (!WhiffRig.BindWeapon(*this, M5_035_MakeHitscanDefinition(), {}, {}))
	{
		return true;
	}
	WhiffRig.Player->OnFireInputPressed();
	WhiffRig.Player->OnFireInputReleased();
	WhiffRig.TickFrames(2);
	TestEqual(TEXT("the whiff shot still commits (the trigger is honest)"), WhiffRig.CommittedShots(), 1);
	WhiffRig.Record(TEXT("whiff"), TEXT(""), -1.0f, 0.0f, TEXT("lane_empty_no_target"));
	WhiffRig.ExportJson(*this, TEXT("weapons-hitscan.json"));

	// Wall: the ray stops at the unregistered environment before the target.
	FM5_035_Rig WallRig;
	if (!WallRig.Build(*this, /*bWithTarget*/ true, /*bWithWall*/ true) || !WallRig.Settle(*this))
	{
		return true;
	}
	if (!WallRig.BindWeapon(*this, M5_035_MakeHitscanDefinition(), {}, {}))
	{
		return true;
	}
	const float WallBefore = WallRig.TargetHealth->GetHealth();
	WallRig.Player->OnFireInputPressed();
	WallRig.Player->OnFireInputReleased();
	WallRig.TickFrames(2);
	TestEqual(TEXT("the wall-blocked shot commits"), WallRig.CommittedShots(), 1);
	TestEqual(TEXT("the wall keeps the target's pool untouched"), WallRig.TargetHealth->GetHealth(), WallBefore);
	WallRig.Record(TEXT("wall_block"), TEXT("dummy_normal"), WallRig.TargetHealth->GetHealth(), 0.0f,
		TEXT("wall_blocked_line"));
	WallRig.ExportJson(*this, TEXT("weapons-hitscan.json"));
	return true;
}

// ---------------------------------------------------------------------------
// StraightProjectileHitWhiffAndWallBlock
// ---------------------------------------------------------------------------

// The straight projectile family: the pellet reaches the 400 cm target inside
// its flight life and wounds it through the unified entry; the whiff flies
// the full lifetime out and expires (the live population returns to
// baseline); the wall blocks the pellet before the target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035StraightProjectileHitWhiffAndWallBlock,
	"UEMMO.Tasks.M5_035.StraightProjectileHitWhiffAndWallBlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035StraightProjectileHitWhiffAndWallBlock::RunTest(const FString& Parameters)
{
	const FProjectileDefinition Straight = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_straight")), EProjectileMotion::Straight);

	// Positive: the pellet lands the 10-damage round on the normal target.
	FM5_035_Rig HitRig;
	if (!HitRig.Build(*this, /*bWithTarget*/ true) || !HitRig.Settle(*this))
	{
		return true;
	}
	if (!HitRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_straight")), Straight.ProjectileId, 120.0f, 1),
		{Straight}, {}))
	{
		return true;
	}
	const float HitBefore = HitRig.TargetHealth->GetHealth();
	HitRig.Player->OnFireInputPressed();
	HitRig.Player->OnFireInputReleased();
	HitRig.TickFrames(12);
	TestEqual(TEXT("the straight shot commits exactly one shot"), HitRig.CommittedShots(), 1);
	TestEqual(TEXT("the straight pellet removes exactly the configured 10"), HitRig.TargetHealth->GetHealth(), HitBefore - M5_035_Damage);
	TestEqual(TEXT("the spent pellet left the live population"), HitRig.Player->GetLiveWeaponProjectileCount(), 0);
	HitRig.Record(TEXT("hit"), TEXT("dummy_normal"), HitRig.TargetHealth->GetHealth(), M5_035_Damage, TEXT(""));
	HitRig.ExportJson(*this, TEXT("weapons-straight.json"));

	// Whiff: the pellet flies the full lifetime out and expires harmlessly.
	FM5_035_Rig WhiffRig;
	if (!WhiffRig.Build(*this, /*bWithTarget*/ false) || !WhiffRig.Settle(*this))
	{
		return true;
	}
	if (!WhiffRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_straight")), Straight.ProjectileId, 120.0f, 1),
		{Straight}, {}))
	{
		return true;
	}
	WhiffRig.Player->OnFireInputPressed();
	WhiffRig.Player->OnFireInputReleased();
	WhiffRig.TickFrames(8);
	TestEqual(TEXT("the whiff pellet is live mid-flight"), WhiffRig.Player->GetLiveWeaponProjectileCount(), 1);
	WhiffRig.TickFrames(40);
	TestEqual(TEXT("the whiff pellet expired by its lifetime"), WhiffRig.Player->GetLiveWeaponProjectileCount(), 0);
	WhiffRig.Record(TEXT("whiff"), TEXT(""), -1.0f, 0.0f, TEXT("lane_empty_lifetime_expired"));
	WhiffRig.ExportJson(*this, TEXT("weapons-straight.json"));

	// Wall: the pellet stops at the unregistered wall before the target.
	FM5_035_Rig WallRig;
	if (!WallRig.Build(*this, /*bWithTarget*/ true, /*bWithWall*/ true) || !WallRig.Settle(*this))
	{
		return true;
	}
	if (!WallRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_straight")), Straight.ProjectileId, 120.0f, 1),
		{Straight}, {}))
	{
		return true;
	}
	const float WallBefore = WallRig.TargetHealth->GetHealth();
	WallRig.Player->OnFireInputPressed();
	WallRig.Player->OnFireInputReleased();
	WallRig.TickFrames(12);
	TestEqual(TEXT("the wall-blocked shot commits"), WallRig.CommittedShots(), 1);
	TestEqual(TEXT("the wall keeps the target's pool untouched"), WallRig.TargetHealth->GetHealth(), WallBefore);
	TestEqual(TEXT("the blocked pellet left the live population"), WallRig.Player->GetLiveWeaponProjectileCount(), 0);
	WallRig.Record(TEXT("wall_block"), TEXT("dummy_normal"), WallRig.TargetHealth->GetHealth(), 0.0f,
		TEXT("wall_blocked_line"));
	WallRig.ExportJson(*this, TEXT("weapons-straight.json"));
	return true;
}

// ---------------------------------------------------------------------------
// ParabolicArcHitsNearAndMissesFarWithWall
// ---------------------------------------------------------------------------

// The parabolic family rides the M5-029 ballistic policy through the
// production chain: gravity drops the pellet - the near target still catches
// it (the arc is shallow at 400 cm), the far 1200 cm target is missed (the
// pellet drops below the box and meets the floor), and the wall blocks the
// short arc before anything else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035ParabolicArcHitsNearAndMissesFarWithWall,
	"UEMMO.Tasks.M5_035.ParabolicArcHitsNearAndMissesFarWithWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035ParabolicArcHitsNearAndMissesFarWithWall::RunTest(const FString& Parameters)
{
	FProjectileDefinition Parabolic = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_parabolic")), EProjectileMotion::Parabolic);

	// Positive: the near target catches the shallow arc.
	FM5_035_Rig HitRig;
	if (!HitRig.Build(*this, /*bWithTarget*/ true) || !HitRig.Settle(*this))
	{
		return true;
	}
	if (!HitRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_parabolic")), Parabolic.ProjectileId, 120.0f, 1),
		{Parabolic}, {}))
	{
		return true;
	}
	const float HitBefore = HitRig.TargetHealth->GetHealth();
	HitRig.Player->OnFireInputPressed();
	HitRig.Player->OnFireInputReleased();
	HitRig.TickFrames(12);
	TestEqual(TEXT("the parabolic shot commits exactly one shot"), HitRig.CommittedShots(), 1);
	TestEqual(TEXT("the shallow arc still wounds the near target"), HitRig.TargetHealth->GetHealth(), HitBefore - M5_035_Damage);
	HitRig.Record(TEXT("hit"), TEXT("dummy_normal"), HitRig.TargetHealth->GetHealth(), M5_035_Damage, TEXT(""));
	HitRig.ExportJson(*this, TEXT("weapons-parabolic.json"));

	// Far miss: gravity drops the pellet below the 1200 cm target's box.
	FM5_035_Rig FarRig;
	if (!FarRig.Build(*this, /*bWithTarget*/ false) || !FarRig.Settle(*this))
	{
		return true;
	}
	UHealthComponent* FarHealth = nullptr;
	AActor* FarTarget = nullptr;
	{
		FActorSpawnParameters Params;
		FarTarget = FarRig.World->SpawnActor<AActor>(
			AActor::StaticClass(), M5_035_SceneBase + M5_035_FarTargetOffset, FRotator::ZeroRotator, Params);
		if (!TestNotNull(TEXT("the far target spawns"), FarTarget))
		{
			return true;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(FarTarget, TEXT("M5_035_FarBody"));
		FarTarget->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_035_TargetBoxHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(M5_035_SceneBase + M5_035_FarTargetOffset);
		FarHealth = NewObject<UHealthComponent>(FarTarget, TEXT("M5_035_FarHealth"));
		FarHealth->RegisterComponent();
		FarHealth->SetMaxHealth(100.0f);
		FarHealth->ResetHealth();
	}
	if (!FarRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_parabolic")), Parabolic.ProjectileId, 120.0f, 1),
		{Parabolic}, {}))
	{
		return true;
	}
	FarRig.World->EnsureCollisionTreeIsBuilt();
	FarRig.Player->OnFireInputPressed();
	FarRig.Player->OnFireInputReleased();
	FarRig.TickFrames(30);
	TestEqual(TEXT("the far target takes nothing (the arc dropped short)"), FarHealth->GetHealth(), 100.0f);
	TestEqual(TEXT("the dropped pellet left the live population"), FarRig.Player->GetLiveWeaponProjectileCount(), 0);
	FarRig.Record(TEXT("whiff"), TEXT("dummy_far"), FarHealth->GetHealth(), 0.0f,
		TEXT("gravity_dropped_short_of_far_target"));
	FarRig.ExportJson(*this, TEXT("weapons-parabolic.json"));

	// Wall: the arc meets the unregistered wall before the target.
	FM5_035_Rig WallRig;
	if (!WallRig.Build(*this, /*bWithTarget*/ true, /*bWithWall*/ true) || !WallRig.Settle(*this))
	{
		return true;
	}
	if (!WallRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_parabolic")), Parabolic.ProjectileId, 120.0f, 1),
		{Parabolic}, {}))
	{
		return true;
	}
	const float WallBefore = WallRig.TargetHealth->GetHealth();
	WallRig.Player->OnFireInputPressed();
	WallRig.Player->OnFireInputReleased();
	WallRig.TickFrames(12);
	TestEqual(TEXT("the wall keeps the near target's pool untouched"), WallRig.TargetHealth->GetHealth(), WallBefore);
	WallRig.Record(TEXT("wall_block"), TEXT("dummy_normal"), WallRig.TargetHealth->GetHealth(), 0.0f,
		TEXT("wall_blocked_line"));
	WallRig.ExportJson(*this, TEXT("weapons-parabolic.json"));
	return true;
}

// ---------------------------------------------------------------------------
// HomingTracksBoundTargetAndDegradesStraightUnbound
// ---------------------------------------------------------------------------

// The homing family rides the M5-030 policy through the production chain:
// with the target seam bound the pellet curves onto the 300 cm-deep target
// the straight line can never reach; without a bound target the shot
// honestly degrades to the straight reference motion and misses the same
// offset target; the wall stops even the guided pellet.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035HomingTracksBoundTargetAndDegradesStraightUnbound,
	"UEMMO.Tasks.M5_035.HomingTracksBoundTargetAndDegradesStraightUnbound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035HomingTracksBoundTargetAndDegradesStraightUnbound::RunTest(const FString& Parameters)
{
	FProjectileDefinition Homing = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_homing")), EProjectileMotion::Homing);
	Homing.HomingTurnRateDegS = M5_035_HomingTurnRateDegS;
	Homing.LifetimeS = 1.0f;

	// Positive: the bound seam curves the pellet onto the deep target.
	FM5_035_Rig HitRig;
	if (!HitRig.Build(*this, /*bWithTarget*/ false) || !HitRig.Settle(*this))
	{
		return true;
	}
	{
		FActorSpawnParameters Params;
		UHealthComponent* DeepHealth = nullptr;
		AActor* DeepTarget = M5_035_SpawnTarget(*HitRig.World, M5_035_SceneBase, M5_035_DeepTargetOffset, 100.0f, &DeepHealth);
		if (!TestNotNull(TEXT("the deep target spawns"), DeepTarget) || DeepHealth == nullptr)
		{
			return true;
		}
		HitRig.World->EnsureCollisionTreeIsBuilt();
		if (!HitRig.BindWeapon(*this,
			M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_homing")), Homing.ProjectileId, 120.0f, 1),
			{Homing}, {}))
		{
			return true;
		}
		// The production target seam: the provider hands the tracked actor to
		// the pawn (the registry id resolves through the lazy production path).
		AActor* Tracked = DeepTarget;
		HitRig.Player->SetWeaponHomingTargetProvider([Tracked]() -> AActor* { return Tracked; });
		HitRig.Player->OnFireInputPressed();
		HitRig.Player->OnFireInputReleased();
		HitRig.TickFrames(60);
		TestEqual(TEXT("the guided pellet wounds the deep target"), DeepHealth->GetHealth(), 100.0f - M5_035_Damage);
		HitRig.Record(TEXT("hit"), TEXT("dummy_deep"), DeepHealth->GetHealth(), M5_035_Damage, TEXT(""));
	}
	HitRig.ExportJson(*this, TEXT("weapons-homing.json"));

	// Downgrade: no provider -> the straight reference motion -> the deep
	// target is missed exactly like any straight pellet would miss it.
	FM5_035_Rig WhiffRig;
	if (!WhiffRig.Build(*this, /*bWithTarget*/ false) || !WhiffRig.Settle(*this))
	{
		return true;
	}
	{
		UHealthComponent* DeepHealth = nullptr;
		AActor* DeepTarget = M5_035_SpawnTarget(*WhiffRig.World, M5_035_SceneBase, M5_035_DeepTargetOffset, 100.0f, &DeepHealth);
		if (!TestNotNull(TEXT("the deep target spawns"), DeepTarget) || DeepHealth == nullptr)
		{
			return true;
		}
		WhiffRig.World->EnsureCollisionTreeIsBuilt();
		if (!WhiffRig.BindWeapon(*this,
			M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_homing")), Homing.ProjectileId, 120.0f, 1),
			{Homing}, {}))
		{
			return true;
		}
		WhiffRig.Player->OnFireInputPressed();
		WhiffRig.Player->OnFireInputReleased();
		WhiffRig.TickFrames(60);
		TestEqual(TEXT("the unbound homing shot degrades straight and misses the deep target"),
			DeepHealth->GetHealth(), 100.0f);
		WhiffRig.Record(TEXT("whiff"), TEXT("dummy_deep"), DeepHealth->GetHealth(), 0.0f,
			TEXT("homing_unbound_straight_degrade"));
	}
	WhiffRig.ExportJson(*this, TEXT("weapons-homing.json"));

	// Wall: the guided pellet still stops at the unregistered environment.
	FM5_035_Rig WallRig;
	if (!WallRig.Build(*this, /*bWithTarget*/ false, /*bWithWall*/ true) || !WallRig.Settle(*this))
	{
		return true;
	}
	{
		UHealthComponent* DeepHealth = nullptr;
		AActor* DeepTarget = M5_035_SpawnTarget(*WallRig.World, M5_035_SceneBase, M5_035_DeepTargetOffset, 100.0f, &DeepHealth);
		if (!TestNotNull(TEXT("the deep target spawns"), DeepTarget) || DeepHealth == nullptr)
		{
			return true;
		}
		WallRig.World->EnsureCollisionTreeIsBuilt();
		if (!WallRig.BindWeapon(*this,
			M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_homing")), Homing.ProjectileId, 120.0f, 1),
			{Homing}, {}))
		{
			return true;
		}
		AActor* Tracked = DeepTarget;
		WallRig.Player->SetWeaponHomingTargetProvider([Tracked]() -> AActor* { return Tracked; });
		WallRig.Player->OnFireInputPressed();
		WallRig.Player->OnFireInputReleased();
		WallRig.TickFrames(30);
		TestEqual(TEXT("the wall keeps the deep target's pool untouched"), DeepHealth->GetHealth(), 100.0f);
		WallRig.Record(TEXT("wall_block"), TEXT("dummy_deep"), DeepHealth->GetHealth(), 0.0f,
			TEXT("wall_blocked_line"));
	}
	WallRig.ExportJson(*this, TEXT("weapons-homing.json"));
	return true;
}

// ---------------------------------------------------------------------------
// ExplosionBlastDamagesRadiusAndOccludedWallDetonation
// ---------------------------------------------------------------------------

// The explosion family rides the M5-032 policy through the production chain:
// a direct contact detonates once and wounds the target inside the blast
// radius (the falloff scales the blast profile down from the center), a
// target outside the radius takes nothing, the wall detonation occludes the
// target standing behind it (the world-static blocker cuts the blast line),
// and the whiff into an empty lane expires without any detonation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035ExplosionBlastDamagesRadiusAndOccludedWallDetonation,
	"UEMMO.Tasks.M5_035.ExplosionBlastDamagesRadiusAndOccludedWallDetonation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035ExplosionBlastDamagesRadiusAndOccludedWallDetonation::RunTest(const FString& Parameters)
{
	// The blast round: straight motion, a 250 cm radius, the blast profile's
	// 30 damage (the edge-scale wiring pins the linear decay to zero).
	FProjectileDefinition Blast = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_blast")), EProjectileMotion::Straight);
	Blast.ExplosionRadiusCm = M5_035_ExplosionRadiusCm;
	Blast.ExplosionDamageProfileId = FName(TEXT("dmg_rig_blast"));

	// The blast profile carries the configured control the detonation submits.
	FDamageProfile BlastProfile;
	BlastProfile.DamageProfileId = FName(TEXT("dmg_rig_blast"));
	BlastProfile.BaseDamage = M5_035_BlastDamage;
	BlastProfile.HitStunSeconds = 1.0f;
	BlastProfile.KnockbackCmPerSecond = 70.0f;
	BlastProfile.LaunchCmPerSecond = 700.0f;
	BlastProfile.HitStopSeconds = 0.04f;

	// Positive: the direct hit detonates and wounds the target inside the
	// radius; the 700 cm second target sits outside the radius and takes
	// nothing (the falloff never reaches it).
	FM5_035_Rig HitRig;
	if (!HitRig.Build(*this, /*bWithTarget*/ true) || !HitRig.Settle(*this))
	{
		return true;
	}
	UHealthComponent* OuterHealth = nullptr;
	HitRig.ExtraTarget = M5_035_SpawnTarget(*HitRig.World, M5_035_SceneBase, M5_035_SecondTargetOffset, 300.0f, &OuterHealth);
	if (!TestNotNull(TEXT("the outer target spawns"), HitRig.ExtraTarget) || OuterHealth == nullptr)
	{
		return true;
	}
	HitRig.World->EnsureCollisionTreeIsBuilt();
	if (!HitRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_blast")), Blast.ProjectileId, 120.0f, 1),
		{Blast}, {BlastProfile}))
	{
		return true;
	}
	const float InnerBefore = HitRig.TargetHealth->GetHealth();
	HitRig.Player->OnFireInputPressed();
	HitRig.Player->OnFireInputReleased();
	HitRig.TickFrames(12);
	TestEqual(TEXT("the blast shot commits exactly one shot"), HitRig.CommittedShots(), 1);
	TestTrue(TEXT("the blast wounded the target inside the radius"),
		HitRig.TargetHealth->GetHealth() < InnerBefore);
	TestTrue(TEXT("the blast damage never exceeds the blast profile"),
		HitRig.TargetHealth->GetHealth() >= InnerBefore - M5_035_BlastDamage);
	TestEqual(TEXT("the outer target is outside the blast radius"), OuterHealth->GetHealth(), 300.0f);
	TestEqual(TEXT("the detonated pellet left the live population"), HitRig.Player->GetLiveWeaponProjectileCount(), 0);
	HitRig.ControlText = M5_035_ControlText(BlastProfile);
	HitRig.Record(TEXT("detonation"), TEXT("dummy_normal"), HitRig.TargetHealth->GetHealth(),
		InnerBefore - HitRig.TargetHealth->GetHealth(), TEXT(""));
	HitRig.ExportJson(*this, TEXT("weapons-explosion.json"));

	// Whiff: an empty lane expires the pellet without any detonation.
	FM5_035_Rig WhiffRig;
	if (!WhiffRig.Build(*this, /*bWithTarget*/ false) || !WhiffRig.Settle(*this))
	{
		return true;
	}
	if (!WhiffRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_blast")), Blast.ProjectileId, 120.0f, 1),
		{Blast}, {BlastProfile}))
	{
		return true;
	}
	WhiffRig.Player->OnFireInputPressed();
	WhiffRig.Player->OnFireInputReleased();
	WhiffRig.TickFrames(40);
	TestEqual(TEXT("the whiffed blast round expired undetonated"), WhiffRig.Player->GetLiveWeaponProjectileCount(), 0);
	WhiffRig.ControlText = M5_035_ControlText(BlastProfile);
	WhiffRig.Record(TEXT("whiff"), TEXT(""), -1.0f, 0.0f, TEXT("lane_empty_no_detonation"));
	WhiffRig.ExportJson(*this, TEXT("weapons-explosion.json"));

	// Wall: the detonation happens at the wall and the target standing
	// behind it is occluded (the world-static blocker cuts the blast line).
	FM5_035_Rig WallRig;
	if (!WallRig.Build(*this, /*bWithTarget*/ true, /*bWithWall*/ true) || !WallRig.Settle(*this))
	{
		return true;
	}
	if (!WallRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_blast")), Blast.ProjectileId, 120.0f, 1),
		{Blast}, {BlastProfile}))
	{
		return true;
	}
	const float WallBefore = WallRig.TargetHealth->GetHealth();
	WallRig.Player->OnFireInputPressed();
	WallRig.Player->OnFireInputReleased();
	WallRig.TickFrames(12);
	TestEqual(TEXT("the occluded target behind the wall takes nothing"), WallRig.TargetHealth->GetHealth(), WallBefore);
	WallRig.ControlText = M5_035_ControlText(BlastProfile);
	WallRig.Record(TEXT("wall_block"), TEXT("dummy_normal"), WallRig.TargetHealth->GetHealth(), 0.0f,
		TEXT("wall_detonation_occluded_target"));
	WallRig.ExportJson(*this, TEXT("weapons-explosion.json"));
	return true;
}

// ---------------------------------------------------------------------------
// PierceWoundsThroughTargetsAndWallStopsBudget
// ---------------------------------------------------------------------------

// The piercing family rides the M5-031 policy through the production chain:
// the pellet wounds the first two inline targets (the pierce budget of one
// extra passage is exactly spent) and terminates; the wall stops the
// piercing pellet before the first target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035PierceWoundsThroughTargetsAndWallStopsBudget,
	"UEMMO.Tasks.M5_035.PierceWoundsThroughTargetsAndWallStopsBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035PierceWoundsThroughTargetsAndWallStopsBudget::RunTest(const FString& Parameters)
{
	FProjectileDefinition Piercing = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_piercing")), EProjectileMotion::Straight);
	Piercing.PierceCount = 1;

	// Positive: the first two inline targets are wounded; the budget ends
	// the pellet before anything further.
	FM5_035_Rig HitRig;
	if (!HitRig.Build(*this, /*bWithTarget*/ true) || !HitRig.Settle(*this))
	{
		return true;
	}
	UHealthComponent* SecondHealth = nullptr;
	HitRig.ExtraTarget = M5_035_SpawnTarget(*HitRig.World, M5_035_SceneBase, M5_035_SecondTargetOffset, 100.0f, &SecondHealth);
	if (!TestNotNull(TEXT("the second inline target spawns"), HitRig.ExtraTarget) || SecondHealth == nullptr)
	{
		return true;
	}
	HitRig.World->EnsureCollisionTreeIsBuilt();
	if (!HitRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_piercing")), Piercing.ProjectileId, 120.0f, 1),
		{Piercing}, {}))
	{
		return true;
	}
	const float FirstBefore = HitRig.TargetHealth->GetHealth();
	const float SecondBefore = SecondHealth->GetHealth();
	HitRig.Player->OnFireInputPressed();
	HitRig.Player->OnFireInputReleased();
	HitRig.TickFrames(26);
	TestEqual(TEXT("the piercing shot commits exactly one shot"), HitRig.CommittedShots(), 1);
	TestEqual(TEXT("the pellet wounded the first inline target"), HitRig.TargetHealth->GetHealth(), FirstBefore - M5_035_Damage);
	TestEqual(TEXT("the pellet pierced through and wounded the second target"), SecondHealth->GetHealth(), SecondBefore - M5_035_Damage);
	TestEqual(TEXT("the budget-spent pellet left the live population"), HitRig.Player->GetLiveWeaponProjectileCount(), 0);
	HitRig.Record(TEXT("pierce_hit"), TEXT("dummy_normal+dummy_second"),
		HitRig.TargetHealth->GetHealth(), M5_035_Damage * 2.0f, TEXT(""));
	HitRig.ExportJson(*this, TEXT("weapons-pierce.json"));

	// Wall: the unregistered wall stops the piercing pellet cold.
	FM5_035_Rig WallRig;
	if (!WallRig.Build(*this, /*bWithTarget*/ true, /*bWithWall*/ true) || !WallRig.Settle(*this))
	{
		return true;
	}
	if (!WallRig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_piercing")), Piercing.ProjectileId, 120.0f, 1),
		{Piercing}, {}))
	{
		return true;
	}
	const float WallBefore = WallRig.TargetHealth->GetHealth();
	WallRig.Player->OnFireInputPressed();
	WallRig.Player->OnFireInputReleased();
	WallRig.TickFrames(12);
	TestEqual(TEXT("the wall keeps the target's pool untouched"), WallRig.TargetHealth->GetHealth(), WallBefore);
	WallRig.Record(TEXT("wall_block"), TEXT("dummy_normal"), WallRig.TargetHealth->GetHealth(), 0.0f,
		TEXT("wall_blocked_line"));
	WallRig.ExportJson(*this, TEXT("weapons-pierce.json"));
	return true;
}

// ---------------------------------------------------------------------------
// BurstStopsOnReleaseAndReloadWindowGatesFire
// ---------------------------------------------------------------------------

// The production burst pacing through the real input layer: the press+release
// runs exactly the configured 3-round burst and stops; the T reload opens the
// real window and the Q press inside it commits nothing; the deadline
// completion transfers the reserve grant; the post-reload fire lands damage
// again (the repeatable chain).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035BurstStopsOnReleaseAndReloadWindowGatesFire,
	"UEMMO.Tasks.M5_035.BurstStopsOnReleaseAndReloadWindowGatesFire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035BurstStopsOnReleaseAndReloadWindowGatesFire::RunTest(const FString& Parameters)
{
	const FProjectileDefinition Straight = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_straight")), EProjectileMotion::Straight);
	FM5_035_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_burst")), Straight.ProjectileId, M5_035_BurstRpm, 3),
		{Straight}, {}))
	{
		return true;
	}

	// The burst: press+release runs exactly 3 sub-shots and stops.
	const float HealthBefore = Rig.TargetHealth->GetHealth();
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(24);
	TestEqual(TEXT("the burst committed exactly its configured 3 shots"), Rig.CommittedShots(), 3);
	TestEqual(TEXT("the magazine drained 3 rounds"), Rig.MagazineRounds(), 12);
	Rig.TickFrames(8);
	TestEqual(TEXT("the burst damage landed 3 x 10"), Rig.TargetHealth->GetHealth(), HealthBefore - 3.0f * M5_035_Damage);
	Rig.Record(TEXT("burst_stop"), TEXT("dummy_normal"), Rig.TargetHealth->GetHealth(), 3.0f * M5_035_Damage, TEXT(""));

	// The reload window: T opens it; the Q press inside commits nothing.
	Rig.Player->OnReloadInputPressed();
	Rig.TickFrames(1);
	TestTrue(TEXT("the reload window is pending"), Rig.Player->IsWeaponReloadPending());
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(4);
	TestEqual(TEXT("the fire press inside the reload window commits nothing"), Rig.CommittedShots(), 3);
	Rig.Record(TEXT("reload_gate"), TEXT("dummy_normal"), Rig.TargetHealth->GetHealth(), 0.0f,
		TEXT("fire_refused_reload_window"));

	// The deadline completes the window: the grant refills the magazine.
	Rig.TickFrames(130);
	TestFalse(TEXT("the reload window closed on its deadline"), Rig.Player->IsWeaponReloadPending());
	TestEqual(TEXT("the magazine refilled to capacity"), Rig.MagazineRounds(), 15);
	TestEqual(TEXT("the reserve paid the grant"), Rig.ReserveRounds(), 117);
	Rig.Record(TEXT("reload_complete"), TEXT(""), -1.0f, 0.0f, TEXT(""));

	// The post-reload fire lands damage again (the chain is repeatable).
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(12);
	TestEqual(TEXT("the post-reload shot committed"), Rig.CommittedShots(), 4);
	TestEqual(TEXT("the post-reload damage landed"), Rig.TargetHealth->GetHealth(), HealthBefore - 4.0f * M5_035_Damage);
	Rig.Record(TEXT("hit"), TEXT("dummy_normal"), Rig.TargetHealth->GetHealth(), M5_035_Damage, TEXT(""));
	Rig.ExportJson(*this, TEXT("weapons-burst-reload.json"));
	return true;
}

// ---------------------------------------------------------------------------
// TargetDeathRefusesDamageAndRetryRestoresChain
// ---------------------------------------------------------------------------

// The death path through the real chain: repeated real shots walk the target
// pool down to zero (the lethal shot marks it dead through its own health
// component), the dead body refuses every further submission (the pool stays
// 0 while the shots keep committing), and the unified reset + a fresh target
// restore the chain - the same shots wound the new target again (the
// death-retry cycle is repeatable).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035TargetDeathRefusesDamageAndRetryRestoresChain,
	"UEMMO.Tasks.M5_035.TargetDeathRefusesDamageAndRetryRestoresChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035TargetDeathRefusesDamageAndRetryRestoresChain::RunTest(const FString& Parameters)
{
	const FProjectileDefinition Straight = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_straight")), EProjectileMotion::Straight);
	FM5_035_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_straight")), Straight.ProjectileId, M5_035_KillRpm, 1),
		{Straight}, {}))
	{
		return true;
	}

	// Ten real shots (10 x 10) walk the pool down and the last one is lethal.
	for (int32 Shot = 1; Shot <= 10; ++Shot)
	{
		Rig.Player->OnFireInputPressed();
		Rig.Player->OnFireInputReleased();
		Rig.TickFrames(8);
	}
	TestEqual(TEXT("the ten shots committed"), Rig.CommittedShots(), 10);
	TestTrue(TEXT("the lethal shot marked the target dead through its own health entry"),
		!Rig.TargetHealth->IsAlive());
	TestEqual(TEXT("the dead pool stays exactly at zero"), Rig.TargetHealth->GetHealth(), 0.0f);
	Rig.Record(TEXT("death"), TEXT("dummy_normal"), Rig.TargetHealth->GetHealth(), M5_035_Damage, TEXT(""));

	// The dead body refuses every further submission: the shots keep
	// committing, the pool never moves again.
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(8);
	TestEqual(TEXT("the shot against the corpse still committed (the trigger is honest)"), Rig.CommittedShots(), 11);
	TestEqual(TEXT("the dead body refused the damage"), Rig.TargetHealth->GetHealth(), 0.0f);
	Rig.Record(TEXT("dead_refusal"), TEXT("dummy_normal"), Rig.TargetHealth->GetHealth(), 0.0f,
		TEXT("dead_body_refuses_damage"));

	// The retry: the unified reset restores the pawn and a fresh target
	// reopens the same chain - the identical shot wounds the new target.
	Rig.Player->ApplyTrainingRoomReset();
	Rig.ExtraTarget = M5_035_SpawnTarget(*Rig.World, M5_035_SceneBase, M5_035_TargetOffset, 100.0f, &Rig.ExtraTargetHealth);
	if (!TestNotNull(TEXT("the retry target spawns"), Rig.ExtraTarget) || Rig.ExtraTargetHealth == nullptr)
	{
		return true;
	}
	Rig.World->EnsureCollisionTreeIsBuilt();
	Rig.TickFrames(4);
	const float RetryBefore = Rig.ExtraTargetHealth->GetHealth();
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(12);
	TestEqual(TEXT("the retry shot committed"), Rig.CommittedShots(), 12);
	TestEqual(TEXT("the retry restored the chain (the fresh target took the round)"),
		Rig.ExtraTargetHealth->GetHealth(), RetryBefore - M5_035_Damage);
	Rig.Record(TEXT("retry"), TEXT("dummy_retry"), Rig.ExtraTargetHealth->GetHealth(), M5_035_Damage, TEXT(""));
	Rig.ExportJson(*this, TEXT("weapons-death-retry.json"));
	return true;
}

// ---------------------------------------------------------------------------
// ConfigChangeChangesMeasuredDamageAndArrival
// ---------------------------------------------------------------------------

// The config-driven chain: the same scenario with the adjusted catalog
// measures a different wound (25 instead of 10) and a different arrival (the
// slowed round is still mid-flight when the fast round already landed). The
// revisions of both catalogs ride the recorded events.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035ConfigChangeChangesMeasuredDamageAndArrival,
	"UEMMO.Tasks.M5_035.ConfigChangeChangesMeasuredDamageAndArrival",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035ConfigChangeChangesMeasuredDamageAndArrival::RunTest(const FString& Parameters)
{
	// Config A: the standard catalog (10 damage at 3000 cm/s).
	const FProjectileDefinition Fast = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_fast")), EProjectileMotion::Straight);
	FM5_035_Rig RigA;
	if (!RigA.Build(*this, /*bWithTarget*/ true) || !RigA.Settle(*this))
	{
		return true;
	}
	if (!RigA.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_fast")), Fast.ProjectileId, 120.0f, 1),
		{Fast}, {}))
	{
		return true;
	}
	const float BeforeA = RigA.TargetHealth->GetHealth();
	RigA.Player->OnFireInputPressed();
	RigA.Player->OnFireInputReleased();
	RigA.TickFrames(12);
	TestEqual(TEXT("config A measured exactly the configured 10"), RigA.TargetHealth->GetHealth(), BeforeA - M5_035_Damage);
	RigA.Record(TEXT("hit"), TEXT("dummy_normal"), RigA.TargetHealth->GetHealth(), M5_035_Damage, TEXT(""));
	RigA.ExportJson(*this, TEXT("weapons-config-change.json"));

	// Config B: the adjusted catalog (25 damage at 750 cm/s) - a fresh rig.
	const FProjectileDefinition Slow = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_rig_slow")), EProjectileMotion::Straight);
	Slow.SpeedCmS = M5_035_SlowSpeedCmS;
	FDamageProfile Adjusted;
	Adjusted.DamageProfileId = FName(TEXT("dmg_rig_round"));
	Adjusted.BaseDamage = M5_035_AdjustedDamage;
	FM5_035_Rig RigB;
	if (!RigB.Build(*this, /*bWithTarget*/ true) || !RigB.Settle(*this))
	{
		return true;
	}
	if (!RigB.BindWeapon(*this,
		M5_035_MakeProjectileWeaponDefinition(FName(TEXT("weapon_rig_slow")), Slow.ProjectileId, 120.0f, 1),
		{Slow}, {Adjusted}))
	{
		return true;
	}
	const float BeforeB = RigB.TargetHealth->GetHealth();
	RigB.Player->OnFireInputPressed();
	RigB.Player->OnFireInputReleased();
	// Ten frames in, the fast round had already landed; the slowed round is
	// still 125 cm into its 400 cm flight.
	RigB.TickFrames(10);
	TestEqual(TEXT("the slowed round is still mid-flight (no damage yet)"), RigB.TargetHealth->GetHealth(), BeforeB);
	RigB.Record(TEXT("slow_in_flight"), TEXT("dummy_normal"), RigB.TargetHealth->GetHealth(), 0.0f,
		TEXT("slow_round_still_flying"));
	RigB.TickFrames(30);
	TestEqual(TEXT("config B measured exactly the adjusted 25"), RigB.TargetHealth->GetHealth(), BeforeB - M5_035_AdjustedDamage);
	RigB.Record(TEXT("hit"), TEXT("dummy_normal"), RigB.TargetHealth->GetHealth(), M5_035_AdjustedDamage, TEXT(""));
	RigB.ExportJson(*this, TEXT("weapons-config-change.json"));
	return true;
}

// ---------------------------------------------------------------------------
// WeaponScenarioCapture (the rendered capture companion; ClientContext only)
// ---------------------------------------------------------------------------

// In a real rendered game world the production chain is staged at the game
// pawn's location: the rig catalogs mount into the real weapon component
// (MountCatalogs swaps the pair), the wall and the normal/heavy targets
// stand in front of the pawn, the slow straight volley flies visibly, and
// the blast round detonates on the wall. Headless runs skip silently; the
// chain assertions live in the suites above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_035WeaponScenarioCapture,
	"UEMMO.Tasks.M5_035.WeaponScenarioCapture",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_035WeaponScenarioCapture::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	APrototypeCharacter* GamePawn = World != nullptr
		? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0))
		: nullptr;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game || GamePawn == nullptr)
	{
		AddInfo(TEXT("weapon scenario capture skipped: no rendered game world with the prototype pawn"));
		return true;
	}

	const FString CaptureDir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-035") / TEXT("scenario-json") / TEXT("render"));
	IFileManager::Get().MakeDirectory(*CaptureDir, /*Tree*/ true);

	// The visual-scenario definitions: a slow 3-pellet straight volley and a
	// slow blast round (both visible in flight at the fixed camera).
	FWeaponDefinition VolleyWeapon = M5_035_MakeProjectileWeaponDefinition(
		FName(TEXT("weapon_capture_volley")), FName(TEXT("bullet_capture_volley")), 120.0f, 1);
	VolleyWeapon.PelletCount = 3;
	VolleyWeapon.SpreadDegrees = 2.0f;
	FProjectileDefinition VolleyRound = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_capture_volley")), EProjectileMotion::Straight);
	VolleyRound.SpeedCmS = 600.0f;
	FWeaponDefinition BlastWeapon = M5_035_MakeProjectileWeaponDefinition(
		FName(TEXT("weapon_capture_blast")), FName(TEXT("bullet_capture_blast")), 120.0f, 1);
	FProjectileDefinition BlastRound = M5_035_MakeProjectileDefinition(
		FName(TEXT("bullet_capture_blast")), EProjectileMotion::Straight);
	BlastRound.SpeedCmS = 600.0f;
	BlastRound.ExplosionRadiusCm = M5_035_ExplosionRadiusCm;
	BlastRound.ExplosionDamageProfileId = FName(TEXT("dmg_capture_blast"));
	FDamageProfile BlastProfile;
	BlastProfile.DamageProfileId = FName(TEXT("dmg_capture_blast"));
	BlastProfile.BaseDamage = M5_035_BlastDamage;

	// The capture's catalog values must outlive the latent chain and the
	// mount's non-owning pointers; they are leaked deliberately (the capture
	// process exits right after, so the mount never points into freed memory).
	struct FM5_035_CaptureBind
	{
		FCombatCatalog Catalog;
		FItemDefinitionCatalog Items;
	};
	auto BuildCaptureCatalog = [](FM5_035_CaptureBind& Bind, const FWeaponDefinition& WeaponDef,
		const TArray<FProjectileDefinition>& ProjectileDefs, const TArray<FDamageProfile>& ExtraProfiles) -> bool
	{
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;
		FDamageProfile Profile;
		Profile.DamageProfileId = WeaponDef.DamageProfileId;
		Profile.BaseDamage = M5_035_Damage;
		Parsed.DamageProfiles.Add(Profile);
		for (const FDamageProfile& Extra : ExtraProfiles)
		{
			Parsed.DamageProfiles.Add(Extra);
		}
		if (!WeaponDef.AmmoId.IsNone())
		{
			FAmmoType Ammo;
			Ammo.AmmoId = WeaponDef.AmmoId;
			Ammo.MaxReserve = 600;
			Ammo.MagazineSize = WeaponDef.MagazineSize;
			Parsed.AmmoTypes.Add(Ammo);
		}
		for (const FProjectileDefinition& ProjectileDef : ProjectileDefs)
		{
			Parsed.Projectiles.Add(ProjectileDef);
		}
		FWeaponDefinition Weapon = WeaponDef;
		Parsed.Weapons.Add(Weapon);
		FString BuildErrors;
		if (!FCombatCatalog::BuildFromParsed(Parsed, Bind.Catalog, BuildErrors))
		{
			return false;
		}
		FItemDefinition Item;
		Item.DefinitionId = FName(TEXT("item_capture_scenario"));
		Item.DisplayName = TEXT("Capture Scenario Weapon");
		Item.Slot = EItemSlot::Weapon;
		Item.Rarity = EItemRarity::Normal;
		Item.WeaponDefinitionId = WeaponDef.WeaponId;
		FString ItemError;
		return Bind.Items.AddDefinition(Item, &ItemError);
	};
	auto BindCaptureWeapon = [GamePawn](FM5_035_CaptureBind& Bind, const FWeaponDefinition& WeaponDef) -> bool
	{
		UWeaponComponent* Mount = GamePawn->GetWeaponMount();
		FString MountError;
		if (Mount == nullptr || !Mount->MountCatalogs(&Bind.Catalog, &Bind.Items, &MountError))
		{
			return false;
		}
		if (!WeaponDef.AmmoId.IsNone())
		{
			Mount->GetAmmoModel().RegisterAmmoType(WeaponDef.AmmoId, 600, 300);
		}
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x0350C000u, 0x0BECu, 0x5335u, 1u);
		Instance.DefinitionId = FName(TEXT("item_capture_scenario"));
		Instance.RollSeed = 0x52035;
		Instance.Level = 1;
		const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
		if (!Outcome.bSucceeded || !Outcome.bWeaponBound)
		{
			return false;
		}
		GamePawn->ApplyWeaponBindWiring();
		return true;
	};

	FM5_035_CaptureBind& VolleyBind = *new FM5_035_CaptureBind();
	FM5_035_CaptureBind& BlastBind = *new FM5_035_CaptureBind();
	if (!TestTrue(TEXT("the capture volley catalog builds"),
		BuildCaptureCatalog(VolleyBind, VolleyWeapon, {VolleyRound}, {}))
		|| !TestTrue(TEXT("the capture blast catalog builds"),
			BuildCaptureCatalog(BlastBind, BlastWeapon, {BlastRound}, {BlastProfile})))
	{
		return true;
	}

	// Stage the production scenario at the game pawn's location: the wall,
	// the normal target and the heavy target behind it (spawned with the
	// adjust-always override so the staged boxes never collide-refuse).
	const FVector Base = GamePawn->GetActorLocation();
	FActorSpawnParameters StageParams;
	StageParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AActor* Wall = World->SpawnActor<AActor>(
		AActor::StaticClass(), Base + M5_035_WallCenter, FRotator::ZeroRotator, StageParams);
	AActor* NormalTarget = World->SpawnActor<AActor>(
		AActor::StaticClass(), Base + M5_035_TargetOffset, FRotator::ZeroRotator, StageParams);
	AActor* HeavyTarget = World->SpawnActor<AActor>(
		AActor::StaticClass(), Base + M5_035_SecondTargetOffset, FRotator::ZeroRotator, StageParams);
	auto GiveTargetBody = [](AActor* Actor, float MaxHealth) -> bool
	{
		if (Actor == nullptr)
		{
			return false;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_035_CaptureBody"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_035_TargetBoxHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_035_CaptureHealth"));
		Health->RegisterComponent();
		Health->SetMaxHealth(MaxHealth);
		Health->ResetHealth();
		return true;
	};
	if (Wall != nullptr)
	{
		UBoxComponent* Body = NewObject<UBoxComponent>(Wall, TEXT("M5_035_CaptureWall"));
		Wall->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_035_WallHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
	}
	if (!TestTrue(TEXT("the capture scenario staged"),
		Wall != nullptr && GiveTargetBody(NormalTarget, 100.0f) && GiveTargetBody(HighTargetFallback(), 300.0f)))
	{
		return true;
	}
	World->EnsureCollisionTreeIsBuilt();

	// Screenshot 1: the staged scenario (wall + normal + heavy targets).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-targets.png"), true, false);
		return true;
	}));

	// The volley: the slow 3-pellet straight round through the real bind and
	// the real input path (the boot menu dismisses first if it is up).
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn, &VolleyBind, VolleyWeapon, BindCaptureWeapon]()
	{
		if (APlayerController* PC = Cast<APlayerController>(GamePawn->Controller))
		{
			if (APrototypeHUD* Hud = Cast<APrototypeHUD>(PC->GetHUD()))
			{
				if (UMapSelectWidget* BootMenu = Hud->PeekMenuWidget())
				{
					BootMenu->EnterAccepted.Broadcast();
				}
			}
		}
		const bool bBound = BindCaptureWeapon(VolleyBind, VolleyWeapon);
		if (bBound)
		{
			GamePawn->OnFireInputPressed();
			GamePawn->OnFireInputReleased();
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f));
	// Screenshot 2: the volley mid-flight (three visible pellets).
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-flight.png"), true, false);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.2f));

	// The blast: the slow blast round detonates on the wall.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn, &BlastBind, BlastWeapon, BindCaptureWeapon]()
	{
		const bool bBound = BindCaptureWeapon(BlastBind, BlastWeapon);
		if (bBound)
		{
			GamePawn->OnFireInputPressed();
			GamePawn->OnFireInputReleased();
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.45f));
	// Screenshot 3: the blast moment on the wall.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-blast.png"), true, false);
		return true;
	}));

	// Cleanup: retire the staged actors (the shared game world stays clean).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Wall, NormalTarget, HeavyTarget]()
	{
		for (AActor* Staged : {Wall, NormalTarget, HighTargetFallback()})
		{
			if (Staged != nullptr && IsValid(Staged))
			{
				Staged->Destroy();
			}
		}
		return true;
	}));
	return true;
}

#endif
