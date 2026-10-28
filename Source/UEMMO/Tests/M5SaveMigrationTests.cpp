// M5-046: the old-save v1 -> v2 lossless migration (interface contract
// section 7 / section 10.9 of the M5 baseline review). Pins the extended
// save contract end to end, always through pure serializer calls or the
// dedicated "TestSlot_" prefix (player "Profile_" saves are unreachable by
// construction):
// - a complete v1 fixture (legacy accessory_training equipped AND claimed,
//   melee training sword, armor, pending draft, applied ids) migrates with
//   every id, stat, equipment reference and claimed settlement preserved;
//   the legacy id is rewritten to charm_training through the manifest while
//   every other snapshot string stays byte-identical; the migration runs
//   EXACTLY ONCE (the migrated save is already schema 2) and a following
//   save/load cycle never re-runs the initial weapon-state grant;
// - firearm instances of a v1 save seed an explicit EMPTY magazine row
//   (loaded 0 - mounting never mints ammunition), melee instances none;
// - an unknown and unregistered definition id is REJECTED with the input
//   preserved - the registered alias is history compatibility, an unknown
//   id is corruption, and the two are never conflated;
// - unknown versions, corrupt payloads and duplicate/invalid/dangling v2
//   weapon state are refused with the input byte-preserved;
// - the manifest itself is validated (acyclic, targets present, disjoint
//   vocabularies, sane seeds) and data-driven;
// - the ConfigRevision compatibility gate: an additive config change loads
//   and hands the restored state to the caller for rebinding; a removed
//   ammo kind or a shrunk magazine/reserve constraint refuses the load and
//   keeps the original file; the revision STRINGS are never compared;
// - the A/B transaction keeps its order (write INACTIVE slot -> read back
//   -> commit index): an index-commit failure leaves the old save active
//   and loadable, a corrupted read-back never moves the index;
// - a byte-realistic v1 SLOT (old-build digest form) loads through the real
//   service startup path into the migrated v2 state.
//
// Storage: the service tests run on an in-memory ISaveStorage with a fault-
// injection decorator - no real file is touched; cleanup deletes exactly
// the three TestSlot_ names.
//
// Stub-failure note: against the pre-M5-046 code the migration/manifest
// tests fail on the missing symbols' behavior (v1 loads are refused, no
// weapon state exists); the service tests keep passing where M3-014 already
// pinned the order - they restate it as the M5-046 acceptance.

#include "Misc/AutomationTest.h"

#include "../Persistence/ProfileSaveService.h"
#include "../Persistence/ProfileSerializer.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../Weapons/AmmoTypes.h"

#include "Misc/Crc.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_046
{
	// -- Fixture helpers ---------------------------------------------------------

	/** Fixed character identity of the automation fixture (never all-zero). */
	static FGuid M5_046_CharacterId()
	{
		return FGuid(0x046C046Cu, 0x1357u, 0x2468ACE0u, 0x00460C46u);
	}

	/**
	 * Deterministic item instance (distinct seeds never collide) with binary-
	 * exact stat fractions so snapshot strings round trip bit-exactly.
	 */
	static FItemInstance M5_046_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP, int32 ItemLevel)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x5EED0000u + Seed, 0x046Cu, 0x00460Cu, Seed * 17u + 5u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(0x460CBA9876543210ULL) + static_cast<int64>(Seed);
		Instance.RolledStats.Attack = Attack;
		Instance.RolledStats.Defense = Defense;
		Instance.RolledStats.MaxHP = MaxHP;
		Instance.Level = ItemLevel;
		return Instance;
	}

	/**
	 * The complete v1 fixture of the acceptance: identity, Level 3/XP 45, a
	 * melee training sword (equipped), the training armor, the LEGACY
	 * accessory_training charm (equipped accessory AND inside the unclaimed
	 * draft) and two claimed settlement ids. Built through the current
	 * serializer's base mapping and then stamped back to schema_version 1:
	 * the v1 layout is exactly this field subset (empty weapon containers,
	 * marker false, no revision - asserted below so the fixture cannot
	 * silently drift).
	 */
	struct FM5_046_V1Fixture
	{
		UProfileSaveGame* Save = nullptr;
		FItemInstance Sword;
		FItemInstance Armor;
		FItemInstance LegacyCharm;
		FItemInstance DraftCharm;

		static FM5_046_V1Fixture Make(FAutomationTestBase& Test)
		{
			FM5_046_V1Fixture Fixture;
			Fixture.Sword = M5_046_MakeInstance(1, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);
			Fixture.Armor = M5_046_MakeInstance(2, TEXT("armor_training"), 0.0f, 3.0f, 0.0f, 1);
			Fixture.LegacyCharm = M5_046_MakeInstance(3, TEXT("accessory_training"), 1.0f, 0.0f, 5.0f, 1);
			Fixture.DraftCharm = M5_046_MakeInstance(4, TEXT("accessory_training"), 1.0f, 0.0f, 5.0f, 1);

			FProfileSnapshot Snapshot;
			Snapshot.CharacterId = M5_046_CharacterId();
			Snapshot.Level = 3;
			Snapshot.XP = 45;
			Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(3);
			Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(3);
			Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(3);

			FInventoryModel Inventory;
			if (Inventory.TryAdd(Fixture.Sword) != EInventoryAddResult::Added ||
				Inventory.TryAdd(Fixture.Armor) != EInventoryAddResult::Added ||
				Inventory.TryAdd(Fixture.LegacyCharm) != EInventoryAddResult::Added)
			{
				Test.AddError(TEXT("fixture: the v1 inventory TryAdd chain was rejected"));
			}

			TMap<EItemSlot, FGuid> Equipped;
			Equipped.Add(EItemSlot::Weapon, Fixture.Sword.InstanceId);
			Equipped.Add(EItemSlot::Accessory, Fixture.LegacyCharm.InstanceId);

			TArray<FPendingReward> Pending;
			FPendingReward Draft;
			Draft.SettlementId = 5001;
			Draft.XP = 50;
			Draft.Items.Add(Fixture.DraftCharm);
			Pending.Add(Draft);

			TSet<uint64> Applied;
			Applied.Add(900);
			Applied.Add(901);

			// The 5-argument form writes the pre-M5-046 field subset.
			Fixture.Save = FProfileSerializer::ToSaveGame(Snapshot, Inventory, Pending, Applied, Equipped);
			if (!Test.TestNotNull(TEXT("fixture: the v1 payload was built"), Fixture.Save))
			{
				return Fixture;
			}
			Fixture.Save->SchemaVersion = UProfileSaveGame::LegacySchemaVersion;

			// The fixture must be byte-realistic v1: no weapon containers, no
			// marker, no revision (a drift here would make every migration
			// assertion below vacuous).
			Test.TestEqual(TEXT("fixture: the v1 payload carries no magazine rows"),
				Fixture.Save->WeaponMagazines.Num(), 0);
			Test.TestEqual(TEXT("fixture: the v1 payload carries no reserve rows"),
				Fixture.Save->ReserveAmmoPools.Num(), 0);
			Test.TestFalse(TEXT("fixture: the v1 payload has no weapon-state marker"),
				Fixture.Save->bWeaponStateInitialized);
			Test.TestTrue(TEXT("fixture: the v1 payload carries no config revision"),
				Fixture.Save->ConfigRevision.IsEmpty());
			return Fixture;
		}
	};

	/**
	 * Sentinel-filled Out* bundle for rejection tests: after a FAILED load,
	 * every output must still carry exactly these sentinel values - proof
	 * that the serializer never writes partial state on the error path.
	 */
	struct FM5_046_LoadOuts
	{
		FProfileSnapshot Snapshot;
		FInventoryModel Inventory;
		TArray<FPendingReward> Pending;
		TSet<uint64> Applied;
		TMap<EItemSlot, FGuid> Equipped;
		FProfileWeaponStateOut WeaponState;
		FProfileLoadError Error;

		static FM5_046_LoadOuts MakeSentinel()
		{
			FM5_046_LoadOuts Outs;
			Outs.Snapshot.CharacterId = FGuid(0xDEAD0460u, 0x1u, 0x1u, 0x1u);
			Outs.Snapshot.Level = -7;
			Outs.Snapshot.XP = -9;
			FItemInstance SentinelItem;
			SentinelItem.InstanceId = FGuid(0xF0000460u, 0xFu, 0xFu, 0xFu);
			SentinelItem.DefinitionId = FName(TEXT("sentinel"));
			Outs.Inventory.TryAdd(SentinelItem);
			FPendingReward SentinelDraft;
			SentinelDraft.SettlementId = 0xEEEEull;
			SentinelDraft.XP = 1;
			Outs.Pending.Add(SentinelDraft);
			Outs.Applied.Add(0x1234ull);
			Outs.Equipped.Add(EItemSlot::Weapon, FGuid(0xBEEF0460u, 0x2u, 0x2u, 0x2u));
			Outs.WeaponState.bWeaponStateInitialized = true;
			Outs.WeaponState.ConfigRevision = TEXT("sentinel-revision");
			FWeaponMagazineSaveEntry SentinelMagazine;
			SentinelMagazine.InstanceId = FGuid(0xAAAA0460u, 0x3u, 0x3u, 0x3u);
			SentinelMagazine.AmmoId = FName(TEXT("sentinel_ammo"));
			SentinelMagazine.Capacity = 9;
			SentinelMagazine.LoadedRounds = 9;
			Outs.WeaponState.WeaponMagazines.Add(SentinelMagazine);
			FAmmoReserveSaveEntry SentinelPool;
			SentinelPool.AmmoId = FName(TEXT("sentinel_ammo"));
			SentinelPool.MaxReserve = 8;
			SentinelPool.Rounds = 8;
			Outs.WeaponState.ReserveAmmoPools.Add(SentinelPool);
			return Outs;
		}

		bool StillSentinel() const
		{
			return Snapshot.CharacterId == FGuid(0xDEAD0460u, 0x1u, 0x1u, 0x1u)
				&& Snapshot.Level == -7
				&& Snapshot.XP == -9
				&& Inventory.Count() == 1
				&& Inventory.Contains(FGuid(0xF0000460u, 0xFu, 0xFu, 0xFu))
				&& Pending.Num() == 1
				&& Pending[0].SettlementId == 0xEEEEull
				&& Applied.Num() == 1
				&& Applied.Contains(0x1234ull)
				&& Equipped.Num() == 1
				&& Equipped.Contains(EItemSlot::Weapon)
				&& Equipped[EItemSlot::Weapon] == FGuid(0xBEEF0460u, 0x2u, 0x2u, 0x2u)
				&& WeaponState.bWeaponStateInitialized
				&& WeaponState.ConfigRevision == TEXT("sentinel-revision")
				&& WeaponState.WeaponMagazines.Num() == 1
				&& WeaponState.WeaponMagazines[0].AmmoId == FName(TEXT("sentinel_ammo"))
				&& WeaponState.ReserveAmmoPools.Num() == 1
				&& WeaponState.ReserveAmmoPools[0].AmmoId == FName(TEXT("sentinel_ammo"));
		}
	};

	/** Asserts one restored instance preserved its full identity verbatim. */
	static void M5_046_ExpectSameInstanceIdentity(FAutomationTestBase& Test,
		const FItemInstance& Actual, const FItemInstance& Expected, const TCHAR* What)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: InstanceId preserved"), What),
			Actual.InstanceId == Expected.InstanceId);
		Test.TestTrue(FString::Printf(TEXT("%s: RollSeed preserved"), What),
			Actual.RollSeed == Expected.RollSeed);
		Test.TestEqual(FString::Printf(TEXT("%s: Level preserved"), What),
			Actual.Level, Expected.Level);
		Test.TestEqual(FString::Printf(TEXT("%s: RolledStats.Attack preserved"), What),
			Actual.RolledStats.Attack, Expected.RolledStats.Attack, 1.e-6f);
		Test.TestEqual(FString::Printf(TEXT("%s: RolledStats.Defense preserved"), What),
			Actual.RolledStats.Defense, Expected.RolledStats.Defense, 1.e-6f);
		Test.TestEqual(FString::Printf(TEXT("%s: RolledStats.MaxHP preserved"), What),
			Actual.RolledStats.MaxHP, Expected.RolledStats.MaxHP, 1.e-6f);
	}

	/** Field-named inventory lookup (order-independent). */
	static const FItemInstance* M5_046_FindInstance(const FInventoryModel& Model, const FGuid& InstanceId)
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

	// -- Service test doubles (same shape as the M3-014 suite, suite-local) ------

	/**
	 * In-memory ISaveStorage: every write stores an independent rooted
	 * duplicate, every read returns a fresh duplicate - the same semantics as
	 * disk bytes. Counts WriteSlot calls so a test can pin exactly how many
	 * writes one transaction performs (slot + index = 2).
	 */
	class FM5_046_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM5_046_MemoryStorage() override
		{
			for (TPair<FString, USaveGame*>& Pair : Slots)
			{
				if (Pair.Value)
				{
					Pair.Value->RemoveFromRoot();
				}
			}
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			if (!Data)
			{
				return false;
			}
			USaveGame* Copy = DuplicateObject(Data, GetTransientPackage());
			if (!Copy)
			{
				return false;
			}
			Copy->AddToRoot();
			USaveGame** Existing = Slots.Find(SlotName);
			if (Existing && *Existing)
			{
				(*Existing)->RemoveFromRoot();
			}
			Slots.Add(SlotName, Copy);
			++WriteCallCount;
			return true;
		}

		virtual USaveGame* ReadSlot(const FString& SlotName) override
		{
			USaveGame* const* Found = Slots.Find(SlotName);
			return Found ? DuplicateObject(*Found, GetTransientPackage()) : nullptr;
		}

		virtual bool DeleteSlot(const FString& SlotName) override
		{
			USaveGame** Existing = Slots.Find(SlotName);
			if (!Existing)
			{
				return false;
			}
			if (*Existing)
			{
				(*Existing)->RemoveFromRoot();
			}
			Slots.Remove(SlotName);
			return true;
		}

		virtual bool DoesSlotExist(const FString& SlotName) override
		{
			return Slots.Contains(SlotName);
		}

		/** Number of WriteSlot invocations (slot writes AND index commits). */
		int32 WriteCallCount = 0;

	private:
		TMap<FString, USaveGame*> Slots;
	};

	/** Fault-injection decorator over another storage (see the M3-014 suite). */
	class FM5_046_FaultStorage final : public ISaveStorage
	{
	public:
		explicit FM5_046_FaultStorage(ISaveStorage* InInner)
			: Inner(InInner)
		{
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			if (bFailAllWrites || (!FailWriteSlotName.IsEmpty() && FailWriteSlotName == SlotName))
			{
				return false;
			}
			return Inner->WriteSlot(SlotName, Data);
		}

		virtual USaveGame* ReadSlot(const FString& SlotName) override
		{
			USaveGame* Loaded = Inner->ReadSlot(SlotName);
			if (Loaded && !TamperReadSlotName.IsEmpty() && TamperReadSlotName == SlotName)
			{
				if (UProfileSlotSaveGame* Slot = Cast<UProfileSlotSaveGame>(Loaded))
				{
					Slot->PayloadDigest ^= 0x046C046u; // tampered integrity field
				}
			}
			return Loaded;
		}

		virtual bool DeleteSlot(const FString& SlotName) override
		{
			return Inner->DeleteSlot(SlotName);
		}

		virtual bool DoesSlotExist(const FString& SlotName) override
		{
			return Inner->DoesSlotExist(SlotName);
		}

		bool bFailAllWrites = false;
		FString FailWriteSlotName;
		FString TamperReadSlotName;

	private:
		ISaveStorage* Inner = nullptr;
	};

	/** The three TestSlot_ slot names every service test operates on. */
	static FString M5_046_SlotAName() { return FString(TEXT("TestSlot_A")); }
	static FString M5_046_SlotBName() { return FString(TEXT("TestSlot_B")); }
	static FString M5_046_IndexSlotName() { return FString(TEXT("TestSlot_Index")); }

	/** Creates the service on the TestSlot_ prefix with the given storage. */
	static UProfileSaveService* M5_046_NewService(FAutomationTestBase& Test, ISaveStorage* Storage)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Test.TestNotNull(TEXT("setup: the save service was created"), Service))
		{
			return nullptr;
		}
		if (!Test.TestTrue(TEXT("setup: Initialize accepted the TestSlot_ prefix"),
			Service->Initialize(TEXT("TestSlot_"))))
		{
			return nullptr;
		}
		Service->SetStorage(Storage);
		return Service;
	}

	/**
	 * Cleanup: deletes the service's test slots and asserts the three
	 * TestSlot_ names are gone afterwards (only M5-prefixed slots were used).
	 */
	static void M5_046_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service, ISaveStorage& Storage)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: TestSlot_A deleted"), !Storage.DoesSlotExist(M5_046_SlotAName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_B deleted"), !Storage.DoesSlotExist(M5_046_SlotBName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_Index deleted"), !Storage.DoesSlotExist(M5_046_IndexSlotName()));
	}

	/** Deterministic save request for the service tests (Level/XP/items). */
	static FProfileSaveRequest M5_046_MakeRequest(FAutomationTestBase& Test, int32 Level, int32 XP, int32 ItemCount)
	{
		FProfileSaveRequest Request;
		Request.Snapshot.CharacterId = M5_046_CharacterId();
		Request.Snapshot.Level = Level;
		Request.Snapshot.XP = XP;
		Request.Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(Level);
		Request.Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(Level);
		Request.Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(Level);
		for (int32 Index = 0; Index < ItemCount; ++Index)
		{
			const FItemInstance Instance = M5_046_MakeInstance(static_cast<uint32>(Level * 100 + Index),
				TEXT("weapon_training"), 1.0f + 0.5f * Index, 0.25f * Index, 2.5f * Index, 1);
			if (Request.Inventory.TryAdd(Instance) != EInventoryAddResult::Added)
			{
				Test.AddError(FString::Printf(TEXT("fixture: TryAdd(item %d) was rejected"), Index));
			}
			if (Index == 0)
			{
				Request.EquippedMap.Add(EItemSlot::Weapon, Instance.InstanceId);
			}
		}
		FPendingReward Draft;
		Draft.SettlementId = 7000ull + static_cast<uint64>(Level);
		Draft.XP = 50;
		Request.PendingRewards.Add(Draft);
		Request.AppliedSettlementIds.Add(8000ull + static_cast<uint64>(Level));
		return Request;
	}

	/** Reads the raw index object straight from storage (bypassing the service). */
	static UProfileIndexSaveGame* M5_046_ReadRawIndex(ISaveStorage& Storage)
	{
		return Cast<UProfileIndexSaveGame>(Storage.ReadSlot(M5_046_IndexSlotName()));
	}

	/**
	 * Byte-exact replica of the FROZEN M3-014 canonical payload digest form
	 * for v1-shaped payloads. Safe by construction: the M5-046 digest appends
	 * its weapon-state segments ONLY for non-default content, so a v1 payload
	 * hashes to exactly this pre-M5-046 string - which is what an old build
	 * stamped into a real v1 slot file. If this replica ever drifts from the
	 * real digest for v1 payloads the startup test below fails loudly, and
	 * that is the desired tripwire: the v1 digest compatibility is the load
	 * path's most fragile promise.
	 */
	static uint32 M5_046_ComputeLegacyV1PayloadDigest(const UProfileSaveGame& Save)
	{
		FString Canonical;
		Canonical += FString::Printf(TEXT("schema=%d|id=%s|level=%d|xp=%d|volume=%s|"),
			Save.SchemaVersion, *Save.CharacterId.ToString(), Save.Level, Save.XP,
			*FString::SanitizeFloat(Save.MasterVolume));
		for (const FString& Snapshot : Save.InventoryInstanceSnapshots)
		{
			Canonical += FString::Printf(TEXT("item=%s|"), *Snapshot);
		}
		for (const FEquippedSlotSavePair& Pair : Save.EquippedIds)
		{
			Canonical += FString::Printf(TEXT("equip=%u:%s|"), Pair.Slot, *Pair.InstanceId.ToString());
		}
		for (const FPendingRewardSaveEntry& Entry : Save.PendingRewards)
		{
			Canonical += FString::Printf(TEXT("draft=%llu:%d|"), Entry.SettlementId, Entry.XP);
			for (const FString& Item : Entry.ItemSnapshots)
			{
				Canonical += FString::Printf(TEXT("draftitem=%s|"), *Item);
			}
		}
		for (const uint64 AppliedId : Save.AppliedSettlementIds)
		{
			Canonical += FString::Printf(TEXT("applied=%llu|"), AppliedId);
		}
		return FCrc::StrCrc32(*Canonical);
	}

	/**
	 * Copies every reflected UProfileSaveGame property of Payload into a fresh
	 * slot save (the same reflection copy M3_014_BuildSlotSave performs) and
	 * stamps the transaction envelope - the test-side way to plant a
	 * byte-realistic slot payload.
	 */
	static UProfileSlotSaveGame* M5_046_WrapIntoSlotSave(UProfileSaveGame& Payload, int32 Generation, uint32 Digest)
	{
		UProfileSlotSaveGame* Slot = NewObject<UProfileSlotSaveGame>(GetTransientPackage());
		for (TFieldIterator<FProperty> It(UProfileSaveGame::StaticClass()); It; ++It)
		{
			FProperty* Property = *It;
			Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Slot),
				Property->ContainerPtrToValuePtr<void>(&Payload));
		}
		Slot->SlotGeneration = Generation;
		Slot->PayloadInstanceCount = Slot->InventoryInstanceSnapshots.Num();
		Slot->CharacterIdSummary = Slot->CharacterId.ToString();
		Slot->PayloadDigest = Digest;
		return Slot;
	}
}

using namespace UE::UEMMO::Tasks::M5_046;

// -- 1: the complete v1 fixture migrates losslessly -----------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046V1CompleteFixtureMigratesLosslessly,
	"UEMMO.Tasks.M5_046.V1CompleteFixtureMigratesLosslessly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046V1CompleteFixtureMigratesLosslessly::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	const FM5_046_V1Fixture Fixture = FM5_046_V1Fixture::Make(Test);
	if (!Fixture.Save)
	{
		return true;
	}
	UProfileSaveGame* const V1Save = Fixture.Save;

	// Keep byte-identity witnesses of the input: the migration must never
	// touch it, whatever the outcome.
	const int32 InputSchemaVersion = V1Save->SchemaVersion;
	const TArray<FString> InputInventory = V1Save->InventoryInstanceSnapshots;
	const int32 InputEquipCount = V1Save->EquippedIds.Num();

	// The explicit one-time migration, stamped with the caller's revision.
	FProfileLoadError MigrationError;
	UProfileSaveGame* Migrated = FProfileSerializer::MigrateV1ToV2(*V1Save,
		FProfileItemManifest::Get(), MigrationError, FString(TEXT("rev-m5-046")));
	if (!Test.TestNotNull(TEXT("the complete v1 fixture migrated"), Migrated))
	{
		Test.AddError(FString::Printf(TEXT("migration failed: %s"), *MigrationError.Message));
		return true;
	}

	// Schema moved forward; everything else survived. The migration stamps
	// the CURRENT layout symbolically: since M6-003 the v1->v2 output is the
	// v3 file shape (the v2 fields verbatim plus the explicitly empty
	// RoomClears set), so the literal-2 pin went symbolic - a later bump must
	// not rot this assertion again.
	Test.TestEqual(TEXT("the migrated save carries the current schema version"),
		Migrated->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion);
	Test.TestEqual(TEXT("the migrated save seeds no clear records (the explicit v3 empty set)"),
		Migrated->RoomClears.Num(), 0);
	Test.TestTrue(TEXT("CharacterId preserved"), Migrated->CharacterId == V1Save->CharacterId);
	Test.TestEqual(TEXT("Level preserved"), Migrated->Level, 3);
	Test.TestEqual(TEXT("XP preserved"), Migrated->XP, 45);
	Test.TestEqual(TEXT("the migrated inventory carries every instance"), Migrated->InventoryInstanceSnapshots.Num(), 3);
	Test.TestEqual(TEXT("equipment references preserved"), Migrated->EquippedIds.Num(), InputEquipCount);
	Test.TestEqual(TEXT("pending drafts preserved"), Migrated->PendingRewards.Num(), 1);
	Test.TestEqual(TEXT("claimed settlement ids preserved"), Migrated->AppliedSettlementIds.Num(), 2);

	// The legacy alias was rewritten onto the current kind; every untouched
	// snapshot stayed byte-identical.
	bool bLegacyRewritten = false;
	for (int32 Index = 0; Index < InputInventory.Num(); ++Index)
	{
		const FString& Original = InputInventory[Index];
		const FString& MigratedText = Migrated->InventoryInstanceSnapshots[Index];
		if (Original.Contains(TEXT("definition_id=accessory_training")))
		{
			bLegacyRewritten = true;
			Test.TestTrue(TEXT("the legacy charm snapshot resolves to charm_training"),
				MigratedText.Contains(TEXT("definition_id=charm_training")));
			Test.TestTrue(TEXT("the legacy charm snapshot no longer names the legacy id"),
				!MigratedText.Contains(TEXT("definition_id=accessory_training")));
			Test.TestTrue(TEXT("the legacy charm keeps its instance_id"),
				MigratedText.Contains(FString::Printf(TEXT("instance_id=%s"), *Fixture.LegacyCharm.InstanceId.ToString())));
			Test.TestTrue(TEXT("the legacy charm keeps its roll seed"),
				MigratedText.Contains(FString::Printf(TEXT("roll_seed=%lld"), Fixture.LegacyCharm.RollSeed)));
		}
		else
		{
			Test.TestTrue(TEXT("a non-aliased snapshot stays byte-identical"), MigratedText == Original);
		}
	}
	Test.TestTrue(TEXT("the fixture contained the legacy alias (setup sanity)"), bLegacyRewritten);

	// Equipment still points at the SAME instance ids (slot bytes included).
	Test.TestTrue(TEXT("the weapon binding kept its instance"),
		Migrated->EquippedIds.ContainsByPredicate([&Fixture](const FEquippedSlotSavePair& Pair)
		{
			return Pair.Slot == static_cast<uint8>(EItemSlot::Weapon) && Pair.InstanceId == Fixture.Sword.InstanceId;
		}));
	Test.TestTrue(TEXT("the accessory binding kept the legacy charm's instance"),
		Migrated->EquippedIds.ContainsByPredicate([&Fixture](const FEquippedSlotSavePair& Pair)
		{
			return Pair.Slot == static_cast<uint8>(EItemSlot::Accessory) && Pair.InstanceId == Fixture.LegacyCharm.InstanceId;
		}));

	// The draft's legacy item was rewritten too; id and XP untouched.
	if (Migrated->PendingRewards.Num() == 1)
	{
		Test.TestEqual(TEXT("the draft keeps its settlement id"), Migrated->PendingRewards[0].SettlementId, 5001ull);
		Test.TestEqual(TEXT("the draft keeps its XP"), Migrated->PendingRewards[0].XP, 50);
		Test.TestTrue(TEXT("the draft's legacy item resolves to charm_training"),
			Migrated->PendingRewards[0].ItemSnapshots.Num() == 1
			&& Migrated->PendingRewards[0].ItemSnapshots[0].Contains(TEXT("definition_id=charm_training")));
	}

	// The one-time weapon-state initialization: the melee sword carries NO
	// magazine (no seed row), the containers are explicitly settled and the
	// marker makes "initialized" distinguishable from "never initialized".
	Test.TestEqual(TEXT("a melee-only v1 save migrates to zero magazine rows"), Migrated->WeaponMagazines.Num(), 0);
	Test.TestEqual(TEXT("the migration seeds no reserve rows"), Migrated->ReserveAmmoPools.Num(), 0);
	Test.TestTrue(TEXT("the weapon state is marked initialized"), Migrated->bWeaponStateInitialized);
	Test.TestEqual(TEXT("the migration recorded the caller's revision"), Migrated->ConfigRevision, FString(TEXT("rev-m5-046")));

	// The input save object is untouched - "original file preserved".
	Test.TestEqual(TEXT("the input keeps schema_version 1"), V1Save->SchemaVersion, InputSchemaVersion);
	Test.TestTrue(TEXT("the input keeps its inventory snapshots"), V1Save->InventoryInstanceSnapshots == InputInventory);

	// The migrated save restores cleanly, and the restored state re-saves
	// without any further weapon-state grant: the save/load cycle is a fixed
	// point (this is the "no more initial ammo" acceptance at data level).
	FM5_046_LoadOuts Restored;
	if (!Test.TestTrue(TEXT("the migrated save restores"),
		FProfileSerializer::FromSaveGame(Migrated, Restored.Snapshot, Restored.Inventory, Restored.Pending,
			Restored.Applied, Restored.Equipped, Restored.Error, nullptr, nullptr, &Restored.WeaponState)))
	{
		Test.AddError(FString::Printf(TEXT("restore failed: %s"), *Restored.Error.Message));
		return true;
	}
	const FItemInstance* RestoredCharm = M5_046_FindInstance(Restored.Inventory, Fixture.LegacyCharm.InstanceId);
	Test.TestTrue(TEXT("the restored charm carries the current kind"),
		RestoredCharm != nullptr && RestoredCharm->DefinitionId == FName(TEXT("charm_training")));
	if (RestoredCharm)
	{
		M5_046_ExpectSameInstanceIdentity(Test, *RestoredCharm, Fixture.LegacyCharm,
			TEXT("the restored legacy charm"));
	}
	Test.TestEqual(TEXT("the restored applied set survived"), Restored.Applied.Num(), 2);
	Test.TestTrue(TEXT("the restored weapon state is marked initialized"), Restored.WeaponState.bWeaponStateInitialized);
	Test.TestEqual(TEXT("the restored weapon state carries no magazines"), Restored.WeaponState.WeaponMagazines.Num(), 0);

	// Re-save exactly what was restored and reload it: no new rows, no new
	// rounds, marker stable - a migrated profile never re-runs the grant.
	UProfileSaveGame* Resaved = FProfileSerializer::ToSaveGame(Restored.Snapshot, Restored.Inventory,
		Restored.Pending, Restored.Applied, Restored.Equipped,
		Restored.WeaponState.WeaponMagazines, Restored.WeaponState.ReserveAmmoPools,
		Restored.WeaponState.bWeaponStateInitialized, Restored.WeaponState.ConfigRevision);
	if (!Test.TestNotNull(TEXT("the re-save was built"), Resaved))
	{
		return true;
	}
	FM5_046_LoadOuts Reloaded;
	if (!Test.TestTrue(TEXT("the re-saved state reloads"),
		FProfileSerializer::FromSaveGame(Resaved, Reloaded.Snapshot, Reloaded.Inventory, Reloaded.Pending,
			Reloaded.Applied, Reloaded.Equipped, Reloaded.Error, nullptr, nullptr, &Reloaded.WeaponState)))
	{
		Test.AddError(FString::Printf(TEXT("reload failed: %s"), *Reloaded.Error.Message));
		return true;
	}
	Test.TestTrue(TEXT("the re-saved weapon state is unchanged by the cycle"),
		Reloaded.WeaponState.bWeaponStateInitialized == Restored.WeaponState.bWeaponStateInitialized
		&& Reloaded.WeaponState.WeaponMagazines.Num() == Restored.WeaponState.WeaponMagazines.Num()
		&& Reloaded.WeaponState.ReserveAmmoPools.Num() == Restored.WeaponState.ReserveAmmoPools.Num());
	Test.TestEqual(TEXT("the cycle granted no magazine"), Reloaded.WeaponState.WeaponMagazines.Num(), 0);

	// The migration is a ONE-TIME door: the migrated v2 save is refused by the
	// explicit v1 migration (nothing left to migrate).
	FProfileLoadError TwiceError;
	Test.TestTrue(TEXT("migrating a migrated save is refused"),
		FProfileSerializer::MigrateV1ToV2(*Migrated, FProfileItemManifest::Get(), TwiceError) == nullptr);
	Test.TestEqual(TEXT("the double-migration refusal classifies as UnsupportedSchema"),
		static_cast<int32>(TwiceError.Code), static_cast<int32>(EProfileLoadError::UnsupportedSchema));
	return true;
}

// -- 2: firearm instances seed an explicit empty magazine -----------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046V1FirearmInstancesSeedExplicitEmptyMagazines,
	"UEMMO.Tasks.M5_046.V1FirearmInstancesSeedExplicitEmptyMagazines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046V1FirearmInstancesSeedExplicitEmptyMagazines::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// A realistic M5-era v1 save: the HUD staging put the hitscan rifle into
	// the bag (it maps to weapon_hitscan_sample / ammo_cell 12) next to the
	// melee sword. Schema 1 stored no magazine state at all.
	const FItemInstance Rifle = M5_046_MakeInstance(11, TEXT("weapon_hitscan_gun"), 5.0f, 0.0f, 0.0f, 1);
	const FItemInstance Sword = M5_046_MakeInstance(12, TEXT("weapon_training"), 5.0f, 0.0f, 0.0f, 1);

	FProfileSnapshot Snapshot;
	Snapshot.CharacterId = M5_046_CharacterId();
	Snapshot.Level = 2;
	Snapshot.XP = 10;
	Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(2);
	Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(2);
	Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(2);
	FInventoryModel Inventory;
	if (!Test.TestTrue(TEXT("fixture: the rifle and sword were added"),
		Inventory.TryAdd(Rifle) == EInventoryAddResult::Added && Inventory.TryAdd(Sword) == EInventoryAddResult::Added))
	{
		return true;
	}
	TMap<EItemSlot, FGuid> Equipped;
	Equipped.Add(EItemSlot::Weapon, Rifle.InstanceId);
	UProfileSaveGame* V1Save = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, Equipped);
	if (!Test.TestNotNull(TEXT("fixture: the v1 payload was built"), V1Save))
	{
		return true;
	}
	V1Save->SchemaVersion = UProfileSaveGame::LegacySchemaVersion;

	FProfileLoadError MigrationError;
	UProfileSaveGame* Migrated = FProfileSerializer::MigrateV1ToV2(*V1Save, FProfileItemManifest::Get(), MigrationError);
	if (!Test.TestNotNull(TEXT("the v1 firearm save migrated"), Migrated))
	{
		Test.AddError(FString::Printf(TEXT("migration failed: %s"), *MigrationError.Message));
		return true;
	}

	// Exactly one magazine row: the FIREARM instance, keyed by its item
	// identity, seeded EMPTY (loading 0 rounds - mounting never mints
	// ammunition). The melee sword got none.
	Test.TestEqual(TEXT("exactly the firearm instance seeded a magazine"), Migrated->WeaponMagazines.Num(), 1);
	if (Migrated->WeaponMagazines.Num() == 1)
	{
		const FWeaponMagazineSaveEntry& Magazine = Migrated->WeaponMagazines[0];
		Test.TestTrue(TEXT("the magazine belongs to the rifle instance"), Magazine.InstanceId == Rifle.InstanceId);
		Test.TestEqual(TEXT("the magazine refills from ammo_cell"), Magazine.AmmoId, FName(TEXT("ammo_cell")));
		Test.TestEqual(TEXT("the magazine carries the table capacity 12"), Magazine.Capacity, 12);
		Test.TestEqual(TEXT("the magazine is seeded empty"), Magazine.LoadedRounds, 0);
	}

	// Reload and re-save: the explicit empty magazine is the fixed point - no
	// initial-ammo grant ever runs against the migrated profile again.
	FM5_046_LoadOuts Restored;
	if (!Test.TestTrue(TEXT("the migrated save restores"),
		FProfileSerializer::FromSaveGame(Migrated, Restored.Snapshot, Restored.Inventory, Restored.Pending,
			Restored.Applied, Restored.Equipped, Restored.Error, nullptr, nullptr, &Restored.WeaponState)))
	{
		Test.AddError(FString::Printf(TEXT("restore failed: %s"), *Restored.Error.Message));
		return true;
	}
	UProfileSaveGame* Resaved = FProfileSerializer::ToSaveGame(Restored.Snapshot, Restored.Inventory,
		Restored.Pending, Restored.Applied, Restored.Equipped,
		Restored.WeaponState.WeaponMagazines, Restored.WeaponState.ReserveAmmoPools,
		Restored.WeaponState.bWeaponStateInitialized, Restored.WeaponState.ConfigRevision);
	FM5_046_LoadOuts Reloaded;
	if (Test.TestNotNull(TEXT("the re-save was built"), Resaved)
		&& Test.TestTrue(TEXT("the re-saved state reloads"),
			FProfileSerializer::FromSaveGame(Resaved, Reloaded.Snapshot, Reloaded.Inventory, Reloaded.Pending,
				Reloaded.Applied, Reloaded.Equipped, Reloaded.Error, nullptr, nullptr, &Reloaded.WeaponState)))
	{
		Test.TestEqual(TEXT("the cycle granted no second magazine"), Reloaded.WeaponState.WeaponMagazines.Num(), 1);
		if (Reloaded.WeaponState.WeaponMagazines.Num() == 1)
		{
			Test.TestEqual(TEXT("the cycle minted no rounds"),
				Reloaded.WeaponState.WeaponMagazines[0].LoadedRounds, 0);
			Test.TestEqual(TEXT("the cycle kept the capacity"),
				Reloaded.WeaponState.WeaponMagazines[0].Capacity, 12);
		}
	}
	return true;
}

// -- 3: unknown and unregistered definitions are rejected explicitly ------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046UnknownUnregisteredDefinitionRejectedExplicitly,
	"UEMMO.Tasks.M5_046.UnknownUnregisteredDefinitionRejectedExplicitly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046UnknownUnregisteredDefinitionRejectedExplicitly::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// An id this build neither defines nor registered as a legacy alias: the
	// load must distinguish "corrupt / newer-than-known content" from the
	// registered history mapping and refuse with the input preserved.
	const FItemInstance Relic = M5_046_MakeInstance(21, TEXT("relic_obsolete"), 2.0f, 2.0f, 2.0f, 1);
	FProfileSnapshot Snapshot;
	Snapshot.CharacterId = M5_046_CharacterId();
	Snapshot.Level = 1;
	Snapshot.XP = 0;
	Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(1);
	Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(1);
	Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(1);
	FInventoryModel Inventory;
	Inventory.TryAdd(Relic);
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {});
	if (!Test.TestNotNull(TEXT("fixture: the unknown-definition payload was built"), Save))
	{
		return true;
	}
	Save->SchemaVersion = UProfileSaveGame::LegacySchemaVersion;
	const FString InputSnapshot = Save->InventoryInstanceSnapshots[0];

	FM5_046_LoadOuts Outs = FM5_046_LoadOuts::MakeSentinel();
	Test.TestFalse(TEXT("an unknown unregistered definition is refused"),
		FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending,
			Outs.Applied, Outs.Equipped, Outs.Error, &FProfileItemManifest::Get()));
	Test.TestEqual(TEXT("the refusal classifies as UnknownItemDefinition"),
		static_cast<int32>(Outs.Error.Code), static_cast<int32>(EProfileLoadError::UnknownItemDefinition));
	Test.TestTrue(TEXT("the message names the offending id"), Outs.Error.Message.Contains(TEXT("relic_obsolete")));
	Test.TestTrue(TEXT("the message separates history compatibility from corruption"),
		Outs.Error.Message.Contains(TEXT("alias")));
	Test.TestTrue(TEXT("the refused load left every Out* untouched"), Outs.StillSentinel());
	Test.TestTrue(TEXT("the input snapshot stayed byte-identical"),
		Save->InventoryInstanceSnapshots.Num() == 1 && Save->InventoryInstanceSnapshots[0] == InputSnapshot);

	// The explicit migration refuses the same save for the same reason.
	FProfileLoadError MigrationError;
	Test.TestTrue(TEXT("the migration refuses an unknown definition too"),
		FProfileSerializer::MigrateV1ToV2(*Save, FProfileItemManifest::Get(), MigrationError) == nullptr);
	Test.TestEqual(TEXT("the migration refusal classifies as UnknownItemDefinition"),
		static_cast<int32>(MigrationError.Code), static_cast<int32>(EProfileLoadError::UnknownItemDefinition));
	Test.TestTrue(TEXT("the migration left the input untouched"),
		Save->InventoryInstanceSnapshots[0] == InputSnapshot);

	// Contrast (history compatibility): the SAME fixture shape with the
	// registered legacy alias loads and resolves - only the unregistered id
	// is corruption.
	const FItemInstance LegacyCharm = M5_046_MakeInstance(22, TEXT("accessory_training"), 1.0f, 0.0f, 5.0f, 1);
	FInventoryModel AliasInventory;
	AliasInventory.TryAdd(LegacyCharm);
	UProfileSaveGame* AliasSave = FProfileSerializer::ToSaveGame(Snapshot, AliasInventory, {}, {}, {});
	FM5_046_LoadOuts AliasOuts;
	if (Test.TestNotNull(TEXT("fixture: the alias payload was built"), AliasSave)
		&& Test.TestTrue(TEXT("the registered legacy alias loads"),
			FProfileSerializer::FromSaveGame(AliasSave, AliasOuts.Snapshot, AliasOuts.Inventory, AliasOuts.Pending,
				AliasOuts.Applied, AliasOuts.Equipped, AliasOuts.Error, &FProfileItemManifest::Get())))
	{
		const FItemInstance* RestoredCharm = M5_046_FindInstance(AliasOuts.Inventory, LegacyCharm.InstanceId);
		Test.TestTrue(TEXT("the alias resolved onto charm_training"),
			RestoredCharm != nullptr && RestoredCharm->DefinitionId == FName(TEXT("charm_training")));
	}

	// Without a manifest the serializer stays purely structural (content
	// gating is opt-in) - but only for the CURRENT schema: a v1 save always
	// enters the migration channel, which carries the default manifest. So
	// the same unknown id is first refused at v2 under the manifest, then
	// loads ungated once the manifest is withheld.
	Save->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
	FM5_046_LoadOuts GatedOuts = FM5_046_LoadOuts::MakeSentinel();
	Test.TestFalse(TEXT("the unknown definition is refused at v2 under the manifest too"),
		FProfileSerializer::FromSaveGame(Save, GatedOuts.Snapshot, GatedOuts.Inventory, GatedOuts.Pending,
			GatedOuts.Applied, GatedOuts.Equipped, GatedOuts.Error, &FProfileItemManifest::Get()));
	Test.TestEqual(TEXT("the v2 refusal classifies as UnknownItemDefinition"),
		static_cast<int32>(GatedOuts.Error.Code), static_cast<int32>(EProfileLoadError::UnknownItemDefinition));
	FM5_046_LoadOuts UngatedOuts;
	Test.TestTrue(TEXT("without a manifest the unknown definition is not gated"),
		FProfileSerializer::FromSaveGame(Save, UngatedOuts.Snapshot, UngatedOuts.Inventory, UngatedOuts.Pending,
			UngatedOuts.Applied, UngatedOuts.Equipped, UngatedOuts.Error));
	return true;
}

// -- 4: unknown versions, corruption and broken v2 weapon state -----------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046UnknownVersionCorruptAndDuplicateStateRejectedInputPreserved,
	"UEMMO.Tasks.M5_046.UnknownVersionCorruptAndDuplicateStateRejectedInputPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046UnknownVersionCorruptAndDuplicateStateRejectedInputPreserved::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// Base fixture: one sword (attack=1.5) so the corruption probe has a
	// stable needle.
	const FItemInstance Sword = M5_046_MakeInstance(31, TEXT("weapon_training"), 1.5f, 0.25f, 0.0f, 1);
	FProfileSnapshot Snapshot;
	Snapshot.CharacterId = M5_046_CharacterId();
	Snapshot.Level = 1;
	Snapshot.XP = 0;
	Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(1);
	Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(1);
	Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(1);
	FInventoryModel Inventory;
	Inventory.TryAdd(Sword);

	auto ExpectRejected = [&Test](UProfileSaveGame* Candidate, EProfileLoadError ExpectedCode,
		const TCHAR* MessagePart, const TCHAR* Scenario)
	{
		FM5_046_LoadOuts Outs = FM5_046_LoadOuts::MakeSentinel();
		const bool bLoaded = FProfileSerializer::FromSaveGame(Candidate, Outs.Snapshot, Outs.Inventory,
			Outs.Pending, Outs.Applied, Outs.Equipped, Outs.Error);
		Test.TestFalse(FString::Printf(TEXT("%s: the load is refused"), Scenario), bLoaded);
		Test.TestEqual(FString::Printf(TEXT("%s: expected error code"), Scenario),
			static_cast<int32>(Outs.Error.Code), static_cast<int32>(ExpectedCode));
		Test.TestTrue(FString::Printf(TEXT("%s: the message names the field ('%s' inside '%s')"),
			Scenario, MessagePart, *Outs.Error.Message), Outs.Error.Message.Contains(MessagePart));
		Test.TestTrue(FString::Printf(TEXT("%s: the outputs stayed sentinel"), Scenario), Outs.StillSentinel());
	};

	// Unknown versions: 0 (below every known schema) and 3 (a newer build's
	// layout). The input objects stay byte-preserved either way.
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {});
	if (!Test.TestNotNull(TEXT("fixture: the base payload was built"), Save))
	{
		return true;
	}
	const FString InputSnapshot = Save->InventoryInstanceSnapshots[0];

	Save->SchemaVersion = 0;
	ExpectRejected(Save, EProfileLoadError::UnsupportedSchema, TEXT("schema_version"), TEXT("version=0"));
	Test.TestEqual(TEXT("the version=0 input keeps its data"), Save->InventoryInstanceSnapshots.Num(), 1);

	Save->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion + 1;
	ExpectRejected(Save, EProfileLoadError::UnsupportedSchema, TEXT("newer game build"), TEXT("version=3"));
	Test.TestEqual(TEXT("the version=3 input keeps its data"), Save->InventoryInstanceSnapshots.Num(), 1);

	// Corrupt payload: a NaN stat inside the snapshot string. (v1 or v2
	// alike - the field rules are shared.)
	Save->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
	Save->InventoryInstanceSnapshots[0] = InputSnapshot.Replace(TEXT("attack=1.5"), TEXT("attack=nan"));
	ExpectRejected(Save, EProfileLoadError::BadInstanceSnapshot, TEXT("attack"), TEXT("NaN stat"));

	// v2 weapon state: duplicate magazine identity.
	TArray<FWeaponMagazineSaveEntry> DuplicateMagazines;
	FWeaponMagazineSaveEntry First;
	First.InstanceId = Sword.InstanceId;
	First.AmmoId = FName(TEXT("ammo_cell"));
	First.Capacity = 12;
	First.LoadedRounds = 5;
	FWeaponMagazineSaveEntry Second = First;
	DuplicateMagazines.Add(First);
	DuplicateMagazines.Add(Second);
	UProfileSaveGame* DuplicateSave = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
		DuplicateMagazines, {}, false, FString());
	ExpectRejected(DuplicateSave, EProfileLoadError::DuplicateWeaponState, TEXT("more than one magazine row"), TEXT("duplicate magazine"));

	// v2 weapon state: a magazine for an instance that is not in the
	// inventory (orphaned state must not masquerade as restored state).
	TArray<FWeaponMagazineSaveEntry> DanglingMagazines;
	FWeaponMagazineSaveEntry Dangling;
	Dangling.InstanceId = FGuid(0xD1A6C1A9u, 0x1u, 0x2u, 0x3u);
	Dangling.AmmoId = FName(TEXT("ammo_cell"));
	Dangling.Capacity = 12;
	Dangling.LoadedRounds = 1;
	DanglingMagazines.Add(Dangling);
	UProfileSaveGame* DanglingSave = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
		DanglingMagazines, {}, false, FString());
	ExpectRejected(DanglingSave, EProfileLoadError::DanglingMagazineReference, TEXT("does not exist in the inventory"), TEXT("dangling magazine"));

	// v2 weapon state: loaded rounds beyond the row's own capacity.
	TArray<FWeaponMagazineSaveEntry> OverfullMagazines;
	FWeaponMagazineSaveEntry Overfull = First;
	Overfull.LoadedRounds = 13;
	OverfullMagazines.Add(Overfull);
	UProfileSaveGame* OverfullSave = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
		OverfullMagazines, {}, false, FString());
	ExpectRejected(OverfullSave, EProfileLoadError::InvalidWeaponState, TEXT("LoadedRounds"), TEXT("overfull magazine"));

	// v2 weapon state: reserve rounds beyond the row's own cap, and a
	// duplicated pool identity.
	TArray<FAmmoReserveSaveEntry> OverfullPools;
	FAmmoReserveSaveEntry Pool;
	Pool.AmmoId = FName(TEXT("ammo_cell"));
	Pool.MaxReserve = 120;
	Pool.Rounds = 121;
	OverfullPools.Add(Pool);
	UProfileSaveGame* PoolSave = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
		{}, OverfullPools, false, FString());
	ExpectRejected(PoolSave, EProfileLoadError::InvalidWeaponState, TEXT("Rounds"), TEXT("overfull reserve"));

	TArray<FAmmoReserveSaveEntry> DuplicatePools;
	// The base row must be LEGAL on its own: the per-row rules run before the
	// duplicate check, so reusing the overfull row above would classify as
	// InvalidWeaponState and never reach DuplicateWeaponState.
	FAmmoReserveSaveEntry DuplicateBase;
	DuplicateBase.AmmoId = FName(TEXT("ammo_cell"));
	DuplicateBase.MaxReserve = 120;
	DuplicateBase.Rounds = 10;
	DuplicatePools.Add(DuplicateBase);
	FAmmoReserveSaveEntry PoolCopy = DuplicateBase;
	PoolCopy.Rounds = 0;
	DuplicatePools.Add(PoolCopy);
	UProfileSaveGame* DuplicatePoolSave = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
		{}, DuplicatePools, false, FString());
	ExpectRejected(DuplicatePoolSave, EProfileLoadError::DuplicateWeaponState, TEXT("more than one pool row"), TEXT("duplicate reserve"));

	// A legal v2 state with the same shapes still loads (the refusals above
	// were the rows' own fault, not the container's presence).
	TArray<FWeaponMagazineSaveEntry> LegalMagazines;
	LegalMagazines.Add(First);
	TArray<FAmmoReserveSaveEntry> LegalPools;
	// A legal pool row (the "Pool" probe above is deliberately overfull and
	// must not leak into the legal-state fixture).
	FAmmoReserveSaveEntry LegalPool;
	LegalPool.AmmoId = FName(TEXT("ammo_cell"));
	LegalPool.MaxReserve = 120;
	LegalPool.Rounds = 10;
	LegalPools.Add(LegalPool);
	UProfileSaveGame* LegalSave = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
		LegalMagazines, LegalPools, true, FString(TEXT("rev-legal")));
	FM5_046_LoadOuts LegalOuts;
	if (Test.TestTrue(TEXT("the legal v2 weapon state loads"),
		FProfileSerializer::FromSaveGame(LegalSave, LegalOuts.Snapshot, LegalOuts.Inventory, LegalOuts.Pending,
			LegalOuts.Applied, LegalOuts.Equipped, LegalOuts.Error, nullptr, nullptr, &LegalOuts.WeaponState)))
	{
		Test.TestEqual(TEXT("the legal magazine round-trips"), LegalOuts.WeaponState.WeaponMagazines.Num(), 1);
		Test.TestEqual(TEXT("the legal pool round-trips"), LegalOuts.WeaponState.ReserveAmmoPools.Num(), 1);
	}
	return true;
}

// -- 5: the manifest is explicit, data-driven and self-validated ----------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046ManifestIsExplicitDataDrivenAndValidated,
	"UEMMO.Tasks.M5_046.ManifestIsExplicitDataDrivenAndValidated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046ManifestIsExplicitDataDrivenAndValidated::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// The build-wide manifest mirrors the shipped content and the frozen
	// alias; it must pass its own structural validation.
	FString Problem;
	Test.TestTrue(TEXT("the default manifest validates"), FProfileItemManifest::Get().Validate(Problem));
	FName Resolved;
	Test.TestTrue(TEXT("the default manifest knows weapon_training"),
		FProfileItemManifest::Get().ResolveDefinitionId(FName(TEXT("weapon_training")), Resolved)
		&& Resolved == FName(TEXT("weapon_training")));
	Test.TestTrue(TEXT("the frozen alias resolves accessory_training onto charm_training"),
		FProfileItemManifest::Get().ResolveDefinitionId(FName(TEXT("accessory_training")), Resolved)
		&& Resolved == FName(TEXT("charm_training")));
	Test.TestFalse(TEXT("an unregistered id resolves to nothing"),
		FProfileItemManifest::Get().ResolveDefinitionId(FName(TEXT("relic_obsolete")), Resolved));

	// Data-driven: a test-local manifest extends the same mechanism.
	FProfileItemManifest Custom;
	Custom.KnownDefinitionIds = { FName(TEXT("charm_training")), FName(TEXT("armor_training")) };
	FLegacyItemAliasEntry Alias;
	Alias.LegacyId = FName(TEXT("accessory_old"));
	Alias.CurrentId = FName(TEXT("charm_training"));
	Custom.LegacyAliases.Add(Alias);
	Test.TestTrue(TEXT("a well-formed custom manifest validates"), Custom.Validate(Problem));

	auto ExpectBrokenManifest = [&Test](const FProfileItemManifest& Candidate, const TCHAR* Why)
	{
		FString Broken;
		Test.TestTrue(FString::Printf(TEXT("%s is rejected"), Why), !Candidate.Validate(Broken));
		Test.TestFalse(FString::Printf(TEXT("%s names its row"), Why), Broken.IsEmpty());
	};

	// Missing target: the alias would resolve into the unknown (this is also
	// the "revision coverage" edge - a config that removed the target breaks
	// the manifest and the migration refuses).
	FProfileItemManifest MissingTarget = Custom;
	MissingTarget.KnownDefinitionIds = { FName(TEXT("armor_training")) };
	ExpectBrokenManifest(MissingTarget, TEXT("an alias whose target is not a known definition"));

	// Self-alias: resolves nothing.
	FProfileItemManifest SelfAlias = Custom;
	SelfAlias.LegacyAliases[0].CurrentId = FName(TEXT("accessory_old"));
	ExpectBrokenManifest(SelfAlias, TEXT("a self-alias"));

	// Vocabulary overlap: a legacy id doubling as a current definition.
	FProfileItemManifest Overlap = Custom;
	Overlap.KnownDefinitionIds.Add(FName(TEXT("accessory_old")));
	ExpectBrokenManifest(Overlap, TEXT("a legacy id that is also a current definition"));

	// A two-row cycle (a->b, b->a) is caught by the structural rules.
	FProfileItemManifest Cycle;
	Cycle.KnownDefinitionIds = { FName(TEXT("item_a")), FName(TEXT("item_b")) };
	FLegacyItemAliasEntry CycleFirst;
	CycleFirst.LegacyId = FName(TEXT("legacy_a"));
	CycleFirst.CurrentId = FName(TEXT("item_b"));
	FLegacyItemAliasEntry CycleSecond;
	CycleSecond.LegacyId = FName(TEXT("item_b"));
	CycleSecond.CurrentId = FName(TEXT("item_a"));
	Cycle.LegacyAliases.Add(CycleFirst);
	Cycle.LegacyAliases.Add(CycleSecond);
	ExpectBrokenManifest(Cycle, TEXT("an alias chain that loops"));

	// Duplicate legacy rows.
	FProfileItemManifest Duplicate = Custom;
	Duplicate.LegacyAliases.Add(Alias);
	ExpectBrokenManifest(Duplicate, TEXT("a duplicated legacy row"));

	// Seed rows must reference known definitions and sane values.
	FProfileItemManifest BadSeed = Custom;
	BadSeed.WeaponAmmoSeeds.Add(FName(TEXT("weapon_unknown")), { FName(TEXT("ammo_cell")), 12 });
	ExpectBrokenManifest(BadSeed, TEXT("a seed for an unknown definition"));
	FProfileItemManifest ZeroCapacitySeed = Custom;
	ZeroCapacitySeed.WeaponAmmoSeeds.Add(FName(TEXT("charm_training")), { FName(TEXT("ammo_cell")), 0 });
	ExpectBrokenManifest(ZeroCapacitySeed, TEXT("a seed with a zero capacity"));
	FProfileItemManifest BadAmmoSeed = Custom;
	BadAmmoSeed.WeaponAmmoSeeds.Add(FName(TEXT("charm_training")), { FName(TEXT("AMMO-CELL")), 12 });
	ExpectBrokenManifest(BadAmmoSeed, TEXT("a seed with a malformed ammo id"));
	return true;
}

// -- 6: the ConfigRevision gate (compatible rebind / incompatible refusal) ------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046ConfigRevisionCompatibleRebindAndIncompatibleRefusal,
	"UEMMO.Tasks.M5_046.ConfigRevisionCompatibleRebindAndIncompatibleRefusal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046ConfigRevisionCompatibleRebindAndIncompatibleRefusal::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// A v2 save recorded under revision "rev-A": one loaded rifle magazine
	// (5 of 12) and a shared ammo_cell reserve (30 of 120).
	const FItemInstance Rifle = M5_046_MakeInstance(41, TEXT("weapon_hitscan_gun"), 5.0f, 0.0f, 0.0f, 1);
	FProfileSnapshot Snapshot;
	Snapshot.CharacterId = M5_046_CharacterId();
	Snapshot.Level = 2;
	Snapshot.XP = 10;
	Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(2);
	Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(2);
	Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(2);
	FInventoryModel Inventory;
	if (!Test.TestTrue(TEXT("fixture: the rifle was added"), Inventory.TryAdd(Rifle) == EInventoryAddResult::Added))
	{
		return true;
	}
	TArray<FWeaponMagazineSaveEntry> Magazines;
	FWeaponMagazineSaveEntry Magazine;
	Magazine.InstanceId = Rifle.InstanceId;
	Magazine.AmmoId = FName(TEXT("ammo_cell"));
	Magazine.Capacity = 12;
	Magazine.LoadedRounds = 5;
	Magazines.Add(Magazine);
	TArray<FAmmoReserveSaveEntry> Pools;
	FAmmoReserveSaveEntry Pool;
	Pool.AmmoId = FName(TEXT("ammo_cell"));
	Pool.MaxReserve = 120;
	Pool.Rounds = 30;
	Pools.Add(Pool);
	UProfileSaveGame* Save = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
		Magazines, Pools, true, FString(TEXT("rev-A")));
	if (!Test.TestNotNull(TEXT("fixture: the v2 weapon-state payload was built"), Save))
	{
		return true;
	}

	// The current config view: ammo_cell unchanged, plus one NEW kind (the
	// "only added rows" shape of a config update).
	auto MakeCurrentView = []()
	{
		TMap<FName, FAmmoType> View;
		FAmmoType Cell;
		Cell.AmmoId = FName(TEXT("ammo_cell"));
		Cell.MaxReserve = 120;
		Cell.MagazineSize = 12;
		View.Add(Cell.AmmoId, Cell);
		return View;
	};

	// Compatible: the additive change loads; the restored weapon state is the
	// rebind input (loaded rounds and reserve stock exactly as stored - the
	// caller rebinds against the current catalog, no re-grant happens).
	{
		TMap<FName, FAmmoType> View = MakeCurrentView();
		FAmmoType Light;
		Light.AmmoId = FName(TEXT("ammo_light"));
		Light.MaxReserve = 96;
		Light.MagazineSize = 12;
		View.Add(Light.AmmoId, Light);
		FM5_046_LoadOuts Outs;
		if (!Test.TestTrue(TEXT("an additive config change loads the old v2 save"),
			FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending,
				Outs.Applied, Outs.Equipped, Outs.Error, nullptr, &View, &Outs.WeaponState)))
		{
			Test.AddError(FString::Printf(TEXT("additive load failed: %s"), *Outs.Error.Message));
			return true;
		}
		Test.TestEqual(TEXT("the loaded state keeps the stored load"), Outs.WeaponState.WeaponMagazines.Num() == 1
			? Outs.WeaponState.WeaponMagazines[0].LoadedRounds : -1, 5);
		Test.TestEqual(TEXT("the loaded state keeps the stored reserve"), Outs.WeaponState.ReserveAmmoPools.Num() == 1
			? Outs.WeaponState.ReserveAmmoPools[0].Rounds : -1, 30);
		Test.TestEqual(TEXT("the loaded state keeps the recorded revision"),
			Outs.WeaponState.ConfigRevision, FString(TEXT("rev-A")));
	}

	// Incompatible: the ammo kind was removed. Refuse; keep the file.
	{
		TMap<FName, FAmmoType> ViewWithoutCell;
		FAmmoType Light;
		Light.AmmoId = FName(TEXT("ammo_light"));
		Light.MaxReserve = 96;
		Light.MagazineSize = 12;
		ViewWithoutCell.Add(Light.AmmoId, Light);
		FM5_046_LoadOuts Outs = FM5_046_LoadOuts::MakeSentinel();
		Test.TestFalse(TEXT("a removed ammo kind refuses the load"),
			FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending,
				Outs.Applied, Outs.Equipped, Outs.Error, nullptr, &ViewWithoutCell, &Outs.WeaponState));
		Test.TestEqual(TEXT("the removed-kind refusal names the weapon state"),
			static_cast<int32>(Outs.Error.Code), static_cast<int32>(EProfileLoadError::InvalidWeaponState));
		Test.TestTrue(TEXT("the refusal names the kind and the recorded revision"),
			Outs.Error.Message.Contains(TEXT("ammo_cell")) && Outs.Error.Message.Contains(TEXT("rev-A")));
		Test.TestTrue(TEXT("the refused load left every Out* untouched"), Outs.StillSentinel());
		Test.TestEqual(TEXT("the refused save keeps its magazine rows"), Save->WeaponMagazines.Num(), 1);
	}

	// Incompatible: the current magazine capacity shrunk below the stored
	// load ("容量小于已装弹").
	{
		TMap<FName, FAmmoType> ShrunkView;
		FAmmoType Cell;
		Cell.AmmoId = FName(TEXT("ammo_cell"));
		Cell.MaxReserve = 120;
		Cell.MagazineSize = 3;
		ShrunkView.Add(Cell.AmmoId, Cell);
		FM5_046_LoadOuts Outs = FM5_046_LoadOuts::MakeSentinel();
		Test.TestFalse(TEXT("a magazine capacity below the stored load refuses"),
			FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending,
				Outs.Applied, Outs.Equipped, Outs.Error, nullptr, &ShrunkView, &Outs.WeaponState));
		Test.TestTrue(TEXT("the shrunk-magazine refusal names the constraint"),
			Outs.Error.Message.Contains(TEXT("capacity")));
		Test.TestTrue(TEXT("the shrunk-magazine refusal left the outputs untouched"), Outs.StillSentinel());
	}

	// Incompatible: the current reserve cap shrunk below the stored stock.
	{
		TMap<FName, FAmmoType> ShrunkReserveView;
		FAmmoType Cell;
		Cell.AmmoId = FName(TEXT("ammo_cell"));
		Cell.MaxReserve = 10;
		Cell.MagazineSize = 12;
		ShrunkReserveView.Add(Cell.AmmoId, Cell);
		FM5_046_LoadOuts Outs = FM5_046_LoadOuts::MakeSentinel();
		Test.TestFalse(TEXT("a reserve cap below the stored stock refuses"),
			FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending,
				Outs.Applied, Outs.Equipped, Outs.Error, nullptr, &ShrunkReserveView, &Outs.WeaponState));
		Test.TestTrue(TEXT("the shrunk-reserve refusal left the outputs untouched"), Outs.StillSentinel());
	}

	// The revision STRING is provenance only: the same state recorded under a
	// different revision loads against the same compatible view - there is
	// no revision-string comparison anywhere in the gate.
	{
		UProfileSaveGame* OtherRevision = FProfileSerializer::ToSaveGame(Snapshot, Inventory, {}, {}, {},
			Magazines, Pools, true, FString(TEXT("rev-something-else-entirely")));
		TMap<FName, FAmmoType> View = MakeCurrentView();
		FM5_046_LoadOuts Outs;
		if (Test.TestNotNull(TEXT("fixture: the other-revision payload was built"), OtherRevision))
		{
			Test.TestTrue(TEXT("a different revision string alone never refuses"),
				FProfileSerializer::FromSaveGame(OtherRevision, Outs.Snapshot, Outs.Inventory, Outs.Pending,
					Outs.Applied, Outs.Equipped, Outs.Error, nullptr, &View, &Outs.WeaponState));
		}
	}

	// Without a view the gate is disarmed (the M3-014/M3-015 call sites stay
	// ungated until the wiring card arms them with the live catalog).
	{
		FM5_046_LoadOuts Outs;
		Test.TestTrue(TEXT("without a current view the save loads ungated"),
			FProfileSerializer::FromSaveGame(Save, Outs.Snapshot, Outs.Inventory, Outs.Pending,
				Outs.Applied, Outs.Equipped, Outs.Error));
	}
	return true;
}

// -- 7: the A/B transaction keeps its order; index failures keep the old save ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046InactiveSlotReadBackBeforeIndexCommitAndIndexFailureKeepsOldSave,
	"UEMMO.Tasks.M5_046.InactiveSlotReadBackBeforeIndexCommitAndIndexFailureKeepsOldSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046InactiveSlotReadBackBeforeIndexCommitAndIndexFailureKeepsOldSave::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM5_046_MemoryStorage Storage;
	UProfileSaveService* Service = M5_046_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}

	// Save #1: slot A written, read back, then the index committed - exactly
	// two storage writes, in that order, and the index names A with gen 1.
	const FProfileSaveRequest FirstRequest = M5_046_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveOutcome FirstSave = Service->SaveProfile(FirstRequest);
	Test.TestEqual(TEXT("the first save committed"),
		static_cast<int32>(FirstSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the first save wrote slot A"), FirstSave.WrittenSlotIndex, 0);
	Test.TestEqual(TEXT("one transaction performs exactly two writes (slot + index)"), Storage.WriteCallCount, 2);
	UProfileIndexSaveGame* RawIndex = M5_046_ReadRawIndex(Storage);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("the index points at slot A"), RawIndex->ActiveSlotIndex, 0);
		Test.TestEqual(TEXT("the index records generation 1 for A"),
			RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[0] : -1, 1);
	}

	// Index-commit failure: the new slot content exists but the index was
	// never moved - the OLD save stays active and loadable.
	FM5_046_FaultStorage IndexFault(&Storage);
	IndexFault.FailWriteSlotName = M5_046_IndexSlotName();
	Service->SetStorage(&IndexFault);
	const FProfileSaveRequest SecondRequest = M5_046_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome SecondSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("the index-commit failure is reported"),
		static_cast<int32>(SecondSave.Result), static_cast<int32>(EProfileSaveResult::FailedWriteIndex));
	RawIndex = M5_046_ReadRawIndex(Storage);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("the index still points at slot A after the failed commit"), RawIndex->ActiveSlotIndex, 0);
	}
	const FProfileLoadOutcome ReloadedAfterIndexFailure = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("the old save still loads after the index-commit failure"),
		static_cast<int32>(ReloadedAfterIndexFailure.Result), static_cast<int32>(EProfileLoadResult::Success));
	Test.TestEqual(TEXT("the old save kept its generation"), ReloadedAfterIndexFailure.Generation, 1);
	Test.TestTrue(TEXT("the old save kept its identity"),
		ReloadedAfterIndexFailure.Snapshot.CharacterId == M5_046_CharacterId());

	// Read-back corruption: the freshly written slot fails its verification
	// and the index is never pointed at it.
	FM5_046_FaultStorage TamperFault(&Storage);
	TamperFault.TamperReadSlotName = M5_046_SlotBName();
	Service->SetStorage(&TamperFault);
	const FProfileSaveOutcome TamperedSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("the corrupted read-back is reported"),
		static_cast<int32>(TamperedSave.Result), static_cast<int32>(EProfileSaveResult::FailedReadBack));
	RawIndex = M5_046_ReadRawIndex(Storage);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("the index still points at slot A after the failed read-back"), RawIndex->ActiveSlotIndex, 0);
	}
	Test.TestEqual(TEXT("the old save still loads after the failed read-back"),
		static_cast<int32>(Service->LoadActiveProfile().Result), static_cast<int32>(EProfileLoadResult::Success));

	// Faults cleared: the same request finally commits to slot B (gen 2) and
	// the index moves only then.
	Service->SetStorage(&Storage);
	const FProfileSaveOutcome CommittedSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("the retry committed"),
		static_cast<int32>(CommittedSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the retry wrote slot B"), CommittedSave.WrittenSlotIndex, 1);
	Test.TestEqual(TEXT("the retry advanced the generation"), CommittedSave.Generation, 2);
	RawIndex = M5_046_ReadRawIndex(Storage);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("the index now points at slot B"), RawIndex->ActiveSlotIndex, 1);
	}
	const FProfileLoadOutcome FinalReload = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("the new save loads with its own data"),
		static_cast<int32>(FinalReload.Result), static_cast<int32>(EProfileLoadResult::Success));
	Test.TestEqual(TEXT("the new save reports its generation"), FinalReload.Generation, 2);

	M5_046_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- 8: a byte-realistic v1 slot loads through the real service path ------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_046V1SlotLoadsThroughServiceMigration,
	"UEMMO.Tasks.M5_046.V1SlotLoadsThroughServiceMigration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_046V1SlotLoadsThroughServiceMigration::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM5_046_MemoryStorage Storage;
	UProfileSaveService* Service = M5_046_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}

	// Plant a byte-realistic v1 slot: the M3-013 payload layout, the old
	// build's digest form (the replica - identical to what an old build
	// stamped, because the M5-046 digest leaves v1 payloads untouched), and
	// the transaction envelope fields.
	const FM5_046_V1Fixture Fixture = FM5_046_V1Fixture::Make(Test);
	if (!Fixture.Save)
	{
		return true;
	}
	const uint32 LegacyDigest = M5_046_ComputeLegacyV1PayloadDigest(*Fixture.Save);
	UProfileSlotSaveGame* LegacySlot = M5_046_WrapIntoSlotSave(*Fixture.Save, 1, LegacyDigest);
	if (!Test.TestTrue(TEXT("setup: the planted v1 slot was written"),
		Storage.WriteSlot(M5_046_SlotAName(), LegacySlot)))
	{
		return true;
	}
	UProfileIndexSaveGame* Index = NewObject<UProfileIndexSaveGame>(GetTransientPackage());
	Index->ActiveSlotIndex = 0;
	Index->SlotGenerations.Add(1);
	Index->SlotGenerations.Add(0);
	if (!Test.TestTrue(TEXT("setup: the planted index was written"),
		Storage.WriteSlot(M5_046_IndexSlotName(), Index)))
	{
		return true;
	}

	// The startup path must classify the v1 slot as recoverable (NOT corrupt)
	// and restore it through the explicit migration.
	const FStartupLoadOutcome Startup = Service->StartupLoad();
	Test.TestEqual(TEXT("the v1 slot recovered through the migration channel"),
		static_cast<int32>(Startup.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("the recovered slot kept its generation"), Startup.Generation, 1);
	Test.TestTrue(TEXT("the recovered profile kept its identity"),
		Startup.Snapshot.CharacterId == M5_046_CharacterId());
	Test.TestEqual(TEXT("the recovered profile kept its level"), Startup.Snapshot.Level, 3);
	Test.TestEqual(TEXT("the recovered inventory carried every instance"), Startup.Inventory.Count(), 3);
	const FItemInstance* RestoredCharm = M5_046_FindInstance(Startup.Inventory, Fixture.LegacyCharm.InstanceId);
	Test.TestTrue(TEXT("the legacy charm came back under its current kind"),
		RestoredCharm != nullptr && RestoredCharm->DefinitionId == FName(TEXT("charm_training")));
	M5_046_ExpectSameInstanceIdentity(Test,
		RestoredCharm ? *RestoredCharm : Fixture.LegacyCharm, Fixture.LegacyCharm,
		TEXT("the service-restored legacy charm"));
	const FGuid* EquippedAccessory = Startup.EquippedMap.Find(EItemSlot::Accessory);
	Test.TestTrue(TEXT("the recovered equipment binding kept the legacy charm"),
		EquippedAccessory != nullptr && *EquippedAccessory == Fixture.LegacyCharm.InstanceId);
	Test.TestTrue(TEXT("the recovered applied set kept both claims"),
		Startup.AppliedSettlementIds.Contains(900) && Startup.AppliedSettlementIds.Contains(901));

	// The next ordinary save through the service commits a v2 slot and the
	// load path reads it back - the pipeline now runs entirely on v2.
	FProfileSaveRequest FollowUp;
	FollowUp.Snapshot = Startup.Snapshot;
	FollowUp.Inventory = Startup.Inventory;
	FollowUp.PendingRewards = Startup.PendingRewards;
	FollowUp.AppliedSettlementIds = Startup.AppliedSettlementIds;
	FollowUp.EquippedMap = Startup.EquippedMap;
	const FProfileSaveOutcome FollowUpSave = Service->SaveProfile(FollowUp);
	Test.TestEqual(TEXT("the follow-up v2 save committed"),
		static_cast<int32>(FollowUpSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the follow-up save advanced the generation"), FollowUpSave.Generation, 2);
	const FProfileLoadOutcome FinalLoad = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("the v2 slot loads back"),
		static_cast<int32>(FinalLoad.Result), static_cast<int32>(EProfileLoadResult::Success));
	const FItemInstance* FinalCharm = M5_046_FindInstance(FinalLoad.Inventory, Fixture.LegacyCharm.InstanceId);
	Test.TestTrue(TEXT("the migrated id survived the v2 round trip"),
		FinalCharm != nullptr && FinalCharm->DefinitionId == FName(TEXT("charm_training")));

	M5_046_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

#endif
