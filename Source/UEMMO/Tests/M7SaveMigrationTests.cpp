#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/UObjectGlobals.h"

#include "Kismet/GameplayStatics.h"
#include "../Persistence/ProfileSaveGame.h"
#include "../Persistence/ProfileSaveService.h"
#include "../Persistence/ProfileSerializer.h"
#include "../Persistence/RosterService.h"
#include "../Profile/ProfileSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M7_002
{
	/**
	 * Deterministic item instance whose snapshot string is the REAL
	 * M3-001 wire form (the serializer validates the version tag, so a
	 * hand-written fake snapshot would be refused before any profession
	 * rule is reached).
	 */
	static FItemInstance M7_002_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP, int32 ItemLevel)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x7EED0000u + Seed, 0x0772u, 0x0072u, Seed * 19u + 3u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(0x0772CBA9876543210ULL) + static_cast<int64>(Seed);
		Instance.RolledStats.Attack = Attack;
		Instance.RolledStats.Defense = Defense;
		Instance.RolledStats.MaxHP = MaxHP;
		Instance.Level = ItemLevel;
		return Instance;
	}

	static UProfileSaveGame* MakeV3Save(UObject* Outer, const FGuid& CharacterId)
	{
		UProfileSaveGame* Save = NewObject<UProfileSaveGame>(Outer ? Outer : GetTransientPackage());
		Save->SchemaVersion = UProfileSaveGame::SchemaVersion3;
		Save->CharacterId = CharacterId;
		Save->Level = 3;
		Save->XP = 210;
		const FItemInstance Item = M7_002_MakeInstance(7, TEXT("weapon_training"), 2.0f, 3.0f, 1.0f, 1);
		Save->InventoryInstanceSnapshots.Add(Item.ToStringSnapshot());
		FAmmoReserveSaveEntry Pool;
		Pool.AmmoId = FName(TEXT("ammo_cell"));
		Pool.MaxReserve = 60;
		Pool.Rounds = 12;
		Save->ReserveAmmoPools.Add(Pool);
		// The magazine belongs to the inventory instance above; an orphaned
		// magazine identity is rejected on load.
		FWeaponMagazineSaveEntry Mag;
		Mag.InstanceId = Item.InstanceId;
		Mag.AmmoId = FName(TEXT("ammo_cell"));
		Mag.Capacity = 12;
		Mag.LoadedRounds = 5;
		Save->WeaponMagazines.Add(Mag);
		Save->bWeaponStateInitialized = true;
		Save->ConfigRevision = TEXT("v3-revision");
		FRoomClearSaveEntry Clear;
		Clear.RoomId = FName(TEXT("room_training_01"));
		Clear.ClearCount = 2;
		Clear.LastClearUnixSeconds = 1700000000;
		Save->RoomClears.Add(Clear);
		return Save;
	}

	class FMemoryRosterStorage : public ISaveStorage
	{
	public:
		TMap<FString, USaveGame*> Slots;
		bool bRefuseWrites = false;

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			if (bRefuseWrites)
			{
				return false;
			}
			Slots.Add(SlotName, DuplicateObject(Data, nullptr));
			return true;
		}
		virtual USaveGame* ReadSlot(const FString& SlotName) override
		{
			USaveGame* const* Found = Slots.Find(SlotName);
			return Found ? DuplicateObject(*Found, nullptr) : nullptr;
		}
		virtual bool DeleteSlot(const FString& SlotName) override
		{
			return Slots.Remove(SlotName) > 0;
		}
		virtual bool DoesSlotExist(const FString& SlotName) override
		{
			return Slots.Contains(SlotName);
		}
	};

	/**
	 * Byte-exact replica of the FROZEN M3-014 canonical payload digest form
	 * (UProfileSaveService::M3_014_ComputePayloadDigest), including the M7-002
	 * version gate: the profession-state segments join ONLY for a schema 4
	 * payload, so a planted v3 slot hashes to exactly the pre-M7 canonical
	 * string the real service would have stamped. If this replica ever drifts
	 * from the real digest the import test below fails loudly - that is the
	 * desired tripwire.
	 */
	static uint32 M7_002_ComputeCanonicalDigest(const UProfileSaveGame& Save)
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
		for (const FWeaponMagazineSaveEntry& Magazine : Save.WeaponMagazines)
		{
			Canonical += FString::Printf(TEXT("mag=%s:%s:%d:%d|"),
				*Magazine.InstanceId.ToString(), *Magazine.AmmoId.ToString(),
				Magazine.Capacity, Magazine.LoadedRounds);
		}
		for (const FAmmoReserveSaveEntry& Pool : Save.ReserveAmmoPools)
		{
			Canonical += FString::Printf(TEXT("pool=%s:%d:%d|"),
				*Pool.AmmoId.ToString(), Pool.MaxReserve, Pool.Rounds);
		}
		if (Save.bWeaponStateInitialized)
		{
			Canonical += TEXT("winit=1|");
		}
		if (!Save.ConfigRevision.IsEmpty())
		{
			Canonical += FString::Printf(TEXT("rev=%s|"), *Save.ConfigRevision);
		}
		for (const FRoomClearSaveEntry& Clear : Save.RoomClears)
		{
			Canonical += FString::Printf(TEXT("room=%s:%d:%lld|"),
				*Clear.RoomId.ToString(), Clear.ClearCount, Clear.LastClearUnixSeconds);
		}
		if (Save.SchemaVersion >= UProfileSaveGame::SchemaVersion3 + 1)
		{
			Canonical += FString::Printf(TEXT("prof=%s|sp=%d|dc=%d|prev=%s|"),
				*Save.ProfessionId.ToString(), Save.UnspentSP, Save.DungeonClears,
				*Save.ProfessionConfigRevision);
			for (const FSkillRankSaveEntry& Rank : Save.SkillRanks)
			{
				Canonical += FString::Printf(TEXT("rank=%s:%d|"), *Rank.SkillId.ToString(), Rank.Rank);
			}
			for (const FSkillSlotSaveEntry& SlotEntry : Save.SkillLoadout)
			{
				Canonical += FString::Printf(TEXT("slot=%d:%s|"), SlotEntry.SlotIndex, *SlotEntry.SkillId.ToString());
			}
		}
		return FCrc::StrCrc32(*Canonical);
	}

	static URosterService* NewRosterService(FAutomationTestBase& Test, FMemoryRosterStorage& Storage,
		const TCHAR* Prefix = TEXT("TestRoster_"))
	{
		URosterService* Service = NewObject<URosterService>(GetTransientPackage());
		if (!Test.TestTrue(TEXT("the roster service initializes"), Service->Initialize(Prefix)))
		{
			return nullptr;
		}
		Service->SetStorage(&Storage);
		return Service;
	}

	static FProfileSaveRequest MakeInitialStateRequest()
	{
		FProfileSaveRequest Request;
		Request.Snapshot.Level = 1;
		Request.Snapshot.XP = 0;
		Request.ProfessionState = FProfessionSaveState::MakeLegacyPrototype();
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002V3ToV4MigrationPreservesValues,
	"UEMMO.Tasks.M7_002.V3ToV4MigrationPreservesValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002V3ToV4MigrationPreservesValues::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	const FGuid CharacterId(0xAABBu, 0xCCDDu, 0xEEFFu, 0x0011u);
	UProfileSaveGame* V3 = MakeV3Save(nullptr, CharacterId);

	FProfileLoadError Error;
	UProfileSaveGame* V4 = FProfileSerializer::MigrateV3ToV4(*V3, Error);
	if (!Test.TestNotNull(TEXT("the migration produced a v4 save"), V4))
	{
		Test.AddError(FString::Printf(TEXT("v3->v4 migration failed: %s"), *Error.Message));
		return false;
	}
	TestEqual(TEXT("schema 4 stamped"), V4->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion);
	TestEqual(TEXT("identity preserved"), V4->CharacterId, CharacterId);
	TestEqual(TEXT("level preserved"), V4->Level, 3);
	TestEqual(TEXT("xp preserved"), V4->XP, 210);
	TestEqual(TEXT("inventory preserved"), V4->InventoryInstanceSnapshots.Num(), 1);
	TestEqual(TEXT("weapon state preserved"), V4->WeaponMagazines.Num(), 1);
	TestEqual(TEXT("reserve preserved"), V4->ReserveAmmoPools.Num(), 1);
	TestEqual(TEXT("config revision preserved"), V4->ConfigRevision, FString(TEXT("v3-revision")));
	TestEqual(TEXT("room clears preserved"), V4->RoomClears.Num(), 1);
	TestEqual(TEXT("profession seeds prototype"), V4->ProfessionId, FName(TEXT("prototype")));
	TestEqual(TEXT("the four legacy attacks are learned"), V4->SkillRanks.Num(), 4);
	for (const FSkillRankSaveEntry& Rank : V4->SkillRanks)
	{
		TestEqual(FString::Printf(TEXT("legacy skill '%s' at rank 1"), *Rank.SkillId.ToString()), Rank.Rank, 1);
	}
	TestEqual(TEXT("unspent SP seeds 0"), V4->UnspentSP, 0);
	TestEqual(TEXT("loadout seeds empty"), V4->SkillLoadout.Num(), 0);
	TestEqual(TEXT("dungeon clears seed 0"), V4->DungeonClears, 0);
	TestEqual(TEXT("the v3 input is untouched"), V3->SchemaVersion, UProfileSaveGame::SchemaVersion3);
	TestTrue(TEXT("the v3 input has no profession id"), V3->ProfessionId.IsNone());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002V1ChainToV4,
	"UEMMO.Tasks.M7_002.V1ChainToV4",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002V1ChainToV4::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	UProfileSaveGame* V1 = NewObject<UProfileSaveGame>(GetTransientPackage());
	V1->SchemaVersion = UProfileSaveGame::LegacySchemaVersion;
	V1->CharacterId = FGuid(0x11223344u, 0x5566u, 0x7788u, 0x99AAu);
	V1->Level = 5;
	V1->XP = 250;
	V1->InventoryInstanceSnapshots.Add(
		M7_002_MakeInstance(3, TEXT("accessory_training"), 1.0f, 0.0f, 5.0f, 1).ToStringSnapshot());

	FProfileLoadError Error;
	FProfileSnapshot Snapshot;
	FInventoryModel Inventory;
	TArray<FPendingReward> Pending;
	TSet<uint64> Applied;
	TMap<EItemSlot, FGuid> Equipped;
	FProfessionStateOut ProfessionState;
	const bool bLoaded = FProfileSerializer::FromSaveGame(V1, Snapshot, Inventory, Pending, Applied,
		Equipped, Error, nullptr, nullptr, nullptr, nullptr, &ProfessionState);
	if (!bLoaded)
	{
		AddError(FString::Printf(TEXT("the v1 chain load error: %s"), *Error.Message));
	}
	TestTrue(TEXT("the v1 save loads through the chained migrations"), bLoaded);
	TestEqual(TEXT("the chained profession is prototype"), ProfessionState.ProfessionId, FName(TEXT("prototype")));
	TestEqual(TEXT("the chained legacy attacks are learned"), ProfessionState.SkillRanks.Num(), 4);
	TestEqual(TEXT("the chained unspent SP is 0"), ProfessionState.UnspentSP, 0);
	TestEqual(TEXT("the chained loadout is empty"), ProfessionState.SkillLoadout.Num(), 0);
	TestEqual(TEXT("the chained dungeon clears is 0"), ProfessionState.DungeonClears, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002FutureSchemaRefused,
	"UEMMO.Tasks.M7_002.FutureSchemaRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002FutureSchemaRefused::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	UProfileSaveGame* Future = NewObject<UProfileSaveGame>(GetTransientPackage());
	Future->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion + 1;
	Future->CharacterId = FGuid(0x1111u, 0x2222u, 0x3333u, 0x4444u);
	Future->Level = 2;
	Future->XP = 10;

	FProfileLoadError Error;
	FProfileSnapshot Snapshot;
	FInventoryModel Inventory;
	TArray<FPendingReward> Pending;
	TSet<uint64> Applied;
	TMap<EItemSlot, FGuid> Equipped;
	FProfessionStateOut ProfessionState;
	FProfileSnapshot FutureSnapshot;
	FutureSnapshot.CharacterId = Future->CharacterId;
	FutureSnapshot.Level = Future->Level;
	FutureSnapshot.XP = Future->XP;
	TestFalse(TEXT("a future-schema save is refused"),
		FProfileSerializer::FromSaveGame(Future, FutureSnapshot, Inventory, Pending, Applied, Equipped, Error,
			nullptr, nullptr, nullptr, nullptr, &ProfessionState));
	TestEqual(TEXT("the refusal is UnsupportedSchema"), Error.Code, EProfileLoadError::UnsupportedSchema);
	TestEqual(TEXT("the future save object is untouched"), Future->CharacterId, FGuid(0x1111u, 0x2222u, 0x3333u, 0x4444u));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002ProfessionStateValidation,
	"UEMMO.Tasks.M7_002.ProfessionStateValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002ProfessionStateValidation::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	auto MakeValidV4 = [](UObject* Outer) -> UProfileSaveGame*
	{
		UProfileSaveGame* Save = NewObject<UProfileSaveGame>(Outer ? Outer : GetTransientPackage());
		Save->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
		Save->CharacterId = FGuid(0xAAAAu, 0xBBBBu, 0xCCCCu, 0xDDDDu);
		Save->ProfessionId = FName(TEXT("prototype"));
		Save->Level = 2;
		Save->XP = 50;
		FSkillRankSaveEntry Rank;
		Rank.SkillId = FName(TEXT("launcher_z"));
		Rank.Rank = 1;
		Save->SkillRanks.Add(Rank);
		FSkillSlotSaveEntry Slot;
		Slot.SlotIndex = 2;
		Slot.SkillId = FName(TEXT("launcher_z"));
		Save->SkillLoadout.Add(Slot);
		Save->UnspentSP = 3;
		Save->DungeonClears = 1;
		return Save;
	};
	auto RunFrom = [](UProfileSaveGame* Save, FProfessionStateOut& Out,
		FProfileLoadError& OutError) -> bool
	{
		FProfileSnapshot ScratchSnapshot;
		FInventoryModel ScratchInventory;
		TArray<FPendingReward> ScratchPending;
		TSet<uint64> ScratchApplied;
		TMap<EItemSlot, FGuid> ScratchEquipped;
		return FProfileSerializer::FromSaveGame(Save, ScratchSnapshot, ScratchInventory,
			ScratchPending, ScratchApplied, ScratchEquipped, OutError,
			nullptr, nullptr, nullptr, nullptr, &Out);
	};

	{
		UProfileSaveGame* Save = MakeValidV4(nullptr);
		FProfessionStateOut Out;
		FProfileLoadError ValidError;
		TestTrue(TEXT("a valid v4 payload loads"), RunFrom(Save, Out, ValidError));
		TestEqual(TEXT("the profession state restores"), Out.ProfessionId, FName(TEXT("prototype")));
		TestEqual(TEXT("the rank restores"), Out.SkillRanks.Num(), 1);
		TestEqual(TEXT("unspent SP restores"), Out.UnspentSP, 3);
		TestEqual(TEXT("dungeon clears restore"), Out.DungeonClears, 1);
	}
	{
		UProfileSaveGame* Save = MakeValidV4(nullptr);
		// Copy before re-adding: an element reference into the same TArray
		// being modified trips the container-aliasing assert.
		const FSkillRankSaveEntry Duplicate = Save->SkillRanks[0];
		Save->SkillRanks.Add(Duplicate);
		FProfessionStateOut Out;
		FProfileLoadError Error;
		TestFalse(TEXT("a duplicate rank row is refused"), RunFrom(Save, Out, Error));
		TestEqual(TEXT("duplicate rank refused"), Error.Code, EProfileLoadError::DuplicateProfessionState);
	}
	{
		UProfileSaveGame* Save = MakeValidV4(nullptr);
		Save->SkillRanks[0].Rank = 9;
		FProfessionStateOut Out;
		FProfileLoadError Error;
		TestFalse(TEXT("a rank outside 1..8 is refused"), RunFrom(Save, Out, Error));
		TestEqual(TEXT("rank range refused"), Error.Code, EProfileLoadError::InvalidProfessionState);
	}
	{
		UProfileSaveGame* Save = MakeValidV4(nullptr);
		FSkillSlotSaveEntry Slot;
		Slot.SlotIndex = 3;
		Slot.SkillId = FName(TEXT("singlekick_w"));
		Save->SkillLoadout.Add(Slot);
		FProfessionStateOut Out;
		FProfileLoadError Error;
		TestFalse(TEXT("a dangling loadout skill is refused"), RunFrom(Save, Out, Error));
		TestEqual(TEXT("dangling loadout refused"), Error.Code, EProfileLoadError::DanglingLoadoutSkill);
	}
	{
		UProfileSaveGame* Save = MakeValidV4(nullptr);
		Save->UnspentSP = -1;
		FProfessionStateOut Out;
		FProfileLoadError Error;
		TestFalse(TEXT("a negative UnspentSP is refused"), RunFrom(Save, Out, Error));
		TestEqual(TEXT("negative SP refused"), Error.Code, EProfileLoadError::InvalidProfessionState);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002RosterCreateAndLoad,
	"UEMMO.Tasks.M7_002.RosterCreateAndLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002RosterCreateAndLoad::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	FMemoryRosterStorage Storage;
	URosterService* Service = NewRosterService(*this, Storage);
	if (!Service)
	{
		return false;
	}

	FRosterLoadOutcome Fresh = Service->LoadRoster();
	TestEqual(TEXT("a fresh state is NoRoster"), Fresh.Result, ERosterLoadResult::NoRoster);

	const FRosterCreateOutcome Created = Service->CreateCharacter(
		FName(TEXT("prototype")), TEXT("c1"), MakeInitialStateRequest());
	TestEqual(TEXT("the character is created"), Created.Result, ERosterCreateResult::Created);
	TestTrue(TEXT("the created identity is a valid guid"), Created.CharacterId.IsValid());
	TestTrue(TEXT("the derived prefix starts with Char_"), Created.SavePrefix.StartsWith(TEXT("Char_")));
	TestTrue(TEXT("the derived prefix is filename-safe"),
		!Created.SavePrefix.Contains(TEXT("-")) && !Created.SavePrefix.Contains(TEXT("{")));

	FRosterLoadOutcome Loaded = Service->LoadRoster();
	TestEqual(TEXT("the roster loads"), Loaded.Result, ERosterLoadResult::Recovered);
	TestEqual(TEXT("the roster carries one entry"), Loaded.Entries.Num(), 1);
	TestEqual(TEXT("the entry carries the profession"), Loaded.Entries[0].ProfessionId, FName(TEXT("prototype")));
	TestTrue(TEXT("the entry is active"), Loaded.Entries[0].bActive);
	TestEqual(TEXT("the active character id matches"), Loaded.ActiveCharacterId, Created.CharacterId);

	UProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
	TestTrue(TEXT("the character save service initializes on the derived prefix"),
		CharService->Initialize(Created.SavePrefix));
	CharService->SetStorage(&Storage);
	const FProfileLoadOutcome CharLoad = CharService->LoadActiveProfile();
	TestEqual(TEXT("the character's own save loads"), CharLoad.Result, EProfileLoadResult::Success);
	TestEqual(TEXT("the character save carries the identity"), CharLoad.Snapshot.CharacterId, Created.CharacterId);
	TestEqual(TEXT("the character save carries the profession"),
		CharLoad.ProfessionState.ProfessionId, FName(TEXT("prototype")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002TwoCharacterIsolation,
	"UEMMO.Tasks.M7_002.TwoCharacterIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002TwoCharacterIsolation::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	FMemoryRosterStorage Storage;
	URosterService* Service = NewRosterService(*this, Storage);
	if (!Service)
	{
		return false;
	}

	const FRosterCreateOutcome First = Service->CreateCharacter(
		FName(TEXT("prototype")), TEXT("one"), MakeInitialStateRequest());
	const FRosterCreateOutcome Second = Service->CreateCharacter(
		FName(TEXT("prototype")), TEXT("two"), MakeInitialStateRequest());
	TestEqual(TEXT("the first character is created"), First.Result, ERosterCreateResult::Created);
	TestEqual(TEXT("the second character is created"), Second.Result, ERosterCreateResult::Created);
	TestTrue(TEXT("the two identities differ"), First.CharacterId != Second.CharacterId);
	TestTrue(TEXT("the two prefixes differ"), First.SavePrefix != Second.SavePrefix);

	FRosterLoadOutcome Loaded = Service->LoadRoster();
	TestEqual(TEXT("the roster carries two entries"), Loaded.Entries.Num(), 2);
	int32 ActiveCount = 0;
	FString FirstPrefix;
	for (const FRosterEntry& Entry : Loaded.Entries)
	{
		ActiveCount += Entry.bActive ? 1 : 0;
		if (!Entry.bActive)
		{
			FirstPrefix = Entry.SavePrefix;
		}
	}
	TestEqual(TEXT("exactly one entry is active"), ActiveCount, 1);

	TestTrue(TEXT("the switch commits"), Service->SetActiveCharacter(First.CharacterId));
	Loaded = Service->LoadRoster();
	for (const FRosterEntry& Entry : Loaded.Entries)
	{
		TestEqual(FString::Printf(TEXT("entry %s active flag"), *Entry.SavePrefix),
			Entry.bActive, Entry.CharacterId == First.CharacterId);
	}

	UProfileSaveService* ServiceA = NewObject<UProfileSaveService>(GetTransientPackage());
	TestTrue(TEXT("service A initializes"), ServiceA->Initialize(First.SavePrefix));
	ServiceA->SetStorage(&Storage);
	const FProfileLoadOutcome LoadA = ServiceA->LoadActiveProfile();
	TestEqual(TEXT("the first character's save loads under its own prefix"),
		LoadA.Result, EProfileLoadResult::Success);
	UProfileSaveService* ServiceB = NewObject<UProfileSaveService>(GetTransientPackage());
	TestTrue(TEXT("service B initializes"), ServiceB->Initialize(Second.SavePrefix));
	ServiceB->SetStorage(&Storage);
	const FProfileLoadOutcome LoadB = ServiceB->LoadActiveProfile();
	TestEqual(TEXT("the second character's save loads under its own prefix"),
		LoadB.Result, EProfileLoadResult::Success);
	TestTrue(TEXT("the two characters' saves carry different identities"),
		LoadA.Snapshot.CharacterId != LoadB.Snapshot.CharacterId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002CorruptRosterGuard,
	"UEMMO.Tasks.M7_002.CorruptRosterGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002CorruptRosterGuard::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	FMemoryRosterStorage Storage;
	URosterService* Service = NewRosterService(*this, Storage);
	if (!Service)
	{
		return false;
	}

	TestTrue(TEXT("setup: the character is created"),
		Service->CreateCharacter(FName(TEXT("prototype")), TEXT("x"), MakeInitialStateRequest()).Result
			== ERosterCreateResult::Created);
	URosterIndexSaveGame* Index = Cast<URosterIndexSaveGame>(Storage.ReadSlot(TEXT("TestRoster_Index")));
	if (!Test.TestNotNull(TEXT("setup: the roster index exists"), Index))
	{
		return false;
	}
	const int32 ActiveSlot = Index->ActiveSlotIndex;
	const FString ActiveSlotName = (ActiveSlot == 0) ? TEXT("TestRoster_A") : TEXT("TestRoster_B");
	USaveGame** StoredRaw = Storage.Slots.Find(ActiveSlotName);

	URosterSlotSaveGame* Stored = StoredRaw ? Cast<URosterSlotSaveGame>(*StoredRaw) : nullptr;

	if (!Test.TestNotNull(TEXT("setup: the stored active roster slot exists"), Stored))

	{

		return false;

	}

	Stored->EntryDigest = 0xDEADBEEFu; // break the integrity digest

	URosterSlotSaveGame* Corrupted = Stored;
	if (!Test.TestNotNull(TEXT("setup: the active roster slot exists"), Corrupted))
	{
		return false;
	}
	Corrupted->EntryDigest = 0xDEADBEEFu;

	FRosterLoadOutcome Loaded = Service->LoadRoster();
	TestEqual(TEXT("the corrupt roster is an explicit recovery error"),
		Loaded.Result, ERosterLoadResult::RecoveryError);

	const FRosterCreateOutcome Created = Service->CreateCharacter(
		FName(TEXT("prototype")), TEXT("y"), MakeInitialStateRequest());
	TestFalse(TEXT("character creation refuses on a corrupt roster"),
		Created.Result == ERosterCreateResult::Created);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002LegacyImportPreservesValues,
	"UEMMO.Tasks.M7_002.LegacyImportPreservesValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002LegacyImportPreservesValues::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	FMemoryRosterStorage RosterStorage;
	URosterService* Service = NewRosterService(*this, RosterStorage);
	if (!Service)
	{
		return false;
	}
	FMemoryRosterStorage LegacyStorage;
	UProfileSaveService* LegacyService = NewObject<UProfileSaveService>(GetTransientPackage());
	if (!Test.TestTrue(TEXT("the legacy service initializes on Profile_"),
		LegacyService->Initialize(TEXT("Profile_"))))
	{
		return false;
	}
	LegacyService->SetStorage(&LegacyStorage);

	const FGuid LegacyCharacterId(0xF00Du, 0x1111u, 0x2222u, 0x3333u);
	UProfileSlotSaveGame* LegacySlot = NewObject<UProfileSlotSaveGame>(GetTransientPackage());
	LegacySlot->SchemaVersion = UProfileSaveGame::SchemaVersion3;
	LegacySlot->CharacterId = LegacyCharacterId;
	LegacySlot->Level = 4;
	LegacySlot->XP = 320;
	LegacySlot->InventoryInstanceSnapshots.Add(
		M7_002_MakeInstance(11, TEXT("weapon_training"), 2.0f, 3.0f, 1.0f, 1).ToStringSnapshot());
	LegacySlot->SlotGeneration = 1;
	LegacySlot->PayloadInstanceCount = LegacySlot->InventoryInstanceSnapshots.Num();
	LegacySlot->CharacterIdSummary = LegacySlot->CharacterId.ToString();
	// One cleared room travels through the import like every other v3 field.
	FRoomClearSaveEntry LegacyClear;
	LegacyClear.RoomId = FName(TEXT("room_training_01"));
	LegacyClear.ClearCount = 2;
	LegacyClear.LastClearUnixSeconds = 1700000000;
	LegacySlot->RoomClears.Add(LegacyClear);
	// The service refuses a slot whose stored integrity digest does not match
	// its payload, so the planted legacy slot must carry the frozen canonical
	// digest exactly like a slot written by the old build.
	LegacySlot->PayloadDigest = M7_002_ComputeCanonicalDigest(*LegacySlot);
	TestTrue(TEXT("setup: the legacy slot was written"),
		LegacyStorage.WriteSlot(TEXT("Profile_A"), LegacySlot));
	UProfileIndexSaveGame* LegacyIndex = NewObject<UProfileIndexSaveGame>(GetTransientPackage());
	LegacyIndex->ActiveSlotIndex = 0;
	LegacyIndex->SlotGenerations = { 1, 0 };
	TestTrue(TEXT("setup: the legacy index was written"),
		LegacyStorage.WriteSlot(TEXT("Profile_Index"), LegacyIndex));

	UProfileSubsystem* Profile = nullptr;
	// UGameInstanceSubsystem requires a GameInstance-chain outer.
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	Profile = NewObject<UProfileSubsystem>(Owner);
	Owner->AddToRoot();
	Profile->AddToRoot();
	const FRosterImportOutcome Import = Service->ImportLegacyProfile(*LegacyService, Profile);
	// A refused import leaves the roster empty; guard so the downstream
	// indexing can never run against a failed import.
	if (!Test.TestTrue(TEXT("the legacy import succeeds"),
		Import.Result == ERosterImportResult::Imported))
	{
		AddError(FString::Printf(TEXT("the legacy import was refused: %s"), *Import.Message));
		Profile->RemoveFromRoot();
		return false;
	}
	TestEqual(TEXT("the import PRESERVES the legacy identity"), Import.CharacterId, LegacyCharacterId);
	TestTrue(TEXT("the import derives the new prefix"), Import.SavePrefix.StartsWith(TEXT("Char_")));

	const UProfileSlotSaveGame* LegacyAfter = Cast<UProfileSlotSaveGame>(LegacyStorage.ReadSlot(TEXT("Profile_A")));
	if (Test.TestNotNull(TEXT("the legacy slot still exists"), LegacyAfter))
	{
		TestEqual(TEXT("the legacy slot kept its identity"), LegacyAfter->CharacterId, LegacyCharacterId);
		TestEqual(TEXT("the legacy slot kept its generation"), LegacyAfter->SlotGeneration, 1);
	}

	UProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
	TestTrue(TEXT("the imported character's service initializes"), CharService->Initialize(Import.SavePrefix));
	CharService->SetStorage(&RosterStorage);
	const FProfileLoadOutcome CharLoad = CharService->LoadActiveProfile();
	TestEqual(TEXT("the imported character save loads"), CharLoad.Result, EProfileLoadResult::Success);
	TestEqual(TEXT("the imported level is preserved"), CharLoad.Snapshot.Level, 4);
	TestEqual(TEXT("the imported xp is preserved"), CharLoad.Snapshot.XP, 320);
	TestEqual(TEXT("the imported inventory is preserved"), CharLoad.Inventory.Count(), 1);
	TestEqual(TEXT("the imported room clears are preserved"), CharLoad.RoomClears.Num(), 1);
	TestEqual(TEXT("the imported profession is prototype"), CharLoad.ProfessionState.ProfessionId, FName(TEXT("prototype")));
	TestEqual(TEXT("the imported legacy attacks are learned"), CharLoad.ProfessionState.SkillRanks.Num(), 4);

	const FRosterLoadOutcome Roster = Service->LoadRoster();
	TestEqual(TEXT("the roster carries the imported character"), Roster.Entries.Num(), 1);
	TestEqual(TEXT("the roster entry carries the preserved identity"),
		Roster.Entries[0].CharacterId, LegacyCharacterId);
	TestTrue(TEXT("the imported character is active"), Roster.Entries[0].bActive);
	Profile->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002FreshInstallBootstrapsRoster,
	"UEMMO.Tasks.M7_002.FreshInstallBootstrapsRoster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002FreshInstallBootstrapsRoster::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	FMemoryRosterStorage RosterStorage;
	URosterService* Service = NewRosterService(*this, RosterStorage);
	if (!Service)
	{
		return false;
	}
	FMemoryRosterStorage LegacyStorage;
	UProfileSaveService* LegacyService = NewObject<UProfileSaveService>(GetTransientPackage());
	TestTrue(TEXT("the legacy service initializes on Profile_"), LegacyService->Initialize(TEXT("Profile_")));
	LegacyService->SetStorage(&LegacyStorage);

	UProfileSubsystem* Profile = nullptr;
	// UGameInstanceSubsystem requires a GameInstance-chain outer.
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	Profile = NewObject<UProfileSubsystem>(Owner);
	Owner->AddToRoot();
	Profile->AddToRoot();
	const FRosterImportOutcome Import = Service->ImportLegacyProfile(*LegacyService, Profile);
	TestEqual(TEXT("the fresh install bootstraps the roster"),
		Import.Result, ERosterImportResult::FreshInstallCreated);
	TestTrue(TEXT("the fresh-install character has a valid identity"), Import.CharacterId.IsValid());

	const FRosterLoadOutcome Roster = Service->LoadRoster();
	TestEqual(TEXT("the roster carries the fresh character"), Roster.Entries.Num(), 1);
	TestEqual(TEXT("the fresh character's profession is prototype"),
		Roster.Entries[0].ProfessionId, FName(TEXT("prototype")));
	TestTrue(TEXT("the fresh character is active"), Roster.Entries[0].bActive);
	Profile->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002RealStorageRoundTrip,
	"UEMMO.Tasks.M7_002.RealStorageRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002RealStorageRoundTrip::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	// The REAL UGameplayStatics storage path (the M3-014 smoke test precedent):
	// create a character through the roster, then re-initialize the service on
	// the same prefix (the restart simulation) and verify everything survives.
	FGameplayStaticsSaveStorage RealStorage;
	const TCHAR* Prefix = TEXT("M7_002RoundTrip_");
	URosterService* Service = NewObject<URosterService>(GetTransientPackage());
	TestTrue(TEXT("the roster service initializes"), Service->Initialize(Prefix));
	Service->SetStorage(&RealStorage);

	const FRosterCreateOutcome Created = Service->CreateCharacter(
		FName(TEXT("prototype")), TEXT("roundtrip"), MakeInitialStateRequest());
	if (!Test.TestEqual(TEXT("the character is created through the real storage"),
		Created.Result, ERosterCreateResult::Created))
	{
		Service->DeleteTestSlots();
		return false;
	}

	URosterService* Restarted = NewObject<URosterService>(GetTransientPackage());
	TestTrue(TEXT("the restarted roster service initializes"), Restarted->Initialize(Prefix));
	Restarted->SetStorage(&RealStorage);
	const FRosterLoadOutcome Loaded = Restarted->LoadRoster();
	TestEqual(TEXT("the roster survives the restart simulation"),
		Loaded.Result, ERosterLoadResult::Recovered);
	TestEqual(TEXT("the roster survives with one entry"), Loaded.Entries.Num(), 1);
	TestEqual(TEXT("the roster survives with the same identity"),
		Loaded.ActiveCharacterId, Created.CharacterId);

	UProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
	TestTrue(TEXT("the character service initializes after the restart"),
		CharService->Initialize(Created.SavePrefix));
	CharService->SetStorage(&RealStorage);
	const FProfileLoadOutcome CharLoad = CharService->LoadActiveProfile();
	TestEqual(TEXT("the character save survives the restart simulation"),
		CharLoad.Result, EProfileLoadResult::Success);
	TestEqual(TEXT("the character save carries the profession after the restart"),
		CharLoad.ProfessionState.ProfessionId, FName(TEXT("prototype")));

	Service->DeleteTestSlots();
	Restarted->DeleteTestSlots();
	return true;
}

// M7-002 packaged-followup: the REAL user save files (the old package's v3
// trio) migrate through the real file storage. This is the exact path the
// packaged startup takes and the synthetic fixtures cannot reach: the .sav
// bytes were written by the OLD build.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_002RealUserSaveMigration,
	"UEMMO.Tasks.M7_002.RealUserSaveMigration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_002RealUserSaveMigration::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	using namespace UE::UEMMO::Tasks::M7_002;
	const FString BackupDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Artifacts/Tasks/M7-Package/user-saves-backup"));
	const FString SaveDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SaveGames"));
	IFileManager::Get().MakeDirectory(*SaveDir, true);
	bool bAnyCopied = false;
	for (const TCHAR* Name : { TEXT("Profile_A.sav"), TEXT("Profile_B.sav"), TEXT("Profile_Index.sav") })
	{
		const FString From = FPaths::Combine(BackupDir, Name);
		if (IFileManager::Get().FileExists(*From))
		{
			Test.TestTrue(FString::Printf(TEXT("fixture: %s copied"), Name),
				IFileManager::Get().Copy(*FPaths::Combine(SaveDir, Name), *From, 1) >= 0);
			bAnyCopied = true;
		}
	}
	if (!bAnyCopied)
	{
		AddError(TEXT("no real user save files found; the packaged-followup test cannot run here"));
		return true;
	}

	// Raw-slot forensics: what did the OLD build actually stamp?
	if (UGameplayStatics::DoesSaveGameExist(TEXT("Profile_A"), 0))
	{
		if (USaveGame* Raw = UGameplayStatics::LoadGameFromSlot(TEXT("Profile_A"), 0))
		{
			if (UProfileSlotSaveGame* Slot = Cast<UProfileSlotSaveGame>(Raw))
			{
				// UProfileSlotSaveGame derives from UProfileSaveGame: the
				// digest reads the base payload face directly. The canonical
				// string dumps too - the field that diverges from the write
				// path shows up line by line.
				FString Canonical;
				Canonical += FString::Printf(TEXT("schema=%d|id=%s|level=%d|xp=%d|volume=%s|"),
					Slot->SchemaVersion, *Slot->CharacterId.ToString(), Slot->Level, Slot->XP,
					*FString::SanitizeFloat(Slot->MasterVolume));
				for (const FString& Snapshot : Slot->InventoryInstanceSnapshots)
				{
					Canonical += FString::Printf(TEXT("item=%s|"), *Snapshot);
				}
				for (const FEquippedSlotSavePair& Pair : Slot->EquippedIds)
				{
					Canonical += FString::Printf(TEXT("equip=%u:%s|"), Pair.Slot, *Pair.InstanceId.ToString());
				}
				for (const FPendingRewardSaveEntry& Entry : Slot->PendingRewards)
				{
					Canonical += FString::Printf(TEXT("draft=%llu:%d|"), Entry.SettlementId, Entry.XP);
					for (const FString& Item : Entry.ItemSnapshots)
					{
						Canonical += FString::Printf(TEXT("draftitem=%s|"), *Item);
					}
				}
				for (const uint64 AppliedId : Slot->AppliedSettlementIds)
				{
					Canonical += FString::Printf(TEXT("applied=%llu|"), AppliedId);
				}
				for (const FWeaponMagazineSaveEntry& Magazine : Slot->WeaponMagazines)
				{
					Canonical += FString::Printf(TEXT("mag=%s:%s:%d:%d|"),
						*Magazine.InstanceId.ToString(), *Magazine.AmmoId.ToString(),
						Magazine.Capacity, Magazine.LoadedRounds);
				}
				for (const FAmmoReserveSaveEntry& Pool : Slot->ReserveAmmoPools)
				{
					Canonical += FString::Printf(TEXT("pool=%s:%d:%d|"),
						*Pool.AmmoId.ToString(), Pool.MaxReserve, Pool.Rounds);
				}
				if (Slot->bWeaponStateInitialized)
				{
					Canonical += TEXT("winit=1|");
				}
				if (!Slot->ConfigRevision.IsEmpty())
				{
					Canonical += FString::Printf(TEXT("rev=%s|"), *Slot->ConfigRevision);
				}
				for (const FRoomClearSaveEntry& Clear : Slot->RoomClears)
				{
					Canonical += FString::Printf(TEXT("room=%s:%d:%lld|"),
						*Clear.RoomId.ToString(), Clear.ClearCount, Clear.LastClearUnixSeconds);
				}
				Canonical += FString::Printf(TEXT("prof=%s|sp=%d|dc=%d|prev=%s|"),
					*Slot->ProfessionId.ToString(), Slot->UnspentSP, Slot->DungeonClears,
					*Slot->ProfessionConfigRevision);
				for (const FSkillRankSaveEntry& Rank : Slot->SkillRanks)
				{
					Canonical += FString::Printf(TEXT("rank=%s:%d|"), *Rank.SkillId.ToString(), Rank.Rank);
				}
				for (const FSkillSlotSaveEntry& SlotEntry : Slot->SkillLoadout)
				{
					Canonical += FString::Printf(TEXT("slot=%d:%s|"), SlotEntry.SlotIndex, *SlotEntry.SkillId.ToString());
				}
				AddInfo(FString::Printf(TEXT("RAW Profile_A: schema=%d storedDigest=%08X recomputed=%08X production=%08X inst=%d/%d idOk=%d"),
					Slot->SchemaVersion, Slot->PayloadDigest,
					M7_002_ComputeCanonicalDigest(*Slot),
					UProfileSaveService::M3_014_ComputePayloadDigest(*Slot),
					Slot->PayloadInstanceCount, Slot->InventoryInstanceSnapshots.Num(),
					static_cast<int32>(Slot->CharacterIdSummary == Slot->CharacterId.ToString())));
				AddInfo(FString::Printf(TEXT("RAW canonical: %s"), *Canonical));
			}
		}
	}

	UProfileSaveService* LegacyService = NewObject<UProfileSaveService>(GetTransientPackage());
	if (!Test.TestTrue(TEXT("the legacy service initializes on the real files"),
		LegacyService->Initialize(TEXT("Profile_"))))
	{
		return true;
	}
	const FProfileLoadOutcome Legacy = LegacyService->LoadActiveProfile();
	// THE evidence line: the exact result and problem the packaged startup hit.
	AddInfo(FString::Printf(TEXT("REAL-FILE legacy load: result=%d message=%s"),
		static_cast<int32>(Legacy.Result), *Legacy.Message));
	Test.TestTrue(TEXT("the real v3 save loads"),
		Legacy.Result == EProfileLoadResult::Success || Legacy.Result == EProfileLoadResult::SuccessFallback);
	Test.TestTrue(TEXT("the real legacy identity is valid"), Legacy.Snapshot.CharacterId.IsValid());

	// The import over the REAL legacy service (the roster slots ride the
	// test prefix; the legacy "Profile_" prefix above is the real one).
	UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
	Owner->AddToRoot();
	UProfileSubsystem* Profile = NewObject<UProfileSubsystem>(Owner);
	Profile->AddToRoot();
	URosterService* Roster = NewObject<URosterService>(GetTransientPackage());
	Test.TestTrue(TEXT("the roster service initializes"), Roster->Initialize(TEXT("TestRoster_RealMig_")));
	// Hermetic: earlier runs of THIS test persisted a roster under the test
	// prefix; the import's healthy-roster guard would refuse. Clear first.
	Roster->DeleteTestSlots();
	const FRosterImportOutcome Import = Roster->ImportLegacyProfile(*LegacyService, Profile);
	AddInfo(FString::Printf(TEXT("REAL-FILE import: result=%d message=%s"),
		static_cast<int32>(Import.Result), *Import.Message));
	Test.TestTrue(TEXT("the real legacy save imports"),
		Import.Result == ERosterImportResult::Imported);
	Profile->RemoveFromRoot();
	Owner->RemoveFromRoot();
	// Hermetic cleanup: the copied REAL saves must not leak into the shared
	// project Saved dir (a later GameInstance's startup would restore them
	// and skew every following test's save-state assumptions).
	for (const TCHAR* Name : { TEXT("Profile_A.sav"), TEXT("Profile_B.sav"), TEXT("Profile_Index.sav") })
	{
		IFileManager::Get().Delete(*FPaths::Combine(SaveDir, Name));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
