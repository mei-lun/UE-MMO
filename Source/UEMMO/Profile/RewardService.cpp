// M3-008: settlement reward drafts with idempotent application. GREEN
// implementation of the stubbed contract: BeginReward turns one Cleared run
// result into an FPendingReward{SettlementId, XP=50, Items[1]} stored in the
// GameInstance-level profile, derives the RewardSeed as a fixed salted mix of
// the room Seed (never a wall clock), and answers a repeat settlement with
// the ORIGINAL draft (the random result is generated exactly once per
// settlement and a retry never re-rolls it).

#include "RewardService.h"

#include "ProfileSubsystem.h"

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
