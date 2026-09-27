#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemInstance.h"
#include "../Items/ItemDefinition.h"
#include "../Room/RoomResult.h"

#include "RewardService.generated.h"

class UProfileSubsystem;
class UProfileSaveService;

/**
 * One settlement reward draft (interface contract section 8, M3-008). A draft
 * is created exactly once per SettlementId by URewardService::BeginReward and
 * lives in UProfileSubsystem::PendingRewards, so it survives every World
 * switch (the profile is a UGameInstanceSubsystem). Plain value struct on
 * purpose, like FItemInstance/FProfileSnapshot: the members are all engine
 * free value types and the save layer (M3-013) owns any serialized form.
 *
 * The item instances are PRE-GENERATED here (M3-007 FDropGenerator): the
 * random result is produced exactly once when the draft is created and a
 * retry of the settlement never re-rolls it. The claim into the inventory
 * (including the full-inventory retention rule) belongs to M3-009.
 */
struct FPendingReward
{
	/** Business identity of the settlement this draft belongs to; unique per run. */
	uint64 SettlementId = 0;

	/** XP granted by the settlement (design: 50 per cleared room). */
	int32 XP = 0;

	/** Pre-generated item instances; the roll happened once at draft time. */
	TArray<FItemInstance> Items;
};

/**
 * Result of one URewardService::BeginReward request (M3-008).
 *
 * Idempotency semantics (the task card allows merging "already applied" with
 * "return the original draft"; the merge is documented here and in the task
 * report):
 * - Applied: a new draft was created and stored in the profile.
 * - AlreadyApplied: the settlement was already processed. Two sub-cases:
 *   (a) a pending draft for the SettlementId already exists -> OutDraft
 *   carries the ORIGINAL draft unchanged (field-for-field identical, no
 *   re-roll, no second item); (b) the SettlementId was already claimed into
 *   the profile (AppliedSettlementIds, the M3-009 claim flow) -> OutDraft
 *   stays default (a claimed settlement no longer carries a pending draft).
 * - RejectedFailedResult / RejectedNoProfile / RejectedGeneration: nothing
 *   was stored, no random result was ever consumed.
 */
enum class ERewardBeginResult : uint8
{
	/** New draft created and stored in the profile's PendingRewards. */
	Applied,

	/** Settlement already processed: original pending draft returned, or no draft when already claimed. */
	AlreadyApplied,

	/** The FRoomResult is Failed (not reward eligible); never any draft. */
	RejectedFailedResult,

	/** No bound profile subsystem or no existing profile to hold the draft. */
	RejectedNoProfile,

	/** FDropGenerator rejected its inputs (Error names the offending field). */
	RejectedGeneration
};

/** Outcome of one BeginReward request: result enum, draft copy, error text. */
struct FRewardBeginOutcome
{
	/** What happened; check Error on the rejected paths. */
	ERewardBeginResult Result = ERewardBeginResult::RejectedNoProfile;

	/**
	 * The draft: the newly created one for Applied, the unchanged ORIGINAL
	 * draft for the AlreadyApplied-with-pending-draft sub-case, default
	 * (empty) for every other result.
	 */
	FPendingReward Draft;

	/** Empty on Applied/AlreadyApplied; names the rejection reason otherwise. */
	FString Error;
};

/**
 * Result of one URewardService::TryClaimPending request (M3-009). The claim
 * never loses a pending item: only an instance verifiably stored by the
 * inventory (TryAdd == Added) leaves its draft, every rejection retains the
 * instance unchanged with its original InstanceId and one-time roll.
 */
enum class ERewardClaimResult : uint8
{
	/**
	 * Every pending item was stored (a defensively empty draft is consumed
	 * too); the draft was removed from PendingRewards and the settlement id
	 * stays recorded as applied.
	 */
	Claimed,

	/** At least one item was stored and at least one was retained in the draft (multi-item drafts only). */
	PartiallyClaimed,

	/**
	 * Nothing was stored; every pending item is retained unchanged (the
	 * full-inventory shape the caller surfaces as "InventoryFull").
	 */
	InventoryFull,

	/** The settlement was fully claimed earlier: nothing left to grant, nothing changed (idempotent repeat). */
	AlreadyClaimed,

	/** No pending draft and no applied record carries this SettlementId: unknown id, nothing changed. */
	UnknownSettlement,

	/** No bound profile subsystem or no existing profile to claim from. */
	RejectedNoProfile
};

/** Outcome of one TryClaimPending request: result enum, per-call counters, XP flag, error text. */
struct FRewardClaimOutcome
{
	/** What happened; check Error on InventoryFull/UnknownSettlement/RejectedNoProfile. */
	ERewardClaimResult Result = ERewardClaimResult::RejectedNoProfile;

	/** Number of items THIS call stored into the inventory (0..draft size). */
	int32 ClaimedItemCount = 0;

	/** Number of items still retained in the pending draft after this call (0 = draft consumed/removed). */
	int32 RetainedItemCount = 0;

	/**
	 * True when THIS call granted the settlement XP: exactly the first claim
	 * attempt grants it (a full inventory does not hold XP back - XP is
	 * settlement progress, not inventory payload); every later attempt for
	 * the same SettlementId grants nothing again.
	 */
	bool bGrantedXP = false;

	/** Empty on Claimed/PartiallyClaimed/AlreadyClaimed; names the retention/rejection reason otherwise. */
	FString Error;
};

/**
 * M3-016: result of one URewardService::ClaimPendingAtomic request. The
 * atomic claim commits the reward (XP, items, PendingRewards change and
 * AppliedSettlementIds) as ONE profile snapshot save; the caller may confirm
 * the claim to the UI only from the returned result - never from memory
 * state. Only Claimed (and PartiallyClaimed) report stored items; a save
 * failure NEVER reports a stored item.
 */
enum class ERewardClaimAtomicResult : uint8
{
	/**
	 * Every pending item was stored into the inventory AND the post-claim
	 * snapshot save committed: the persisted state and the memory agree, the
	 * caller may confirm the claim to the UI.
	 */
	Claimed,

	/**
	 * At least one item was stored and at least one was retained (multi-item
	 * drafts only); the post-claim snapshot (with the retained draft) save
	 * committed.
	 */
	PartiallyClaimed,

	/**
	 * Nothing was stored (full inventory): the granted XP, the applied-id
	 * record and the RETAINED pending draft are committed as one snapshot,
	 * so the unclaimed item survives a restart with its original InstanceId.
	 * Not a claim success: the UI surfaces the retention.
	 */
	InventoryFull,

	/**
	 * The persisted state already records this settlement as fully claimed
	 * (applied record, no pending draft): idempotent repeat, nothing changed
	 * and nothing saved. The decision came from the profile state (restored
	 * from the last committed save), never from call-history assumptions.
	 */
	AlreadyClaimed,

	/** No pending draft and no applied record carries this SettlementId: unknown id, nothing changed. */
	UnknownSettlement,

	/**
	 * A save failed (draft persistence or claim-snapshot persistence): the
	 * in-memory claim was rolled back to the exact last committed state, the
	 * pending draft stays unchanged (original InstanceId, no re-roll), and
	 * the UI must NOT show claim success. Error names the failing save step.
	 */
	SaveFailed,

	/** No profile subsystem, no existing profile, or the passed pointers are null. */
	RejectedNoProfile,

	/** The save service is null or was never initialized with a slot prefix. */
	RejectedNoSaveService,

	/**
	 * The last committed save exists but is unreadable (all slots corrupt):
	 * the claim refuses to save on top of unreadable evidence and changes
	 * nothing.
	 */
	RejectedUnreadableSave
};

/**
 * Outcome of one ClaimPendingAtomic request. The UI contract: a claim may be
 * surfaced as claimed ONLY when Result is Claimed/PartiallyClaimed (and
 * InventoryFull surfaces the retention); every failure value keeps the
 * pending draft untouched so a retry re-attempts the SAME pre-generated
 * items.
 */
struct FRewardClaimAtomicOutcome
{
	/** What happened; check Error on every non-success value. */
	ERewardClaimAtomicResult Result = ERewardClaimAtomicResult::RejectedNoProfile;

	/** Number of items THIS call stored into the inventory (0..draft size). */
	int32 ClaimedItemCount = 0;

	/** Number of items still retained in the pending draft after this call (0 = draft consumed/removed). */
	int32 RetainedItemCount = 0;

	/** True when THIS call granted the settlement XP (exactly the first claim attempt ever does). */
	bool bGrantedXP = false;

	/**
	 * True only when THIS call's post-claim snapshot save committed (the
	 * Claimed/PartiallyClaimed/InventoryFull paths). AlreadyClaimed is durable
	 * through the EARLIER committed save (the persisted applied record), so
	 * the flag stays false there - it means "this call committed a save",
	 * not "the state is durable".
	 */
	bool bSaveCommitted = false;

	/** Empty on success paths; names the failing save step / rejection reason otherwise. */
	FString Error;
};

/**
 * M3-008: settlement reward drafts with idempotent application. An independent
 * UObject service (no World, no subsystem registration): it holds a weak
 * reference to the GameInstance-level UProfileSubsystem (BindProfile) and
 * writes the drafts through the profile's PendingRewards container, so a
 * draft outlives every World switch exactly like the profile itself.
 *
 * BeginReward(RoomResult, Catalog) turns one Cleared run result into a
 * pending draft: XP 50 (design constant RewardXPPerClear) plus exactly one
 * equipment instance generated by the M3-007 FDropGenerator from the pinned
 * starter drop table. Everything is deterministic:
 * - RewardSeed = DeriveRewardSeed(RoomResult.Seed): a fixed salted mix of the
 *   run's room seed - never a wall clock - so the same run always re-derives
 *   the same roll inputs and different runs derive different ones.
 * - InstanceId is a function of (SettlementId, RewardIndex) inside the
 *   generator, so the same settlement always maps to the same identity.
 * - A repeat BeginReward for the same SettlementId returns the ORIGINAL draft
 *   (AlreadyApplied) without generating again: the random result is produced
 *   exactly once per settlement and a retry never re-rolls it.
 * A Failed result is always rejected (the value-level IsRewardEligible=false
 * path) and never creates a draft. Different runs (different SettlementIds)
 * keep independent drafts.
 *
 * No disk persistence here (M3-013 owns the save layer) and no inventory
 * mutation (the claim, including full-inventory retention, belongs to M3-009).
 */
UCLASS()
class UEMMO_API URewardService : public UObject
{
	GENERATED_BODY()

public:
	// -- Design constants --------------------------------------------------------

	/** XP granted per cleared room's settlement (interface contract: XP 50). */
	static constexpr int32 RewardXPPerClear = 50;

	// -- Wiring ------------------------------------------------------------------

	/**
	 * Binds the GameInstance-level profile the drafts live in (weak reference;
	 * a destroyed subsystem is detected on use). Passing null unbinds.
	 */
	void BindProfile(UProfileSubsystem* Profile);

	/** The currently bound profile (nullptr when unbound or destroyed). */
	UProfileSubsystem* GetBoundProfile() const;

	// -- Seed derivation -----------------------------------------------------------

	/**
	 * RewardSeed = fixed-salt mix of the run's room Seed (FRoomResult.Seed).
	 * Pure and clock-free: the same RoomSeed always derives the same
	 * RewardSeed (a retry reproduces the identical draft), a different
	 * RoomSeed derives a different one. 64-bit murmur3 finalizer with a
	 * purpose-distinct salt, folded to int64; mirrors the FDropGenerator
	 * derivation style while never sharing its raw values.
	 */
	static int64 DeriveRewardSeed(int32 RoomSeed);

	// -- Settlement entry ----------------------------------------------------------

	/**
	 * Begins (or replays) the reward settlement of one finished run.
	 *
	 * - Failed result: rejected (RejectedFailedResult), no draft, no roll.
	 * - No bound profile / no profile: rejected (RejectedNoProfile).
	 * - SettlementId already claimed (AppliedSettlementIds, M3-009): AlreadyApplied
	 *   with a default draft - no draft, no re-roll.
	 * - SettlementId already has a pending draft: AlreadyApplied with the
	 *   ORIGINAL draft returned unchanged (idempotent replay, no re-roll).
	 * - Otherwise: one equipment piece from the starter table via
	 *   FDropGenerator::GenerateReward (RewardSeed from the room Seed,
	 *   SettlementId as the identity), stored as
	 *   FPendingReward{SettlementId, XP=50, Items[1]} in the profile's
	 *   PendingRewards; returns Applied with the new draft.
	 * Generation failure (broken table/catalog data): RejectedGeneration with
	 * the generator's field-naming error; nothing is stored.
	 */
	FRewardBeginOutcome BeginReward(const FRoomResult& Result, const FItemDefinitionCatalog& Catalog);

	// -- Claim entry (M3-009) ------------------------------------------------------

	/**
	 * Claims the pending reward draft of one settlement into the caller's
	 * inventory (interface contract section 8: items that fit enter the
	 * inventory by their pre-generated InstanceId, items that do not fit stay
	 * pending; the XP is granted exactly once per settlement). The draft and
	 * the applied-id record live in the bound GameInstance-level profile, so
	 * a claim survives World switches like the rest of the profile state.
	 *
	 * Per request:
	 * - No bound profile / no profile: RejectedNoProfile, nothing changed.
	 * - Unknown SettlementId (no pending draft AND no applied record):
	 *   UnknownSettlement with a naming error, nothing changed.
	 * - Fully claimed earlier (applied record, draft already removed):
	 *   AlreadyClaimed, nothing changed - a repeat click is always safe.
	 * - Draft found:
	 *   - XP: granted exactly once per settlement on the FIRST claim attempt
	 *     (Profile.AddXP with the draft's XP) and recorded via
	 *     UProfileSubsystem::MarkSettlementApplied; a full inventory does not
	 *     hold the XP back, and every later attempt for the same id grants
	 *     nothing again (the total stays the design 50, never 100).
	 *   - Items one by one: only TryAdd == Added removes the instance from
	 *     the draft (stored under its ORIGINAL InstanceId - no new FGuid, no
	 *     re-roll, no random refresh); every rejection retains the instance
	 *     unchanged, keeping its draft order.
	 *   - All items consumed: the empty draft is removed from PendingRewards
	 *     while the applied record STAYS (IsSettlementApplied remains true,
	 *     and a replayed BeginReward answers AlreadyApplied with a default
	 *     draft). Some retained: the draft stays with only the retained
	 *     items, so freeing inventory space and claiming again adds exactly
	 *     the original instances.
	 *
	 * No disk persistence here (M3-016 owns the atomic commit of the claim
	 * with the inventory/XP snapshot; M3-013 the save layer).
	 */
	FRewardClaimOutcome TryClaimPending(uint64 SettlementId, FInventoryModel& Inventory);

	// -- Atomic claim entry (M3-016) ------------------------------------------------

	/**
	 * Claims the pending reward draft of one settlement as ONE durable profile
	 * snapshot commit (interface contract section 8: the XP, the inventory
	 * items, the PendingRewards change and AppliedSettlementIds travel in the
	 * same save; the caller may confirm the claim to the UI only after that
	 * save committed). The profile and the save service are passed explicitly
	 * (the same GameInstance-level UProfileSubsystem the drafts live in and
	 * the UProfileSaveService owning the A/B slots), so the entry works on any
	 * freshly restored profile without relying on call history.
	 *
	 * Ordered flow per request:
	 * 1. Persisted-state gates: no profile / no save service is rejected; a
	 *    fully-claimed settlement (persisted applied record, no draft) answers
	 *    AlreadyClaimed and an unknown id answers UnknownSettlement - both
	 *    from the profile state alone, no save, no mutation.
	 * 2. Draft persistence FIRST ("StartRoom semantics"): the current profile
	 *    state including the pending draft is saved, so the pre-generated
	 *    item identity is durable before anything is claimed. A failure here
	 *    ends the request with SaveFailed and NOTHING mutated.
	 * 3. In-memory claim from the draft (M3-009 semantics via the shared
	 *    claim body): XP granted exactly once, items stored under their
	 *    original InstanceIds, rejections retained in the draft.
	 * 4. Claim-snapshot save: the POST-claim profile state (XP, inventory,
	 *    remaining pending drafts, applied ids) is saved as one snapshot.
	 *    Success confirms the claim (Claimed/PartiallyClaimed/InventoryFull
	 *    per the claim body). A failure rolls the in-memory commit back to
	 *    the exact state step 2 committed (RestoreFromSave with the captured
	 *    request) and reports SaveFailed: no half claim, no re-roll, the UI
	 *    can never show claim success.
	 *
	 * Interrupt windows (the task card's three injection points live in the
	 * claim-snapshot save): a process death before the slot write, after the
	 * slot write, or at the index commit leaves the last COMMITTED save at
	 * step 2's draft state (or, for a torn index commit, lets the startup
	 * generation scan recover the committed claim snapshot). Either way a
	 * restart + re-claim grants the XP exactly once and stores exactly the
	 * original instances - idempotency is decided from persisted state.
	 */
	FRewardClaimAtomicOutcome ClaimPendingAtomic(uint64 SettlementId, UProfileSubsystem* Profile, UProfileSaveService* SaveService);

private:
	/** GameInstance-level profile holding PendingRewards; weak on purpose. */
	TWeakObjectPtr<UProfileSubsystem> ProfilePtr;

	/**
	 * M3-016: the shared in-memory claim body of TryClaimPending and
	 * ClaimPendingAtomic - locates the draft in the profile, grants the XP
	 * exactly once (MarkSettlementApplied on the first attempt), stores items
	 * under their original InstanceIds and retains every rejected item
	 * unchanged. Pure memory semantics: no save happens here.
	 */
	static FRewardClaimOutcome M3_016_ApplyClaimInMemory(uint64 SettlementId, UProfileSubsystem& Profile, FInventoryModel& Inventory);
};
