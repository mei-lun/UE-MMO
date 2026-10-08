// M5-033: the production input -> fire -> delivery wiring tests. The card
// wires the Q/T keys onto the real pawn: Q opens fire when a ranged weapon
// authorizes it (keeping the M1-040 skill-slot intent otherwise), T reloads,
// the press/release edges route single/automatic/burst semantics through the
// M5-022..025 policies, and every committed shot reaches the world through
// the M5-026 hitscan executor or the M5-027/028 projectile service - the
// production pawn damages targets ONLY through the real weapon chain (the
// interface contract section 185 rule: no direct health writes in tests).
//
// The suites run in a real ticked world (the engine FTestWorldWrapper
// precedent from CombatInputFeelTests.cpp). The weapon bind goes through the
// real UWeaponComponent public API (MountCatalogs + ApplyEquippedWeapon) with
// a production-shaped catalog fixture: the production items source carries no
// weapon mappings yet (the M5-019 lineage), so the HUD/profile bind path
// stays a named refusal and the tests drive the same component surface the
// production refresh tail drives.
#include "Misc/AutomationTest.h"

#include "../PrototypeCharacter.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/HealthComponent.h"
#include "../Items/ItemDefinition.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_033
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M5_033_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention). X horizontal, Y
	// depth, Z height; the floor top sits exactly at the base Z.
	const FVector M5_033_SceneBase(150000.0, 46000.0, 600.0);

	const float M5_033_FloorHalfThickness = 100.0f;
	const float M5_033_FloorHalfExtentXY = 4000.0f;
	const float M5_033_PlayerSpawnHeight = 120.0f;

	// The target stands 400 cm ahead of the pawn (facing +X); the shot origin
	// rides the feet offset (30, 0, 50) and the trace flies straight +X, so a
	// box centered at the same height catches every pellet.
	const FVector M5_033_TargetOffset(400.0, 0.0, 50.0);
	const FVector M5_033_TargetBoxHalfExtent(50.0, 50.0, 60.0);

	// The input-layer reload duration constant (the card's T semantics; no
	// reload-duration field exists in the M5-004 schema).
	constexpr double M5_033_ReloadDurationSeconds = 2.0;

	// Weapon fixture values: 120 rpm = 0.5 s cooldown (the hold test), 450 rpm
	// = 0.1333 s (the burst test), and a straight 3000 cm/s linear projectile.
	constexpr float M5_033_AutoRpm = 120.0f;
	constexpr float M5_033_BurstRpm = 450.0f;
	constexpr float M5_033_ProjectileSpeedCmS = 3000.0f;
	constexpr float M5_033_HitscanRangeCm = 5000.0f;

	// World-static blocking floor box the pawn stands on (temp worlds ship no
	// geometry; the M1-041 precedent verbatim).
	static AActor* M5_033_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M5_033_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_033_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M5_033_FloorHalfExtentXY, M5_033_FloorHalfExtentXY, M5_033_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M5_033_FloorHalfThickness));
		return Actor;
	}

	// A bare hostile target: a world-static box plus a real health pool (the
	// unified entry's Stage 1 ignores bodies without one). Registered into the
	// pawn's fire registry lazily by the pawn's own target-identity resolver
	// on the first hit - exactly the production path.
	static AActor* M5_033_SpawnTarget(UWorld& World, const FVector& Base, UHealthComponent** OutHealth)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base + M5_033_TargetOffset, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_033_TargetBody"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_033_TargetBoxHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		Body->SetWorldLocation(Base + M5_033_TargetOffset);
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_033_TargetHealth"));
		Health->RegisterComponent();
		if (OutHealth != nullptr)
		{
			*OutHealth = Health;
		}
		return Actor;
	}

	// One full rig: the wrapper owns the manually ticked temp world, the rig
	// owns the catalog VALUES mounted into the real pawn's weapon component,
	// and the shared test clock feeds both the pawn's input layer and the
	// mount's fire clock (deterministic pacing).
	struct FM5_033_Rig
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		AActor* Target = nullptr;
		UHealthComponent* TargetHealth = nullptr;
		FCombatCatalog CatalogValue;
		FItemDefinitionCatalog ItemsValue;
		FGuid BoundInstanceId;
		double ClockSeconds = 100.0;

		bool Build(FAutomationTestBase& Test, bool bWithTarget)
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
			if (M5_033_SpawnFloor(*World, M5_033_SceneBase) == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			World->EnsureCollisionTreeIsBuilt();
			FActorSpawnParameters PlayerParams;
			Player = World->SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(),
				M5_033_SceneBase + FVector(0.0, 0.0, M5_033_PlayerSpawnHeight), FRotator::ZeroRotator, PlayerParams);
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
				Target = M5_033_SpawnTarget(*World, M5_033_SceneBase, &TargetHealth);
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

		// Ticks the world until the pawn settled onto the floor (the M3-031
		// settle pattern) so the feet origin and the facing are final.
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
			for (int32 Frame = 0; Frame < 300; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks (settle)"), Wrapper.TickTestWorld(M5_033_FrameSeconds)))
				{
					return false;
				}
				ClockSeconds += M5_033_FrameSeconds;
				if (Movement != nullptr && Movement->MovementMode == MOVE_Walking && Movement->Velocity.Size() < 1.0f)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the pawn never settled onto the floor"));
			return false;
		}

		// Builds a production-shaped catalog (one damage profile, the weapon,
		// its ammo and - when referenced - its projectile), mounts it into the
		// pawn's real weapon component, pre-seeds the shared reserve and binds
		// a real item instance through the production bind entry.
		bool BindWeapon(FAutomationTestBase& Test, const FWeaponDefinition& WeaponDef)
		{
			FParsedCombatConfig Parsed;
			Parsed.SchemaVersion = 1;
			FDamageProfile Profile;
			Profile.DamageProfileId = FName(TEXT("dmg_rig_rifle"));
			Profile.BaseDamage = 10.0f;
			Parsed.DamageProfiles.Add(Profile);
			FAmmoType Ammo;
			Ammo.AmmoId = WeaponDef.AmmoId;
			Ammo.MaxReserve = 120;
			Ammo.MagazineSize = WeaponDef.MagazineSize;
			Parsed.AmmoTypes.Add(Ammo);
			if (!WeaponDef.ProjectileId.IsNone())
			{
				FProjectileDefinition Projectile;
				Projectile.ProjectileId = WeaponDef.ProjectileId;
				Projectile.Motion = EProjectileMotion::Straight;
				Projectile.SpeedCmS = M5_033_ProjectileSpeedCmS;
				Projectile.LifetimeS = 5.0f;
				Projectile.DamageProfileId = Profile.DamageProfileId;
				Projectile.PierceCount = 0;
				Parsed.Projectiles.Add(Projectile);
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
			Item.DefinitionId = FName(TEXT("item_rig_rifle"));
			Item.DisplayName = TEXT("Rig Rifle");
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
			// The reserve is pre-seeded so the bind's own zero-reserve
			// registration is a refused duplicate (the M5AutomaticFireTests
			// fixture pattern): the shared pool keeps its 60 rounds.
			if (!Test.TestTrue(TEXT("the rig reserve ammo registers"),
				Mount->GetAmmoModel().RegisterAmmoType(WeaponDef.AmmoId, 120, 60)))
			{
				return false;
			}
			FItemInstance Instance;
			Instance.InstanceId = FGuid(0x0330A000u, 0x0BECu, 0x5331u, 1u);
			Instance.DefinitionId = Item.DefinitionId;
			Instance.RollSeed = 0x52033;
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
			return Test.TestTrue(TEXT("the bound mount authorizes fire"), Mount->IsFireAuthorized());
		}

		// Advances the world and the shared rig clock one frame at a time.
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Wrapper.TickTestWorld(M5_033_FrameSeconds))
				{
					break;
				}
				ClockSeconds += M5_033_FrameSeconds;
			}
		}

		int32 CommittedShots() const
		{
			return Player != nullptr && Player->GetWeaponMount() != nullptr
				? Player->GetWeaponMount()->GetCommittedShotCount()
				: 0;
		}
	};

	// The four production-shaped weapon definitions the suites bind.
	static FWeaponDefinition M5_033_MakeAutoDefinition()
	{
		FWeaponDefinition Weapon;
		Weapon.WeaponId = FName(TEXT("weapon_rig_auto"));
		Weapon.FireMode = EWeaponFireMode::Hitscan;
		Weapon.DamageProfileId = FName(TEXT("dmg_rig_rifle"));
		Weapon.AmmoId = FName(TEXT("ammo_rig_cell"));
		Weapon.MagazineSize = 12;
		Weapon.FireRateRpm = M5_033_AutoRpm;
		Weapon.BurstCount = 1;
		Weapon.PelletCount = 1;
		Weapon.SpreadDegrees = 0.0f;
		Weapon.RangeCm = M5_033_HitscanRangeCm;
		return Weapon;
	}

	static FWeaponDefinition M5_033_MakeBurstDefinition()
	{
		FWeaponDefinition Weapon = M5_033_MakeAutoDefinition();
		Weapon.WeaponId = FName(TEXT("weapon_rig_burst"));
		Weapon.FireRateRpm = M5_033_BurstRpm;
		Weapon.BurstCount = 3;
		Weapon.MagazineSize = 15;
		return Weapon;
	}

	static FWeaponDefinition M5_033_MakeProjectileDefinition()
	{
		FWeaponDefinition Weapon = M5_033_MakeAutoDefinition();
		Weapon.WeaponId = FName(TEXT("weapon_rig_projectile"));
		Weapon.FireMode = EWeaponFireMode::Projectile;
		Weapon.DamageProfileId = FName(NAME_None);
		Weapon.ProjectileId = FName(TEXT("bullet_rig_linear"));
		Weapon.RangeCm = 0.0f;
		return Weapon;
	}
}

using namespace UE::UEMMO::Tasks::M5_033;

// ---------------------------------------------------------------------------
// NoWeaponQKeepsSkillIntent
// ---------------------------------------------------------------------------

// Without a bound ranged weapon the Q press keeps the M1-040 skill-slot
// intent exactly as before: the slot counter increments, the mount commits
// nothing and no fire bookkeeping appears.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_033NoWeaponQKeepsSkillIntent,
	"UEMMO.Tasks.M5_033.NoWeaponQKeepsSkillIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_033NoWeaponQKeepsSkillIntent::RunTest(const FString& Parameters)
{
	FM5_033_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false))
	{
		return true;
	}
	if (!Rig.Settle(*this))
	{
		return true;
	}

	TestFalse(TEXT("the unbound mount authorizes no fire"), Rig.Player->GetWeaponMount()->IsFireAuthorized());
	const int32 PressesBefore = Rig.Player->GetSkillSlotPressCount(1);

	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();

	TestEqual(TEXT("the Q press lands as the skill-slot 1 intent"),
		Rig.Player->GetSkillSlotPressCount(1), PressesBefore + 1);
	TestEqual(TEXT("the unbound mount commits no shot"), Rig.CommittedShots(), 0);
	TestFalse(TEXT("the release leaves no held trigger"), Rig.Player->IsWeaponFireHeld());

	// T without a weapon is inert: no reload window opens, nothing changes.
	Rig.Player->OnReloadInputPressed();
	Rig.TickFrames(1);
	TestEqual(TEXT("the T press without a weapon commits nothing"), Rig.CommittedShots(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// HitscanShotDamagesTarget
// ---------------------------------------------------------------------------

// One Q press on a bound hitscan weapon damages the target through the whole
// real chain: input -> TryFire commit -> the delivery bridge's shot-pattern
// plan -> the M5-026 executor -> the unified entry. The test never writes
// health directly; the pool drop is the proof.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_033HitscanShotDamagesTarget,
	"UEMMO.Tasks.M5_033.HitscanShotDamagesTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_033HitscanShotDamagesTarget::RunTest(const FString& Parameters)
{
	FM5_033_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_033_MakeAutoDefinition()))
	{
		return true;
	}
	if (!TestNotNull(TEXT("the target carries a health pool"), Rig.TargetHealth))
	{
		return true;
	}

	const float HealthBefore = Rig.TargetHealth->GetHealth();
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);

	TestEqual(TEXT("the Q press commits exactly one shot"), Rig.CommittedShots(), 1);
	TestTrue(TEXT("the target's pool dropped through the unified entry"),
		Rig.TargetHealth->GetHealth() < HealthBefore);
	return true;
}

// ---------------------------------------------------------------------------
// HoldAutomaticFirePacesAndStopsOnRelease
// ---------------------------------------------------------------------------

// Holding Q polls TryFire once per frame with the same-frame ReleaseFire; the
// component's fire-rate cooldown paces the stream (120 rpm = 0.5 s), and the
// release stops it with no stray shot afterwards.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_033HoldAutomaticFirePacesAndStopsOnRelease,
	"UEMMO.Tasks.M5_033.HoldAutomaticFirePacesAndStopsOnRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_033HoldAutomaticFirePacesAndStopsOnRelease::RunTest(const FString& Parameters)
{
	FM5_033_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_033_MakeAutoDefinition()))
	{
		return true;
	}

	Rig.Player->OnFireInputPressed();
	TestTrue(TEXT("the press holds the trigger"), Rig.Player->IsWeaponFireHeld());
	TestEqual(TEXT("the press edge fires the first shot immediately"), Rig.CommittedShots(), 1);
	Rig.TickFrames(60);
	// One second of hold at 120 rpm (a 0.5 s cooldown): the press shot plus
	// the +0.5 s follow-up, with the +1.0 s boundary shot landing only when
	// the accumulated frame clock reaches the interval exactly (2..3 total).
	const int32 HeldShots = Rig.CommittedShots();
	TestTrue(TEXT("one held second paces 2..3 shots at 120 rpm"), HeldShots >= 2 && HeldShots <= 3);

	Rig.Player->OnFireInputReleased();
	TestFalse(TEXT("the release drops the trigger"), Rig.Player->IsWeaponFireHeld());
	Rig.TickFrames(60);
	TestEqual(TEXT("the release stops the stream with no stray shot"), Rig.CommittedShots(), HeldShots);
	return true;
}

// ---------------------------------------------------------------------------
// BurstOnePressFiresOneBurst
// ---------------------------------------------------------------------------

// One press edge arms exactly one burst of three: the sub-shots pace through
// the component cooldown, a held release never extends it, and a fresh press
// after completion re-arms.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_033BurstOnePressFiresOneBurst,
	"UEMMO.Tasks.M5_033.BurstOnePressFiresOneBurst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_033BurstOnePressFiresOneBurst::RunTest(const FString& Parameters)
{
	FM5_033_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_033_MakeBurstDefinition()))
	{
		return true;
	}

	// Press + immediate release: the started burst runs to its end.
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(30);
	TestEqual(TEXT("one press edge fires exactly one three-round burst"), Rig.CommittedShots(), 3);

	// A fresh press edge after the burst completed re-arms.
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(30);
	TestEqual(TEXT("a fresh press re-arms the burst"), Rig.CommittedShots(), 6);
	return true;
}

// ---------------------------------------------------------------------------
// ReloadTRefillsMagazineAfterDuration
// ---------------------------------------------------------------------------

// T opens the reload window through the component's real reload entry, Q
// during the window refuses, and the deadline poll completes the reload: the
// magazine moves from the shared reserve back to full.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_033ReloadTRefillsMagazineAfterDuration,
	"UEMMO.Tasks.M5_033.ReloadTRefillsMagazineAfterDuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_033ReloadTRefillsMagazineAfterDuration::RunTest(const FString& Parameters)
{
	FM5_033_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_033_MakeAutoDefinition()))
	{
		return true;
	}
	UWeaponComponent* Mount = Rig.Player->GetWeaponMount();
	const FWeaponBindingRecord* Binding = Mount->GetActiveBinding();
	if (!TestTrue(TEXT("the active binding is available"), Binding != nullptr))
	{
		return true;
	}
	const int32 Capacity = Binding->MagazineCapacity;

	// Spend one round, then reload through T.
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(1);
	const FWeaponBindingRecord* AfterShot = Mount->GetActiveBinding();
	if (!TestTrue(TEXT("the binding survives the shot"), AfterShot != nullptr))
	{
		return true;
	}
	TestEqual(TEXT("the shot consumed one magazine round"), AfterShot->LoadedRounds, Capacity - 1);

	Rig.Player->OnReloadInputPressed();
	TestTrue(TEXT("the T press opens the reload window"), Rig.Player->IsWeaponReloadPending());

	// While the window is open Q refuses (Reloading).
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(1);
	TestEqual(TEXT("the mid-reload Q press fires nothing"), Rig.CommittedShots(), 1);

	// The deadline poll completes the reload: the magazine refills from the
	// shared reserve.
	Rig.TickFrames(135);
	TestFalse(TEXT("the deadline poll closed the reload window"), Rig.Player->IsWeaponReloadPending());
	const FWeaponBindingRecord* AfterReload = Mount->GetActiveBinding();
	if (!TestTrue(TEXT("the binding survives the reload"), AfterReload != nullptr))
	{
		return true;
	}
	TestEqual(TEXT("the reload refilled the magazine to full"), AfterReload->LoadedRounds, Capacity);

	// Firing works again after the refill.
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(1);
	TestEqual(TEXT("the post-reload Q press fires again"), Rig.CommittedShots(), 2);
	return true;
}

// ---------------------------------------------------------------------------
// DeadPawnDoesNotFire
// ---------------------------------------------------------------------------

// A dead pawn's Q press fires nothing: the death teardown closed the mount's
// fire authorization, and the input layer keeps no held trigger across it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_033DeadPawnDoesNotFire,
	"UEMMO.Tasks.M5_033.DeadPawnDoesNotFire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_033DeadPawnDoesNotFire::RunTest(const FString& Parameters)
{
	FM5_033_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ false) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_033_MakeAutoDefinition()))
	{
		return true;
	}

	Rig.Player->GetHealth()->ApplyDamage(999.0f);
	Rig.TickFrames(2);
	TestFalse(TEXT("the death closed the fire authorization"),
		Rig.Player->GetWeaponMount()->IsFireAuthorized());

	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(2);
	TestEqual(TEXT("the dead pawn's Q press fires nothing"), Rig.CommittedShots(), 0);
	TestFalse(TEXT("the dead pawn holds no trigger"), Rig.Player->IsWeaponFireHeld());
	return true;
}

// ---------------------------------------------------------------------------
// ProjectileWeaponSpawnsAndDamagesTarget
// ---------------------------------------------------------------------------

// The projectile delivery bridge: one Q press reserves and commits the real
// pellet actor through the M5-027 world service, the pawn's per-tick motion
// advance flies it, and the M5-028 linear policy lands the unified hit on the
// target - the pool drop is the end-to-end proof.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_033ProjectileWeaponSpawnsAndDamagesTarget,
	"UEMMO.Tasks.M5_033.ProjectileWeaponSpawnsAndDamagesTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_033ProjectileWeaponSpawnsAndDamagesTarget::RunTest(const FString& Parameters)
{
	FM5_033_Rig Rig;
	if (!Rig.Build(*this, /*bWithTarget*/ true) || !Rig.Settle(*this))
	{
		return true;
	}
	if (!Rig.BindWeapon(*this, M5_033_MakeProjectileDefinition()))
	{
		return true;
	}
	if (!TestNotNull(TEXT("the target carries a health pool"), Rig.TargetHealth))
	{
		return true;
	}

	const float HealthBefore = Rig.TargetHealth->GetHealth();
	Rig.Player->OnFireInputPressed();
	Rig.Player->OnFireInputReleased();
	Rig.TickFrames(1);
	TestEqual(TEXT("the Q press commits exactly one shot"), Rig.CommittedShots(), 1);
	TestEqual(TEXT("the committed shot bound one live pellet actor"),
		Rig.Player->GetLiveWeaponProjectileCount(), 1);

	// 3000 cm/s over ~370 cm: a couple of frames of motion suffice; the cap
	// covers physics registration latency.
	bool bHit = false;
	for (int32 Frame = 0; Frame < 120 && !bHit; ++Frame)
	{
		Rig.TickFrames(1);
		bHit = Rig.TargetHealth->GetHealth() < HealthBefore;
	}
	TestTrue(TEXT("the flying pellet damaged the target through the unified entry"), bHit);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
