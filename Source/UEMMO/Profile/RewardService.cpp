// M3-008: settlement reward drafts with idempotent application. GREEN
// implementation of the stubbed contract: BeginReward turns one Cleared run
// result into an FPendingReward{SettlementId, XP=50, Items[1]} stored in the
// GameInstance-level profile, derives the RewardSeed as a fixed salted mix of
// the room Seed (never a wall clock), and answers a repeat settlement with
// the ORIGINAL draft (the random result is generated exactly once per
// settlement and a retry never re-rolls it).
// M3-009: TryClaimPending claims a pending draft into the inventory with
// full-inventory retention (nothing is lost; a retry claims the original
// instances) and grants the settlement XP exactly once.
// M3-016: ClaimPendingAtomic commits the claim durably - the pending draft
// is persisted FIRST ("StartRoom semantics"), the shared in-memory claim
// body applies the reward to the profile, and the post-claim snapshot save
// is the only confirmation gate; a failed save rolls the memory back to the
// committed state so a claim never halves, duplicates or re-rolls.

#include "RewardService.h"

#include "ProfileSubsystem.h"
#include "../Persistence/ProfileSaveService.h"

#include "../Items/DropGenerator.h"
#include "../Items/DropTable.h"

namespace
{
	// 64-bit finalizer (murmur3 fmix): deterministic, platform-independent,
	// and spreads single-bit input differences across all output bits. Same
	// construction as the drop generator's internal mixer, with a distinct
	// purpose salt so the reward-seed stream never shares raw values with the
	// drop-selection / roll-seed / instance-identity streams.
	uint64 RewardMix64(uint64 X)
	{
		X ^= X >> 33;
		X *= 0xFF51AFD7ED558CCDull;
		X ^= X >> 33;
		X *= 0xC4CEB9FE1A85EC53ull;
		X ^= X >> 33;
		return X;
	}

	// Purpose salt for RoomSeed -> RewardSeed. Fixed by design: the seed must
	// never depend on a wall clock, the global random stream or call order.
	constexpr uint64 RewardSaltRoomSeed = 0xA24BAED4963EE407ull;

	// M3-016 helpers (file-local, uniquely prefixed like every suite's
	// constants): draft location plus save-request assembly for the atomic
	// claim. No state, no engine services beyond the passed objects.

	/**
	 * The pending draft index of one settlement in the profile (INDEX_NONE
	 * when the profile holds no draft for it).
	 */
	int32 M3_016_FindDraftIndex(const UProfileSubsystem& Profile, uint64 SettlementId)
	{
		const TArray<FPendingReward>& Pending = Profile.GetPendingRewards();
		for (int32 Index = 0; Index < Pending.Num(); ++Index)
		{
			if (Pending[Index].SettlementId == SettlementId)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * Assembles one FProfileSaveRequest from the live profile state. The
	 * snapshot, the inventory and the pending drafts are read from the
	 * profile; the applied-id set and the equipment bindings TRAVEL from the
	 * last committed save: UProfileSubsystem exposes membership checks only
	 * (no enumeration), so the atomic claim - the persistence point of the
	 * claim flow - carries the previously persisted values forward unchanged
	 * and merges the one id it claims. Rule: every settlement marked applied
	 * is persisted by the claim that marked it, so the last committed save
	 * always holds the complete applied history of the claim flow.
	 */
	FProfileSaveRequest M3_016_MakeRequestFromProfile(const UProfileSubsystem& Profile,
		const TSet<uint64>& CarriedAppliedIds, const TMap<EItemSlot, FGuid>& CarriedEquippedMap)
	{
		FProfileSaveRequest Request;
		Request.Snapshot = Profile.GetProfileSnapshot();
		Request.Inventory = Profile.GetInventory();
		Request.PendingRewards = Profile.GetPendingRewards();
		Request.AppliedSettlementIds = CarriedAppliedIds;
		Request.EquippedMap = CarriedEquippedMap;
		return Request;
	}
}

void URewardService::BindProfile(UProfileSubsystem* Profile)
{
	ProfilePtr = Profile;
}

UProfileSubsystem* URewardService::GetBoundProfile() const
{
	return ProfilePtr.Get();
}

int64 URewardService::DeriveRewardSeed(int32 RoomSeed)
{
	// Fold the int32 room seed through the fixed-salt finalizer into an int64.
	// Pure function of RoomSeed: the same run always re-derives the same
	// RewardSeed, a different run (different Seed) derives a different one.
	const uint64 Mixed = RewardMix64(static_cast<uint64>(static_cast<uint32>(RoomSeed)) ^ RewardSaltRoomSeed);
	return static_cast<int64>(Mixed ^ (Mixed >> 32));
}

FRewardBeginOutcome URewardService::BeginReward(const FRoomResult& Result, const FItemDefinitionCatalog& Catalog)
{
	FRewardBeginOutcome Outcome;

	// 1. Reward eligibility at value level (URoomSessionSubsystem::
	//    IsRewardEligible semantics: only a Cleared run may ever enter the
	//    reward path). Checked first, so a Failed result is rejected no
	//    matter what earlier state exists.
	if (!Result.bCleared)
	{
		Outcome.Result = ERewardBeginResult::RejectedFailedResult;
		Outcome.Error = FString::Printf(TEXT("SettlementId %llu is not reward eligible: only a Cleared run result may begin a reward (a Failed result is always rejected)"),
			Result.SettlementId);
		return Outcome;
	}

	// 2. The draft lives in the GameInstance-level profile; without a bound
	//    subsystem or an existing profile there is nowhere to store it.
	UProfileSubsystem* Profile = ProfilePtr.Get();
	if (Profile == nullptr || !Profile->HasProfile())
	{
		Outcome.Result = ERewardBeginResult::RejectedNoProfile;
		Outcome.Error = TEXT("no bound profile subsystem or no existing profile: a reward draft needs a GameInstance-level profile to live in");
		return Outcome;
	}

	// 3. Terminal guard for the claim flow (M3-009 fills AppliedSettlementIds
	//    when a draft is claimed): an already-claimed settlement answers
	//    AlreadyApplied with no draft - nothing is re-rolled, nothing stored.
	if (Profile->IsSettlementApplied(Result.SettlementId))
	{
		Outcome.Result = ERewardBeginResult::AlreadyApplied;
		return Outcome;
	}

	// 4. Idempotent replay: this settlement already owns a pending draft, so
	//    return the ORIGINAL one unchanged (AlreadyApplied). The random
	//    result was generated exactly once when the draft was created; a
	//    retry never generates again, adds no item and changes no entry.
	const TArray<FPendingReward>& Pending = Profile->GetPendingRewards();
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		if (Pending[Index].SettlementId == Result.SettlementId)
		{
			Outcome.Result = ERewardBeginResult::AlreadyApplied;
			Outcome.Draft = Pending[Index];
			return Outcome;
		}
	}

	// 5. RewardSeed: fixed derivation from the run's room Seed (step 4's
	//    replay path above guarantees this code runs at most once per
	//    settlement, so the "same seed reused on retry" contract of the drop
	//    generator holds by construction).
	const int64 RewardSeed = DeriveRewardSeed(Result.Seed);

	// 6. Exactly one equipment piece from the pinned starter table (M3-007),
	//    identified by the settlement id. No random source other than the
	//    derived seed participates.
	const FDropTable Table = MakeStarterDropTable();
	const FDropRewardResult Drop = FDropGenerator::GenerateReward(RewardSeed, Result.SettlementId, Table, Catalog);
	if (!Drop.bSuccess)
	{
		// Broken table/catalog data: nothing is stored and the caller gets
		// the generator's field-naming error unchanged.
		Outcome.Result = ERewardBeginResult::RejectedGeneration;
		Outcome.Error = FString::Printf(TEXT("drop generation failed for SettlementId %llu: %s"),
			Result.SettlementId, *Drop.Error);
		return Outcome;
	}

	// 7. Build and store the draft: the design XP 50 per cleared room plus
	//    the single pre-generated instance. No inventory mutation here - the
	//    claim (including full-inventory retention) belongs to M3-009, and no
	//    disk persistence happens in this card (M3-013 owns the save layer).
	FPendingReward Draft;
	Draft.SettlementId = Result.SettlementId;
	Draft.XP = RewardXPPerClear;
	Draft.Items.Add(Drop.Instance);
	Profile->GetPendingRewards().Add(Draft);

	Outcome.Result = ERewardBeginResult::Applied;
	Outcome.Error.Reset();
	Outcome.Draft = Draft;
	return Outcome;
}

// -- Claim entry (M3-009) ------------------------------------------------------

// GREEN implementation of the M3-009 contract: the claim never loses a
// pending item (only a verifiably stored instance leaves its draft, every
// rejection is retained unchanged with its original InstanceId and one-time
// roll) and the XP is granted exactly once per settlement (the first claim
// attempt grants and records it; a full inventory does not hold XP back).
// M3-016: the memory semantics moved verbatim into the shared
// M3_016_ApplyClaimInMemory body so the atomic claim executes the exact same
// claim (same messages, same retention) - only the persistence differs.

FRewardClaimOutcome URewardService::TryClaimPending(uint64 SettlementId, FInventoryModel& Inventory)
{
	FRewardClaimOutcome Outcome;

	// 1. The draft and the applied record live in the GameInstance-level
	//    profile; without a bound subsystem or an existing profile there is
	//    nothing to claim from.
	UProfileSubsystem* Profile = ProfilePtr.Get();
	if (Profile == nullptr || !Profile->HasProfile())
	{
		Outcome.Result = ERewardClaimResult::RejectedNoProfile;
		Outcome.Error = TEXT("no bound profile subsystem or no existing profile: a pending reward needs a GameInstance-level profile to claim from");
		return Outcome;
	}

	// 2. The shared memory claim (no save here - M3-009 stays memory-only).
	return M3_016_ApplyClaimInMemory(SettlementId, *Profile, Inventory);
}

// The shared claim body: locates the draft in the profile, grants the XP
// exactly once (MarkSettlementApplied on the first attempt), stores the items
// under their original InstanceIds and retains every rejection unchanged.
FRewardClaimOutcome URewardService::M3_016_ApplyClaimInMemory(uint64 SettlementId, UProfileSubsystem& Profile, FInventoryModel& Inventory)
{
	FRewardClaimOutcome Outcome;

	// 1. Locate the settlement's draft in the profile's PendingRewards.
	TArray<FPendingReward>& Pending = Profile.GetPendingRewards();
	int32 DraftIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		if (Pending[Index].SettlementId == SettlementId)
		{
			DraftIndex = Index;
			break;
		}
	}

	if (DraftIndex == INDEX_NONE)
	{
		if (Profile.IsSettlementApplied(SettlementId))
		{
			// Fully claimed earlier (draft already removed, applied record
			// kept): an idempotent repeat - no XP again, no items again,
			// nothing changed.
			Outcome.Result = ERewardClaimResult::AlreadyClaimed;
			return Outcome;
		}
		Outcome.Result = ERewardClaimResult::UnknownSettlement;
		Outcome.Error = FString::Printf(TEXT("SettlementId %llu has no pending reward draft and no applied record: nothing to claim"), SettlementId);
		return Outcome;
	}

	// 2. XP exactly once per settlement: granted by the FIRST claim attempt
	//    (a full inventory does not hold XP back - XP is settlement progress,
	//    not inventory payload) and recorded in AppliedSettlementIds, so
	//    every later attempt (repeat click, free-space retry) skips it.
	FPendingReward& Draft = Pending[DraftIndex];
	if (!Profile.IsSettlementApplied(SettlementId))
	{
		Profile.AddXP(Draft.XP);
		Profile.MarkSettlementApplied(SettlementId);
		Outcome.bGrantedXP = true;
	}

	// 3. Items one by one, keyed by their pre-generated InstanceId: only a
	//    verifiably stored instance (TryAdd == Added) leaves the draft; every
	//    rejection retains the instance UNCHANGED (original InstanceId and
	//    rolled stats - the draft is the single source of the one-time roll,
	//    a retry never re-rolls or re-identifies). Retained items keep their
	//    draft order.
	TArray<FItemInstance> Retained;
	int32 ClaimedCount = 0;
	bool bRetainedNonFullRejection = false;
	for (int32 Index = 0; Index < Draft.Items.Num(); ++Index)
	{
		const FItemInstance& Item = Draft.Items[Index];
		const EInventoryAddResult AddResult = Inventory.TryAdd(Item);
		if (AddResult == EInventoryAddResult::Added)
		{
			++ClaimedCount;
		}
		else
		{
			Retained.Add(Item);
			bRetainedNonFullRejection |= (AddResult != EInventoryAddResult::InventoryFull);
		}
	}

	Outcome.ClaimedItemCount = ClaimedCount;
	Outcome.RetainedItemCount = Retained.Num();

	if (Retained.Num() == 0)
	{
		// 4a. Everything consumed (or the draft carried no items at all):
		//     remove the empty draft; the applied record STAYS
		//     (IsSettlementApplied remains true - a replayed BeginReward
		//     answers AlreadyApplied with a default draft from here on).
		Pending.RemoveAt(DraftIndex);
		Outcome.Result = ERewardClaimResult::Claimed;
		Outcome.Error.Reset();
	}
	else if (ClaimedCount > 0)
	{
		// 4b. Partial success (multi-item drafts): the stored items stay
		//     stored, only the retained ones keep waiting in the draft.
		Draft.Items = Retained;
		Outcome.Result = ERewardClaimResult::PartiallyClaimed;
		Outcome.Error.Reset();
	}
	else
	{
		// 4c. Nothing fit: the draft keeps every item unchanged and the
		//     caller surfaces InventoryFull. The retention itself is
		//     unconditional - the message names a non-full rejection when
		//     one occurred (duplicate/invalid identity retained for retry).
		Draft.Items = Retained;
		Outcome.Result = ERewardClaimResult::InventoryFull;
		Outcome.Error = bRetainedNonFullRejection
			? TEXT("no pending item could enter the inventory; every item is retained unchanged in the pending draft (at least one rejection was not InventoryFull)")
			: TEXT("inventory is full: every pending item is retained unchanged in the pending draft");
	}
	return Outcome;
}

// -- Atomic claim entry (M3-016) ---------------------------------------------------

// GREEN implementation of the atomic commit (interface contract section 8:
// the reward claim, AppliedSettlementIds, inventory and XP persist as ONE
// profile snapshot; only a committed save may confirm the claim). Ordered
// flow: persisted-state gates -> draft persistence ("StartRoom semantics")
// -> the shared in-memory claim -> claim-snapshot save (failure rolls the
// memory back to the committed state). Restart idempotency is decided from
// the persisted state alone: an interrupted claim-snapshot save leaves the
// draft save committed (the startup pass recovers it), so a re-claim grants
// the XP exactly once and stores exactly the original instances.

FRewardClaimAtomicOutcome URewardService::ClaimPendingAtomic(uint64 SettlementId, UProfileSubsystem* Profile, UProfileSaveService* SaveService)
{
	FRewardClaimAtomicOutcome Outcome;

	// 1. Explicit-parameter guards: the claim needs a GameInstance-level
	//    profile with existing state and an initialized save service (the
	//    same objects the caller restored from the last committed save).
	if (Profile == nullptr || !Profile->HasProfile())
	{
		Outcome.Result = ERewardClaimAtomicResult::RejectedNoProfile;
		Outcome.Error = TEXT("no profile subsystem or no existing profile: an atomic claim needs a GameInstance-level profile to claim from");
		return Outcome;
	}
	if (SaveService == nullptr || SaveService->GetSlotPrefix().IsEmpty())
	{
		Outcome.Result = ERewardClaimAtomicResult::RejectedNoSaveService;
		Outcome.Error = TEXT("no save service or no initialized slot prefix: an atomic claim must persist the profile snapshot");
		return Outcome;
	}

	// 2. Persisted-state gates: the in-memory profile is the restore of the
	//    last committed save, so these answers carry across restarts (a
	//    restart re-processing the same SettlementId never re-rolls or
	//    re-grants from call history).
	if (M3_016_FindDraftIndex(*Profile, SettlementId) == INDEX_NONE)
	{
		if (Profile->IsSettlementApplied(SettlementId))
		{
			// Fully claimed earlier (the applied record stays even after the
			// draft was consumed): idempotent repeat - no XP again, no item
			// again, nothing changed, nothing saved.
			Outcome.Result = ERewardClaimAtomicResult::AlreadyClaimed;
			Outcome.Error = TEXT("the settlement was already claimed (persisted applied record); nothing left to claim");
			return Outcome;
		}
		Outcome.Result = ERewardClaimAtomicResult::UnknownSettlement;
		Outcome.Error = FString::Printf(TEXT("SettlementId %llu has no pending reward draft and no applied record: nothing to claim"), SettlementId);
		return Outcome;
	}

	// 3. The applied-id set and the equipment bindings of every request
	//    travel from the LAST COMMITTED save (step 4's rule). Data exists
	//    but is unreadable (all slots corrupt): refuse instead of saving on
	//    top of the evidence.
	const FProfileLoadOutcome LastCommitted = SaveService->LoadActiveProfile();
	if (LastCommitted.Result == EProfileLoadResult::FailedCorrupt)
	{
		Outcome.Result = ERewardClaimAtomicResult::RejectedUnreadableSave;
		Outcome.Error = FString::Printf(TEXT("the last committed profile save is unreadable (%s); the atomic claim refuses to save on top of it"),
			*LastCommitted.Message);
		return Outcome;
	}

	// 4. SAVE A - persist the pending draft FIRST ("StartRoom semantics":
	//    the profile save carries PendingRewards). After this commit the
	//    pre-generated item identity is durable; any interruption during the
	//    rest of the request restarts from a state that still owns the
	//    original draft. Nothing was mutated yet, so a failure here simply
	//    leaves the pending draft as it was.
	const FProfileSaveRequest DraftRequest = M3_016_MakeRequestFromProfile(*Profile,
		LastCommitted.AppliedSettlementIds, LastCommitted.EquippedMap);
	const FProfileSaveOutcome DraftSave = SaveService->SaveProfile(DraftRequest);
	if (DraftSave.Result != EProfileSaveResult::Success)
	{
		Outcome.Result = ERewardClaimAtomicResult::SaveFailed;
		Outcome.Error = FString::Printf(TEXT("the pending-draft save failed before any claim mutation (%s); the draft stays pending unchanged"),
			*DraftSave.Message);
		return Outcome;
	}

	// 5. In-memory claim from the durable draft - the exact M3-009 body
	//    (XP exactly once, items under their original InstanceIds, rejections
	//    retained). The equipped bonus row is captured because the rollback
	//    below re-pushes it (RestoreFromSave resets the row like every
	//    restore; the gameplay layer would re-derive it).
	const FItemStats BonusBeforeClaim = Profile->GetEquippedStatBonus();
	const FRewardClaimOutcome Claim = M3_016_ApplyClaimInMemory(SettlementId, *Profile, Profile->GetInventory());
	Outcome.ClaimedItemCount = Claim.ClaimedItemCount;
	Outcome.RetainedItemCount = Claim.RetainedItemCount;
	Outcome.bGrantedXP = Claim.bGrantedXP;

	// 6. SAVE C - persist the POST-claim profile state as ONE snapshot: the
	//    XP, the inventory, the remaining pending drafts and the applied ids
	//    (the carried set merged with this settlement's record). The A/B
	//    transaction commits only after the verified index commit, so this
	//    snapshot is all-or-nothing on disk.
	FProfileSaveRequest ClaimRequest = M3_016_MakeRequestFromProfile(*Profile,
		LastCommitted.AppliedSettlementIds, LastCommitted.EquippedMap);
	if (Profile->IsSettlementApplied(SettlementId))
	{
		ClaimRequest.AppliedSettlementIds.Add(SettlementId);
	}
	const FProfileSaveOutcome ClaimSave = SaveService->SaveProfile(ClaimRequest);
	if (ClaimSave.Result != EProfileSaveResult::Success)
	{
		// Roll the in-memory commit back to the exact state SAVE A
		// committed: memory and disk agree again, the draft is pending
		// unchanged (original InstanceId - no re-roll), and the returned
		// failure keeps the UI from ever showing claim success. The
		// counters describe the NET effect of the call: nothing was stored,
		// the rolled-back draft retains its original items again, no XP
		// stayed granted.
		Profile->RestoreFromSave(DraftRequest.Snapshot, DraftRequest.Inventory, DraftRequest.PendingRewards,
			DraftRequest.AppliedSettlementIds, DraftRequest.EquippedMap);
		Profile->SetEquippedStatBonus(BonusBeforeClaim);
		const int32 RolledBackIndex = M3_016_FindDraftIndex(*Profile, SettlementId);
		Outcome.ClaimedItemCount = 0;
		Outcome.RetainedItemCount = (RolledBackIndex != INDEX_NONE)
			? Profile->GetPendingRewards()[RolledBackIndex].Items.Num()
			: 0;
		Outcome.bGrantedXP = false;
		Outcome.Result = ERewardClaimAtomicResult::SaveFailed;
		Outcome.Error = FString::Printf(TEXT("the claim-snapshot save failed (%s); the claim was rolled back and the draft stays pending unchanged"),
			*ClaimSave.Message);
		return Outcome;
	}

	// 7. Committed: memory and disk agree; the caller may confirm the claim
	//    (the retention shape keeps its M3-009 message for the UI).
	Outcome.bSaveCommitted = true;
	switch (Claim.Result)
	{
	case ERewardClaimResult::PartiallyClaimed:
		Outcome.Result = ERewardClaimAtomicResult::PartiallyClaimed;
		break;
	case ERewardClaimResult::InventoryFull:
		Outcome.Result = ERewardClaimAtomicResult::InventoryFull;
		Outcome.Error = Claim.Error;
		break;
	default:
		Outcome.Result = ERewardClaimAtomicResult::Claimed;
		break;
	}
	return Outcome;
}
