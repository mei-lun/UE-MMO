// M5-058: the weapon-fire perceivable feedback (the presentation face of the
// committed-shot callback). The M5-026 executor stays a pure query-and-submit
// pipeline; the pawn's callback owns the tracer beams, the fire sound and the
// per-shot log. The suites run the real production chain in a manually ticked
// world (the FTestWorldWrapper precedent) and pin:
//
// - One beam per hitscan pellet, geometrically honest: the hit beam ends at
//   the target's impact point, the whiff beam ends at the max-range endpoint,
//   and the wall-blocked beam stops AT THE WALL (no overdraw through the
//   blocker - the H02 wall-block acceptance reads this).
// - The beams retire through the engine LifeSpan (0.12s): after the flash
//   window the world holds no tracer bodies.
// - A projectile-mode shot spawns NO tracers (the pellet cube is the visual);
//   the feedback seam never double-presents.

#include "Misc/AutomationTest.h"

#include "../PrototypeCharacter.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/HitscanTracer.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundWave.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_058
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M5_058_FrameSeconds = 1.0f / 60.0f;
	// Remote scene base so the temp world can never collide with arena content.
	const FVector M5_058_SceneBase(238000.0, 64000.0, 600.0);
	constexpr float M5_058_FloorHalfThickness = 100.0f;
	constexpr float M5_058_FloorHalfExtentXY = 6000.0f;
	constexpr float M5_058_PlayerSpawnHeight = 120.0f;
	// The standard target stands 400 cm ahead (facing +X); the shot origin
	// rides the feet offset (30, 0, 50), so the beam runs at feet+50 height.
	const FVector M5_058_TargetOffset(400.0, 0.0, 50.0);
	const FVector M5_058_TargetBoxHalfExtent(50.0, 50.0, 60.0);
	// The blocking wall between pawn and target lane.
	const FVector M5_058_WallCenter(200.0, 0.0, 100.0);
	const FVector M5_058_WallHalfExtent(20.0, 3000.0, 250.0);
	// The rig hitscan range; the whiff beam spans exactly this length.
	constexpr float M5_058_HitscanRangeCm = 3000.0f;
	// The beam flash window (the tracer's InitialLifeSpan).
	constexpr float M5_058_TracerLifeSeconds = 0.12f;
	// The position/length tolerance for the geometric assertions (cm).
	constexpr float M5_058_GeometryToleranceCm = 8.0f;

	static AActor* M5_058_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M5_058_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_058_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M5_058_FloorHalfExtentXY, M5_058_FloorHalfExtentXY, M5_058_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M5_058_FloorHalfThickness));
		return Actor;
	}

	static AActor* M5_058_SpawnWall(UWorld& World)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), M5_058_SceneBase + M5_058_WallCenter, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_058_Wall"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_058_WallHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(M5_058_SceneBase + M5_058_WallCenter);
		return Actor;
	}

	static AActor* M5_058_SpawnTarget(UWorld& World, UHealthComponent*& OutHealth)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), M5_058_SceneBase + M5_058_TargetOffset, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_058_Target"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_058_TargetBoxHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(M5_058_SceneBase + M5_058_TargetOffset);
		OutHealth = NewObject<UHealthComponent>(Actor, TEXT("M5_058_TargetHealth"));
		OutHealth->RegisterComponent();
		return Actor;
	}

	// One full rig: the wrapper owns the manually ticked temp world, the rig
	// owns the catalog VALUES mounted into the real pawn's weapon component;
	// the shared clock feeds both the pawn's input layer and the mount's fire
	// clock (the M5-022 injection surface).
	struct FM5_058_Rig
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;
		FCombatCatalog CatalogValue;
		FItemDefinitionCatalog ItemsValue;
		double ClockSeconds = 300.0;

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
			if (M5_058_SpawnFloor(*World, M5_058_SceneBase) == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			if (bWithWall && M5_058_SpawnWall(*World) == nullptr)
			{
				Test.AddError(TEXT("the wall failed to spawn"));
				return false;
			}
			World->EnsureCollisionTreeIsBuilt();
			FActorSpawnParameters PlayerParams;
			Player = World->SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(),
				M5_058_SceneBase + FVector(0.0, 0.0, M5_058_PlayerSpawnHeight), FRotator::ZeroRotator, PlayerParams);
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
				Target = M5_058_SpawnTarget(*World, TargetHealth);
				if (!Test.TestNotNull(TEXT("the target actor spawns"), Target))
				{
					return false;
				}
			}
			Player->SetWeaponInputClockProvider([this]() { return ClockSeconds; });
			Player->GetWeaponMount()->SetFireClockProvider([this]() { return ClockSeconds; });
			return true;
		}

		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
			for (int32 Frame = 0; Frame < 300; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks (settle)"), Wrapper.TickTestWorld(M5_058_FrameSeconds)))
				{
					return false;
				}
				ClockSeconds += M5_058_FrameSeconds;
				if (Movement != nullptr && Movement->MovementMode == MOVE_Walking && Movement->Velocity.Size() < 1.0f)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the pawn never settled onto the floor"));
			return false;
		}

		/** The rig hitscan definition (single pellet unless overridden). */
		static FWeaponDefinition MakeHitscanDefinition(int32 PelletCount)
		{
			FWeaponDefinition Weapon;
			Weapon.WeaponId = FName(TEXT("weapon_rig_feedback_hitscan"));
			Weapon.FireMode = EWeaponFireMode::Hitscan;
			Weapon.DamageProfileId = FName(TEXT("dmg_rig_feedback"));
			Weapon.AmmoId = FName(TEXT("ammo_rig_feedback"));
			Weapon.MagazineSize = 30;
			Weapon.FireRateRpm = 120.0f;
			Weapon.BurstCount = 1;
			Weapon.PelletCount = PelletCount;
			Weapon.SpreadDegrees = 0.0f;
			Weapon.RangeCm = M5_058_HitscanRangeCm;
			return Weapon;
		}

		/** The rig straight-projectile definition (the no-tracer contrast). */
		static FWeaponDefinition MakeProjectileDefinition()
		{
			FWeaponDefinition Weapon;
			Weapon.WeaponId = FName(TEXT("weapon_rig_feedback_projectile"));
			Weapon.FireMode = EWeaponFireMode::Projectile;
			Weapon.DamageProfileId = FName(TEXT("dmg_rig_feedback"));
			Weapon.AmmoId = FName(TEXT("ammo_rig_feedback"));
			Weapon.MagazineSize = 12;
			Weapon.FireRateRpm = 120.0f;
			Weapon.BurstCount = 1;
			Weapon.PelletCount = 1;
			Weapon.SpreadDegrees = 0.0f;
			Weapon.RangeCm = M5_058_HitscanRangeCm;
			Weapon.ProjectileId = FName(TEXT("proj_rig_feedback"));
			return Weapon;
		}

		bool BindWeapon(FAutomationTestBase& Test, const FWeaponDefinition& WeaponDef)
		{
			FParsedCombatConfig Parsed;
			Parsed.SchemaVersion = 1;
			FDamageProfile Profile;
			Profile.DamageProfileId = WeaponDef.DamageProfileId;
			Profile.BaseDamage = 10.0f;
			Parsed.DamageProfiles.Add(Profile);
			if (!WeaponDef.AmmoId.IsNone())
			{
				FAmmoType Ammo;
				Ammo.AmmoId = WeaponDef.AmmoId;
				Ammo.MaxReserve = 120;
				Ammo.MagazineSize = WeaponDef.MagazineSize;
				Parsed.AmmoTypes.Add(Ammo);
			}
			if (WeaponDef.FireMode == EWeaponFireMode::Projectile)
			{
				FProjectileDefinition Projectile;
				Projectile.ProjectileId = WeaponDef.ProjectileId;
				Projectile.DamageProfileId = WeaponDef.DamageProfileId;
				Projectile.SpeedCmS = 900.0f;
				Projectile.LifetimeS = 3.0f;
				Projectile.Motion = EProjectileMotion::Straight;
				Projectile.ExplosionRadiusCm = 0.0f;
				Projectile.PierceCount = 0;
				Parsed.Projectiles.Add(Projectile);
			}
			Parsed.Weapons.Add(WeaponDef);
			FString BuildErrors;
			if (!Test.TestTrue(TEXT("the rig catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, CatalogValue, BuildErrors)))
			{
				Test.AddError(FString::Printf(TEXT("the rig catalog build refused: %s"), *BuildErrors));
				return false;
			}
			FItemDefinition Item;
			Item.DefinitionId = FName(TEXT("item_rig_feedback"));
			Item.DisplayName = TEXT("Rig Feedback Weapon");
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
				if (!Test.TestTrue(TEXT("the rig reserve ammo registers"),
					Mount->GetAmmoModel().RegisterAmmoType(WeaponDef.AmmoId, 120, 60)))
				{
					return false;
				}
			}
			FItemInstance Instance;
			Instance.InstanceId = FGuid(0x0580B000u, 0x0BECu, 0x5335u, 1u);
			Instance.DefinitionId = Item.DefinitionId;
			Instance.RollSeed = 0x05805;
			Instance.Level = 1;
			const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
			if (!Test.TestTrue(TEXT("the rig weapon binds"), Outcome.bSucceeded && Outcome.bWeaponBound))
			{
				Test.AddError(FString::Printf(TEXT("the rig bind refused (%d): %s"),
					static_cast<int32>(Outcome.Reject), *Outcome.RejectDetail));
				return false;
			}
			Player->ApplyWeaponBindWiring();
			return Test.TestTrue(TEXT("the bound mount authorizes fire"), Mount->IsFireAuthorized());
		}

		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Wrapper.TickTestWorld(M5_058_FrameSeconds))
				{
					break;
				}
				ClockSeconds += M5_058_FrameSeconds;
			}
		}

		/**
		 * The shot origin the M5-033 contract rides: the capsule-bottom feet
		 * origin (ResolveFeetLocation) plus the (30, 0, 50) feet offset.
		 */
		FVector ShotOrigin() const
		{
			const UCapsuleComponent* Capsule = Player->GetCapsuleComponent();
			const float HalfHeight = (Capsule != nullptr) ? Capsule->GetScaledCapsuleHalfHeight() : 0.0f;
			return Player->GetActorLocation() - FVector(0.0f, 0.0f, HalfHeight) + FVector(30.0f, 0.0f, 50.0f);
		}

		int32 CountTracers() const
		{
			int32 Count = 0;
			for (TActorIterator<AHitscanTracer> It(GetWorld()); It; ++It)
			{
				++Count;
			}
			return Count;
		}

		UWorld* GetWorld() const { return World; }
	};
}

using namespace UE::UEMMO::Tasks::M5_058;

// ---------------------------------------------------------------------------
// HitscanBeamPerPelletWithHonestGeometry
// ---------------------------------------------------------------------------

// The hit, the whiff and the wall-block each draw their beam where the shot
// actually ended: at the target impact, at the max-range endpoint, at the
// blocker - never overdrawn through a wall.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_058HitscanBeamPerPelletWithHonestGeometry,
	"UEMMO.Tasks.M5_058.HitscanBeamPerPelletWithHonestGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_058HitscanBeamPerPelletWithHonestGeometry::RunTest(const FString& Parameters)
{
	// Hit: one beam from the shot origin to the target's front face.
	FM5_058_Rig HitRig;
	if (!HitRig.Build(*this, /*bWithTarget*/ true) || !HitRig.Settle(*this))
	{
		return true;
	}
	if (!HitRig.BindWeapon(*this, FM5_058_Rig::MakeHitscanDefinition(1)))
	{
		return true;
	}
	HitRig.Player->OnFireInputPressed();
	HitRig.Player->OnFireInputReleased();
	HitRig.TickFrames(2);
	TestEqual(TEXT("the hit shot draws exactly one tracer"), HitRig.CountTracers(), 1);
	if (const AHitscanTracer* Tracer = *TActorIterator<AHitscanTracer>(HitRig.GetWorld()))
	{
		const FVector Origin = HitRig.ShotOrigin();
		const float ExpectedFrontFaceX = (M5_058_SceneBase.X + M5_058_TargetOffset.X) - M5_058_TargetBoxHalfExtent.X;
		const float ExpectedMidX = (Origin.X + ExpectedFrontFaceX) * 0.5f;
		const FVector BeamLocation = Tracer->GetActorLocation();
		TestTrue(TEXT("the hit beam centers between origin and impact (X)"),
			FMath::Abs(BeamLocation.X - ExpectedMidX) < M5_058_GeometryToleranceCm);
		TestTrue(TEXT("the hit beam rides the shot height (Z)"),
			FMath::Abs(BeamLocation.Z - (Origin.Z)) < M5_058_GeometryToleranceCm);
		// The stretched cube's X bbox half-extent is half the beam length:
		// ~160 cm for the 320 cm origin-to-front-face span. Bounds come from
		// CalcBounds (a manually ticked headless world never refreshes the
		// cached component bounds).
		const FVector BoxExtent = Tracer->Beam->CalcBounds(Tracer->Beam->GetComponentTransform()).BoxExtent;
		TestTrue(FString::Printf(TEXT("the hit beam length matches origin-to-impact (extent=%.1f expected=%.1f)"),
			BoxExtent.X, (ExpectedFrontFaceX - Origin.X) * 0.5f),
			FMath::Abs(BoxExtent.X - (ExpectedFrontFaceX - Origin.X) * 0.5f) < M5_058_GeometryToleranceCm * 0.5f + 5.0f);
	}

	// Whiff: one beam spanning the full range into the empty lane.
	FM5_058_Rig WhiffRig;
	if (!WhiffRig.Build(*this, /*bWithTarget*/ false) || !WhiffRig.Settle(*this))
	{
		return true;
	}
	if (!WhiffRig.BindWeapon(*this, FM5_058_Rig::MakeHitscanDefinition(1)))
	{
		return true;
	}
	WhiffRig.Player->OnFireInputPressed();
	WhiffRig.Player->OnFireInputReleased();
	WhiffRig.TickFrames(2);
	TestEqual(TEXT("the whiff shot still draws its full-length beam"), WhiffRig.CountTracers(), 1);
	if (const AHitscanTracer* Tracer = *TActorIterator<AHitscanTracer>(WhiffRig.GetWorld()))
	{
		const FVector BoxExtent = Tracer->Beam->CalcBounds(Tracer->Beam->GetComponentTransform()).BoxExtent;
		TestTrue(FString::Printf(TEXT("the whiff beam spans the full range (extent=%.1f expected=%.1f)"),
			BoxExtent.X, M5_058_HitscanRangeCm * 0.5f),
			FMath::Abs(BoxExtent.X - M5_058_HitscanRangeCm * 0.5f) < M5_058_GeometryToleranceCm * 0.5f + 5.0f);
	}

	// Wall: the beam stops AT the blocker (the impact point the executor
	// echoes), never overdrawn toward the target behind it.
	FM5_058_Rig WallRig;
	if (!WallRig.Build(*this, /*bWithTarget*/ true, /*bWithWall*/ true) || !WallRig.Settle(*this))
	{
		return true;
	}
	if (!WallRig.BindWeapon(*this, FM5_058_Rig::MakeHitscanDefinition(1)))
	{
		return true;
	}
	WallRig.Player->OnFireInputPressed();
	WallRig.Player->OnFireInputReleased();
	WallRig.TickFrames(2);
	TestEqual(TEXT("the wall-blocked shot draws exactly one tracer"), WallRig.CountTracers(), 1);
	if (const AHitscanTracer* Tracer = *TActorIterator<AHitscanTracer>(WallRig.GetWorld()))
	{
		const float WallFrontX = (M5_058_SceneBase.X + M5_058_WallCenter.X) - M5_058_WallHalfExtent.X;
		const FVector BoxExtent = Tracer->Beam->CalcBounds(Tracer->Beam->GetComponentTransform()).BoxExtent;
		const float BeamEndX = Tracer->GetActorLocation().X + BoxExtent.X;
		const float OriginX = WallRig.ShotOrigin().X;
		TestTrue(FString::Printf(TEXT("the wall-blocked beam ends at the wall's front face (end=%.1f wall=%.1f)"),
			BeamEndX, WallFrontX),
			FMath::Abs((BeamEndX - OriginX) - (WallFrontX - OriginX)) < M5_058_GeometryToleranceCm);
	}
	return true;
}

// ---------------------------------------------------------------------------
// BeamFleetAndLifeSpanRetirement
// ---------------------------------------------------------------------------

// A five-pellet shot draws five beams; every beam retires through the engine
// LifeSpan after the flash window (the world holds no leftover bodies).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_058BeamFleetAndLifeSpanRetirement,
	"UEMMO.Tasks.M5_058.BeamFleetAndLifeSpanRetirement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_058BeamFleetAndLifeSpanRetirement::RunTest(const FString& Parameters)
{
	FM5_058_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, FM5_058_Rig::MakeHitscanDefinition(5)))
	{
		return true;
	}
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);
	TestEqual(TEXT("the five-pellet shot draws five beams"), Rig.CountTracers(), 5);

	// The flash window retires: 0.12s at 60fps = ~8 frames; 30 frames clears
	// every LifeSpan timer with margin.
	Rig.TickFrames(30);
	TestEqual(TEXT("the beams retire through the engine LifeSpan"), Rig.CountTracers(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// ProjectileModeSpawnsNoTracers
// ---------------------------------------------------------------------------

// The projectile family's visual is the pellet cube itself: the feedback seam
// never draws tracer beams for a projectile shot (no double presentation).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_058ProjectileModeSpawnsNoTracers,
	"UEMMO.Tasks.M5_058.ProjectileModeSpawnsNoTracers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_058ProjectileModeSpawnsNoTracers::RunTest(const FString& Parameters)
{
	FM5_058_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, FM5_058_Rig::MakeProjectileDefinition()))
	{
		return true;
	}
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);
	TestEqual(TEXT("the projectile shot draws no tracer beams"), Rig.CountTracers(), 0);
	int32 ProjectileCount = 0;
	for (TActorIterator<ACombatProjectile> It(Rig.GetWorld()); It; ++It)
	{
		++ProjectileCount;
	}
	TestEqual(TEXT("the projectile shot still spawns its pellet body"), ProjectileCount, 1);
	return true;
}

// ---------------------------------------------------------------------------
// FireFeedbackSoundAssetResolves
// ---------------------------------------------------------------------------

// The CC0 fire sound loads through the same soft-path pattern as the M1-034
// precedent (a missing asset would degrade to visual-only feedback; the test
// pins the committed content so a packaging regression surfaces here).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_058FireFeedbackSoundAssetResolves,
	"UEMMO.Tasks.M5_058.FireFeedbackSoundAssetResolves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_058FireFeedbackSoundAssetResolves::RunTest(const FString& Parameters)
{
	const USoundWave* FireSound = LoadObject<USoundWave>(nullptr,
		TEXT("/Game/ThirdParty/Kenney/Impact/impactPunch_heavy_000.impactPunch_heavy_000"));
	TestNotNull(TEXT("the committed CC0 fire sound resolves"), FireSound);
	return true;
}

// ---------------------------------------------------------------------------
// ProductionBindTailDeliversWithoutManualWiring
// ---------------------------------------------------------------------------

// M5-059: the production bind path (mount catalogs -> register ammo ->
// ApplyEquippedWeapon, exactly what RefreshWeaponMountFromEquipment performs,
// with NO manual ApplyWeaponBindWiring call) must deliver: the binding-applied
// seam re-derives the input-layer wiring, so the commit callback's generation
// guard passes and the shot reaches the target (the v6 root cause killed every
// delivery in the real game while the manual-wiring tests stayed green).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_059ProductionBindTailDeliversWithoutManualWiring,
	"UEMMO.Tasks.M5_059.ProductionBindTailDeliversWithoutManualWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_059ProductionBindTailDeliversWithoutManualWiring::RunTest(const FString& Parameters)
{
	FM5_058_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}

	// The production-style bind: the same three calls the refresh tail makes,
	// deliberately WITHOUT the manual ApplyWeaponBindWiring the test suites
	// used to own (the M5-035 fixture pattern). The binding-applied seam must
	// close the gap.
	FParsedCombatConfig Parsed;
	Parsed.SchemaVersion = 1;
	FDamageProfile Profile;
	Profile.DamageProfileId = FName(TEXT("dmg_rig_feedback"));
	Profile.BaseDamage = 10.0f;
	Parsed.DamageProfiles.Add(Profile);
	FAmmoType Ammo;
	Ammo.AmmoId = FName(TEXT("ammo_rig_feedback"));
	Ammo.MaxReserve = 120;
	Ammo.MagazineSize = 30;
	Parsed.AmmoTypes.Add(Ammo);
	Parsed.Weapons.Add(FM5_058_Rig::MakeHitscanDefinition(1));
	FString BuildErrors;
	if (!TestTrue(TEXT("the rig catalog builds"),
		FCombatCatalog::BuildFromParsed(Parsed, Rig.CatalogValue, BuildErrors)))
	{
		return true;
	}
	FItemDefinition Item;
	Item.DefinitionId = FName(TEXT("item_rig_production_tail"));
	Item.DisplayName = TEXT("Production Tail Rifle");
	Item.Slot = EItemSlot::Weapon;
	Item.Rarity = EItemRarity::Normal;
	Item.WeaponDefinitionId = FName(TEXT("weapon_rig_feedback_hitscan"));
	FString ItemError;
	if (!TestTrue(TEXT("the rig item registers"), Rig.ItemsValue.AddDefinition(Item, &ItemError)))
	{
		return true;
	}
	UWeaponComponent* Mount = Rig.Player->GetWeaponMount();
	FString MountError;
	if (!TestTrue(TEXT("the catalogs mount"), Mount->MountCatalogs(&Rig.CatalogValue, &Rig.ItemsValue, &MountError)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the reserve registers"),
		Mount->GetAmmoModel().RegisterAmmoType(FName(TEXT("ammo_rig_feedback")), 120, 60)))
	{
		return true;
	}
	FItemInstance Instance;
	Instance.InstanceId = FGuid(0x0590C000u, 0x0BECu, 0x5335u, 9u);
	Instance.DefinitionId = Item.DefinitionId;
	Instance.RollSeed = 0x05905;
	Instance.Level = 1;
	const FWeaponMountOutcome BindOutcome = Mount->ApplyEquippedWeapon(&Instance);
	if (!TestTrue(TEXT("the production bind succeeds"), BindOutcome.bSucceeded && BindOutcome.bWeaponBound))
	{
		return true;
	}

	// Fire through the real input pair; the delivery must land.
	const float HealthBefore = Rig.TargetHealth->GetHealth();
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);
	TestEqual(TEXT("the production-bound shot commits exactly once"), Mount->GetCommittedShotCount(), 1);
	TestTrue(TEXT("the production-bound shot DELIVERS (the v6 fix: no manual wiring)"),
		Rig.TargetHealth->GetHealth() < HealthBefore);
	TestEqual(TEXT("the delivered shot draws its tracer"), Rig.CountTracers(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// HitscanHitsRealMeleeEnemyPawn
// ---------------------------------------------------------------------------

// M5-060: the v7 retest found every Q shot landing hits=0 blocked=1 against
// the real combat room - the M5-035 rig targets were box components, so the
// weapon path never met a real AMeleeEnemy pawn in any suite. This regression
// puts a REAL melee enemy pawn in the firing lane (production-style bind, no
// manual wiring) and requires the shot to wound it; the manual probe trace
// reports what the engine query sees for diagnosis.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_060HitscanHitsRealMeleeEnemyPawn,
	"UEMMO.Tasks.M5_060.HitscanHitsRealMeleeEnemyPawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_060HitscanHitsRealMeleeEnemyPawn::RunTest(const FString& Parameters)
{
	FM5_058_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}

	// The real enemy stands 400 cm ahead (+X), same depth, settling on the
	// floor. Its controller attaches per the production wiring (SpawnDefault
	// on spawn); the bare test world keeps it idle - a standing target.
	AMeleeEnemy* Enemy = Rig.GetWorld()->SpawnActor<AMeleeEnemy>(AMeleeEnemy::StaticClass(),
		M5_058_SceneBase + FVector(400.0, 0.0, M5_058_PlayerSpawnHeight), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the real melee enemy spawns"), Enemy))
	{
		return true;
	}
	UHealthComponent* EnemyHealth = Enemy->GetHealthComponent();
	if (!TestNotNull(TEXT("the real enemy carries a health component"), EnemyHealth))
	{
		return true;
	}
	Rig.TickFrames(30);
	TestTrue(TEXT("the real enemy settles alive on the floor"), EnemyHealth->IsAlive());
	AddInfo(FString::Printf(TEXT("the enemy settled at %s (player at %s)"),
		*Enemy->GetActorLocation().ToString(), *Rig.Player->GetActorLocation().ToString()));

	// The production-style bind (mount + register + apply, no manual wiring).
	FParsedCombatConfig Parsed;
	Parsed.SchemaVersion = 1;
	FDamageProfile Profile;
	Profile.DamageProfileId = FName(TEXT("dmg_rig_feedback"));
	Profile.BaseDamage = 10.0f;
	Parsed.DamageProfiles.Add(Profile);
	FAmmoType Ammo;
	Ammo.AmmoId = FName(TEXT("ammo_rig_feedback"));
	Ammo.MaxReserve = 120;
	Ammo.MagazineSize = 30;
	Parsed.AmmoTypes.Add(Ammo);
	Parsed.Weapons.Add(FM5_058_Rig::MakeHitscanDefinition(1));
	FString BuildErrors;
	if (!TestTrue(TEXT("the rig catalog builds"),
		FCombatCatalog::BuildFromParsed(Parsed, Rig.CatalogValue, BuildErrors)))
	{
		return true;
	}
	FItemDefinition Item;
	Item.DefinitionId = FName(TEXT("item_rig_real_enemy"));
	Item.DisplayName = TEXT("Real Enemy Rifle");
	Item.Slot = EItemSlot::Weapon;
	Item.Rarity = EItemRarity::Normal;
	Item.WeaponDefinitionId = FName(TEXT("weapon_rig_feedback_hitscan"));
	FString ItemError;
	if (!TestTrue(TEXT("the rig item registers"), Rig.ItemsValue.AddDefinition(Item, &ItemError)))
	{
		return true;
	}
	UWeaponComponent* Mount = Rig.Player->GetWeaponMount();
	FString MountError;
	if (!TestTrue(TEXT("the catalogs mount"), Mount->MountCatalogs(&Rig.CatalogValue, &Rig.ItemsValue, &MountError)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the reserve registers"),
		Mount->GetAmmoModel().RegisterAmmoType(FName(TEXT("ammo_rig_feedback")), 120, 60)))
	{
		return true;
	}
	FItemInstance Instance;
	Instance.InstanceId = FGuid(0x0600D000u, 0x0BECu, 0x5335u, 1u);
	Instance.DefinitionId = Item.DefinitionId;
	Instance.RollSeed = 0x06006;
	Instance.Level = 1;
	const FWeaponMountOutcome BindOutcome = Mount->ApplyEquippedWeapon(&Instance);
	if (!TestTrue(TEXT("the production bind succeeds"), BindOutcome.bSucceeded && BindOutcome.bWeaponBound))
	{
		return true;
	}

	// The manual probe: the same object query the executor runs, reported for
	// diagnosis (what does the engine see on the shot line?).
	{
		FHitResult ProbeHit;
		FCollisionObjectQueryParams ProbeParams(ECC_Pawn);
		ProbeParams.AddObjectTypesToQuery(ECC_WorldDynamic);
		ProbeParams.AddObjectTypesToQuery(ECC_WorldStatic);
		FCollisionQueryParams ProbeQueryParams(SCENE_QUERY_STAT(M5_060_Probe), false);
		ProbeQueryParams.AddIgnoredActor(Rig.Player);
		const FVector ProbeEnd = Rig.ShotOrigin() + FVector(M5_058_HitscanRangeCm, 0.0, 0.0);
		const bool bProbeHit = Rig.GetWorld()->LineTraceSingleByObjectType(ProbeHit, Rig.ShotOrigin(), ProbeEnd, ProbeParams, ProbeQueryParams);
		AddInfo(FString::Printf(TEXT("the probe trace %s: actor=%s component=%s distance=%.1f"),
			bProbeHit ? TEXT("hit") : TEXT("missed"),
			ProbeHit.GetActor() ? *ProbeHit.GetActor()->GetName() : TEXT("none"),
			ProbeHit.GetComponent() ? *ProbeHit.GetComponent()->GetName() : TEXT("none"),
			ProbeHit.Distance));
	}

	// Fire through the real input pair; the real enemy must take the damage.
	const float HealthBefore = EnemyHealth->GetHealth();
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);
	TestEqual(TEXT("the shot commits exactly once"), Mount->GetCommittedShotCount(), 1);
	TestTrue(FString::Printf(TEXT("the shot WOUNDS the real enemy pawn (before=%.1f after=%.1f)"),
		HealthBefore, EnemyHealth->GetHealth()),
		EnemyHealth->GetHealth() < HealthBefore);
	TestEqual(TEXT("the delivered shot draws its tracer"), Rig.CountTracers(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// PointBlankEnemyStillHittable
// ---------------------------------------------------------------------------

// M5-060: the v7 retest pattern - the user fired while the melee grunts were
// AT the player (adjacent, attacking). A line trace whose origin sits INSIDE
// the enemy capsule skips it and flies to the wall behind (blocked=1,
// hits=0, zero damage) - the point-blank miss. This suite pins the fix
// contract: an enemy overlapping the shot origin still takes the hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_060PointBlankEnemyStillHittable,
	"UEMMO.Tasks.M5_060.PointBlankEnemyStillHittable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_060PointBlankEnemyStillHittable::RunTest(const FString& Parameters)
{
	FM5_058_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}

	// The enemy at MELEE range: 70 cm ahead - its capsule (radius ~42) spans
	// roughly 28..112 cm ahead of the player center, so the shot origin
	// (feet + 30 cm) lands INSIDE the enemy capsule.
	AMeleeEnemy* Enemy = Rig.GetWorld()->SpawnActor<AMeleeEnemy>(AMeleeEnemy::StaticClass(),
		M5_058_SceneBase + FVector(70.0, 0.0, M5_058_PlayerSpawnHeight), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the point-blank enemy spawns"), Enemy))
	{
		return true;
	}
	UHealthComponent* EnemyHealth = Enemy->GetHealthComponent();
	if (!TestNotNull(TEXT("the real enemy carries a health component"), EnemyHealth))
	{
		return true;
	}
	Rig.TickFrames(30);
	AddInfo(FString::Printf(TEXT("point-blank enemy at %s (player at %s)"),
		*Enemy->GetActorLocation().ToString(), *Rig.Player->GetActorLocation().ToString()));

	// Production-style bind (the M5-059 seam; no manual wiring).
	FParsedCombatConfig Parsed;
	Parsed.SchemaVersion = 1;
	FDamageProfile Profile;
	Profile.DamageProfileId = FName(TEXT("dmg_rig_feedback"));
	Profile.BaseDamage = 10.0f;
	Parsed.DamageProfiles.Add(Profile);
	FAmmoType Ammo;
	Ammo.AmmoId = FName(TEXT("ammo_rig_feedback"));
	Ammo.MaxReserve = 120;
	Ammo.MagazineSize = 30;
	Parsed.AmmoTypes.Add(Ammo);
	Parsed.Weapons.Add(FM5_058_Rig::MakeHitscanDefinition(1));
	FString BuildErrors;
	if (!TestTrue(TEXT("the rig catalog builds"),
		FCombatCatalog::BuildFromParsed(Parsed, Rig.CatalogValue, BuildErrors)))
	{
		return true;
	}
	FItemDefinition Item;
	Item.DefinitionId = FName(TEXT("item_rig_point_blank"));
	Item.DisplayName = TEXT("Point Blank Rifle");
	Item.Slot = EItemSlot::Weapon;
	Item.Rarity = EItemRarity::Normal;
	Item.WeaponDefinitionId = FName(TEXT("weapon_rig_feedback_hitscan"));
	FString ItemError;
	if (!TestTrue(TEXT("the rig item registers"), Rig.ItemsValue.AddDefinition(Item, &ItemError)))
	{
		return true;
	}
	UWeaponComponent* Mount = Rig.Player->GetWeaponMount();
	FString MountError;
	if (!TestTrue(TEXT("the catalogs mount"), Mount->MountCatalogs(&Rig.CatalogValue, &Rig.ItemsValue, &MountError)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the reserve registers"),
		Mount->GetAmmoModel().RegisterAmmoType(FName(TEXT("ammo_rig_feedback")), 120, 60)))
	{
		return true;
	}
	FItemInstance Instance;
	Instance.InstanceId = FGuid(0x0600D000u, 0x0BECu, 0x5335u, 2u);
	Instance.DefinitionId = Item.DefinitionId;
	Instance.RollSeed = 0x06007;
	Instance.Level = 1;
	const FWeaponMountOutcome BindOutcome = Mount->ApplyEquippedWeapon(&Instance);
	if (!TestTrue(TEXT("the production bind succeeds"), BindOutcome.bSucceeded && BindOutcome.bWeaponBound))
	{
		return true;
	}

	const float HealthBefore = EnemyHealth->GetHealth();
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);
	TestEqual(TEXT("the point-blank shot commits"), Mount->GetCommittedShotCount(), 1);
	TestTrue(FString::Printf(TEXT("the point-blank enemy takes the hit (before=%.1f after=%.1f)"),
		HealthBefore, EnemyHealth->GetHealth()),
		EnemyHealth->GetHealth() < HealthBefore);
	return true;
}

// M5-061: load actual shipped rows; the rig only arranges collision geometry.
static bool M5_061BindShipped(FM5_058_Rig& Rig, FAutomationTestBase& Test, FName WeaponId)
{
	FCombatDataTableSet Tables;
	TArray<FString> Problems;
	if (!Test.TestTrue(TEXT("shipped tables load"), LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data/CombatSystem"), Tables, Problems))) { return false; }
	const FWeaponDefinition* Weapon = Tables.Weapons.Find(WeaponId);
	if (!Test.TestNotNull(TEXT("shipped weapon exists"), Weapon)) { return false; }
	if (!Test.TestTrue(TEXT("ordinary gun fires a physical projectile"), Weapon->FireMode == EWeaponFireMode::Projectile)) { return false; }
	FParsedCombatConfig Parsed;
	Parsed.SchemaVersion = 1;
	for (const auto& Row : Tables.DamageProfiles) { Parsed.DamageProfiles.Add(Row.Value); }
	for (const auto& Row : Tables.AmmoTypes) { Parsed.AmmoTypes.Add(Row.Value); }
	for (const auto& Row : Tables.Projectiles) { Parsed.Projectiles.Add(Row.Value); }
	Parsed.Weapons.Add(*Weapon);
	FString Error;
	if (!Test.TestTrue(TEXT("shipped catalog builds"), FCombatCatalog::BuildFromParsed(Parsed, Rig.CatalogValue, Error))) { return false; }
	FItemDefinition Item;
	Item.DefinitionId = TEXT("bullet_test_item"); Item.DisplayName = TEXT("Bullet");
	Item.WeaponDefinitionId = WeaponId;
	if (!Rig.ItemsValue.AddDefinition(Item, &Error)) { Test.AddError(Error); return false; }
	UWeaponComponent* Mount = Rig.Player->GetWeaponMount();
	if (!Mount->MountCatalogs(&Rig.CatalogValue, &Rig.ItemsValue, &Error)) { Test.AddError(Error); return false; }
	FItemInstance Instance;
	Instance.InstanceId = FGuid::NewGuid(); Instance.DefinitionId = Item.DefinitionId; Instance.Level = 1;
	return Test.TestTrue(TEXT("production binding applies without manual wiring"), Mount->ApplyEquippedWeapon(&Instance).bWeaponBound);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM5_061PhysicalFlight,
	"UEMMO.Tasks.M5_061.PhysicalFlightCollisionAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FM5_061PhysicalFlight::RunTest(const FString& Parameters)
{
	for (const bool bWall : {false, true})
	{
		FM5_058_Rig Rig;
		if (!Rig.Build(*this, true, bWall) || !Rig.Settle(*this) || !M5_061BindShipped(Rig, *this, TEXT("weapon_hitscan_sample"))) { return false; }
		const float Before = Rig.TargetHealth->GetHealth();
		Rig.Player->OnFireInputPressed(); Rig.Player->OnFireInputReleased();
		ACombatProjectile* Bullet = *TActorIterator<ACombatProjectile>(Rig.World);
		if (!TestNotNull(TEXT("one real bullet created on fire"), Bullet)) { return false; }
		TWeakObjectPtr<ACombatProjectile> WeakBullet(Bullet);
		TestEqual(TEXT("no instantaneous hitscan damage"), Rig.TargetHealth->GetHealth(), Before);
		TestEqual(TEXT("no long tracer column"), Rig.CountTracers(), 0);
		const FVector Start = Bullet->GetActorLocation();
		const float Speed = Rig.CatalogValue.FindProjectile(Bullet->Context.ProjectileId)->SpeedCmS;
		Rig.TickFrames(1);
		if (!TestTrue(TEXT("bullet still flying before collision"), WeakBullet.IsValid())) { return false; }
		TestTrue(TEXT("distance is speed times simulation dt"), FMath::IsNearlyEqual(FVector::Dist(Start, Bullet->GetActorLocation()), Speed / 60.0, 0.1));
		TestEqual(TEXT("target stays healthy while bullet is in flight"), Rig.TargetHealth->GetHealth(), Before);
		TestTrue(TEXT("rounded mesh replaces cube"), Bullet->Visual->GetStaticMesh()->GetPathName().Contains(TEXT("/Sphere.Sphere")));
		TestTrue(TEXT("bullet stays small and does not stretch to target"), Bullet->Visual->Bounds.BoxExtent.GetMax() <= 5.0);
		Rig.TickFrames(10);
		TestFalse(TEXT("impact destroys bullet entity"), WeakBullet.IsValid());
		TestEqual(TEXT("wall blocks; open target receives exactly one bullet hit"), Rig.TargetHealth->GetHealth(), Before - (bWall ? 0.0f : 10.0f));
		TestEqual(TEXT("live owner records return to zero"), Rig.Player->GetLiveWeaponProjectileCount(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FM5_061LifetimeReset,
	"UEMMO.Tasks.M5_061.ShotgunEntitiesTimeoutResetAndOwnerDestroy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FM5_061LifetimeReset::RunTest(const FString& Parameters)
{
	for (int32 Cleanup = 0; Cleanup < 3; ++Cleanup)
	{
		FM5_058_Rig Rig;
		if (!Rig.Build(*this, false) || !Rig.Settle(*this) || !M5_061BindShipped(Rig, *this, TEXT("weapon_pellet_sample"))) { return false; }
		Rig.Player->OnFireInputPressed(); Rig.Player->OnFireInputReleased();
		TArray<TWeakObjectPtr<ACombatProjectile>> Bullets;
		TSet<FGuid> Ids;
		for (TActorIterator<ACombatProjectile> It(Rig.World); It; ++It) { Bullets.Add(*It); Ids.Add(It->Context.ProjectileInstanceId); }
		TestEqual(TEXT("five pellets create five separate entities"), Bullets.Num(), 5);
		TestEqual(TEXT("every pellet has a unique identity"), Ids.Num(), 5);
		Rig.TickFrames(3);
		TestTrue(TEXT("volley remains live during flight"), Bullets.Num() == 5 && Bullets[0].IsValid());
		if (Cleanup == 0) { Rig.TickFrames(125); }
		else if (Cleanup == 1) { Rig.Player->ApplyTrainingRoomReset(); }
		else { Rig.Player->Destroy(); }
		for (const auto& Bullet : Bullets) { TestFalse(TEXT("timeout/reset/owner destruction retires every pellet"), Bullet.IsValid()); }
		if (Cleanup != 2) { TestEqual(TEXT("no live owner entries remain"), Rig.Player->GetLiveWeaponProjectileCount(), 0); }
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
