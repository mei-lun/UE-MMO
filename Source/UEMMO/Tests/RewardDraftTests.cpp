// M3-008: settlement reward drafts with idempotent application (interface
// contract section 8). Locks the URewardService contract: a Cleared run
// result creates exactly one FPendingReward{SettlementId, XP=50, Items[1]}
// in the GameInstance-level profile; a repeat settlement for the same
// SettlementId returns the ORIGINAL draft without re-rolling (the random
// result is generated exactly once); a Failed result is always rejected;
// different runs keep independent drafts; the RewardSeed is a fixed
// derivation of the room Seed (never a wall clock); and a draft survives a
// World switch with the profile.
//
// Harness note: the profile-integration tests run through a REAL UGameInstance
// created the engine's own FTestWorldWrapper way (same pattern as the M3-003
// suite: NewObject<UGameInstance>(GEngine), a dedicated world context owning
// the instance, UWorld::SetGameInstance + SetCurrentWorld, then Init()). The
// cross-world test shares ONE game instance across two UWorld::CreateWorld
// worlds and never shuts the instance down between them.
//
// Stub-failure note: the red stub refuses every BeginReward with a constant
// RejectedNoProfile and derives a constant 0 RewardSeed, so every test below
// fails on values (assertion red, not build red); only the pure no-profile
// rejection at the top of FailedResultIsRejectedWithoutDraft matches the stub.

#include "Misc/AutomationTest.h"

#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../Items/ItemDefinition.h"
#include "../Room/RoomResult.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_008
{
	// World names are uniquified: several suites can live in one process and
	// two of this suite's own worlds can be alive in one test.
	static int32 M3_008_WorldCounter = 0;

	// One real game instance + its current world, wired exactly like the
	// engine's FTestWorldWrapper (see the M3-003 harness comment). Used only
	// where the profile's GameInstance lifetime is the subject.
	struct FM3_008_ProfileSession
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameInstance* GameInstance = nullptr;
		UProfileSubsystem* Profile = nullptr;

		static FM3_008_ProfileSession Create(FAutomationTestBase& Test, const TCHAR* Tag)
		{
			FM3_008_ProfileSession Session;
			++M3_008_WorldCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M3_008_TestWorld_%s_%d"), Tag, M3_008_WorldCounter));
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
		 * shut down, so every game instance subsystem (and its pending reward
		 * drafts) must survive.
		 */
		bool RebindFreshWorld(FAutomationTestBase& Test, const TCHAR* Tag)
		{
			if (World)
			{
				World->RemoveFromRoot();
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				World = nullptr;
			}
			++M3_008_WorldCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M3_008_TestWorld_%s_%d"), Tag, M3_008_WorldCounter));
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

	/** The three training definitions in a fresh catalog (mirrors Data/items.json). */
	static FItemDefinitionCatalog M3_008_MakeTrainingCatalog()
	{
		FItemDefinitionCatalog Catalog;

		FItemDefinition Weapon;
		Weapon.DefinitionId = FName(TEXT("weapon_training"));
		Weapon.DisplayName = TEXT("weapon_training");
		Weapon.Slot = EItemSlot::Weapon;
		Weapon.BaseStats.Attack = 5.0f;
		Weapon.BaseStats.Defense = 0.0f;
		Weapon.BaseStats.MaxHP = 0.0f;
		Weapon.IconPath = TEXT("");
		Weapon.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Weapon, nullptr);

		FItemDefinition Armor;
		Armor.DefinitionId = FName(TEXT("armor_training"));
		Armor.DisplayName = TEXT("armor_training");
		Armor.Slot = EItemSlot::Armor;
		Armor.BaseStats.Attack = 0.0f;
		Armor.BaseStats.Defense = 3.0f;
		Armor.BaseStats.MaxHP = 0.0f;
		Armor.IconPath = TEXT("");
		Armor.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Armor, nullptr);

		FItemDefinition Charm;
		Charm.DefinitionId = FName(TEXT("charm_training"));
		Charm.DisplayName = TEXT("charm_training");
		Charm.Slot = EItemSlot::Accessory;
		Charm.BaseStats.Attack = 0.0f;
		Charm.BaseStats.Defense = 0.0f;
		Charm.BaseStats.MaxHP = 20.0f;
		Charm.IconPath = TEXT("");
		Charm.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Charm, nullptr);

		return Catalog;
	}

	/** A finished-run result value; RunId is kept distinct alongside SettlementId. */
	static FRoomResult M3_008_MakeResult(uint64 RunId, uint64 SettlementId, int32 Seed, bool bCleared)
	{
		FRoomResult Result;
		Result.RunId = RunId;
		Result.SettlementId = SettlementId;
		Result.RoomId = FName(TEXT("training_room"));
		Result.Seed = Seed;
		Result.bCleared = bCleared;
		Result.KilledCount = 3;
		Result.ElapsedSeconds = 12.5;
		return Result;
	}

	/** True when the picked definition is one of the pinned starter entries. */
	static bool M3_008_IsStarterDefinition(FName DefinitionId)
	{
		return DefinitionId == FName(TEXT("weapon_training")) ||
			DefinitionId == FName(TEXT("armor_training")) ||
			DefinitionId == FName(TEXT("charm_training"));
	}

	/** Field-for-field draft equality (instance identity, roll seed, stats, level). */
	static bool M3_008_DraftsEqual(const FPendingReward& A, const FPendingReward& B)
	{
		if (A.SettlementId != B.SettlementId || A.XP != B.XP || A.Items.Num() != B.Items.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Items.Num(); ++Index)
		{
			if (A.Items[Index].InstanceId != B.Items[Index].InstanceId ||
				A.Items[Index].DefinitionId != B.Items[Index].DefinitionId ||
				A.Items[Index].RollSeed != B.Items[Index].RollSeed ||
				A.Items[Index].Level != B.Items[Index].Level ||
				A.Items[Index].RolledStats.Attack != B.Items[Index].RolledStats.Attack ||
				A.Items[Index].RolledStats.Defense != B.Items[Index].RolledStats.Defense ||
				A.Items[Index].RolledStats.MaxHP != B.Items[Index].RolledStats.MaxHP)
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Shared setup: creates the session, mints a profile and binds a fresh
	 * URewardService to it. Returns a null service (after TestNotNull) when
	 * the harness itself is broken; callers bail out on nullptr.
	 */
	static URewardService* M3_008_MakeBoundService(FAutomationTestBase& Test, FM3_008_ProfileSession& Session, const TCHAR* Tag)
	{
		if (Session.Profile)
		{
			Session.Profile->NewProfile();
		}
		URewardService* Service = NewObject<URewardService>(Session.GameInstance);
		Test.TestNotNull(TEXT("the reward service object is created"), Service);
		if (Service)
		{
			Service->BindProfile(Session.Profile);
		}
		return Service;
	}
}

using namespace UE::UEMMO::Tasks::M3_008;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_008ClearedResultCreatesPendingRewardDraft,
	"UEMMO.Tasks.M3_008.ClearedResultCreatesPendingRewardDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_008ClearedResultCreatesPendingRewardDraft::RunTest(const FString& Parameters)
{
	FM3_008_ProfileSession Session = FM3_008_ProfileSession::Create(*this, TEXT("Cleared"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_008_MakeBoundService(*this, Session, TEXT("Cleared"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	const FRoomResult Result = M3_008_MakeResult(/*RunId*/ 1, /*SettlementId*/ 101, /*Seed*/ 20260927, /*bCleared*/ true);
	const FRewardBeginOutcome Outcome = Service->BeginReward(Result, M3_008_MakeTrainingCatalog());

	TestEqual(TEXT("a Cleared result begins a new draft (Applied)"), Outcome.Result, ERewardBeginResult::Applied);
	TestTrue(TEXT("an applied draft reports no error"), Outcome.Error.IsEmpty());
	TestEqual(TEXT("the draft carries the settlement id"), Outcome.Draft.SettlementId, static_cast<uint64>(101));
	TestEqual(TEXT("the draft grants the design XP 50"), Outcome.Draft.XP, 50);
	TestEqual(TEXT("the draft XP equals the RewardXPPerClear constant"), Outcome.Draft.XP, URewardService::RewardXPPerClear);
	TestEqual(TEXT("the draft carries exactly one item"), Outcome.Draft.Items.Num(), 1);
	if (Outcome.Draft.Items.Num() == 1)
	{
		TestTrue(TEXT("the draft item has a valid InstanceId"), Outcome.Draft.Items[0].InstanceId.IsValid());
		TestTrue(TEXT("the draft item is one of the starter definitions"), M3_008_IsStarterDefinition(Outcome.Draft.Items[0].DefinitionId));
		TestEqual(TEXT("the draft item starts at level 1"), Outcome.Draft.Items[0].Level, 1);
	}

	// The draft is stored in the PROFILE's PendingRewards, not only returned.
	TestEqual(TEXT("the profile holds exactly one pending draft"), Session.Profile->GetPendingRewards().Num(), 1);
	if (Session.Profile->GetPendingRewards().Num() == 1)
	{
		TestTrue(TEXT("the stored draft equals the returned draft"),
			M3_008_DraftsEqual(Session.Profile->GetPendingRewards()[0], Outcome.Draft));
	}

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_008RepeatSettlementKeepsIdenticalDraftWithoutReroll,
	"UEMMO.Tasks.M3_008.RepeatSettlementKeepsIdenticalDraftWithoutReroll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_008RepeatSettlementKeepsIdenticalDraftWithoutReroll::RunTest(const FString& Parameters)
{
	FM3_008_ProfileSession Session = FM3_008_ProfileSession::Create(*this, TEXT("Repeat"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_008_MakeBoundService(*this, Session, TEXT("Repeat"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// The same settlement result replayed twice (the retry-loop shape: the
	// caller may re-send the same finished run after any interruption).
	const FRoomResult Result = M3_008_MakeResult(/*RunId*/ 2, /*SettlementId*/ 202, /*Seed*/ 3001, /*bCleared*/ true);
	const FRewardBeginOutcome First = Service->BeginReward(Result, M3_008_MakeTrainingCatalog());
	const FRewardBeginOutcome Second = Service->BeginReward(Result, M3_008_MakeTrainingCatalog());

	TestEqual(TEXT("the first settlement is Applied"), First.Result, ERewardBeginResult::Applied);
	TestEqual(TEXT("the repeat settlement is not Applied again"), Second.Result, ERewardBeginResult::AlreadyApplied);
	TestTrue(TEXT("the replay reports no error"), Second.Error.IsEmpty());

	// No re-roll: the returned draft is field-for-field identical to the
	// first (same item identity, definition, roll seed, stats and level).
	TestTrue(TEXT("the replayed draft is field-for-field identical to the first draft"),
		M3_008_DraftsEqual(Second.Draft, First.Draft));
	if (Second.Draft.Items.Num() == First.Draft.Items.Num() && First.Draft.Items.Num() == 1)
	{
		TestTrue(TEXT("the replayed item keeps the identical InstanceId"),
			Second.Draft.Items[0].InstanceId == First.Draft.Items[0].InstanceId);
		TestEqual(TEXT("the replayed item keeps the identical DefinitionId"),
			Second.Draft.Items[0].DefinitionId, First.Draft.Items[0].DefinitionId);
		TestEqual(TEXT("the replayed item keeps the identical RollSeed"),
			Second.Draft.Items[0].RollSeed, First.Draft.Items[0].RollSeed);
	}

	// No second item: the profile still holds exactly the original draft.
	TestEqual(TEXT("the profile still holds exactly one pending draft"), Session.Profile->GetPendingRewards().Num(), 1);
	if (Session.Profile->GetPendingRewards().Num() == 1)
	{
		TestTrue(TEXT("the stored draft is unchanged by the replay"),
			M3_008_DraftsEqual(Session.Profile->GetPendingRewards()[0], First.Draft));
	}

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_008FailedResultIsRejectedWithoutDraft,
	"UEMMO.Tasks.M3_008.FailedResultIsRejectedWithoutDraft",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_008FailedResultIsRejectedWithoutDraft::RunTest(const FString& Parameters)
{
	FM3_008_ProfileSession Session = FM3_008_ProfileSession::Create(*this, TEXT("Failed"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// The IsRewardEligible=false path at value level: a Failed result can
	// never enter the reward flow - even before any profile exists.
	URewardService* Service = NewObject<URewardService>(Session.GameInstance);
	TestNotNull(TEXT("the reward service object is created"), Service);
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}
	Service->BindProfile(Session.Profile);
	const FRoomResult Failed = M3_008_MakeResult(/*RunId*/ 3, /*SettlementId*/ 303, /*Seed*/ 4001, /*bCleared*/ false);
	const FRoomResult ClearedNoProfileYet = M3_008_MakeResult(/*RunId*/ 3, /*SettlementId*/ 303, /*Seed*/ 4001, /*bCleared*/ true);

	// The no-profile guard itself: even a CLEARED result is refused while no
	// profile exists (a draft needs a GameInstance-level profile to live in).
	const FRewardBeginOutcome NoProfile = Service->BeginReward(ClearedNoProfileYet, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("a Cleared result without a profile is rejected"), NoProfile.Result, ERewardBeginResult::RejectedNoProfile);
	TestTrue(TEXT("the no-profile rejection names the missing profile"), NoProfile.Error.Contains(TEXT("profile")));
	TestEqual(TEXT("the no-profile rejection stores nothing"), Session.Profile->GetPendingRewards().Num(), 0);

	// Now WITH an existing profile: the Failed result is still rejected, with
	// its own explicit enum value naming the ineligible outcome.
	Session.Profile->NewProfile();
	const FRewardBeginOutcome Rejected = Service->BeginReward(Failed, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("a Failed result is rejected even with a profile"), Rejected.Result, ERewardBeginResult::RejectedFailedResult);
	TestTrue(TEXT("the Failed rejection names the ineligible outcome"), Rejected.Error.Contains(TEXT("not reward eligible")));
	TestTrue(TEXT("a rejected Failed result produces no draft"), Rejected.Draft.Items.Num() == 0);
	TestEqual(TEXT("a rejected Failed result stores nothing"), Session.Profile->GetPendingRewards().Num(), 0);

	// Repeating the Failed settlement stays rejected (no state was created by
	// the first rejection either).
	const FRewardBeginOutcome RejectedAgain = Service->BeginReward(Failed, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("repeating a Failed settlement stays rejected"), RejectedAgain.Result, ERewardBeginResult::RejectedFailedResult);
	TestEqual(TEXT("repeated Failed rejections still store nothing"), Session.Profile->GetPendingRewards().Num(), 0);

	// The same settlement id DID clear through another run result shape? No -
	// a settlement id belongs to exactly one run; but a CLEARED result with a
	// different settlement id right after the rejections must still work,
	// proving the rejections never poisoned the service.
	const FRoomResult Cleared = M3_008_MakeResult(/*RunId*/ 4, /*SettlementId*/ 304, /*Seed*/ 4002, /*bCleared*/ true);
	const FRewardBeginOutcome AfterRejections = Service->BeginReward(Cleared, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("a Cleared result after Failed rejections is Applied"), AfterRejections.Result, ERewardBeginResult::Applied);
	TestEqual(TEXT("only the Cleared settlement was stored"), Session.Profile->GetPendingRewards().Num(), 1);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_008DifferentRoomRunsKeepIndependentDrafts,
	"UEMMO.Tasks.M3_008.DifferentRoomRunsKeepIndependentDrafts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_008DifferentRoomRunsKeepIndependentDrafts::RunTest(const FString& Parameters)
{
	FM3_008_ProfileSession Session = FM3_008_ProfileSession::Create(*this, TEXT("Runs"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_008_MakeBoundService(*this, Session, TEXT("Runs"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Two different runs of the same room: distinct RunIds, distinct
	// process-global SettlementIds, distinct room Seeds.
	const FRoomResult RunA = M3_008_MakeResult(/*RunId*/ 5, /*SettlementId*/ 501, /*Seed*/ 5001, /*bCleared*/ true);
	const FRoomResult RunB = M3_008_MakeResult(/*RunId*/ 6, /*SettlementId*/ 502, /*Seed*/ 5002, /*bCleared*/ true);

	const FRewardBeginOutcome OutcomeA = Service->BeginReward(RunA, M3_008_MakeTrainingCatalog());
	const FRewardBeginOutcome OutcomeB = Service->BeginReward(RunB, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("run A is Applied"), OutcomeA.Result, ERewardBeginResult::Applied);
	TestEqual(TEXT("run B is Applied"), OutcomeB.Result, ERewardBeginResult::Applied);

	TestEqual(TEXT("the profile holds one draft per run"), Session.Profile->GetPendingRewards().Num(), 2);
	if (Session.Profile->GetPendingRewards().Num() == 2)
	{
		const FPendingReward& DraftA = Session.Profile->GetPendingRewards()[0];
		const FPendingReward& DraftB = Session.Profile->GetPendingRewards()[1];
		TestEqual(TEXT("draft A carries run A's settlement id"), DraftA.SettlementId, static_cast<uint64>(501));
		TestEqual(TEXT("draft B carries run B's settlement id"), DraftB.SettlementId, static_cast<uint64>(502));
		TestTrue(TEXT("the two runs own different item identities"),
			DraftA.Items.Num() == 1 && DraftB.Items.Num() == 1 &&
			DraftA.Items[0].InstanceId != DraftB.Items[0].InstanceId);
		TestTrue(TEXT("the two runs' drafts are not identical"),
			!M3_008_DraftsEqual(DraftA, DraftB));
	}

	// Replaying run A after run B still returns run A's own draft and adds
	// nothing: independence is per settlement, and each stays idempotent.
	const FRewardBeginOutcome ReplayA = Service->BeginReward(RunA, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("replaying run A returns its original draft"), ReplayA.Result, ERewardBeginResult::AlreadyApplied);
	TestEqual(TEXT("replay A carries settlement 501"), ReplayA.Draft.SettlementId, static_cast<uint64>(501));
	TestEqual(TEXT("the replay added no third draft"), Session.Profile->GetPendingRewards().Num(), 2);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_008DraftSurvivesWorldSwitchInProfile,
	"UEMMO.Tasks.M3_008.DraftSurvivesWorldSwitchInProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_008DraftSurvivesWorldSwitchInProfile::RunTest(const FString& Parameters)
{
	FM3_008_ProfileSession Session = FM3_008_ProfileSession::Create(*this, TEXT("Switch"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_008_MakeBoundService(*this, Session, TEXT("Switch"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	const FRoomResult Result = M3_008_MakeResult(/*RunId*/ 7, /*SettlementId*/ 701, /*Seed*/ 7001, /*bCleared*/ true);
	const FRewardBeginOutcome Before = Service->BeginReward(Result, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("the draft is created before the world switch"), Before.Result, ERewardBeginResult::Applied);

	// Map switch / menu return: the world dies, the GameInstance (and with it
	// the profile and its pending drafts) stays alive.
	if (!TestTrue(TEXT("the world was rebuilt while the game instance stayed"), Session.RebindFreshWorld(*this, TEXT("SwitchB"))))
	{
		Session.TearDown();
		return true;
	}
	TestTrue(TEXT("the world's game instance is the same instance"),
		Session.World->GetGameInstance() == Session.GameInstance);

	UProfileSubsystem* ProfileAfterSwitch = Session.GameInstance->GetSubsystem<UProfileSubsystem>();
	TestTrue(TEXT("the profile subsystem instance survived the world switch"), ProfileAfterSwitch == Session.Profile);
	TestEqual(TEXT("the pending draft survived the world switch"), ProfileAfterSwitch->GetPendingRewards().Num(), 1);
	if (ProfileAfterSwitch->GetPendingRewards().Num() == 1)
	{
		TestTrue(TEXT("the surviving draft is field-for-field the original draft"),
			M3_008_DraftsEqual(ProfileAfterSwitch->GetPendingRewards()[0], Before.Draft));
	}

	// And the idempotency guard holds after the switch too: replaying the
	// settlement in the new world returns the original draft, no re-roll.
	URewardService* ServiceAfterSwitch = NewObject<URewardService>(Session.GameInstance);
	ServiceAfterSwitch->BindProfile(ProfileAfterSwitch);
	const FRewardBeginOutcome Replay = ServiceAfterSwitch->BeginReward(Result, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("replaying after the world switch is AlreadyApplied"), Replay.Result, ERewardBeginResult::AlreadyApplied);
	TestTrue(TEXT("the post-switch replay draft is field-for-field identical"),
		M3_008_DraftsEqual(Replay.Draft, Before.Draft));
	TestEqual(TEXT("the post-switch replay added no draft"), ProfileAfterSwitch->GetPendingRewards().Num(), 1);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_008RewardSeedDerivationIsDeterministicFromRoomSeed,
	"UEMMO.Tasks.M3_008.RewardSeedDerivationIsDeterministicFromRoomSeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_008RewardSeedDerivationIsDeterministicFromRoomSeed::RunTest(const FString& Parameters)
{
	// Fixed known seeds, fixed assertions: the derivation is a pure function
	// of the room Seed. A wall-clock-based derivation would fail the very
	// first equality (two calls at different times), the distinctness pair
	// pins that different room seeds actually diverge.
	const int64 SeedOfRoom = URewardService::DeriveRewardSeed(20260927);
	TestEqual(TEXT("the same room seed derives the same reward seed again"),
		URewardService::DeriveRewardSeed(20260927), SeedOfRoom);
	const int64 SeedOfOtherRoom = URewardService::DeriveRewardSeed(42);
	TestTrue(TEXT("two known different room seeds derive different reward seeds"),
		SeedOfRoom != SeedOfOtherRoom);
	TestEqual(TEXT("the zero room seed is deterministic too"),
		URewardService::DeriveRewardSeed(0), URewardService::DeriveRewardSeed(0));

	// Draft level: same room Seed (same RewardSeed) but a different
	// settlement -> the same weighted pick and the same recorded RollSeed,
	// while the item identity stays a function of the SettlementId.
	FM3_008_ProfileSession Session = FM3_008_ProfileSession::Create(*this, TEXT("Seed"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_008_MakeBoundService(*this, Session, TEXT("Seed"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}
	const FItemDefinitionCatalog Catalog = M3_008_MakeTrainingCatalog();

	const FRoomResult SameSeedA = M3_008_MakeResult(/*RunId*/ 8, /*SettlementId*/ 801, /*Seed*/ 20260927, /*bCleared*/ true);
	const FRoomResult SameSeedB = M3_008_MakeResult(/*RunId*/ 9, /*SettlementId*/ 802, /*Seed*/ 20260927, /*bCleared*/ true);
	const FRoomResult OtherSeed = M3_008_MakeResult(/*RunId*/ 10, /*SettlementId*/ 803, /*Seed*/ 42, /*bCleared*/ true);

	const FRewardBeginOutcome DraftA = Service->BeginReward(SameSeedA, Catalog);
	const FRewardBeginOutcome DraftB = Service->BeginReward(SameSeedB, Catalog);
	const FRewardBeginOutcome DraftC = Service->BeginReward(OtherSeed, Catalog);
	TestEqual(TEXT("settlement A is Applied"), DraftA.Result, ERewardBeginResult::Applied);
	TestEqual(TEXT("settlement B is Applied"), DraftB.Result, ERewardBeginResult::Applied);
	TestEqual(TEXT("settlement C is Applied"), DraftC.Result, ERewardBeginResult::Applied);
	if (DraftA.Draft.Items.Num() != 1 || DraftB.Draft.Items.Num() != 1 || DraftC.Draft.Items.Num() != 1)
	{
		Session.TearDown();
		return true;
	}

	TestEqual(TEXT("the same room seed picks the same definition for a different settlement"),
		DraftB.Draft.Items[0].DefinitionId, DraftA.Draft.Items[0].DefinitionId);
	TestEqual(TEXT("the same room seed records the same RollSeed for a different settlement"),
		DraftB.Draft.Items[0].RollSeed, DraftA.Draft.Items[0].RollSeed);
	TestTrue(TEXT("a different settlement still owns a different item identity"),
		DraftB.Draft.Items[0].InstanceId != DraftA.Draft.Items[0].InstanceId);
	TestTrue(TEXT("a different room seed records a different RollSeed"),
		DraftC.Draft.Items[0].RollSeed != DraftA.Draft.Items[0].RollSeed);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_008AlreadyAppliedIdIsIdempotent,
	"UEMMO.Tasks.M3_008.AlreadyAppliedIdIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_008AlreadyAppliedIdIsIdempotent::RunTest(const FString& Parameters)
{
	FM3_008_ProfileSession Session = FM3_008_ProfileSession::Create(*this, TEXT("Applied"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_008_MakeBoundService(*this, Session, TEXT("Applied"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Merged semantics (task card option): an already-settled id answers
	// AlreadyApplied AND hands back the original draft unchanged - the random
	// result was generated once and no retry ever re-rolls it.
	const FRoomResult Result = M3_008_MakeResult(/*RunId*/ 11, /*SettlementId*/ 1101, /*Seed*/ 11001, /*bCleared*/ true);
	const FRewardBeginOutcome First = Service->BeginReward(Result, M3_008_MakeTrainingCatalog());
	TestEqual(TEXT("the first settlement is Applied"), First.Result, ERewardBeginResult::Applied);

	for (int32 Replay = 0; Replay < 3; ++Replay)
	{
		const FRewardBeginOutcome Again = Service->BeginReward(Result, M3_008_MakeTrainingCatalog());
		const FString Context = FString::Printf(TEXT("replay %d"), Replay);
		TestEqual(FString::Printf(TEXT("replay %d answers AlreadyApplied"), Replay), Again.Result, ERewardBeginResult::AlreadyApplied);
		TestTrue(FString::Printf(TEXT("replay %d reports no error"), Replay), Again.Error.IsEmpty());
		TestTrue(FString::Printf(TEXT("replay %d returns the original draft field-for-field"), Replay),
			M3_008_DraftsEqual(Again.Draft, First.Draft));
	}

	// The applied set guard (filled by the M3-009 claim flow) is empty in
	// M3-008 by design: the draft path above is the only AlreadyApplied
	// source, so the profile holds exactly the one original draft.
	TestEqual(TEXT("the profile holds exactly one draft after all replays"), Session.Profile->GetPendingRewards().Num(), 1);
	TestTrue(TEXT("a never-settled id is not in the applied set"), !Session.Profile->IsSettlementApplied(999999));

	Session.TearDown();
	return true;
}

#endif
