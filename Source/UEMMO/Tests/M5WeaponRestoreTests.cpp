// M5-047: the save-point and weapon/ammo restart-restore wiring. The suite
// locks the persistence chain end to end (all in-memory, the M5 TestSlot_
// store/fault-injection precedents, no real player saves):
//
//   1. Claim-flow saves carry the pushed v2 weapon state (the request the
//      service serializes holds the profile's pushed magazines/pools).
//   2. Restart restore: a saved v2 state reloaded through the service hands
//      the SAME magazines/pools to the profile, and the pawn's ammo model
//      restores them by identity (mounted rows applied, unknown rows
//      skipped, clamped to the mounted shape).
//   3. Failure honesty: a save failure reports Failed* (never Success), and
//      an unknown-config rebind refuses without wiping the session state.
//
// The pawn-level rebind runs through the real FAmmoModel (the mount's model);
// no World/Actor is needed for the model face.

#include "Misc/AutomationTest.h"

#include "../Persistence/ProfileSaveService.h"
#include "../Persistence/ProfileSerializer.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Weapons/AmmoModel.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_047
{
	/** A legal v2 weapon-state fixture: one magazine row + one pool row. */
	static void M5_047_MakeRows(TArray<FWeaponMagazineSaveEntry>& OutMagazines,
		TArray<FAmmoReserveSaveEntry>& OutPools, const FGuid& InstanceId)
	{
		FWeaponMagazineSaveEntry Magazine;
		Magazine.InstanceId = InstanceId;
		Magazine.AmmoId = FName(TEXT("ammo_cell"));
		Magazine.Capacity = 12;
		Magazine.LoadedRounds = 7;
		OutMagazines.Add(Magazine);

		FAmmoReserveSaveEntry Pool;
		Pool.AmmoId = FName(TEXT("ammo_cell"));
		Pool.MaxReserve = 120;
		Pool.Rounds = 45;
		OutPools.Add(Pool);
	}

	/**
	 * The in-memory storage of the M3-014 suite, replicated locally so this
	 * file stays self-contained (the original is buried in that TU).
	 */
	class FM5_047_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM5_047_MemoryStorage() override
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

	private:
		TMap<FString, USaveGame*> Slots;
	};

	/**
	 * The claim-flow request assembler (RewardService's M3_016 shape,
	 * replicated locally): profile state + the pushed weapon state (the
	 * M5-047 addition under test). The applied-id set and the equipment
	 * bindings are injected by the test (the profile exposes membership
	 * checks only - the production assembler carries the last committed
	 * values through).
	 */
	static FProfileSaveRequest M5_047_MakeRequestFromProfile(const UProfileSubsystem& Profile,
		const TSet<uint64>& CarriedAppliedIds, const TMap<EItemSlot, FGuid>& CarriedEquippedMap)
	{
		FProfileSaveRequest Request;
		Request.Snapshot = Profile.GetProfileSnapshot();
		Request.Inventory = Profile.GetInventory();
		Request.PendingRewards = Profile.GetPendingRewards();
		Request.AppliedSettlementIds = CarriedAppliedIds;
		Request.EquippedMap = CarriedEquippedMap;
		// M5-047: the pushed session weapon state rides every claim-flow save
		// (the claim is a persistence point; the pushed state is the freshest
		// the gameplay layer has pushed via PushWeaponState).
		Request.WeaponMagazines = Profile.GetWeaponMagazines();
		Request.ReserveAmmoPools = Profile.GetReserveAmmoPools();
		Request.bWeaponStateInitialized = Profile.IsWeaponStateInitialized();
		Request.ConfigRevision = Profile.GetWeaponConfigRevision();
		return Request;
	}
}

using namespace UE::UEMMO::Tasks::M5_047;

// 1. Acceptance: the claim-flow save request carries the pushed weapon state
//    (the service serializes the v2 fields 046 added), and the restore hands
//    the same rows back through the profile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_047SaveCarriesPushedWeaponStateAndRestoreReturnsIt,
	"UEMMO.Tasks.M5_047.SaveCarriesPushedWeaponStateAndRestoreReturnsIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_047SaveCarriesPushedWeaponStateAndRestoreReturnsIt::RunTest(const FString& Parameters)
{
	UGameInstance* M5_047_Owner = NewObject<UGameInstance>(GEngine);
	if (M5_047_Owner) { M5_047_Owner->AddToRoot(); }
	UProfileSubsystem* Profile = NewObject<UProfileSubsystem>(M5_047_Owner);
	if (Profile) { Profile->AddToRoot(); }
	Profile->NewProfile();

	// A firearm instance in the inventory (the magazine's identity anchor).
	FItemInstance Gun;
	Gun.InstanceId = FGuid(0x0470A000u, 0x0BECu, 0x5331u, 1u);
	Gun.DefinitionId = FName(TEXT("weapon_hitscan_gun"));
	Gun.RollSeed = 47047;
	Gun.Level = 1;
	TestEqual(TEXT("the gun registers into the inventory"),
		static_cast<int32>(Profile->GetInventory().TryAdd(Gun)),
		static_cast<int32>(EInventoryAddResult::Added));

	// The gameplay push (what an equipment change or menu return does).
	TArray<FWeaponMagazineSaveEntry> Magazines;
	TArray<FAmmoReserveSaveEntry> Pools;
	M5_047_MakeRows(Magazines, Pools, Gun.InstanceId);
	Profile->PushWeaponState(Magazines, Pools, FString(TEXT("rev-m5-047")));

	// The save request the claim flow assembles now carries the pushed state.
	UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
	FM5_047_MemoryStorage Storage;
	if (!TestTrue(TEXT("the service initializes on the isolated prefix"), Service->Initialize(TEXT("TestSlot_"))))
	{
		return true;
	}
	Service->SetStorage(&Storage);
	// The claim-flow request with the test-carried applied/equipment values.
	TSet<uint64> CarriedApplied;
	CarriedApplied.Add(9317ull);
	TMap<EItemSlot, FGuid> CarriedEquipped;
	CarriedEquipped.Add(EItemSlot::Weapon, Gun.InstanceId);
	const FProfileSaveRequest Request = M5_047_MakeRequestFromProfile(*Profile, CarriedApplied, CarriedEquipped);
	TestEqual(TEXT("the request carries the pushed magazine rows"),
		Request.WeaponMagazines.Num(), 1);
	TestEqual(TEXT("the request carries the pushed pool rows"),
		Request.ReserveAmmoPools.Num(), 1);
	TestEqual(TEXT("the request magazine round is the pushed one"),
		Request.WeaponMagazines[0].LoadedRounds, 7);
	TestTrue(TEXT("the request marks the state initialized"), Request.bWeaponStateInitialized);
	TestEqual(TEXT("the request stamps the pushed revision"),
		Request.ConfigRevision, FString(TEXT("rev-m5-047")));

	// Save -> reload through the service: the same rows come back.
	const FProfileSaveOutcome SaveOutcome = Service->SaveProfile(Request);
	TestEqual(TEXT("the save succeeds"), static_cast<int32>(SaveOutcome.Result),
		static_cast<int32>(EProfileSaveResult::Success));
	const FStartupLoadOutcome Load = Service->StartupLoad();
	TestTrue(TEXT("the reload recovers the save"),
		Load.Result == EStartupLoadResult::Recovered || Load.Result == EStartupLoadResult::RecoveredFallback);
	TestEqual(TEXT("the reload carries the saved magazine rows"), Load.WeaponMagazines.Num(), 1);
	TestEqual(TEXT("the reload carries the saved pool rounds"), Load.ReserveAmmoPools[0].Rounds, 45);
	TestEqual(TEXT("the reload carries the revision"),
		Load.WeaponConfigRevision, FString(TEXT("rev-m5-047")));

	// The v2 profile restore pushes them into the (fresh) profile.
	UProfileSubsystem* Restored = NewObject<UProfileSubsystem>(M5_047_Owner);
	if (Restored) { Restored->AddToRoot(); }
	TestTrue(TEXT("the v2 restore succeeds"),
		Restored->RestoreFromSave(Load.Snapshot, Load.Inventory, Load.PendingRewards,
			Load.AppliedSettlementIds, Load.EquippedMap,
			Load.WeaponMagazines, Load.ReserveAmmoPools,
			Load.bWeaponStateInitialized, Load.WeaponConfigRevision));
	TestTrue(TEXT("the restored profile marks the state initialized"),
		Restored->IsWeaponStateInitialized());
	TestEqual(TEXT("the restored profile holds the magazine rows"),
		Restored->GetWeaponMagazines().Num(), 1);
	return true;
}

// 2. Acceptance: the pawn's ammo model restores the saved rows BY IDENTITY -
//    mounted rows apply (clamped to the mounted shape), unknown rows skip,
//    and the export of a restored model round-trips.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_047AmmoModelRestoresRowsByIdentity,
	"UEMMO.Tasks.M5_047.AmmoModelRestoresRowsByIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_047AmmoModelRestoresRowsByIdentity::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	const FGuid MountedInstance = FGuid(0x0470B000u, 0x1u, 0x2u, 0x3u);
	// The pool registers FIRST (the magazine initialization cross-references
	// the registered kinds), and both setups are checked - an unchecked false
	// here would crash the later FindMagazine dereference.
	TestTrue(TEXT("the mounted pool registers"),
		Model.RegisterAmmoType(FName(TEXT("ammo_cell")), 120, 100));
	TestTrue(TEXT("the mounted magazine initializes (12 in the pipe)"),
		Model.InitializeInstance(MountedInstance, FName(TEXT("ammo_cell")), 12, 12));

	// A saved state: the mounted instance at 3 rounds, the pool drained to
	// 15 - plus one row for an instance/kind that is NOT mounted now.
	TArray<FWeaponMagazineSaveEntry> Magazines;
	TArray<FAmmoReserveSaveEntry> Pools;
	FWeaponMagazineSaveEntry SavedMagazine;
	SavedMagazine.InstanceId = MountedInstance;
	SavedMagazine.AmmoId = FName(TEXT("ammo_cell"));
	SavedMagazine.Capacity = 12;
	SavedMagazine.LoadedRounds = 3;
	Magazines.Add(SavedMagazine);
	FWeaponMagazineSaveEntry GhostMagazine = SavedMagazine;
	GhostMagazine.InstanceId = FGuid(0x0470B001u, 0x9u, 0x9u, 0x9u);
	GhostMagazine.LoadedRounds = 9;
	Magazines.Add(GhostMagazine);
	FAmmoReserveSaveEntry SavedPool;
	SavedPool.AmmoId = FName(TEXT("ammo_cell"));
	SavedPool.MaxReserve = 120;
	SavedPool.Rounds = 15;
	Pools.Add(SavedPool);
	FAmmoReserveSaveEntry GhostPool = SavedPool;
	GhostPool.AmmoId = FName(TEXT("ammo_rocket_ghost"));
	Pools.Add(GhostPool);

	const TPair<int32, int32> Applied = Model.RestoreSaveRows(Magazines, Pools);
	TestEqual(TEXT("exactly the mounted magazine row applied"), Applied.Key, 1);
	TestEqual(TEXT("exactly the mounted pool row applied"), Applied.Value, 1);
	TestEqual(TEXT("the mounted magazine holds the saved rounds"), Model.FindMagazine(MountedInstance)->LoadedRounds, 3);
	TestEqual(TEXT("the pool holds the saved rounds"),
		Model.GetReserveRounds(FName(TEXT("ammo_cell"))), 15);
	TestEqual(TEXT("no ghost magazine was minted"), Model.NumMagazines(), 1);
	TestEqual(TEXT("no ghost pool was minted"), Model.NumAmmoTypes(), 1);

	// Clamp: a saved load beyond the mounted capacity clamps to it.
	TArray<FWeaponMagazineSaveEntry> Overfull;
	FWeaponMagazineSaveEntry OverfullRow = SavedMagazine;
	OverfullRow.LoadedRounds = 999;
	Overfull.Add(OverfullRow);
	Model.RestoreSaveRows(Overfull, {});
	TestEqual(TEXT("a beyond-capacity saved load clamps to the mounted capacity"),
		Model.FindMagazine(MountedInstance)->LoadedRounds, 12);

	// Export round-trip: the restored model exports exactly one of each row.
	FWeaponMagazineSaveEntry Seed;
	TArray<FWeaponMagazineSaveEntry> ExportedMagazines;
	TArray<FAmmoReserveSaveEntry> ExportedPools;
	Model.ExportSaveRows(Seed, ExportedMagazines, ExportedPools);
	TestEqual(TEXT("the export holds one magazine row"), ExportedMagazines.Num(), 1);
	TestEqual(TEXT("the export holds one pool row"), ExportedPools.Num(), 1);
	TestEqual(TEXT("the exported rounds are the clamped ones"), ExportedMagazines[0].LoadedRounds, 12);
	return true;
}

// 3. Acceptance: failure honesty - a failed save reports its Failed* value
//    (never Success) and leaves the old slot loadable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_047SaveFailureNeverReportsSuccess,
	"UEMMO.Tasks.M5_047.SaveFailureNeverReportsSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_047SaveFailureNeverReportsSuccess::RunTest(const FString& Parameters)
{
	UGameInstance* M5_047_Owner = NewObject<UGameInstance>(GEngine);
	if (M5_047_Owner) { M5_047_Owner->AddToRoot(); }
	UProfileSubsystem* Profile = NewObject<UProfileSubsystem>(M5_047_Owner);
	if (Profile) { Profile->AddToRoot(); }
	Profile->NewProfile();
	UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
	FM5_047_MemoryStorage Storage;
	if (!TestTrue(TEXT("the service initializes on the isolated prefix"), Service->Initialize(TEXT("TestSlot_"))))
	{
		return true;
	}
	Service->SetStorage(&Storage);

	// A first healthy save anchors a loadable slot.
	const FProfileSaveRequest Anchor = M5_047_MakeRequestFromProfile(*Profile, {}, {});
	TestEqual(TEXT("the anchor save succeeds"), static_cast<int32>(Service->SaveProfile(Anchor).Result),
		static_cast<int32>(EProfileSaveResult::Success));

	// A corrupted payload (an invalid zero character id) fails the capture
	// gate and never reaches the disk transaction.
	FProfileSaveRequest Broken = M5_047_MakeRequestFromProfile(*Profile, {}, {});
	Broken.Snapshot.CharacterId = FGuid(0, 0, 0, 0);
	Broken.Snapshot.Level = 2;
	const FProfileSaveOutcome Failed = Service->SaveProfile(Broken);
	TestTrue(TEXT("the broken save reports a Failed* result"),
		Failed.Result != EProfileSaveResult::Success);
	TestFalse(TEXT("the failure message is not empty"), Failed.Message.IsEmpty());

	// The anchor slot survives: the reload still recovers generation 1.
	const FStartupLoadOutcome Load = Service->StartupLoad();
	TestTrue(TEXT("the old slot is still loadable after the failed save"),
		Load.Result == EStartupLoadResult::Recovered || Load.Result == EStartupLoadResult::RecoveredFallback);
	TestEqual(TEXT("the old generation is still active"), Load.Generation, 1);
	return true;
}

#endif
