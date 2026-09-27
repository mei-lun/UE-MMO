// M3-003: local character profile and new-game initial state (interface
// contract section 8). Locks the UGameInstanceSubsystem behavior: explicit
// NewProfile initial values with a unique CharacterId, the level/XP formulas
// (100 x Level requirement, max level 10, HP 100+10x(L-1), Attack 2x(L-1),
// Defense L-1), multi-level AddXP cascades, the read-only snapshot derived
// from formulas only, inventory integration through M3-002 semantics, and the
// GameInstance lifetime property: the same subsystem instance keeps every
// bit of state across UWorld destruction/rebuild (map switch / return to
// menu), while ResetNewGame stays an explicit API only.
//
// Harness note: every stateful test runs through a REAL UGameInstance created
// the engine's own FTestWorldWrapper way (Engine/Private/Tests/
// AutomationCommon.cpp): NewObject<UGameInstance>(GEngine), a dedicated world
// context owning the instance, UWorld::SetGameInstance + SetCurrentWorld,
// then Init() - the production path that instantiates every registered
// UGameInstanceSubsystem. Cross-world tests share ONE game instance across
// two UWorld::CreateWorld worlds and never shut the instance down between
// them (a map switch keeps the WorldContext's OwningGameInstance alive).
//
// Stub-failure note: the fresh-subsystem prelude of NewProfileCreatesFreshIn-
// itialState (HasProfile()==false, default snapshot) matches the red stub's
// constant "no profile" answers, so that part passes in the red run; every
// other assertion fails on values (assertion red, not build red).

#include "Misc/AutomationTest.h"

#include "../Profile/ProfileSubsystem.h"
#include "../Items/ItemInstance.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_003
{
	// World names are uniquified: several suites can live in one process and
	// two of this suite's own worlds can be alive in one test.
	static int32 M3_003_WorldCounter = 0;

	// One real game instance + its current world, wired exactly like the
	// engine's FTestWorldWrapper. Create() initializes the instance (which
	// creates the profile subsystem through the production subsystem
	// collection); RebindFreshWorld() simulates a map switch / menu return by
	// destroying only the UWorld and rebinding the SAME instance to a fresh
	// one; TearDown() shuts everything down in the engine wrapper's order.
	struct FM3_003_ProfileSession
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameInstance* GameInstance = nullptr;
		UProfileSubsystem* Profile = nullptr;

		static FM3_003_ProfileSession Create(FAutomationTestBase& Test, const TCHAR* Tag)
		{
			FM3_003_ProfileSession Session;
			++M3_003_WorldCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M3_003_TestWorld_%s_%d"), Tag, M3_003_WorldCounter));
			Session.World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
			if (!Test.TestNotNull(TEXT("a private test world is available"), Session.World))
			{
				return Session;
			}
			Session.GameInstance = NewObject<UGameInstance>(GEngine);
			Session.Context = &GEngine->CreateNewWorldContext(EWorldType::Game);
			Session.Context->OwningGameInstance = Session.GameInstance;
			Session.World->SetGameInstance(Session.GameInstance);
			Session.Context->SetCurrentWorld(Session.World);
			// The production path that creates every registered
			// UGameInstanceSubsystem (engine FTestWorldWrapper precedent).
			Session.GameInstance->Init();
			Session.Profile = Session.GameInstance->GetSubsystem<UProfileSubsystem>();
			Test.TestNotNull(TEXT("the profile subsystem exists in the game instance's collection"), Session.Profile);
			return Session;
		}

		/**
		 * Map switch / ordinary menu return: only the UWorld dies; the world
		 * context keeps its OwningGameInstance and the game instance is never
		 * shut down, so every game instance subsystem (and its state) must
		 * survive. Rebinds the SAME context and instance to a fresh world.
		 */
		bool RebindFreshWorld(FAutomationTestBase& Test, const TCHAR* Tag)
		{
			if (World)
			{
				World->RemoveFromRoot();
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				World = nullptr;
			}
			++M3_003_WorldCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M3_003_TestWorld_%s_%d"), Tag, M3_003_WorldCounter));
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
			if (!Test.TestNotNull(TEXT("the rebuilt test world is available"), World))
			{
				return false;
			}
			World->SetGameInstance(GameInstance);
			Context->SetCurrentWorld(World);
			return true;
		}

		/** Full teardown in the engine FTestWorldWrapper::DestroyTestWorld order. */
		void TearDown()
		{
			if (World)
			{
				World->RemoveFromRoot();
				if (GameInstance)
				{
					GameInstance->Shutdown();
					GameInstance = nullptr;
				}
				if (Context)
				{
					GEngine->DestroyWorldContext(World);
					Context = nullptr;
				}
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				World = nullptr;
			}
			Profile = nullptr;
		}
	};

	/**
	 * Deterministic item instance (distinct seeds yield distinct InstanceIds)
	 * so every inventory assertion is reproducible - no FGuid::NewGuid
	 * randomness inside inventory content.
	 */
	static FItemInstance M3_003_MakeTestInstance(uint32 Seed)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(Seed, 0x0BADu, Seed + 11u, Seed * 5u + 3u);
		Instance.DefinitionId = FName(TEXT("weapon_training"));
		Instance.RollSeed = static_cast<int64>(Seed);
		Instance.Level = 1;
		return Instance;
	}

	/** Adds one item through the profile's inventory; reports a concrete error. */
	static bool M3_003_AddOrReport(FAutomationTestBase& Test, UProfileSubsystem* Profile,
		const FItemInstance& Instance, const TCHAR* What)
	{
		const EInventoryAddResult Result = Profile->GetInventory().TryAdd(Instance);
		if (Result != EInventoryAddResult::Added)
		{
			Test.AddError(FString::Printf(TEXT("setup: profile TryAdd(%s) returned result code %d instead of Added"),
				What, static_cast<int32>(Result)));
			return false;
		}
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M3_003;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003NewProfileCreatesFreshInitialState,
	"UEMMO.Tasks.M3_003.NewProfileCreatesFreshInitialState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003NewProfileCreatesFreshInitialState::RunTest(const FString& Parameters)
{
	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("Fresh"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Before any explicit call there is NO profile: no identity was silently
	// minted by initialization, and the snapshot reads as "nothing yet".
	TestFalse(TEXT("a fresh subsystem has no profile"), Session.Profile->HasProfile());
	const FProfileSnapshot EmptySnapshot = Session.Profile->GetProfileSnapshot();
	TestTrue(TEXT("the fresh snapshot has no CharacterId"), !EmptySnapshot.CharacterId.IsValid());
	TestEqual(TEXT("the fresh snapshot has level 0"), EmptySnapshot.Level, 0);
	TestEqual(TEXT("the fresh snapshot has 0 XP"), EmptySnapshot.XP, 0);
	TestEqual(TEXT("the fresh snapshot has 0 MaxHP"), EmptySnapshot.MaxHP, 0);
	TestEqual(TEXT("the fresh snapshot has an empty inventory"), EmptySnapshot.Inventory.Count(), 0);
	TestFalse(TEXT("AddXP without a profile changes nothing"),
		Session.Profile->AddXP(100));

	// NewProfile is the explicit entry: it mints a valid identity and puts the
	// exact level-1 design row in place (Level 1, XP 0, 100/0/0, empty pack).
	Session.Profile->NewProfile();
	TestTrue(TEXT("NewProfile creates a profile"), Session.Profile->HasProfile());
	TestTrue(TEXT("NewProfile mints a valid CharacterId"), Session.Profile->GetCharacterId().IsValid());

	const FProfileSnapshot Snapshot = Session.Profile->GetProfileSnapshot();
	TestTrue(TEXT("the snapshot carries the minted CharacterId"),
		Snapshot.CharacterId == Session.Profile->GetCharacterId());
	TestEqual(TEXT("a new profile starts at level 1"), Snapshot.Level, 1);
	TestEqual(TEXT("a new profile starts with 0 XP"), Snapshot.XP, 0);
	TestEqual(TEXT("GetLevel agrees with the snapshot"), Session.Profile->GetLevel(), 1);
	TestEqual(TEXT("GetXP agrees with the snapshot"), Session.Profile->GetXP(), 0);
	TestEqual(TEXT("a level-1 profile derives MaxHP 100"), Snapshot.MaxHP, 100);
	TestEqual(TEXT("a level-1 profile derives Attack 0"), Snapshot.Attack, 0);
	TestEqual(TEXT("a level-1 profile derives Defense 0"), Snapshot.Defense, 0);
	TestEqual(TEXT("a new profile starts with an empty inventory"), Snapshot.Inventory.Count(), 0);
	TestEqual(TEXT("the backing inventory is empty too"), Session.Profile->GetInventory().Count(), 0);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003NewProfileMintsUniqueCharacterIdsAndResetsState,
	"UEMMO.Tasks.M3_003.NewProfileMintsUniqueCharacterIdsAndResetsState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003NewProfileMintsUniqueCharacterIdsAndResetsState::RunTest(const FString& Parameters)
{
	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("Unique"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	Session.Profile->NewProfile();
	const FGuid FirstId = Session.Profile->GetCharacterId();
	TestTrue(TEXT("the first NewProfile mints a valid id"), FirstId.IsValid());

	// Grow the first profile so the reset check has something to undo.
	if (!TestTrue(TEXT("adding 150 XP levels the first profile up"), Session.Profile->AddXP(150)) ||
		!M3_003_AddOrReport(*this, Session.Profile, M3_003_MakeTestInstance(0x11u), TEXT("FirstItem")))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("the first profile reached level 2"), Session.Profile->GetLevel(), 2);

	// A second NewProfile mints a DIFFERENT valid id and resets everything
	// back to the level-1 row with an empty pack.
	Session.Profile->NewProfile();
	const FGuid SecondId = Session.Profile->GetCharacterId();
	TestTrue(TEXT("the second NewProfile mints a valid id"), SecondId.IsValid());
	TestTrue(TEXT("two NewProfile calls never share a CharacterId"), FirstId != SecondId);
	TestEqual(TEXT("the second profile starts at level 1"), Session.Profile->GetLevel(), 1);
	TestEqual(TEXT("the second profile starts with 0 XP"), Session.Profile->GetXP(), 0);
	TestEqual(TEXT("the second profile's inventory is empty"), Session.Profile->GetInventory().Count(), 0);
	const FProfileSnapshot SecondSnapshot = Session.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the second profile derives the level-1 MaxHP"), SecondSnapshot.MaxHP, 100);

	// And a third call keeps both uniqueness guarantees.
	Session.Profile->NewProfile();
	const FGuid ThirdId = Session.Profile->GetCharacterId();
	TestTrue(TEXT("the third id differs from the first"), ThirdId != FirstId);
	TestTrue(TEXT("the third id differs from the second"), ThirdId != SecondId);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003AddXPCrossesMultipleLevelsWithExactRemainder,
	"UEMMO.Tasks.M3_003.AddXPCrossesMultipleLevelsWithExactRemainder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003AddXPCrossesMultipleLevelsWithExactRemainder::RunTest(const FString& Parameters)
{
	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("XP"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	Session.Profile->NewProfile();

	// The card's exact case: +150 XP from level 1 pays the 100 x 1 requirement
	// and leaves exactly 50 XP carried into level 2.
	if (!TestTrue(TEXT("AddXP(150) from level 1 levels up"), Session.Profile->AddXP(150)))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("150 XP lifts level 1 to level 2"), Session.Profile->GetLevel(), 2);
	TestEqual(TEXT("the leftover XP is exactly 50"), Session.Profile->GetXP(), 50);
	const FProfileSnapshot After150 = Session.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the snapshot mirrors level 2"), After150.Level, 2);
	TestEqual(TEXT("the snapshot mirrors the 50 XP remainder"), After150.XP, 50);

	// One more +450: 50 stored + 450 added = 500; level 2 costs 200 (leaving
	// 300), level 3 costs 300 (leaving 0), level 4 needs 400 (not covered) -
	// a single call cascades two level-ups with an exact remainder.
	if (!TestTrue(TEXT("AddXP(450) cascades through level 3"), Session.Profile->AddXP(450)))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("the cascade ends at level 4"), Session.Profile->GetLevel(), 4);
	TestEqual(TEXT("the cascade leaves exactly 0 XP"), Session.Profile->GetXP(), 0);

	// XP only grows: zero and negative amounts are ignored (no refund, no
	// level change), and a sub-threshold amount just banks.
	if (!TestTrue(TEXT("AddXP(0) reports no level-up"), !Session.Profile->AddXP(0)))
	{
		Session.TearDown();
		return true;
	}
	TestFalse(TEXT("AddXP(-7) is rejected"), Session.Profile->AddXP(-7));
	TestEqual(TEXT("the level is unchanged by the rejected adds"), Session.Profile->GetLevel(), 4);
	TestEqual(TEXT("the XP is unchanged by the rejected adds"), Session.Profile->GetXP(), 0);
	TestTrue(TEXT("AddXP(199) banks below the level-4 requirement"), !Session.Profile->AddXP(199));
	TestEqual(TEXT("the banked XP is exactly 199"), Session.Profile->GetXP(), 199);
	TestEqual(TEXT("199 XP does not buy level 5 (400 required)"), Session.Profile->GetLevel(), 4);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003MaxLevelStopsLevelUpsWithZeroXPRequirement,
	"UEMMO.Tasks.M3_003.MaxLevelStopsLevelUpsWithZeroXPRequirement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003MaxLevelStopsLevelUpsWithZeroXPRequirement::RunTest(const FString& Parameters)
{
	// The full level-1..10 climb costs 100+200+...+900 = 4500 XP.
	TestEqual(TEXT("GetNextLevelXP(9) is 900"), UProfileSubsystem::GetNextLevelXP(9), 900);
	TestEqual(TEXT("GetNextLevelXP(10) is 0 (max level has no next level)"),
		UProfileSubsystem::GetNextLevelXP(10), 0);

	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("Max"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	Session.Profile->NewProfile();

	if (!TestTrue(TEXT("AddXP(4500) reaches the max level in one cascade"), Session.Profile->AddXP(4500)))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("4500 XP lands exactly on level 10"), Session.Profile->GetLevel(), 10);
	TestEqual(TEXT("the max level is reached with 0 leftover XP"), Session.Profile->GetXP(), 0);

	// At the max level nothing can level further, and the requirement reads 0.
	if (!TestTrue(TEXT("AddXP(1000) at max level reports no level-up"), !Session.Profile->AddXP(1000)))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("the level stays at 10"), Session.Profile->GetLevel(), 10);
	TestEqual(TEXT("GetNextLevelXP(10) still reads 0"), UProfileSubsystem::GetNextLevelXP(Session.Profile->GetLevel()), 0);

	// XP only grows and never overflows: a huge addition saturates instead of
	// wrapping into a negative XP value.
	Session.Profile->AddXP(MAX_int32);
	TestEqual(TEXT("a saturating addition keeps the level at 10"), Session.Profile->GetLevel(), 10);
	TestEqual(TEXT("XP saturates at MAX_int32 without wrapping"), Session.Profile->GetXP(), MAX_int32);

	// Out-of-range formula inputs clamp into the closed [1, 10] design table.
	TestEqual(TEXT("GetNextLevelXP(11) clamps to the max level's 0"), UProfileSubsystem::GetNextLevelXP(11), 0);
	TestEqual(TEXT("GetNextLevelXP(0) clamps to the level-1 requirement 100"), UProfileSubsystem::GetNextLevelXP(0), 100);
	TestEqual(TEXT("GetNextLevelXP(-5) clamps to the level-1 requirement 100"), UProfileSubsystem::GetNextLevelXP(-5), 100);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003DerivedStatsFollowLevelFormula,
	"UEMMO.Tasks.M3_003.DerivedStatsFollowLevelFormula",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003DerivedStatsFollowLevelFormula::RunTest(const FString& Parameters)
{
	// The static formula rows: level 1 = 100/0/0, level 3 = 120/4/2, level 10
	// = 190/18/9 (and out-of-range input clamps into the same table).
	TestEqual(TEXT("GetMaxHPForLevel(1) is 100"), UProfileSubsystem::GetMaxHPForLevel(1), 100);
	TestEqual(TEXT("GetAttackForLevel(1) is 0"), UProfileSubsystem::GetAttackForLevel(1), 0);
	TestEqual(TEXT("GetDefenseForLevel(1) is 0"), UProfileSubsystem::GetDefenseForLevel(1), 0);
	TestEqual(TEXT("GetMaxHPForLevel(3) is 120"), UProfileSubsystem::GetMaxHPForLevel(3), 120);
	TestEqual(TEXT("GetAttackForLevel(3) is 4"), UProfileSubsystem::GetAttackForLevel(3), 4);
	TestEqual(TEXT("GetDefenseForLevel(3) is 2"), UProfileSubsystem::GetDefenseForLevel(3), 2);
	TestEqual(TEXT("GetMaxHPForLevel(10) is 190"), UProfileSubsystem::GetMaxHPForLevel(10), 190);
	TestEqual(TEXT("GetAttackForLevel(10) is 18"), UProfileSubsystem::GetAttackForLevel(10), 18);
	TestEqual(TEXT("GetDefenseForLevel(10) is 9"), UProfileSubsystem::GetDefenseForLevel(10), 9);
	TestEqual(TEXT("GetMaxHPForLevel(99) clamps to the level-10 row"), UProfileSubsystem::GetMaxHPForLevel(99), 190);

	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("Stats"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	Session.Profile->NewProfile();

	// The snapshot derives its stats from the formulas, never from a World's
	// HealthComponent (the subsystem holds no World actor at all): a fresh
	// level-1 profile reads exactly the level-1 row...
	FProfileSnapshot Snapshot = Session.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the level-1 snapshot derives MaxHP 100"), Snapshot.MaxHP, 100);
	TestEqual(TEXT("the level-1 snapshot derives Attack 0"), Snapshot.Attack, 0);
	TestEqual(TEXT("the level-1 snapshot derives Defense 0"), Snapshot.Defense, 0);

	// ...and 300 XP (100 for level 2 + 200 for level 3) lands exactly on the
	// level-3 row 120/4/2 in the snapshot, with no World involved anywhere.
	if (!TestTrue(TEXT("AddXP(300) reaches level 3"), Session.Profile->AddXP(300)))
	{
		Session.TearDown();
		return true;
	}
	Snapshot = Session.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the level-3 snapshot reads level 3"), Snapshot.Level, 3);
	TestEqual(TEXT("the level-3 snapshot derives MaxHP 120"), Snapshot.MaxHP, 120);
	TestEqual(TEXT("the level-3 snapshot derives Attack 4"), Snapshot.Attack, 4);
	TestEqual(TEXT("the level-3 snapshot derives Defense 2"), Snapshot.Defense, 2);

	// The snapshot is a copy: mutating it can never reach the live profile.
	Snapshot.Level = 99;
	Snapshot.XP = 99;
	TestEqual(TEXT("mutating the snapshot copy leaves the live level alone"), Session.Profile->GetLevel(), 3);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003ProfileSurvivesMapSwitchAcrossSameGameInstance,
	"UEMMO.Tasks.M3_003.ProfileSurvivesMapSwitchAcrossSameGameInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003ProfileSurvivesMapSwitchAcrossSameGameInstance::RunTest(const FString& Parameters)
{
	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("MapA"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Play a bit: identity, two levels of progress and two packed items.
	Session.Profile->NewProfile();
	const FGuid CharacterId = Session.Profile->GetCharacterId();
	UProfileSubsystem* ProfileBeforeSwitch = Session.Profile;
	if (!TestTrue(TEXT("the map-A profile gains two levels"), Session.Profile->AddXP(150)) ||
		!M3_003_AddOrReport(*this, Session.Profile, M3_003_MakeTestInstance(0xA1u), TEXT("ItemA1")) ||
		!M3_003_AddOrReport(*this, Session.Profile, M3_003_MakeTestInstance(0xA2u), TEXT("ItemA2")))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("the map-A profile reached level 2 with 50 XP"), Session.Profile->GetXP(), 50);

	// Map switch: destroy the world, rebuild one, same game instance. The
	// subsystem belongs to the game instance, NOT to the world, so nothing
	// here may call NewProfile/ResetNewGame on its own.
	if (!TestTrue(TEXT("the map-B world was created and rebound"), Session.RebindFreshWorld(*this, TEXT("MapB"))))
	{
		Session.TearDown();
		return true;
	}
	TestTrue(TEXT("the rebuilt world carries the same game instance"),
		Session.World->GetGameInstance() == Session.GameInstance);

	UProfileSubsystem* ProfileAfterSwitch = Session.GameInstance->GetSubsystem<UProfileSubsystem>();
	TestTrue(TEXT("the same profile subsystem instance survives the map switch"),
		ProfileAfterSwitch == ProfileBeforeSwitch);
	if (ProfileAfterSwitch == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Identity, progress and the packed inventory are all still there, in
	// insertion order, after the world that "held" them was destroyed.
	TestTrue(TEXT("the CharacterId survives the map switch"),
		ProfileAfterSwitch->GetCharacterId() == CharacterId);
	TestTrue(TEXT("the profile still exists after the map switch"), ProfileAfterSwitch->HasProfile());
	TestEqual(TEXT("the level survives the map switch"), ProfileAfterSwitch->GetLevel(), 2);
	TestEqual(TEXT("the XP remainder survives the map switch"), ProfileAfterSwitch->GetXP(), 50);
	const FProfileSnapshot Switched = ProfileAfterSwitch->GetProfileSnapshot();
	TestEqual(TEXT("the switched snapshot carries 2 items"), Switched.Inventory.Count(), 2);
	TestTrue(TEXT("item A1 is still packed after the switch"),
		Switched.Inventory.Contains(M3_003_MakeTestInstance(0xA1u).InstanceId));
	TestTrue(TEXT("item A2 is still packed after the switch"),
		Switched.Inventory.Contains(M3_003_MakeTestInstance(0xA2u).InstanceId));
	TestTrue(TEXT("the insertion order A1, A2 survives the switch"),
		Switched.Inventory.GetByIndex(0) != nullptr && Switched.Inventory.GetByIndex(0)->InstanceId == M3_003_MakeTestInstance(0xA1u).InstanceId &&
		Switched.Inventory.GetByIndex(1) != nullptr && Switched.Inventory.GetByIndex(1)->InstanceId == M3_003_MakeTestInstance(0xA2u).InstanceId);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003MenuReturnWorldRebuildKeepsProfileAndFreshInstanceEmpty,
	"UEMMO.Tasks.M3_003.MenuReturnWorldRebuildKeepsProfileAndFreshInstanceEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003MenuReturnWorldRebuildKeepsProfileAndFreshInstanceEmpty::RunTest(const FString& Parameters)
{
	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("MenuA"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	Session.Profile->NewProfile();
	const FGuid CharacterId = Session.Profile->GetCharacterId();
	if (!TestTrue(TEXT("the menu-test profile gains a level"), Session.Profile->AddXP(150)) ||
		!M3_003_AddOrReport(*this, Session.Profile, M3_003_MakeTestInstance(0xB1u), TEXT("ItemB1")))
	{
		Session.TearDown();
		return true;
	}

	// An ordinary return to the menu is just world teardown and rebuild -
	// no NewProfile and no ResetNewGame anywhere in between. Two rebuilds in
	// a row prove nothing in the world lifecycle touches the profile.
	for (int32 RebuildIndex = 1; RebuildIndex <= 2; ++RebuildIndex)
	{
		if (!TestTrue(TEXT("the menu return rebuilt a world"), Session.RebindFreshWorld(*this, TEXT("MenuB"))))
		{
			Session.TearDown();
			return true;
		}
		UProfileSubsystem* ProfileAfter = Session.GameInstance->GetSubsystem<UProfileSubsystem>();
		if (!TestTrue(TEXT("the rebuilt world still resolves the same profile subsystem"), ProfileAfter == Session.Profile) ||
			ProfileAfter == nullptr)
		{
			Session.TearDown();
			return true;
		}
		TestTrue(*FString::Printf(TEXT("rebuild %d kept the profile alive"), RebuildIndex), ProfileAfter->HasProfile());
		TestTrue(*FString::Printf(TEXT("rebuild %d kept the CharacterId"), RebuildIndex),
			ProfileAfter->GetCharacterId() == CharacterId);
		TestEqual(*FString::Printf(TEXT("rebuild %d kept the level"), RebuildIndex), ProfileAfter->GetLevel(), 2);
		TestEqual(*FString::Printf(TEXT("rebuild %d kept the XP"), RebuildIndex), ProfileAfter->GetXP(), 50);
		TestEqual(*FString::Printf(TEXT("rebuild %d kept the packed item"), RebuildIndex),
			ProfileAfter->GetInventory().Count(), 1);
	}

	// The profile is per game instance, not a process-global: a FRESH game
	// instance starts with no profile at all, even though another instance in
	// the same process holds a fully leveled one.
	FM3_003_ProfileSession FreshSession = FM3_003_ProfileSession::Create(*this, TEXT("MenuFresh"));
	if (FreshSession.Profile != nullptr)
	{
		TestFalse(TEXT("a fresh game instance starts without a profile"), FreshSession.Profile->HasProfile());
		TestTrue(TEXT("a fresh game instance has no CharacterId"),
			!FreshSession.Profile->GetCharacterId().IsValid());
		FreshSession.TearDown();
	}

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003ResetNewGameExplicitCallResetsEverything,
	"UEMMO.Tasks.M3_003.ResetNewGameExplicitCallResetsEverything",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003ResetNewGameExplicitCallResetsEverything::RunTest(const FString& Parameters)
{
	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("Reset"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	Session.Profile->NewProfile();
	const FGuid OldId = Session.Profile->GetCharacterId();
	if (!TestTrue(TEXT("the reset-test profile gains a level"), Session.Profile->AddXP(150)) ||
		!M3_003_AddOrReport(*this, Session.Profile, M3_003_MakeTestInstance(0xC1u), TEXT("ItemC1")))
	{
		Session.TearDown();
		return true;
	}

	// Only an explicit call resets. The map-switch and menu-return tests
	// already proved world rebuilds never reset; this is the explicit path.
	Session.Profile->ResetNewGame();

	TestTrue(TEXT("ResetNewGame leaves a profile in place"), Session.Profile->HasProfile());
	const FGuid NewId = Session.Profile->GetCharacterId();
	TestTrue(TEXT("ResetNewGame mints a valid new CharacterId"), NewId.IsValid());
	TestTrue(TEXT("ResetNewGame replaces the old CharacterId"), NewId != OldId);
	TestEqual(TEXT("ResetNewGame restarts at level 1"), Session.Profile->GetLevel(), 1);
	TestEqual(TEXT("ResetNewGame zeroes the XP"), Session.Profile->GetXP(), 0);
	TestEqual(TEXT("ResetNewGame empties the inventory"), Session.Profile->GetInventory().Count(), 0);
	const FProfileSnapshot Snapshot = Session.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the reset snapshot derives the level-1 MaxHP"), Snapshot.MaxHP, 100);
	TestEqual(TEXT("the reset snapshot derives the level-1 Attack"), Snapshot.Attack, 0);
	TestEqual(TEXT("the reset snapshot derives the level-1 Defense"), Snapshot.Defense, 0);
	TestTrue(TEXT("the old id's item is gone after the reset"),
		!Snapshot.Inventory.Contains(M3_003_MakeTestInstance(0xC1u).InstanceId));

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_003InventoryTryAddVisibleThroughSnapshot,
	"UEMMO.Tasks.M3_003.InventoryTryAddVisibleThroughSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_003InventoryTryAddVisibleThroughSnapshot::RunTest(const FString& Parameters)
{
	FM3_003_ProfileSession Session = FM3_003_ProfileSession::Create(*this, TEXT("Inv"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	Session.Profile->NewProfile();

	// M3-002 semantics keep running behind the profile surface: an add is
	// Accepted with the explicit result, a duplicate id is rejected, and the
	// read-only snapshot sees the stored content without aliasing it.
	const FItemInstance Item = M3_003_MakeTestInstance(0xD1u);
	if (!M3_003_AddOrReport(*this, Session.Profile, Item, TEXT("ItemD1")))
	{
		Session.TearDown();
		return true;
	}
	TestTrue(TEXT("adding the same InstanceId again returns Duplicate"),
		Session.Profile->GetInventory().TryAdd(Item) == EInventoryAddResult::Duplicate);

	const FProfileSnapshot Snapshot = Session.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the snapshot shows 1 packed item"), Snapshot.Inventory.Count(), 1);
	TestTrue(TEXT("the snapshot contains the packed instance"), Snapshot.Inventory.Contains(Item.InstanceId));
	TestTrue(TEXT("the snapshot's slot 0 is the packed instance"),
		Snapshot.Inventory.GetByIndex(0) != nullptr && Snapshot.Inventory.GetByIndex(0)->InstanceId == Item.InstanceId);
	TestTrue(TEXT("the snapshot's copy keeps the definition"),
		Snapshot.Inventory.GetByIndex(0) != nullptr && Snapshot.Inventory.GetByIndex(0)->DefinitionId == FName(TEXT("weapon_training")));

	// Removing through the profile updates the next snapshot too.
	TestTrue(TEXT("removing the packed instance returns Removed"),
		Session.Profile->GetInventory().Remove(Item.InstanceId) == EInventoryRemoveResult::Removed);
	const FProfileSnapshot AfterRemove = Session.Profile->GetProfileSnapshot();
	TestEqual(TEXT("the post-remove snapshot shows 0 items"), AfterRemove.Inventory.Count(), 0);
	TestTrue(TEXT("the post-remove snapshot no longer contains the id"),
		!AfterRemove.Inventory.Contains(Item.InstanceId));

	Session.TearDown();
	return true;
}

#endif
