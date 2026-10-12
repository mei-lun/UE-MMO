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
#include "../Combat/HealthComponent.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/HitscanTracer.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundWave.h"
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

#endif // WITH_DEV_AUTOMATION_TESTS
