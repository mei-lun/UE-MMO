#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_010
{
	// Contract values the four catalog entries must reproduce: the frame and
	// window initial values from interface contract section 3 (mirroring
	// Data/combat-attacks.json). The runtime catalog never reads that JSON;
	// these literals are the checked-in contract the assets were generated
	// from (M1-009).
	struct FExpectedCatalogEntry
	{
		const TCHAR* AttackId;
		int32 DurationFrames;
		int32 ActiveStartFrame;
		int32 ActiveEndFrame;
		int32 CancelStartFrame;
		int32 CancelEndFrame;
	};

	static const FExpectedCatalogEntry ExpectedEntries[] =
	{
		{ TEXT("light_01"), 26, 7, 11, 12, 24 },
		{ TEXT("light_02"), 32, 9, 14, 16, 29 },
		{ TEXT("launcher"), 40, 12, 17, 18, 32 },
		{ TEXT("aerial_01"), 28, 6, 10, 12, 23 }
	};

	static const int32 ExpectedEntryCount = sizeof(ExpectedEntries) / sizeof(ExpectedEntries[0]);

	// Creates a fresh catalog and initializes it from Config/DefaultGame.ini
	// ([/Script/UEMMO.AttackCatalog] AttackAssetPaths). Returns null after
	// reporting the initialization failure so the caller can bail out early.
	static UAttackCatalog* NewConfigInitializedCatalog(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		if (!Test.TestTrue(TEXT("InitializeFromConfig succeeds with the four explicit DefaultGame.ini references"),
			Catalog->InitializeFromConfig(Error)))
		{
			Test.AddError(FString::Printf(TEXT("catalog initialization failed: %s"), *Error.ToString()));
			return nullptr;
		}
		return Catalog;
	}

	// Bare definitions are enough for catalog identity checks: the build only
	// keys entries by AttackId and never validates data fields (per-entry data
	// validation stays in ValidateAttackDefinition, covered by M1-007).
	static UAttackDefinition* MakeBareDefinition(const TCHAR* AttackId)
	{
		UAttackDefinition* Definition = NewObject<UAttackDefinition>();
		Definition->AttackId = FName(AttackId);
		return Definition;
	}
}

using namespace UE::UEMMO::Tasks::M1_010;

// All four known ids resolve through Find and each hit reports the id it was
// looked up by (no cross-wiring between entries).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_010CatalogFindsAllFourKnownIds,
	"UEMMO.Tasks.M1_010.CatalogFindsAllFourKnownIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_010CatalogFindsAllFourKnownIds::RunTest(const FString& Parameters)
{
	UAttackCatalog* Catalog = NewConfigInitializedCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	for (int32 Index = 0; Index < ExpectedEntryCount; ++Index)
	{
		const FExpectedCatalogEntry& Expected = ExpectedEntries[Index];
		const UAttackDefinition* Found = Catalog->Find(FName(Expected.AttackId));
		if (TestNotNull(FString::Printf(TEXT("Find('%s') returns a definition"), Expected.AttackId), Found))
		{
			TestEqual(FString::Printf(TEXT("Find('%s') returns the definition whose AttackId matches"), Expected.AttackId),
				Found->AttackId, FName(Expected.AttackId));
		}
	}
	return true;
}

// The catalog exposes the complete four-entry sequence in configuration order
// and every entry carries the contract duration and frame windows.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_010CatalogSequenceMatchesContractValues,
	"UEMMO.Tasks.M1_010.CatalogSequenceMatchesContractValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_010CatalogSequenceMatchesContractValues::RunTest(const FString& Parameters)
{
	UAttackCatalog* Catalog = NewConfigInitializedCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}

	const TArray<TObjectPtr<UAttackDefinition>>& Entries = Catalog->GetEntries();
	if (!TestEqual(TEXT("catalog holds exactly the four configured definitions"), Entries.Num(), ExpectedEntryCount))
	{
		return true;
	}
	for (int32 Index = 0; Index < ExpectedEntryCount; ++Index)
	{
		const FExpectedCatalogEntry& Expected = ExpectedEntries[Index];
		const UAttackDefinition* Entry = Entries[Index].Get();
		if (!TestNotNull(FString::Printf(TEXT("catalog entry %d is loaded (non-null)"), Index), Entry))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("catalog entry %d is '%s' in configuration order"), Index, Expected.AttackId),
			Entry->AttackId, FName(Expected.AttackId));
		TestEqual(FString::Printf(TEXT("'%s' DurationFrames matches the contract value"), Expected.AttackId),
			Entry->DurationFrames, Expected.DurationFrames);
		TestEqual(FString::Printf(TEXT("'%s' ActiveWindow.StartFrame matches the contract value"), Expected.AttackId),
			Entry->ActiveWindow.StartFrame, Expected.ActiveStartFrame);
		TestEqual(FString::Printf(TEXT("'%s' ActiveWindow.EndFrame matches the contract value"), Expected.AttackId),
			Entry->ActiveWindow.EndFrame, Expected.ActiveEndFrame);
		TestEqual(FString::Printf(TEXT("'%s' CancelWindow.StartFrame matches the contract value"), Expected.AttackId),
			Entry->CancelWindow.StartFrame, Expected.CancelStartFrame);
		TestEqual(FString::Printf(TEXT("'%s' CancelWindow.EndFrame matches the contract value"), Expected.AttackId),
			Entry->CancelWindow.EndFrame, Expected.CancelEndFrame);
	}
	return true;
}

// A missing id returns nullptr, keeps returning nullptr on repeated lookups
// and never aliases (falls back to) the first catalog entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_010CatalogUnknownIdReturnsNull,
	"UEMMO.Tasks.M1_010.CatalogUnknownIdReturnsNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_010CatalogUnknownIdReturnsNull::RunTest(const FString& Parameters)
{
	UAttackCatalog* Catalog = NewConfigInitializedCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}

	const FName MissingId(TEXT("not_in_catalog"));
	TestNull(TEXT("Find of an unknown id returns nullptr"), Catalog->Find(MissingId));
	TestNull(TEXT("a repeated unknown-id lookup still returns nullptr"), Catalog->Find(MissingId));
	TestTrue(TEXT("an unknown id never falls back to the first catalog entry"),
		Catalog->Find(MissingId) != Catalog->Find(FName(TEXT("light_01"))));
	return true;
}

// The injectable build step rejects duplicate ids with the id named in the
// error, leaves the catalog empty, and still builds distinct id arrays.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_010BuildRejectsDuplicateIds,
	"UEMMO.Tasks.M1_010.BuildRejectsDuplicateIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_010BuildRejectsDuplicateIds::RunTest(const FString& Parameters)
{
	UAttackCatalog* Catalog = NewObject<UAttackCatalog>();

	UAttackDefinition* First = MakeBareDefinition(TEXT("light_dup"));
	UAttackDefinition* Other = MakeBareDefinition(TEXT("launcher"));
	UAttackDefinition* Duplicate = MakeBareDefinition(TEXT("light_dup"));

	FText Error;
	TestFalse(TEXT("a definition array with a repeated AttackId is rejected"),
		Catalog->BuildFromDefinitions({ First, Other, Duplicate }, Error));
	TestTrue(FString::Printf(TEXT("duplicate-id error names the id (error: '%s')"), *Error.ToString()),
		Error.ToString().Contains(TEXT("light_dup")));
	TestEqual(TEXT("a rejected build leaves the catalog empty"), Catalog->GetEntries().Num(), 0);
	TestNull(TEXT("a rejected build leaves no findable entry"), Catalog->Find(FName(TEXT("light_dup"))));

	// Positive control over the same injectable entry point: distinct ids
	// build successfully and every built entry stays findable by its id.
	FText BuildError;
	TestTrue(TEXT("a definition array with distinct AttackIds builds the catalog"),
		Catalog->BuildFromDefinitions({ First, Other }, BuildError));
	TestEqual(TEXT("the rebuilt catalog holds exactly the built entries"), Catalog->GetEntries().Num(), 2);
	TestEqual(TEXT("Find returns the entry built for 'light_dup'"),
		Catalog->Find(FName(TEXT("light_dup"))), static_cast<const UAttackDefinition*>(First));
	TestEqual(TEXT("Find returns the entry built for 'launcher'"),
		Catalog->Find(FName(TEXT("launcher"))), static_cast<const UAttackDefinition*>(Other));
	return true;
}

// Initialization fails fast on references outside the allowed content root
// and on unloadable assets, and never leaves a half-initialized catalog.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_010InitializeFromPathsFailsFast,
	"UEMMO.Tasks.M1_010.InitializeFromPathsFailsFast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_010InitializeFromPathsFailsFast::RunTest(const FString& Parameters)
{
	// A reference outside /Game/UEMMO/Combat is rejected before any load.
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		TestFalse(TEXT("a path outside /Game/UEMMO/Combat is rejected"),
			Catalog->InitializeFromPaths({ TEXT("/Game/Other/DA_evil.DA_evil") }, Error));
		TestTrue(FString::Printf(TEXT("out-of-scope error names the allowed root (error: '%s')"), *Error.ToString()),
			Error.ToString().Contains(TEXT("/Game/UEMMO/Combat")));
		TestEqual(TEXT("an out-of-scope rejection keeps the catalog empty"), Catalog->GetEntries().Num(), 0);
	}

	// A non-loadable reference inside the root fails with the path named.
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		TestFalse(TEXT("a non-loadable asset path fails initialization"),
			Catalog->InitializeFromPaths({ TEXT("/Game/UEMMO/Combat/Definitions/DA_missing.DA_missing") }, Error));
		TestTrue(FString::Printf(TEXT("load-failure error names the asset (error: '%s')"), *Error.ToString()),
			Error.ToString().Contains(TEXT("DA_missing")));
		TestEqual(TEXT("a failed load keeps the catalog empty"), Catalog->GetEntries().Num(), 0);
	}

	// One missing asset among valid ones fails the whole initialization.
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		TestFalse(TEXT("one missing asset fails the whole initialization"),
			Catalog->InitializeFromPaths(
			{
				TEXT("/Game/UEMMO/Combat/Definitions/DA_light_01.DA_light_01"),
				TEXT("/Game/UEMMO/Combat/Definitions/DA_missing.DA_missing")
			}, Error));
		TestEqual(TEXT("fail-fast initialization leaves the catalog empty"), Catalog->GetEntries().Num(), 0);
		TestNull(TEXT("no entry of a failed initialization is findable"), Catalog->Find(FName(TEXT("light_01"))));
	}
	return true;
}

#endif
