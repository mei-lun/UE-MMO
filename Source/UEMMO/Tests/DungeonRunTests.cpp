// M7-004: the continuous two-room dungeon - the strict sequence parser, the
// coordinator's run lifecycle (start / room advance at the door / the ONE
// final settlement / failure-and-abort / stale-callback drops), the carried
// snapshot consumption and the free-select flow staying untouched. The
// coordinator is driven through a REAL room session (a WorldSubsystem inside
// a CreateWorld world); the travel/reward seams are counting lambdas (the
// real travel is the GameFlow OpenLevel path; the real reward is BeginReward).

#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

#include "../Dungeon/DungeonCoordinatorSubsystem.h"
#include "../Dungeon/DungeonSequenceParser.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M7_004
{
	static URoomDefinition* MakeRoomDefinition(UObject* Outer, const TCHAR* RoomId)
	{
		URoomDefinition* Definition = NewObject<URoomDefinition>(Outer);
		Definition->RoomId = FName(RoomId);
		Definition->MapPath = FSoftObjectPath(TEXT("/Game/UEMMO/Maps/L_CombatRoom01"));
		Definition->RewardTableId = FName(TEXT("standard_clear"));
		Definition->BoundsCenter = FVector::ZeroVector;
		Definition->BoundsSize = FVector(1200.f, 800.f, 400.f);
		return Definition;
	}

	/** A rooted game world + its room session (the per-world binding target). */
	struct FSessionFixture
	{
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;

		bool Make(FAutomationTestBase& Test)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false);
			if (!Test.TestNotNull(TEXT("fixture: the game world exists"), World))
			{
				return false;
			}
			World->AddToRoot();
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			return Test.TestNotNull(TEXT("fixture: the room session exists"), Session);
		}

		void TearDown()
		{
			if (World != nullptr)
			{
				// The world object releases with the root (the M5-067 shared-
				// process GC territory - DestroyWorld is engine-static-locked
				// here); the fixture keeps exactly two small worlds per run.
				World->RemoveFromRoot();
				World = nullptr;
			}
		}
	};

	/** The dungeon coordinator with the counting seams bound. */
	struct FCoordinatorFixture
	{
		UDungeonCoordinatorSubsystem* Coordinator = nullptr;
		int32 TravelCount = 0;
		FName LastTravelRoom = NAME_None;
		int32 RewardCount = 0;
		uint64 LastSettlementId = 0;

		void Bind()
		{
			// A GameInstanceSubsystem requires a GameInstance-chain outer (the
			// recurring ClassWithin assert).
			UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
			Owner->AddToRoot();
			Coordinator = NewObject<UDungeonCoordinatorSubsystem>(Owner);
			Coordinator->ReloadSequenceTable();
			Coordinator->SetTravelProvider([this](FName RoomId)
			{
				++TravelCount;
				LastTravelRoom = RoomId;
				return true;
			});
			Coordinator->SetRewardProvider([this](const FRoomResult& Result)
			{
				++RewardCount;
				LastSettlementId = Result.SettlementId;
				return true;
			});
		}
	};
}

using namespace UE::UEMMO::Tasks::M7_004;

// -- 1: the sequence parser (happy path + the named refusals) ------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_004SequenceParser,
	"UEMMO.Tasks.M7_004.SequenceParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_004SequenceParser::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	TArray<FDungeonSequenceRow> Rows;
	TArray<FString> Problems;
	Test.TestTrue(TEXT("the shipped table parses"),
		FDungeonSequenceParser::LoadFromFile(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/dungeon_sequences.json")), Rows, Problems));
	if (Rows.Num() > 0)
	{
		Test.TestEqual(TEXT("the shipped dungeon id"), Rows[0].DungeonId.ToString(), FString(TEXT("dungeon_lorien_double")));
		Test.TestEqual(TEXT("the shipped sequence has two rooms"), Rows[0].RoomIds.Num(), 2);
		Test.TestEqual(TEXT("the shipped xp reward"), Rows[0].XPReward, 120);
	}

	// The named refusals: duplicate id, one-room sequence, non-positive XP,
	// duplicate room inside one sequence, wrong schema version.
	struct FCase { const TCHAR* Name; FString Text; };
	const FCase Cases[] = {
		{TEXT("a duplicate dungeon id refuses"),
			TEXT("{\"schema_version\":1,\"rows\":[{\"dungeon_id\":\"d\",\"room_ids\":[\"a\",\"b\"],\"xp_reward\":10},")
			TEXT("{\"dungeon_id\":\"d\",\"room_ids\":[\"c\",\"e\"],\"xp_reward\":10}]}")},
		{TEXT("a one-room sequence refuses"),
			TEXT("{\"schema_version\":1,\"rows\":[{\"dungeon_id\":\"d\",\"room_ids\":[\"a\"],\"xp_reward\":10}]}")},
		{TEXT("a non-positive xp refuses"),
			TEXT("{\"schema_version\":1,\"rows\":[{\"dungeon_id\":\"d\",\"room_ids\":[\"a\",\"b\"],\"xp_reward\":0}]}")},
		{TEXT("a duplicated room id refuses"),
			TEXT("{\"schema_version\":1,\"rows\":[{\"dungeon_id\":\"d\",\"room_ids\":[\"a\",\"a\"],\"xp_reward\":10}]}")},
		{TEXT("a wrong schema version refuses"),
			TEXT("{\"schema_version\":2,\"rows\":[{\"dungeon_id\":\"d\",\"room_ids\":[\"a\",\"b\"],\"xp_reward\":10}]}")},
	};
	for (const FCase& Case : Cases)
	{
		TArray<FDungeonSequenceRow> Rejected;
		TArray<FString> RejectedProblems;
		Test.TestTrue(Case.Name, !FDungeonSequenceParser::ParseFromText(Case.Text, Rejected, RejectedProblems));
	}
	return true;
}

// -- 2: the coordinator lifecycle (start / advance / one settlement / abort) ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_004CoordinatorLifecycle,
	"UEMMO.Tasks.M7_004.CoordinatorLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_004CoordinatorLifecycle::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FSessionFixture Room;
	if (!Room.Make(Test))
	{
		return true;
	}
	FCoordinatorFixture Dungeon;
	Dungeon.Bind();
	Dungeon.Coordinator->RegisterRoomDefinition(MakeRoomDefinition(GetTransientPackage(), TEXT("room_combat_01")));
	Dungeon.Coordinator->RegisterRoomDefinition(MakeRoomDefinition(GetTransientPackage(), TEXT("room_combat_02")));
	Dungeon.Coordinator->BindSession(Room.Session);

	// The run starts; the free-select flow (no run) is untouched by design.
	Test.TestTrue(TEXT("the dungeon run starts"),
		Dungeon.Coordinator->StartDungeonRun(FName(TEXT("dungeon_lorien_double")),
			FGuid(0x0D040000u, 1u, 1u, 1u), TEXT("revision-a")));
	Test.TestTrue(TEXT("the run is active"), Dungeon.Coordinator->IsRunActive());
	Test.TestFalse(TEXT("a second simultaneous run refuses"),
		Dungeon.Coordinator->StartDungeonRun(FName(TEXT("dungeon_lorien_double")),
			FGuid(0x0D040000u, 2u, 2u, 2u), TEXT("revision-a")));
	Test.TestFalse(TEXT("an unknown dungeon refuses"),
		Dungeon.Coordinator->StartDungeonRun(FName(TEXT("no_such_dungeon")),
			FGuid(0x0D040000u, 3u, 3u, 3u), TEXT("revision-a")));

	// Room 1: a run starts, clears (NO settlement - the internal room), the
	// door opens, the exit advances the run with a snapshot.
	Test.TestTrue(TEXT("room 1's run starts"),
		Room.Session->StartRoom(Dungeon.Coordinator->GetCurrentRoomDefinition()));
	Test.TestTrue(TEXT("room 1 clears"), Room.Session->MarkCleared());

	// The clear event carries the room result to the coordinator.
	// (The session broadcast happens inside MarkCleared via OnRunEnded.)
	Test.TestFalse(TEXT("an internal room clear issues no settlement"), Dungeon.RewardCount == 1);
	Test.TestTrue(TEXT("the internal clear is recognized"), Dungeon.Coordinator->IsDungeonRunRoom(FName(TEXT("room_combat_01")), Room.Session->GetRunId()));
	Test.TestFalse(TEXT("a foreign room id is not recognized"),
		Dungeon.Coordinator->IsDungeonRunRoom(FName(TEXT("room_combat_02")), Room.Session->GetRunId()));

	// The door: exit accepted -> snapshot + travel to room 2.
	Dungeon.Coordinator->SetSnapshotProviders(
		[]() { return 55.0f; },
		[]() { return 42; },
		[]() { TMap<FName, float> M; M.Add(FName(TEXT("singlekick_w")), 1.25f); return M; });
	Test.TestTrue(TEXT("the exit begins"), Room.Session->BeginExit());
	Test.TestEqual(TEXT("the travel went to room 2"), Dungeon.LastTravelRoom, FName(TEXT("room_combat_02")));
	Test.TestEqual(TEXT("one travel happened"), Dungeon.TravelCount, 1);

	// The carried snapshot waits for the next room's pawn (exactly once).
	float CarriedHP = -1.f;
	int32 CarriedMP = -1;
	TMap<FName, float> CarriedCooldowns;
	Test.TestTrue(TEXT("the snapshot is pending"), Dungeon.Coordinator->ConsumePendingRoomSnapshot(CarriedHP, CarriedMP, CarriedCooldowns));
	Test.TestEqual(TEXT("the HP carried"), CarriedHP, 55.0f);
	Test.TestEqual(TEXT("the MP carried"), CarriedMP, 42);
	Test.TestEqual(TEXT("the cooldown remainder carried"), static_cast<int32>(CarriedCooldowns.Contains(FName(TEXT("singlekick_w")))), 1);
	Test.TestFalse(TEXT("the snapshot consumes exactly once"), Dungeon.Coordinator->ConsumePendingRoomSnapshot(CarriedHP, CarriedMP, CarriedCooldowns));

	// Room 2: a fresh world session (the restart simulation) rebinds; the
	// final clear issues the ONE settlement with the dungeon identity.
	FSessionFixture Room2;
	if (Room2.Make(Test))
	{
		Dungeon.Coordinator->BindSession(Room2.Session);
		Test.TestTrue(TEXT("room 2's run starts"),
			Room2.Session->StartRoom(Dungeon.Coordinator->GetCurrentRoomDefinition()));
		Test.TestTrue(TEXT("room 2 clears"), Room2.Session->MarkCleared());
		Test.TestEqual(TEXT("exactly one settlement issued"), Dungeon.RewardCount, 1);
		const FDungeonRunState& RunState = Dungeon.Coordinator->GetRunState();
		Test.TestEqual(TEXT("the settlement carries the run token"),
			Dungeon.LastSettlementId == RunState.SettlementToken, true);
		const FRoomResult* Display = Dungeon.Coordinator->GetDungeonDisplayResult();
		if (Test.TestNotNull(TEXT("the display result exists"), const_cast<FRoomResult*>(Display)))
		{
			Test.TestEqual(TEXT("the display room is the final room"),
				Display->RoomId.ToString(), FString(TEXT("room_combat_02")));
			Test.TestEqual(TEXT("the display result is a clear"), Display->bCleared, true);
		}

		// A duplicate clear never re-settles (first-terminal-wins end to end).
		Test.TestFalse(TEXT("a second mark refuses"), Room2.Session->MarkCleared());
		Test.TestEqual(TEXT("the settlement stays single"), Dungeon.RewardCount, 1);

		// The final door ends the run and returns to the menu (NAME_None).
		Test.TestTrue(TEXT("the final exit begins"), Room2.Session->BeginExit());
		Test.TestEqual(TEXT("the menu travel happened"), Dungeon.LastTravelRoom, FName(NAME_None));
		Test.TestFalse(TEXT("the run ended"), Dungeon.Coordinator->IsRunActive());
		Room2.TearDown();
	}

	// The failure path: a fresh run aborts with NO settlement (记失败并回菜单).
	Dungeon.RewardCount = 0;
	Test.TestTrue(TEXT("a second run can start after the end"),
		Dungeon.Coordinator->StartDungeonRun(FName(TEXT("dungeon_lorien_double")),
			FGuid(0x0D040000u, 9u, 9u, 9u), TEXT("revision-a")));
	Dungeon.Coordinator->AbortRun(true);
	Test.TestFalse(TEXT("the aborted run is inactive"), Dungeon.Coordinator->IsRunActive());
	Test.TestEqual(TEXT("the abort issued no settlement"), Dungeon.RewardCount, 0);
	Test.TestEqual(TEXT("the abort traveled to the menu"), Dungeon.LastTravelRoom, FName(NAME_None));

	Room.TearDown();
	return true;
}
#endif
