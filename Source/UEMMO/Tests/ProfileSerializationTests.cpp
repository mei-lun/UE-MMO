// M3-013: versioned profile save structure and round-trip serialization
// (interface contract section 8). Pins the pure serializer pair:
// - a non-empty profile (inventory, equipment bindings, pending reward drafts,
//   applied settlement ids, identity/level/xp) round trips through
//   UProfileSaveGame with every id and stat identical;
// - an empty inventory with a valid (non-empty) CharacterId is also legal;
// - validation rejects: unknown future schema (version 2, and 0) WITHOUT
//   touching the original save data, duplicate instance ids (inside the
//   inventory and across inventory/pending drafts), dangling or duplicated
//   equipment bindings, out-of-range levels (0/11; 1 and 10 stay legal),
//   non-finite stats (NaN/Inf) and an inventory over its 30-slot capacity;
// - a rejected load never touches any Out* parameter (no half-restored
//   profile, no silently dropped items) and reports an error that names the
//   offending field;
// - the save structure contains no object-reference property (no Actor, no
//   World, no asset pointer), verified through reflection with a positive
//   control (UGameInstance does contain such properties).
//
// Pure-logic suite: USaveGame objects are created in memory with NewObject
// (transient package); nothing touches disk, World or wall clock. Item
// instances travel as their M3-001 ToStringSnapshot strings, so the instance
// round trip reuses the already-pinned M3-001 format.
//
// Stub-failure note: against the red stub (empty ToSaveGame, always-failing
// FromSaveGame) the structural test 9 passes trivially, the always-rejecting
// shape of tests 3/4/6/8 still fails on the concrete expected error codes and
// preserved-data assertions, and every other test fails on the missing
// mapping/restore behavior.

#include "Misc/AutomationTest.h"

#include "../Persistence/ProfileSerializer.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"

#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "Engine/GameInstance.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_013
{
	/**
	 * Builds one deterministic item instance: the identity is derived from
	 * Seed (distinct seeds never collide), the stats are exactly representable
	 * binary fractions so the M3-001 snapshot text (6 fractional digits,
	 * trailing-zero trimmed) round trips bit-exactly.
	 */
	static FItemInstance M3_013_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP, int32 ItemLevel)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x5EED0000u + Seed, 0xC0FFEEu, 0x00F00Du, Seed * 7u + 1u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(0x7FEDCBA987654321ULL) + static_cast<int64>(Seed);
		Instance.RolledStats.Attack = Attack;
		Instance.RolledStats.Defense = Defense;
		Instance.RolledStats.MaxHP = MaxHP;
		Instance.Level = ItemLevel;
		return Instance;
	}

	/** Fixed character identity used by every fixture (never all-zero). */
	static FGuid M3_013_FixtureCharacterId()
	{
		return FGuid(0x13572468u, 0x9BCEu, 0x2468ACE0u, 0x13579BDFu);
	}

	/** Deterministic guid NOT stored by any fixture (for dangling references). */
	static FGuid M3_013_UnknownInstanceId()
	{
		return FGuid(0x99999999u, 0x1u, 0x2u, 0x3u);
	}

	/**
	 * In-memory profile state mirroring what UProfileSubsystem holds after a
	 * few played runs: identity, Level 3/XP 45, three inventory items, two of
	 * them equipped, two unclaimed reward drafts and three applied ids.
	 */
	struct FM3_013_Fixture
	{
		FProfileSnapshot Snapshot;
		FInventoryModel Inventory;
		TArray<FPendingReward> PendingRewards;
		TSet<uint64> AppliedSettlementIds;
		TMap<EItemSlot, FGuid> EquippedMap;

		/**
		 * Builds the fixture; setup failures are reported on the test (so a
		 * broken fixture never masquerades as a serializer failure).
		 */
		static FM3_013_Fixture Make(FAutomationTestBase& Test, bool bPopulated)
		{
			FM3_013_Fixture Fixture;
			Fixture.Snapshot.CharacterId = M3_013_FixtureCharacterId();
			Fixture.Snapshot.Level = bPopulated ? 3 : 1;
			Fixture.Snapshot.XP = bPopulated ? 45 : 0;
			// Derived stats are the level-formula BASE row: the save never
			// stores derived stats, they are recomputed on load (and the
			// equipment sum is re-derived by the gameplay layer afterwards).
			Fixture.Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(Fixture.Snapshot.Level);
			Fixture.Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(Fixture.Snapshot.Level);
			Fixture.Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(Fixture.Snapshot.Level);
			if (!bPopulated)
			{
				return Fixture;
			}

			const FItemInstance Weapon = M3_013_MakeInstance(1, TEXT("weapon_training"), 3.5f, 0.25f, 0.0f, 1);
			const FItemInstance Armor = M3_013_MakeInstance(2, TEXT("armor_leather"), 0.0f, 1.5f, 25.5f, 1);
			const FItemInstance Ring = M3_013_MakeInstance(3, TEXT("accessory_ring"), 1.25f, 0.0f, 10.0f, 2);
			if (Fixture.Inventory.TryAdd(Weapon) != EInventoryAddResult::Added)
			{
				Test.AddError(TEXT("fixture: TryAdd(weapon) was rejected"));
			}
			if (Fixture.Inventory.TryAdd(Armor) != EInventoryAddResult::Added)
			{
				Test.AddError(TEXT("fixture: TryAdd(armor) was rejected"));
			}
			if (Fixture.Inventory.TryAdd(Ring) != EInventoryAddResult::Added)
			{
				Test.AddError(TEXT("fixture: TryAdd(ring) was rejected"));
			}
			Fixture.EquippedMap.Add(EItemSlot::Weapon, Weapon.InstanceId);
			Fixture.EquippedMap.Add(EItemSlot::Armor, Armor.InstanceId);

			FPendingReward FirstDraft;
			FirstDraft.SettlementId = 1001;
			FirstDraft.XP = 50;
			FirstDraft.Items.Add(M3_013_MakeInstance(11, TEXT("weapon_training"), 2.0f, 0.0f, 0.0f, 1));

			FPendingReward SecondDraft;
			SecondDraft.SettlementId = 1002;
			SecondDraft.XP = 50;
			SecondDraft.Items.Add(M3_013_MakeInstance(12, TEXT("armor_leather"), 0.0f, 2.5f, 15.5f, 1));
			SecondDraft.Items.Add(M3_013_MakeInstance(13, TEXT("accessory_ring"), 0.75f, 0.75f, 8.25f, 1));

			Fixture.PendingRewards.Add(FirstDraft);
			Fixture.PendingRewards.Add(SecondDraft);
			Fixture.AppliedSettlementIds.Add(900ull);
			Fixture.AppliedSettlementIds.Add(901ull);
			Fixture.AppliedSettlementIds.Add(0xDEADBEEFCAFEF00Dull);
			return Fixture;
		}

		/** Field-named access to the three fixture inventory items. */
		static const FItemInstance* FindInstance(const FInventoryModel& Model, const FGuid& InstanceId)
		{
			for (const FItemInstance& Candidate : Model.GetAll())
			{
				if (Candidate.InstanceId == InstanceId)
				{
					return &Candidate;
				}
			}
			return nullptr;
		}
	};

	/**
	 * Sentinel-filled Out* bundle for rejection tests: after a FAILED load,
	 * every output must still carry exactly these sentinel values - proof that
	 * the serializer never writes partial state on the error path.
	 */
	struct FM3_013_LoadOuts
	{
		FProfileSnapshot Snapshot;
		FInventoryModel Inventory;
		TArray<FPendingReward> Pending;
		TSet<uint64> Applied;
		TMap<EItemSlot, FGuid> Equipped;
		FProfileLoadError Error;

		static FM3_013_LoadOuts MakeSentinel()
		{
			FM3_013_LoadOuts Outs;
			Outs.Snapshot.CharacterId = FGuid(0xDEAD0001u, 0x1u, 0x1u, 0x1u);
			Outs.Snapshot.Level = -7;
			Outs.Snapshot.XP = -9;
			FItemInstance SentinelItem;
			SentinelItem.InstanceId = FGuid(0xF000000Fu, 0xFu, 0xFu, 0xFu);
			SentinelItem.DefinitionId = FName(TEXT("sentinel"));
			Outs.Inventory.TryAdd(SentinelItem);
			FPendingReward SentinelDraft;
			SentinelDraft.SettlementId = 0xEEEEull;
			SentinelDraft.XP = 1;
			SentinelDraft.Items.Add(SentinelItem);
			Outs.Pending.Add(SentinelDraft);
			Outs.Applied.Add(0x1234ull);
			Outs.Equipped.Add(EItemSlot::Weapon, FGuid(0xBEEF0002u, 0x2u, 0x2u, 0x2u));
			return Outs;
		}

		bool StillSentinel() const
		{
			const FItemInstance* SentinelItem = FM3_013_Fixture::FindInstance(
				Inventory, FGuid(0xF000000Fu, 0xFu, 0xFu, 0xFu));
			return Snapshot.CharacterId == FGuid(0xDEAD0001u, 0x1u, 0x1u, 0x1u)
				&& Snapshot.Level == -7
				&& Snapshot.XP == -9
				&& Inventory.Count() == 1
				&& SentinelItem != nullptr
				&& SentinelItem->DefinitionId == FName(TEXT("sentinel"))
				&& Pending.Num() == 1
				&& Pending[0].SettlementId == 0xEEEEull
				&& Pending[0].XP == 1
				&& Applied.Num() == 1
				&& Applied.Contains(0x1234ull)
				&& Equipped.Num() == 1
				&& Equipped.Contains(EItemSlot::Weapon)
				&& Equipped[EItemSlot::Weapon] == FGuid(0xBEEF0002u, 0x2u, 0x2u, 0x2u);
		}
	};

	/**
	 * Runs FromSaveGame on Save and asserts the exact rejection contract:
	 * failed load, expected error code, a message that names the offending
	 * field part, and untouched sentinel outputs. Returns false when the
	 * caller should stop asserting further details.
	 */
	static bool M3_013_ExpectRejectedLoad(FAutomationTestBase& Test, const UProfileSaveGame* Save,
		EProfileLoadError ExpectedCode, const TCHAR* ExpectedMessagePart, const TCHAR* Scenario)
	{
		FM3_013_LoadOuts Outs = FM3_013_LoadOuts::MakeSentinel();
		if (FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending, Outs.Applied, Outs.Equipped, Outs.Error))
		{
			Test.AddError(FString::Printf(TEXT("%s: FromSaveGame unexpectedly succeeded on a save that must be rejected"), Scenario));
			return false;
		}
		Test.TestEqual(FString::Printf(TEXT("%s: expected error code"), Scenario),
			static_cast<int32>(Outs.Error.Code), static_cast<int32>(ExpectedCode));
		Test.TestTrue(FString::Printf(TEXT("%s: error message names the field ('%s' inside '%s')"),
				Scenario, ExpectedMessagePart, *Outs.Error.Message),
			Outs.Error.Message.Contains(ExpectedMessagePart));
		Test.TestTrue(FString::Printf(TEXT("%s: rejected load left every Out* parameter untouched"), Scenario),
			Outs.StillSentinel());
		return Outs.Error.Code == ExpectedCode && Outs.StillSentinel();
	}

	/** Asserts every field of one restored instance matches the original. */
	static void M3_013_ExpectSameInstance(FAutomationTestBase& Test,
		const FItemInstance& Actual, const FItemInstance& Expected, const TCHAR* What)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: InstanceId identical"), What), Actual.InstanceId == Expected.InstanceId);
		Test.TestTrue(FString::Printf(TEXT("%s: DefinitionId identical"), What), Actual.DefinitionId == Expected.DefinitionId);
		Test.TestTrue(FString::Printf(TEXT("%s: RollSeed identical"), What), Actual.RollSeed == Expected.RollSeed);
		Test.TestTrue(FString::Printf(TEXT("%s: Level identical"), What), Actual.Level == Expected.Level);
		Test.TestEqual(FString::Printf(TEXT("%s: RolledStats.Attack identical"), What),
			Actual.RolledStats.Attack, Expected.RolledStats.Attack, 1.e-6f);
		Test.TestEqual(FString::Printf(TEXT("%s: RolledStats.Defense identical"), What),
			Actual.RolledStats.Defense, Expected.RolledStats.Defense, 1.e-6f);
		Test.TestEqual(FString::Printf(TEXT("%s: RolledStats.MaxHP identical"), What),
			Actual.RolledStats.MaxHP, Expected.RolledStats.MaxHP, 1.e-6f);
	}

	/** Asserts an restored uint64 set carries exactly the original ids. */
	static void M3_013_ExpectSameUint64Set(FAutomationTestBase& Test,
		const TSet<uint64>& Actual, const TSet<uint64>& Expected, const TCHAR* What)
	{
		Test.TestEqual(FString::Printf(TEXT("%s: count"), What), Actual.Num(), Expected.Num());
		for (const uint64 Id : Expected)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: contains %llu"), What, Id), Actual.Contains(Id));
		}
	}

	/** Asserts a restored slot->instance map equals the original binding. */
	static void M3_013_ExpectSameEquipMap(FAutomationTestBase& Test,
		const TMap<EItemSlot, FGuid>& Actual, const TMap<EItemSlot, FGuid>& Expected, const TCHAR* What)
	{
		Test.TestEqual(FString::Printf(TEXT("%s: count"), What), Actual.Num(), Expected.Num());
		for (const TPair<EItemSlot, FGuid>& Pair : Expected)
		{
			const FGuid* Found = Actual.Find(Pair.Key);
			Test.TestTrue(FString::Printf(TEXT("%s: slot %d keeps its InstanceId"), What, static_cast<int32>(Pair.Key)),
				Found != nullptr && *Found == Pair.Value);
		}
	}

	/** Asserts restored pending drafts equal the originals field for field. */
	static void M3_013_ExpectSamePendingRewards(FAutomationTestBase& Test,
		const TArray<FPendingReward>& Actual, const TArray<FPendingReward>& Expected, const TCHAR* What)
	{
		Test.TestEqual(FString::Printf(TEXT("%s: draft count"), What), Actual.Num(), Expected.Num());
		for (int32 Index = 0; Index < Expected.Num() && Index < Actual.Num(); ++Index)
		{
			const FPendingReward& ActualDraft = Actual[Index];
			const FPendingReward& ExpectedDraft = Expected[Index];
			Test.TestTrue(FString::Printf(TEXT("%s[%d]: SettlementId identical"), What, Index),
				ActualDraft.SettlementId == ExpectedDraft.SettlementId);
			Test.TestEqual(FString::Printf(TEXT("%s[%d]: XP identical"), What, Index),
				ActualDraft.XP, ExpectedDraft.XP);
			Test.TestEqual(FString::Printf(TEXT("%s[%d]: item count"), What, Index),
				ActualDraft.Items.Num(), ExpectedDraft.Items.Num());
			for (int32 ItemIndex = 0; ItemIndex < ExpectedDraft.Items.Num() && ItemIndex < ActualDraft.Items.Num(); ++ItemIndex)
			{
				M3_013_ExpectSameInstance(Test, ActualDraft.Items[ItemIndex], ExpectedDraft.Items[ItemIndex],
					*FString::Printf(TEXT("%s[%d].Items[%d]"), What, Index, ItemIndex));
			}
		}
	}

	/**
	 * Structural walk: true when the property (or, recursively, its inner
	 * element/struct members) is any kind of object reference. Guarded by a
	 * positive control in the structure test itself.
	 */
	static bool M3_013_PropertyReferencesObject(FProperty* Property)
	{
		if (!Property)
		{
			return false;
		}
		if (Property->IsA<FObjectPropertyBase>() || Property->IsA<FInterfaceProperty>())
		{
			return true;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
		{
			return M3_013_PropertyReferencesObject(Array->Inner);
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(Property))
		{
			return M3_013_PropertyReferencesObject(Set->ElementProp);
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(Property))
		{
			return M3_013_PropertyReferencesObject(Map->KeyProp)
				|| M3_013_PropertyReferencesObject(Map->ValueProp);
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
		{
			for (TFieldIterator<FProperty> Member(Struct->Struct); Member; ++Member)
			{
				if (M3_013_PropertyReferencesObject(*Member))
				{
					return true;
				}
			}
		}
		return false;
	}

	/** Builds a save object directly (hand-authored, no serializer involved). */
	static UProfileSaveGame* M3_013_MakeRawSave()
	{
		UProfileSaveGame* Save = NewObject<UProfileSaveGame>(GetTransientPackage());
		Save->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
		Save->CharacterId = M3_013_FixtureCharacterId();
		Save->Level = 2;
		Save->XP = 10;
		return Save;
	}
}

using namespace UE::UEMMO::Tasks::M3_013;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013NonEmptyProfileRoundTripsCompletely,
	"UEMMO.Tasks.M3_013.NonEmptyProfileRoundTripsCompletely",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013NonEmptyProfileRoundTripsCompletely::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	const FM3_013_Fixture Fixture = FM3_013_Fixture::Make(*this, true);
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	TestNotNull(TEXT("ToSaveGame produced a save object"), Save);
	if (!Save)
	{
		return true;
	}

	// -- The saved data mirrors the inputs -------------------------------------
	TestEqual(TEXT("save stamps the current schema version"), Save->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion);
	TestEqual(TEXT("saved SchemaVersion is 1"), Save->SchemaVersion, 1);
	TestTrue(TEXT("save carries the character id"), Save->CharacterId == Fixture.Snapshot.CharacterId);
	TestEqual(TEXT("save carries the level"), Save->Level, 3);
	TestEqual(TEXT("save carries the xp"), Save->XP, 45);
	TestEqual(TEXT("save carries one snapshot string per inventory item"), Save->InventoryInstanceSnapshots.Num(), 3);
	TestEqual(TEXT("save carries one pair per equipped slot"), Save->EquippedIds.Num(), 2);
	TestEqual(TEXT("save carries one entry per pending draft"), Save->PendingRewards.Num(), 2);
	TestEqual(TEXT("save carries every applied settlement id"), Save->AppliedSettlementIds.Num(), 3);
	if (Save->InventoryInstanceSnapshots.Num() != 3 || Save->EquippedIds.Num() != 2
		|| Save->PendingRewards.Num() < 2)
	{
		Test.AddError(TEXT("setup: the save lost its containers; cannot continue the round trip"));
		return true;
	}
	TestEqual(TEXT("pending draft 0 carries its item snapshot"), Save->PendingRewards[0].ItemSnapshots.Num(), 1);
	TestEqual(TEXT("pending draft 1 carries its item snapshots"), Save->PendingRewards[1].ItemSnapshots.Num(), 2);
	TestTrue(TEXT("saved slot pairs carry a closed slot byte"),
		Save->EquippedIds[0].Slot <= static_cast<uint8>(EItemSlot::Accessory)
		&& Save->EquippedIds[1].Slot <= static_cast<uint8>(EItemSlot::Accessory));

	// -- Restoring ---------------------------------------------------------------
	FM3_013_LoadOuts Outs;
	if (!FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending, Outs.Applied, Outs.Equipped, Outs.Error))
	{
		Test.AddError(FString::Printf(TEXT("FromSaveGame rejected the serializer's own save: %s"), *Outs.Error.Message));
		return true;
	}

	// Identity and progress are identical.
	TestTrue(TEXT("CharacterId is identical after the round trip"), Outs.Snapshot.CharacterId == Fixture.Snapshot.CharacterId);
	TestEqual(TEXT("Level is identical after the round trip"), Outs.Snapshot.Level, 3);
	TestEqual(TEXT("XP is identical after the round trip"), Outs.Snapshot.XP, 45);
	// Derived stats are recomputed from the level formulas, never stored.
	TestEqual(TEXT("MaxHP is the recomputed level-formula row"), Outs.Snapshot.MaxHP, UProfileSubsystem::GetMaxHPForLevel(3));
	TestEqual(TEXT("Attack is the recomputed level-formula row"), Outs.Snapshot.Attack, UProfileSubsystem::GetAttackForLevel(3));
	TestEqual(TEXT("Defense is the recomputed level-formula row"), Outs.Snapshot.Defense, UProfileSubsystem::GetDefenseForLevel(3));

	// Inventory: every instance identical (id, definition, seed, level, stats).
	TestEqual(TEXT("inventory holds the original item count"), Outs.Inventory.Count(), 3);
	const FItemInstance Weapon = M3_013_MakeInstance(1, TEXT("weapon_training"), 3.5f, 0.25f, 0.0f, 1);
	const FItemInstance Armor = M3_013_MakeInstance(2, TEXT("armor_leather"), 0.0f, 1.5f, 25.5f, 1);
	const FItemInstance Ring = M3_013_MakeInstance(3, TEXT("accessory_ring"), 1.25f, 0.0f, 10.0f, 2);
	for (const FItemInstance& Expected : {Weapon, Armor, Ring})
	{
		const FItemInstance* Actual = FM3_013_Fixture::FindInstance(Outs.Inventory, Expected.InstanceId);
		if (!Actual)
		{
			Test.AddError(FString::Printf(TEXT("restored inventory lost instance %s"), *Expected.InstanceId.ToString()));
			continue;
		}
		M3_013_ExpectSameInstance(*this, *Actual, Expected, TEXT("restored inventory item"));
	}

	// Equipment bindings, pending drafts and applied ids are identical.
	M3_013_ExpectSameEquipMap(*this, Outs.Equipped, Fixture.EquippedMap, TEXT("restored equipment map"));
	M3_013_ExpectSamePendingRewards(*this, Outs.Pending, Fixture.PendingRewards, TEXT("restored pending drafts"));
	M3_013_ExpectSameUint64Set(*this, Outs.Applied, Fixture.AppliedSettlementIds, TEXT("restored applied settlement ids"));

	// The snapshot's own inventory copy mirrors the restored inventory too.
	TestEqual(TEXT("snapshot carries the restored inventory copy"), Outs.Snapshot.Inventory.Count(), 3);
	TestTrue(TEXT("snapshot inventory copy holds the weapon"),
		Outs.Snapshot.Inventory.Contains(Weapon.InstanceId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013EmptyInventoryWithValidCharacterIdRoundTrips,
	"UEMMO.Tasks.M3_013.EmptyInventoryWithValidCharacterIdRoundTrips",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013EmptyInventoryWithValidCharacterIdRoundTrips::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	// An empty inventory with a NON-EMPTY CharacterId is a legal save.
	const FM3_013_Fixture Fixture = FM3_013_Fixture::Make(*this, false);
	TestTrue(TEXT("fixture: CharacterId is a valid (non-empty) guid"), Fixture.Snapshot.CharacterId.IsValid());

	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	TestNotNull(TEXT("ToSaveGame produced a save object"), Save);
	if (!Save)
	{
		return true;
	}
	TestEqual(TEXT("empty inventory saves zero snapshot strings"), Save->InventoryInstanceSnapshots.Num(), 0);
	TestEqual(TEXT("no equipment is saved"), Save->EquippedIds.Num(), 0);
	TestEqual(TEXT("no pending drafts are saved"), Save->PendingRewards.Num(), 0);
	TestEqual(TEXT("no applied ids are saved"), Save->AppliedSettlementIds.Num(), 0);

	FM3_013_LoadOuts Outs;
	if (!FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending, Outs.Applied, Outs.Equipped, Outs.Error))
	{
		Test.AddError(FString::Printf(TEXT("FromSaveGame rejected the empty-inventory save: %s"), *Outs.Error.Message));
		return true;
	}
	TestTrue(TEXT("CharacterId survives the empty-profile round trip"), Outs.Snapshot.CharacterId == Fixture.Snapshot.CharacterId);
	TestEqual(TEXT("Level survives"), Outs.Snapshot.Level, 1);
	TestEqual(TEXT("XP survives"), Outs.Snapshot.XP, 0);
	TestEqual(TEXT("MaxHP is the recomputed level-1 row"), Outs.Snapshot.MaxHP, UProfileSubsystem::GetMaxHPForLevel(1));
	TestEqual(TEXT("restored inventory is empty"), Outs.Inventory.Count(), 0);
	TestEqual(TEXT("restored equipment is empty"), Outs.Equipped.Num(), 0);
	TestEqual(TEXT("restored pending drafts are empty"), Outs.Pending.Num(), 0);
	TestEqual(TEXT("restored applied ids are empty"), Outs.Applied.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013FutureSchemaVersionRejectedAndDataPreserved,
	"UEMMO.Tasks.M3_013.FutureSchemaVersionRejectedAndDataPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013FutureSchemaVersionRejectedAndDataPreserved::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	const FM3_013_Fixture Fixture = FM3_013_Fixture::Make(*this, true);
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!Save)
	{
		Test.AddError(TEXT("setup: ToSaveGame returned null"));
		return true;
	}
	if (Save->InventoryInstanceSnapshots.Num() != 3)
	{
		Test.AddError(TEXT("setup: the save does not carry the three inventory snapshots"));
		return true;
	}
	const FString FirstSnapshot = Save->InventoryInstanceSnapshots[0];

	// A version 2 file was written by a NEWER build: refuse to load AND leave
	// the save object (and by extension the underlying file) untouched.
	Save->SchemaVersion = 2;
	if (!M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::UnsupportedSchema, TEXT("schema_version"), TEXT("version=2")))
	{
		return true;
	}

	// Original data is not lost: every field the serializer wrote is still
	// exactly what was written (the rejection never mutated the save).
	TestEqual(TEXT("the rejected save keeps its schema_version=2"), Save->SchemaVersion, 2);
	TestTrue(TEXT("the rejected save keeps its CharacterId"), Save->CharacterId == Fixture.Snapshot.CharacterId);
	TestEqual(TEXT("the rejected save keeps its Level"), Save->Level, 3);
	TestTrue(TEXT("the rejected save keeps its inventory snapshots"),
		Save->InventoryInstanceSnapshots.Num() == 3 && Save->InventoryInstanceSnapshots[0] == FirstSnapshot);
	TestEqual(TEXT("the rejected save keeps its equipment pairs"), Save->EquippedIds.Num(), 2);
	TestEqual(TEXT("the rejected save keeps its pending drafts"), Save->PendingRewards.Num(), 2);
	TestEqual(TEXT("the rejected save keeps its applied ids"), Save->AppliedSettlementIds.Num(), 3);

	// Any unknown version (below the known range too) gets the same treatment.
	Save->SchemaVersion = 0;
	M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::UnsupportedSchema, TEXT("schema_version"), TEXT("version=0"));
	TestEqual(TEXT("the version=0 save still carries its inventory snapshots"), Save->InventoryInstanceSnapshots.Num(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013DuplicateInstanceIdsRejected,
	"UEMMO.Tasks.M3_013.DuplicateInstanceIdsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013DuplicateInstanceIdsRejected::RunTest(const FString& Parameters)
{
	const FItemInstance Weapon = M3_013_MakeInstance(1, TEXT("weapon_training"), 3.5f, 0.25f, 0.0f, 1);

	// Scenario 1: the same instance id twice inside the inventory.
	UProfileSaveGame* Save = M3_013_MakeRawSave();
	Save->InventoryInstanceSnapshots.Add(Weapon.ToStringSnapshot());
	Save->InventoryInstanceSnapshots.Add(Weapon.ToStringSnapshot());
	if (!M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::DuplicateInstanceId, TEXT("InventoryInstances"), TEXT("duplicate inside the inventory")))
	{
		return true;
	}

	// Scenario 2: unique in the inventory, but a pending draft item reuses the
	// same instance id - instances are globally unique, so this is rejected too.
	UProfileSaveGame* CrossSave = M3_013_MakeRawSave();
	CrossSave->InventoryInstanceSnapshots.Add(Weapon.ToStringSnapshot());
	FPendingRewardSaveEntry Entry;
	Entry.SettlementId = 2001;
	Entry.XP = 50;
	Entry.ItemSnapshots.Add(Weapon.ToStringSnapshot());
	CrossSave->PendingRewards.Add(Entry);
	M3_013_ExpectRejectedLoad(*this, CrossSave, EProfileLoadError::DuplicateInstanceId, TEXT("PendingRewards"), TEXT("duplicate across inventory and pending draft"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013InvalidOrDanglingEquipReferencesRejected,
	"UEMMO.Tasks.M3_013.InvalidOrDanglingEquipReferencesRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013InvalidOrDanglingEquipReferencesRejected::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	const FM3_013_Fixture Fixture = FM3_013_Fixture::Make(*this, true);
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!Save)
	{
		Test.AddError(TEXT("setup: ToSaveGame returned null"));
		return true;
	}
	if (Save->EquippedIds.Num() != 2)
	{
		Test.AddError(TEXT("setup: the save does not carry the two equipment pairs"));
		return true;
	}

	// Scenario 1: the equipped instance id exists nowhere in the inventory.
	Save->EquippedIds[0].InstanceId = M3_013_UnknownInstanceId();
	if (!M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::DanglingEquipReference, TEXT("EquippedIds"), TEXT("dangling equipment reference")))
	{
		return true;
	}

	// Scenario 2: a slot byte outside the closed Weapon/Armor/Accessory set.
	Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!Save || Save->EquippedIds.Num() != 2)
	{
		Test.AddError(TEXT("setup: the rebuilt save lost its equipment pairs"));
		return true;
	}
	Save->EquippedIds[0].Slot = 7;
	M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::InvalidEquipSlot, TEXT("EquippedIds"), TEXT("out-of-range slot byte"));

	// Scenario 3: the same slot bound twice (a TMap load would silently drop
	// one binding - that silent loss is exactly what the validator refuses).
	Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!Save || Save->EquippedIds.Num() != 2)
	{
		Test.AddError(TEXT("setup: the rebuilt save lost its equipment pairs"));
		return true;
	}
	Save->EquippedIds[1].Slot = Save->EquippedIds[0].Slot;
	M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::DuplicateEquipSlot, TEXT("EquippedIds"), TEXT("duplicate slot binding"));

	// Scenario 4: equipment may reference INVENTORY instances only (interface
	// contract section 8: "Slot->InstanceId, only references inventory
	// instances") - a binding to a pending-draft item id is dangling too.
	Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!Save || Save->EquippedIds.Num() != 2)
	{
		Test.AddError(TEXT("setup: the rebuilt save lost its equipment pairs"));
		return true;
	}
	const FGuid PendingItemId = M3_013_MakeInstance(11, TEXT("weapon_training"), 2.0f, 0.0f, 0.0f, 1).InstanceId;
	Save->EquippedIds[0].InstanceId = PendingItemId;
	M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::DanglingEquipReference, TEXT("EquippedIds"), TEXT("binding to a pending-draft item"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013LevelOutsideClosedRangeRejected,
	"UEMMO.Tasks.M3_013.LevelOutsideClosedRangeRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013LevelOutsideClosedRangeRejected::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	const FM3_013_Fixture Fixture = FM3_013_Fixture::Make(*this, true);
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!Save)
	{
		Test.AddError(TEXT("setup: ToSaveGame returned null"));
		return true;
	}

	Save->Level = 0;
	if (!M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::InvalidLevel, TEXT("Level"), TEXT("level 0")))
	{
		return true;
	}
	Save->Level = 11;
	M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::InvalidLevel, TEXT("Level"), TEXT("level 11"));

	// The closed boundaries stay legal: levels 1 and 10 load fine.
	for (const int32 BoundaryLevel : {1, 10})
	{
		FM3_013_Fixture BoundaryFixture = FM3_013_Fixture::Make(*this, false);
		BoundaryFixture.Snapshot.Level = BoundaryLevel;
		BoundaryFixture.Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(BoundaryLevel);
		BoundaryFixture.Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(BoundaryLevel);
		BoundaryFixture.Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(BoundaryLevel);
		UProfileSaveGame* BoundarySave = FProfileSerializer::ToSaveGame(BoundaryFixture.Snapshot, BoundaryFixture.Inventory,
			BoundaryFixture.PendingRewards, BoundaryFixture.AppliedSettlementIds, BoundaryFixture.EquippedMap);
		if (!BoundarySave)
		{
			Test.AddError(TEXT("setup: ToSaveGame returned null for the boundary save"));
			continue;
		}
		FM3_013_LoadOuts BoundaryOuts;
		TestTrue(FString::Printf(TEXT("level %d is a legal save"), BoundaryLevel),
			FProfileSerializer::FromSaveGame(BoundarySave, BoundaryOuts.Snapshot, BoundaryOuts.Inventory,
				BoundaryOuts.Pending, BoundaryOuts.Applied, BoundaryOuts.Equipped, BoundaryOuts.Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013NonFiniteStatsRejected,
	"UEMMO.Tasks.M3_013.NonFiniteStatsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013NonFiniteStatsRejected::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	const FM3_013_Fixture Fixture = FM3_013_Fixture::Make(*this, true);
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!Save || Save->InventoryInstanceSnapshots.Num() != 3)
	{
		Test.AddError(TEXT("setup: the save does not carry the three inventory snapshots"));
		return true;
	}

	// Scenario 1: a NaN attack stat inside the first inventory snapshot. The
	// mutation mimics either a corrupted file or a live state that already
	// carried NaN: either way the load must refuse instead of storing it.
	TestTrue(TEXT("setup: the weapon snapshot carries attack=3.5"),
		Save->InventoryInstanceSnapshots[0].Contains(TEXT("attack=3.5")));
	Save->InventoryInstanceSnapshots[0] = Save->InventoryInstanceSnapshots[0].Replace(TEXT("attack=3.5"), TEXT("attack=nan"));
	if (!M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::BadInstanceSnapshot, TEXT("attack"), TEXT("NaN attack stat")))
	{
		return true;
	}

	// Scenario 2: an infinite max_hp stat.
	UProfileSaveGame* InfSave = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!InfSave || InfSave->InventoryInstanceSnapshots.Num() != 3)
	{
		Test.AddError(TEXT("setup: the rebuilt save lost its inventory snapshots"));
		return true;
	}
	TestTrue(TEXT("setup: the armor snapshot carries max_hp=25.5"),
		InfSave->InventoryInstanceSnapshots[1].Contains(TEXT("max_hp=25.5")));
	InfSave->InventoryInstanceSnapshots[1] = InfSave->InventoryInstanceSnapshots[1].Replace(TEXT("max_hp=25.5"), TEXT("max_hp=inf"));
	M3_013_ExpectRejectedLoad(*this, InfSave, EProfileLoadError::BadInstanceSnapshot, TEXT("max_hp"), TEXT("infinite max_hp stat"));

	// Scenario 3: the settings placeholder float is finite-validated too.
	UProfileSaveGame* VolumeSave = FProfileSerializer::ToSaveGame(Fixture.Snapshot, Fixture.Inventory,
		Fixture.PendingRewards, Fixture.AppliedSettlementIds, Fixture.EquippedMap);
	if (!VolumeSave)
	{
		Test.AddError(TEXT("setup: ToSaveGame returned null for the volume save"));
		return true;
	}
	VolumeSave->MasterVolume = std::numeric_limits<float>::quiet_NaN();
	M3_013_ExpectRejectedLoad(*this, VolumeSave, EProfileLoadError::NonFiniteSetting, TEXT("MasterVolume"), TEXT("NaN settings value"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013InventoryOverCapacityRejected,
	"UEMMO.Tasks.M3_013.InventoryOverCapacityRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013InventoryOverCapacityRejected::RunTest(const FString& Parameters)
{
	// A save claiming more items than the 30-slot capacity is corrupt: the
	// overflow must be refused, never silently dropped on restore.
	UProfileSaveGame* Save = M3_013_MakeRawSave();
	for (int32 Index = 0; Index <= FInventoryModel::Capacity; ++Index)
	{
		const FItemInstance Instance = M3_013_MakeInstance(static_cast<uint32>(Index + 0x40u),
			TEXT("weapon_training"), 1.0f, 1.0f, 1.0f, 1);
		Save->InventoryInstanceSnapshots.Add(Instance.ToStringSnapshot());
	}
	TestEqual(TEXT("setup: the save carries Capacity+1 snapshot strings"),
		Save->InventoryInstanceSnapshots.Num(), FInventoryModel::Capacity + 1);
	M3_013_ExpectRejectedLoad(*this, Save, EProfileLoadError::InventoryOverfull, TEXT("InventoryInstances"), TEXT("over-capacity inventory"));

	// Exactly Capacity items load fine.
	UProfileSaveGame* FullSave = M3_013_MakeRawSave();
	for (int32 Index = 0; Index < FInventoryModel::Capacity; ++Index)
	{
		const FItemInstance Instance = M3_013_MakeInstance(static_cast<uint32>(Index + 0x80u),
			TEXT("weapon_training"), 1.0f, 1.0f, 1.0f, 1);
		FullSave->InventoryInstanceSnapshots.Add(Instance.ToStringSnapshot());
	}
	FM3_013_LoadOuts Outs;
	TestTrue(TEXT("a save with exactly Capacity items loads"),
		FProfileSerializer::FromSaveGame(FullSave, Outs.Snapshot, Outs.Inventory, Outs.Pending, Outs.Applied, Outs.Equipped, Outs.Error));
	TestEqual(TEXT("the restored inventory holds every Capacity item"), Outs.Inventory.Count(), FInventoryModel::Capacity);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_013SaveStructureContainsNoActorOrWorldReferences,
	"UEMMO.Tasks.M3_013.SaveStructureContainsNoActorOrWorldReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_013SaveStructureContainsNoActorOrWorldReferences::RunTest(const FString& Parameters)
{
	// Positive control for the checker itself: a reflected engine class known
	// to hold object references (UGameInstance::LocalPlayers) must be flagged.
	bool bControlFoundReference = false;
	for (TFieldIterator<FProperty> It(UGameInstance::StaticClass()); It; ++It)
	{
		if (M3_013_PropertyReferencesObject(*It))
		{
			bControlFoundReference = true;
			break;
		}
	}
	TestTrue(TEXT("checker self-test: UGameInstance does expose object-reference properties"),
		bControlFoundReference);

	// The save class must reflect all its documented value fields (so the
	// negative check below is never a vacuous pass over an empty class).
	int32 PropertyCount = 0;
	for (TFieldIterator<FProperty> It(UProfileSaveGame::StaticClass()); It; ++It)
	{
		++PropertyCount;
	}
	TestTrue(TEXT("UProfileSaveGame reflects its value-type fields"), PropertyCount >= 8);

	// Negative claim (the acceptance criterion): no property of the save - at
	// top level, inside structs, or inside containers - is an object reference.
	bool bSaveReferencesObject = false;
	for (TFieldIterator<FProperty> It(UProfileSaveGame::StaticClass()); It; ++It)
	{
		if (M3_013_PropertyReferencesObject(*It))
		{
			bSaveReferencesObject = true;
			break;
		}
	}
	TestFalse(TEXT("UProfileSaveGame contains no Actor/World/object-reference field"),
		bSaveReferencesObject);
	return true;
}

#endif
