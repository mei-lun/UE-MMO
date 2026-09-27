// M3-010: equipment and growth wired into the local combat attributes. The
// profile snapshot's final stats (level base + equipped bonus, M3-005/M3-006)
// become the pawn's combat attributes: MaxHP onto the UHealthComponent bound
// (no-heal clamp), Attack/Defense into the M1-019 damage formula (the
// attacker's AttackPower plus the VICTIM's Defense read from the target's
// combat component). Entering a room (an accepted StartRoom) re-loads the
// snapshot and restores the pool to the CURRENT max; ordinary equipment
// changes never heal; equip/unequip requests are refused while a run is
// Running.
//
// Harness note: the engine FTestWorldWrapper precedent (GameCombatWiringTests/
// EnemyTelegraphTests) - a manually ticked temp world with real actor ticks,
// real walking physics and real collision. The wrapper's Game world carries a
// REAL UGameInstance (created and Init'ed by the engine wrapper), so the
// production UProfileSubsystem resolution and the run-start binding run
// exactly like the game's. The profile is configured between world creation
// and play begin, so the player's BeginPlay loads it; the test only presses
// keys through the production intent entry SubmitCombatInput and calls the
// production equip entry TryEquipStatBonus.
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"
#include "../Items/ItemDefinition.h"
#include "../Profile/ProfileSubsystem.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Templates/Function.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_010
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_010_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with the other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M3_010_SceneBase(86000.0, 45000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_010_FloorHalfThickness = 100.0f;
	const float M3_010_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: characters drop a short distance and
	// settle onto the floor through real physics.
	const float M3_010_PlayerSpawnHeight = 120.0f;
	const float M3_010_EnemySpawnHeight = 90.0f;

	// The light_01 hit box offset X=95 and half extent X=85 (M1-009 assets)
	// cover base.X+10..base.X+180 for facing +1, so the enemy feet anchor at
	// +95 stays reachable.
	const float M3_010_EnemyFeetOffsetX = 95.0f;

	// Frame caps: the settle phase waits for the ground contact of both
	// characters; the hit wait covers a full press + light_01 start + active
	// window several times over.
	constexpr int32 M3_010_MaxSettleFrames = 300;
	constexpr int32 M3_010_MaxHitWaitFrames = 600;

	// The damage formula (Docs/01 section 8.2, M1-019): damage =
	// max(1, round((baseDamage + AttackPower * coefficient) * 100 / (100 +
	// max(0, Defense)))). light_01 carries baseDamage 10 with coefficient 1.0
	// (Data/combat-attacks.json), so with Defense 0: Attack 0 -> 10,
	// Attack 5 -> 15. UE's RoundToFloat is FloorToDouble(x + 0.5) (half away
	// from zero for positive values), so with Defense 100: 10*100/200 = 5
	// exactly, and 15*100/200 = 7.5 rounds UP to 8.
	constexpr float M3_010_BaseDamageLight01 = 10.0f;
	constexpr float M3_010_DamageAttack5 = 15.0f;
	constexpr float M3_010_DamageDefense100Base = 5.0f;
	constexpr float M3_010_DamageDefense100Attack5 = 8.0f;

	// World-static blocking floor the characters stand on (temp worlds ship
	// no geometry; the walking physics needs one blocker). The standard
	// BlockAll profile is the collision a real level floor carries.
	static AActor* M3_010_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M3_010_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M3_010_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M3_010_FloorHalfExtentXY, M3_010_FloorHalfExtentXY, M3_010_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-022 pattern).
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M3_010_FloorHalfThickness));
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top and enables
	// no-controller physics so it settles like a possessed game pawn (the
	// M1-041 two-step activation).
	static APrototypeCharacter* M3_010_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M3_010_SceneBase + FVector(0.0, 0.0, M3_010_PlayerSpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Player == nullptr)
		{
			return nullptr;
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
		return Player;
	}

	// Spawns the real training enemy at the shared feet anchor (the M1-041
	// placement convention).
	static ATrainingEnemy* M3_010_SpawnEnemy(UWorld& World)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(),
			M3_010_SceneBase + FVector(M3_010_EnemyFeetOffsetX, 0.0, M3_010_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Enemy == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
		}
		return Enemy;
	}

	// Minimal one-wave room definition double (the M2-011 suite precedent);
	// the run tests only call StartRoom (no BeginWaves, no enemies spawned).
	static URoomDefinition* M3_010_MakeRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m3_010_stats"));
		Room->RewardTableId = FName(TEXT("reward_m3_010"));
		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 1;
		Wave0.SpawnLocations.Add(M3_010_SceneBase + FVector(-300.0, 0.0, M3_010_EnemySpawnHeight));
		Room->Waves.Add(Wave0);
		return Room;
	}

	// One full scene: the wrapper owns the manually ticked temp world; the
	// tests configure the profile, press keys and read the results.
	struct FM3_010_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		UHealthComponent* PlayerHealth = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		UProfileSubsystem* Profile = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		TArray<float> HitDamages;

		/**
		 * Builds world + floor + player + enemy. ProfileSetup (may be null)
		 * runs BETWEEN world creation and play begin on the wrapper's real
		 * game instance profile subsystem, so the player's BeginPlay loads
		 * exactly the configured profile (NewProfile + SetEquippedStatBonus
		 * through the production entries). Binds the hit recorder.
		 * Returns false after reporting the problem so the caller can bail.
		 */
		bool Build(FAutomationTestBase& Test, const TCHAR* Tag, const TFunction<void(UProfileSubsystem&)>& ProfileSetup)
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
			// The wrapper's game world carries a real, initialized game
			// instance - the production subsystem resolution source.
			Profile = World->GetGameInstance() ? World->GetGameInstance()->GetSubsystem<UProfileSubsystem>() : nullptr;
			if (ProfileSetup != nullptr)
			{
				if (!Test.TestNotNull(TEXT("the profile subsystem exists in the test game instance"), Profile))
				{
					return false;
				}
				ProfileSetup(*Profile);
			}
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			if (M3_010_SpawnFloor(*World, M3_010_SceneBase) == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load pass that normally
			// builds the scene collision tree; build it once before the ticks.
			World->EnsureCollisionTreeIsBuilt();
			Player = M3_010_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerCombat = Player->GetCombat();
			PlayerHealth = Player->GetHealth();
			if (!Test.TestTrue(TEXT("the player carries combat and health components"),
				PlayerCombat != nullptr && PlayerHealth != nullptr))
			{
				return false;
			}
			Enemy = M3_010_SpawnEnemy(*World);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}
			PlayerCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				HitDamages.Add(Hit.Damage);
			});
			return true;
		}

		// Ticks the world until both characters settled onto the floor. The
		// settle also gives the player's own Tick the frames it needs to
		// inject the input clock (the M1-041 wiring pivot).
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
			for (int32 Frame = 0; Frame < M3_010_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_010_FrameSeconds)))
				{
					return false;
				}
				const bool bPlayerGrounded = PlayerMovement != nullptr
					&& PlayerMovement->MovementMode == MOVE_Walking
					&& PlayerMovement->Velocity.Size() < 1.0f;
				const bool bEnemyGrounded = EnemyMovement != nullptr
					&& EnemyMovement->MovementMode == MOVE_Walking
					&& EnemyMovement->Velocity.Size() < 1.0f;
				if (bPlayerGrounded && bEnemyGrounded)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the characters never settled onto the floor within the settle cap"));
			return false;
		}

		float EnemyHealth() const
		{
			const UHealthComponent* EnemyHealth = Enemy->GetHealthComponent();
			return (EnemyHealth != nullptr) ? EnemyHealth->GetHealth() : -1.0f;
		}

		/**
		 * Presses Light ONCE through the production intent entry (scheduled
		 * 0.10 s after settle, the M1-041 convention) and ticks until the
		 * first hit is confirmed. OutDamage/OutEnemyHpAfterHit receive the
		 * applied damage and the enemy pool read in the hit's own frame.
		 */
		bool LightPressAndWaitForHit(FAutomationTestBase& Test, float& OutDamage, float& OutEnemyHpAfterHit)
		{
			const double PressAt = World->GetTimeSeconds() + 0.10;
			bool bPressed = false;
			for (int32 Frame = 0; Frame < M3_010_MaxHitWaitFrames; ++Frame)
			{
				if (!bPressed && World->GetTimeSeconds() + 1e-6 >= PressAt)
				{
					Player->SubmitCombatInput(ECombatInput::Light);
					bPressed = true;
				}
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_010_FrameSeconds)))
				{
					return false;
				}
				if (HitDamages.Num() >= 1)
				{
					OutDamage = HitDamages[0];
					OutEnemyHpAfterHit = EnemyHealth();
					return true;
				}
			}
			Test.AddError(FString::Printf(TEXT(
				"the light press never landed a hit within the wait cap (started=%d)"),
				PlayerCombat->GetSnapshot().InstanceId != 0 ? 1 : 0));
			return false;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_010;

// Formula regression baseline with the EMPTY equipment row: a level-1 profile
// (snapshot 100/0/0) must wire Attack 0 into the formula, so the light_01 hit
// deducts exactly the definition base damage 10 - the pre-M3-010 result kept
// verbatim, now produced THROUGH the wired profile stats.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010EmptyEquipmentBaseAttackDeductsTen,
	"UEMMO.Tasks.M3_010.EmptyEquipmentBaseAttackDeductsTen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010EmptyEquipmentBaseAttackDeductsTen::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("Base"), [](UProfileSubsystem& Profile)
	{
		Profile.NewProfile();
	}))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	// The level-1 snapshot (100/0/0) is wired: MaxHealth 100 and zero growth
	// attributes.
	TestEqual(TEXT("the level-1 profile wired MaxHealth 100"), Scene.PlayerHealth->GetMaxHealth(), 100.0f, 0.01f);
	TestEqual(TEXT("the level-1 profile wired Attack 0"), Scene.PlayerCombat->GetAttackPower(), 0.0f, 0.01f);
	TestEqual(TEXT("the level-1 profile wired Defense 0"), Scene.PlayerCombat->GetDefense(), 0.0f, 0.01f);

	float Damage = -1.0f;
	float EnemyHpAfter = -1.0f;
	if (!Scene.LightPressAndWaitForHit(*this, Damage, EnemyHpAfter))
	{
		return true;
	}
	TestEqual(TEXT("the empty-equipment light_01 hit deducts exactly the base damage 10"),
		Damage, M3_010_BaseDamageLight01, 0.01f);
	TestEqual(TEXT("the enemy pool dropped from 100 to 90"), EnemyHpAfter, 90.0f, 0.01f);
	return true;
}

// The +5 equipment scenario: the SAME attack (light_01) on the same
// Defense-0 target deducts 15 instead of 10 - the whole +5 attack bonus of
// the equipped stat row flows into the damage formula.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010EquippedAttackBonusDeductsFifteen,
	"UEMMO.Tasks.M3_010.EquippedAttackBonusDeductsFifteen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010EquippedAttackBonusDeductsFifteen::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("Attack5"), [](UProfileSubsystem& Profile)
	{
		Profile.NewProfile();
		FItemStats Bonus;
		Bonus.Attack = 5.0f;
		Profile.SetEquippedStatBonus(Bonus);
	}))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	// The snapshot (100/5/0) is wired: MaxHealth stays the snapshot MaxHP 100
	// (the equipment granted no MaxHP) and the attack bonus is live.
	TestEqual(TEXT("the equipped snapshot wired MaxHealth 100"), Scene.PlayerHealth->GetMaxHealth(), 100.0f, 0.01f);
	TestEqual(TEXT("the equipped snapshot wired Attack 5"), Scene.PlayerCombat->GetAttackPower(), 5.0f, 0.01f);
	TestEqual(TEXT("the equipped snapshot wired Defense 0"), Scene.PlayerCombat->GetDefense(), 0.0f, 0.01f);

	float Damage = -1.0f;
	float EnemyHpAfter = -1.0f;
	if (!Scene.LightPressAndWaitForHit(*this, Damage, EnemyHpAfter))
	{
		return true;
	}
	TestEqual(TEXT("the same light_01 hit with Attack+5 deducts exactly 15"),
		Damage, M3_010_DamageAttack5, 0.01f);
	TestEqual(TEXT("the enemy pool dropped from 100 to 85"), EnemyHpAfter, 85.0f, 0.01f);
	return true;
}

// Defense 100 halves the incoming damage per the formula (10 * 100 / 200 = 5
// exactly). The defender's Defense comes from the VICTIM's combat component
// (the enemy here carries one and is injected directly).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010Defense100HalvesBaseDamagePerFormula,
	"UEMMO.Tasks.M3_010.Defense100HalvesBaseDamagePerFormula",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010Defense100HalvesBaseDamagePerFormula::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("Def100"), [](UProfileSubsystem& Profile)
	{
		Profile.NewProfile();
	}))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	// The victim's defense: the enemy's own combat component (the same
	// surface an enemy profile would feed; the game enemies stay at 0).
	UCombatComponent* EnemyCombat = Scene.Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("the training enemy carries a combat component"), EnemyCombat))
	{
		return true;
	}
	EnemyCombat->SetCombatStats(0.0f, 100.0f);

	float Damage = -1.0f;
	float EnemyHpAfter = -1.0f;
	if (!Scene.LightPressAndWaitForHit(*this, Damage, EnemyHpAfter))
	{
		return true;
	}
	TestEqual(TEXT("the light_01 hit against Defense 100 deducts exactly the halved damage 5"),
		Damage, M3_010_DamageDefense100Base, 0.01f);
	TestEqual(TEXT("the enemy pool dropped from 100 to 95"), EnemyHpAfter, 95.0f, 0.01f);
	return true;
}

// The rounding convention, recorded per the card ("first measure the current
// formula, then assert"): with Attack 5 against Defense 100 the raw result
// is (10+5)*100/200 = 7.5, and UE RoundToFloat (FloorToDouble(x + 0.5)) is
// half-away-from-zero for positive values, so the applied damage is 8.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010Defense100WithAttack5RoundsToEight,
	"UEMMO.Tasks.M3_010.Defense100WithAttack5RoundsToEight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010Defense100WithAttack5RoundsToEight::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("Def100Atk5"), [](UProfileSubsystem& Profile)
	{
		Profile.NewProfile();
		FItemStats Bonus;
		Bonus.Attack = 5.0f;
		Profile.SetEquippedStatBonus(Bonus);
	}))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	UCombatComponent* EnemyCombat = Scene.Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("the training enemy carries a combat component"), EnemyCombat))
	{
		return true;
	}
	EnemyCombat->SetCombatStats(0.0f, 100.0f);

	float Damage = -1.0f;
	float EnemyHpAfter = -1.0f;
	if (!Scene.LightPressAndWaitForHit(*this, Damage, EnemyHpAfter))
	{
		return true;
	}
	TestEqual(TEXT("the light_01 hit with Attack 5 against Defense 100 rounds the 7.5 raw damage UP to 8"),
		Damage, M3_010_DamageDefense100Attack5, 0.01f);
	TestEqual(TEXT("the enemy pool dropped from 100 to 92"), EnemyHpAfter, 92.0f, 0.01f);
	return true;
}

// Room entry matches the profile snapshot, a new run restores the pool to the
// CURRENT max, and an equipment change between runs re-applies the stats
// without healing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010RoomEntryMatchesSnapshotAndNewRunRestoresHealth,
	"UEMMO.Tasks.M3_010.RoomEntryMatchesSnapshotAndNewRunRestoresHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010RoomEntryMatchesSnapshotAndNewRunRestoresHealth::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("RoomEntry"), [](UProfileSubsystem& Profile)
	{
		Profile.NewProfile();
		FItemStats Bonus;
		Bonus.Attack = 5.0f;
		Bonus.MaxHP = 20.0f;
		Profile.SetEquippedStatBonus(Bonus);
	}))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	// The equipped snapshot (120/5/0) is wired at spawn: MaxHealth is the
	// snapshot MaxHP 120 (level base 100 + equipment 20), Attack 5.
	TestEqual(TEXT("the snapshot MaxHP 120 became the HealthComponent max"), Scene.PlayerHealth->GetMaxHealth(), 120.0f, 0.01f);
	TestEqual(TEXT("the spawn pool starts full at the snapshot max"), Scene.PlayerHealth->GetHealth(), 120.0f, 0.01f);
	TestEqual(TEXT("the snapshot Attack 5 is wired"), Scene.PlayerCombat->GetAttackPower(), 5.0f, 0.01f);
	TestEqual(TEXT("the snapshot Defense 0 is wired"), Scene.PlayerCombat->GetDefense(), 0.0f, 0.01f);

	// Damage the pawn, then grow the equipment row between runs: the higher
	// max applies, but the pool is NOT healed (min(CurrentHP, NewMax)). The
	// equip entry REPLACES the whole row (the M3-005 SetEquippedStatBonus
	// semantics), so the raised row carries MaxHP 50: the level-1 base 100
	// plus 50 is the new snapshot MaxHP 150.
	TestEqual(TEXT("the setup damage removes exactly 70"),
		Scene.PlayerHealth->ApplyDamage(70.0f), 70.0f, 0.01f);
	TestEqual(TEXT("the pool sits at 50 after the damage"), Scene.PlayerHealth->GetHealth(), 50.0f, 0.01f);
	FItemStats BiggerBonus;
	BiggerBonus.Attack = 5.0f;
	BiggerBonus.MaxHP = 50.0f;
	TestTrue(TEXT("the equipment change outside a run is accepted"),
		Scene.Player->TryEquipStatBonus(BiggerBonus));
	TestEqual(TEXT("the raised equipment wired the new max 150"), Scene.PlayerHealth->GetMaxHealth(), 150.0f, 0.01f);
	TestEqual(TEXT("the raised max never healed the pool (still 50)"), Scene.PlayerHealth->GetHealth(), 50.0f, 0.01f);

	// Entering the room: StartRoom re-loads the snapshot AND restores the
	// pool to the CURRENT max (150, not the old 120 or the damaged 50).
	URoomDefinition* Room = M3_010_MakeRoom();
	if (!TestTrue(TEXT("the room run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the session is Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the run start re-applied the snapshot max 150"), Scene.PlayerHealth->GetMaxHealth(), 150.0f, 0.01f);
	TestEqual(TEXT("the new run restored the pool to the current max 150"), Scene.PlayerHealth->GetHealth(), 150.0f, 0.01f);
	TestEqual(TEXT("the run start kept the wired Attack 5"), Scene.PlayerCombat->GetAttackPower(), 5.0f, 0.01f);
	return true;
}

// While a room run is Running every equip/unequip request is refused (the
// card: exit the room first) - the profile row and the wired stats stay
// untouched; after leaving the room the same entry is accepted again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010RunningRoomRefusesEquipAndUnequip,
	"UEMMO.Tasks.M3_010.RunningRoomRefusesEquipAndUnequip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010RunningRoomRefusesEquipAndUnequip::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("RunningGate"), [](UProfileSubsystem& Profile)
	{
		Profile.NewProfile();
		FItemStats Bonus;
		Bonus.Attack = 5.0f;
		Profile.SetEquippedStatBonus(Bonus);
	}))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	URoomDefinition* Room = M3_010_MakeRoom();
	if (!TestTrue(TEXT("the room run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the session is Running"), Scene.Session->GetState() == ERoomSessionState::Running);

	// Equip while Running: refused, nothing changes.
	FItemStats Stronger;
	Stronger.Attack = 9.0f;
	TestFalse(TEXT("the equip request is refused while the run is Running"),
		Scene.Player->TryEquipStatBonus(Stronger));
	TestEqual(TEXT("the refused equip left the wired Attack 5"), Scene.PlayerCombat->GetAttackPower(), 5.0f, 0.01f);

	// Unequip while Running (a zero bonus row): refused too.
	TestFalse(TEXT("the unequip request is refused while the run is Running"),
		Scene.Player->TryEquipStatBonus(FItemStats()));
	TestEqual(TEXT("the refused unequip left the wired Attack 5"), Scene.PlayerCombat->GetAttackPower(), 5.0f, 0.01f);

	// A MaxHP change while Running is refused as well.
	FItemStats Tankier;
	Tankier.MaxHP = 50.0f;
	TestFalse(TEXT("the MaxHP equip request is refused while the run is Running"),
		Scene.Player->TryEquipStatBonus(Tankier));
	TestEqual(TEXT("the refused MaxHP equip left MaxHealth 100"), Scene.PlayerHealth->GetMaxHealth(), 100.0f, 0.01f);

	// After leaving the room (Exiting) the same entry is accepted and the
	// fresh snapshot re-applies immediately.
	TestTrue(TEXT("the room is left"), Scene.Session->LeaveRoom());
	TestTrue(TEXT("the same equip request is accepted once the room is left"),
		Scene.Player->TryEquipStatBonus(Stronger));
	TestEqual(TEXT("the post-exit equip wired the new Attack 9"), Scene.PlayerCombat->GetAttackPower(), 9.0f, 0.01f);
	return true;
}

// The M3-005 clamp semantics through the wiring: a LOWERED max clamps the
// current pool down; a RAISED max never restores already-lost health.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010EquipmentChangeClampsDownNeverHeals,
	"UEMMO.Tasks.M3_010.EquipmentChangeClampsDownNeverHeals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010EquipmentChangeClampsDownNeverHeals::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("Clamp"), [](UProfileSubsystem& Profile)
	{
		Profile.NewProfile();
		FItemStats Bonus;
		Bonus.MaxHP = 50.0f;
		Profile.SetEquippedStatBonus(Bonus);
	}))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	TestEqual(TEXT("the +50 MaxHP equipment wired the max 150"), Scene.PlayerHealth->GetMaxHealth(), 150.0f, 0.01f);
	TestEqual(TEXT("the pool starts full at 150"), Scene.PlayerHealth->GetHealth(), 150.0f, 0.01f);
	TestEqual(TEXT("the setup damage removes exactly 30"),
		Scene.PlayerHealth->ApplyDamage(30.0f), 30.0f, 0.01f);
	TestEqual(TEXT("the pool sits at 120"), Scene.PlayerHealth->GetHealth(), 120.0f, 0.01f);

	// Unequip (the max drops 150 -> 100): the pool is clamped DOWN to the new
	// max, never left above it.
	TestTrue(TEXT("the unequip outside a run is accepted"),
		Scene.Player->TryEquipStatBonus(FItemStats()));
	TestEqual(TEXT("the unequip lowered the max to 100"), Scene.PlayerHealth->GetMaxHealth(), 100.0f, 0.01f);
	TestEqual(TEXT("the lowered max clamped the pool down to 100"), Scene.PlayerHealth->GetHealth(), 100.0f, 0.01f);

	// Re-equip (the max rises 100 -> 150): the pool stays where it was - no
	// implicit heal from an equipment change.
	FItemStats Bonus50;
	Bonus50.MaxHP = 50.0f;
	TestTrue(TEXT("the re-equip outside a run is accepted"),
		Scene.Player->TryEquipStatBonus(Bonus50));
	TestEqual(TEXT("the re-equip raised the max to 150"), Scene.PlayerHealth->GetMaxHealth(), 150.0f, 0.01f);
	TestEqual(TEXT("the raised max never healed the pool (still 100)"), Scene.PlayerHealth->GetHealth(), 100.0f, 0.01f);
	return true;
}

// Graceful degradation: without a profile the pawn keeps the component
// defaults, the run-start restore works on the unchanged max, and the equip
// entry refuses (there is no profile to write into).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_010NoProfileKeepsComponentDefaults,
	"UEMMO.Tasks.M3_010.NoProfileKeepsComponentDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_010NoProfileKeepsComponentDefaults::RunTest(const FString& Parameters)
{
	FM3_010_Scene Scene;
	if (!Scene.Build(*this, TEXT("NoProfile"), TFunction<void(UProfileSubsystem&)>()))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	TestTrue(TEXT("the test premise: no profile was created"),
		Scene.Profile != nullptr && !Scene.Profile->HasProfile());

	// The component defaults survive untouched.
	TestEqual(TEXT("no profile keeps the default MaxHealth 100"), Scene.PlayerHealth->GetMaxHealth(), 100.0f, 0.01f);
	TestEqual(TEXT("no profile keeps the default full pool"), Scene.PlayerHealth->GetHealth(), 100.0f, 0.01f);
	TestEqual(TEXT("no profile keeps Attack 0"), Scene.PlayerCombat->GetAttackPower(), 0.0f, 0.01f);
	TestEqual(TEXT("no profile keeps Defense 0"), Scene.PlayerCombat->GetDefense(), 0.0f, 0.01f);

	// The equip entry refuses without a profile.
	FItemStats Bonus;
	Bonus.Attack = 5.0f;
	TestFalse(TEXT("the equip request without a profile is refused"),
		Scene.Player->TryEquipStatBonus(Bonus));
	TestEqual(TEXT("the refused equip kept Attack 0"), Scene.PlayerCombat->GetAttackPower(), 0.0f, 0.01f);

	// A new run still restores the pool to the CURRENT (unchanged) max.
	TestEqual(TEXT("the setup damage removes exactly 60"),
		Scene.PlayerHealth->ApplyDamage(60.0f), 60.0f, 0.01f);
	URoomDefinition* Room = M3_010_MakeRoom();
	if (!TestTrue(TEXT("the room run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestEqual(TEXT("the run start restored the pool to the unchanged max 100"),
		Scene.PlayerHealth->GetHealth(), 100.0f, 0.01f);
	return true;
}

#endif
