// M3-003: local character profile and new-game initial state (interface
// contract section 8). Green implementation of the stubbed contract: explicit
// NewProfile/ResetNewGame entries, the pure level formulas, the saturating
// AddXP cascade and the read-only snapshot. No World actor is ever touched -
// the derived stats are recomputed from the level formulas at copy time.

#include "ProfileSubsystem.h"
#include "ExperienceCurve.h"
#include "../Logging/OperationLogSubsystem.h"

namespace
{
	/**
	 * M3-023: the profile logs its discrete state changes through the log
	 * subsystem of its own game instance (the profile subsystem's outer IS the
	 * game instance). Null (an exotic unwired outer) skips the row silently.
	 */
	UOperationLogSubsystem* M3_023_OpLog(const UProfileSubsystem& Profile)
	{
		return UOperationLogSubsystem::FindForContext(&Profile);
	}

	/**
	 * M3-005: float final stat -> int32 snapshot stat (the M3-003 snapshot
	 * contract stays integral). Rounds half away from zero, never negative,
	 * and saturates into int32 so a huge bonus row cannot overflow the copy.
	 */
	int32 ToSnapshotStat(float Value)
	{
		Value = FMath::Max(0.0f, Value);
		if (Value >= static_cast<float>(MAX_int32))
		{
			return MAX_int32;
		}
		return FMath::RoundToInt(Value);
	}
}

void UProfileSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Fresh "no profile" state: an empty GameInstance must not silently mint a
	// character identity - NewProfile (or the future save-game load, M3-013)
	// is the only path that creates one.
	CharacterId.Invalidate();
	Level = 0;
	XP = 0;
	bHasProfile = false;
	Inventory = FInventoryModel();
	Equipment.Reset();
	EquippedStatBonus = FItemStats();
	PendingRewards.Reset();
	AppliedSettlementIds.Reset();
	AliveIds.Reset();
}

void UProfileSubsystem::NewProfile()
{
	ApplyFreshProfileState(/*bGenerateNewCharacterId*/ true);
	// M3-023: the new-profile state row (identity + design initial values).
	if (UOperationLogSubsystem* OpLog = M3_023_OpLog(*this))
	{
		OpLog->LogState(FString::Printf(TEXT("Profile NewProfile: character=%s level=1 xp=0"),
			*CharacterId.ToString()));
	}
}

void UProfileSubsystem::ResetNewGame()
{
	// Explicit new-game entry only, with identical semantics to NewProfile.
	// Nothing here calls it automatically: when loading exists (M3-013), a
	// failed load must surface the failure - never fall back to a reset.
	ApplyFreshProfileState(/*bGenerateNewCharacterId*/ true);
	// M3-023: the reset state row (a fresh unique identity, level 1 restart).
	if (UOperationLogSubsystem* OpLog = M3_023_OpLog(*this))
	{
		OpLog->LogState(FString::Printf(TEXT("Profile ResetNewGame: character=%s level=1 xp=0"),
			*CharacterId.ToString()));
	}
}

bool UProfileSubsystem::HasProfile() const
{
	return bHasProfile;
}

int32 UProfileSubsystem::GetNextLevelXP(int32 Level)
{
	// M3-006 extraction: FExperienceCurve is the single implementation of
	// every level formula (same values, same [1, MaxLevel] clamping).
	return FExperienceCurve::GetNextLevelXP(Level);
}

int32 UProfileSubsystem::GetMaxHPForLevel(int32 Level)
{
	return FExperienceCurve::GetMaxHPForLevel(Level);
}

int32 UProfileSubsystem::GetAttackForLevel(int32 Level)
{
	return FExperienceCurve::GetAttackForLevel(Level);
}

int32 UProfileSubsystem::GetDefenseForLevel(int32 Level)
{
	return FExperienceCurve::GetDefenseForLevel(Level);
}

bool UProfileSubsystem::AddXP(int32 Amount)
{
	// Without a profile there is nothing to grow; a non-positive amount can
	// never grow XP (XP only ever increases).
	if (!bHasProfile || Amount <= 0)
	{
		return false;
	}

	// Saturate instead of wrapping: XP never overflows into a negative value.
	if (Amount >= MAX_int32 - XP)
	{
		XP = MAX_int32;
	}
	else
	{
		XP += Amount;
	}

	// Cascade while the accumulated XP covers the next requirement: one call
	// can pass several level boundaries and always ends below the next one.
	// M3-006: the requirement now comes from the shared FExperienceCurve
	// (via the delegating static below). The counter deliberately keeps the
	// M3-003 max-level semantics (XP still accumulates, saturating) - the
	// pure curve's AddExperience instead clamps the remaining XP to 0 there.
	bool bLeveledUp = false;
	while (Level < MaxLevel)
	{
		const int32 NextRequirement = GetNextLevelXP(Level);
		if (NextRequirement <= 0 || XP < NextRequirement)
		{
			break;
		}
		XP -= NextRequirement;
		++Level;
		bLeveledUp = true;
	}
	// M3-023: the XP/level state row (the levelUp flag IS the UI hook result).
	if (UOperationLogSubsystem* OpLog = M3_023_OpLog(*this))
	{
		OpLog->LogState(FString::Printf(TEXT("Profile AddXP: amount=%d level=%d xp=%d levelUp=%d"),
			Amount, Level, XP, bLeveledUp ? 1 : 0));
	}
	return bLeveledUp;
}

const FGuid& UProfileSubsystem::GetCharacterId() const
{
	return CharacterId;
}

int32 UProfileSubsystem::GetLevel() const
{
	return Level;
}

int32 UProfileSubsystem::GetXP() const
{
	return XP;
}

FInventoryModel& UProfileSubsystem::GetInventory()
{
	return Inventory;
}

const FInventoryModel& UProfileSubsystem::GetInventory() const
{
	return Inventory;
}

TArray<FPendingReward>& UProfileSubsystem::GetPendingRewards()
{
	return PendingRewards;
}

const TArray<FPendingReward>& UProfileSubsystem::GetPendingRewards() const
{
	return PendingRewards;
}

bool UProfileSubsystem::IsSettlementApplied(uint64 SettlementId) const
{
	return AppliedSettlementIds.Contains(SettlementId);
}

void UProfileSubsystem::MarkSettlementApplied(uint64 SettlementId)
{
	// M3-009: plain set insert (idempotent by construction). The claim flow
	// marks the id exactly once at the first claim attempt, so a repeat claim
	// (or a replayed BeginReward) sees the applied guard; the record stays
	// even after the draft is fully consumed and removed.
	AppliedSettlementIds.Add(SettlementId);
	// M3-023: the applied-id state row (the XP-once guard committed).
	if (UOperationLogSubsystem* OpLog = M3_023_OpLog(*this))
	{
		OpLog->LogState(FString::Printf(TEXT("Profile SettlementApplied: settlement=%llu"), SettlementId));
	}
}

bool UProfileSubsystem::RestoreFromSave(const FProfileSnapshot& Snapshot, const FInventoryModel& InInventory,
	const TArray<FPendingReward>& InPendingRewards, const TSet<uint64>& InAppliedSettlementIds,
	const TMap<EItemSlot, FGuid>& InEquippedMap)
{
	// M3-015: all-or-nothing restore. The load path already validated the
	// save, but this entry is public - the structural invariants are checked
	// again here so a bad caller can never produce a half-restored profile:
	// a named identity, a level inside the closed design range, no negative
	// XP. On rejection NOTHING below runs (the previous state survives).
	if (!Snapshot.CharacterId.IsValid() || Snapshot.Level < 1 || Snapshot.Level > MaxLevel || Snapshot.XP < 0)
	{
		return false;
	}

	// Commit the full saved state: identity and progress (a restore never
	// mints a new id - only NewProfile does), the inventory, the equipped
	// bindings (the M3-004 placeholder map travels inside the save), the
	// pending drafts and the applied settlement ids.
	CharacterId = Snapshot.CharacterId;
	Level = Snapshot.Level;
	XP = Snapshot.XP;
	bHasProfile = true;
	Inventory = InInventory;
	Equipment = InEquippedMap;
	// The equipment stat bonus row resets to zero: the gameplay layer
	// re-derives the sum once the restored bindings are re-applied (the same
	// no-fossilized-sums rule as NewProfile/ResetNewGame).
	EquippedStatBonus = FItemStats();
	PendingRewards = InPendingRewards;
	AppliedSettlementIds = InAppliedSettlementIds;
	// AliveIds is not part of the save schema (no consumer yet): the restored
	// state starts from the same empty placeholder as a fresh profile.
	AliveIds.Reset();
	return true;
}

FProfileSnapshot UProfileSubsystem::GetProfileSnapshot() const
{
	if (!bHasProfile)
	{
		// "No profile yet": Level 0, zero stats, invalid id, empty inventory.
		return FProfileSnapshot();
	}

	FProfileSnapshot Snapshot;
	Snapshot.CharacterId = CharacterId;
	Snapshot.Level = Level;
	Snapshot.XP = XP;
	// The derived stats are recomputed at copy time - never copied out of a
	// World's HealthComponent - so a snapshot can never fossilize temporary
	// in-combat HP as the permanent base.
	//
	// M3-005: the stats are the COMPLETE FStatCalculator recalculation of the
	// level-curve base row plus the stored equipment bonus row (base + Sigma
	// of every equipped instance, hardened and MaxHP-floored by the shared
	// calculator) - the same from-scratch path every future consumer uses,
	// never an incremental add/remove on top of a previous snapshot.
	const FLevelBaseStats LevelBase = FExperienceCurve::GetBaseStatsForLevel(Level);
	FItemStats BaseRow;
	BaseRow.Attack = static_cast<float>(LevelBase.Attack);
	BaseRow.Defense = static_cast<float>(LevelBase.Defense);
	BaseRow.MaxHP = static_cast<float>(LevelBase.MaxHP);
	TArray<FItemStats> EquippedRows;
	EquippedRows.Add(EquippedStatBonus);
	const FItemStats FinalRow = FStatCalculator::Recalculate(BaseRow, EquippedRows);
	Snapshot.MaxHP = ToSnapshotStat(FinalRow.MaxHP);
	Snapshot.Attack = ToSnapshotStat(FinalRow.Attack);
	Snapshot.Defense = ToSnapshotStat(FinalRow.Defense);
	// Value copy: the UI/save layer never aliases the live backing model.
	Snapshot.Inventory = Inventory;
	return Snapshot;
}

void UProfileSubsystem::ApplyFreshProfileState(bool bGenerateNewCharacterId)
{
	// A new game mints a fresh unique identity (two NewProfile/ResetNewGame
	// calls never share a CharacterId) and restarts from the design initial
	// values: level 1, 0 XP, empty inventory and placeholders.
	CharacterId = bGenerateNewCharacterId ? FGuid::NewGuid() : FGuid();
	Level = 1;
	XP = 0;
	bHasProfile = bGenerateNewCharacterId;
	Inventory = FInventoryModel();
	Equipment.Reset();
	EquippedStatBonus = FItemStats();
	PendingRewards.Reset();
	AppliedSettlementIds.Reset();
	AliveIds.Reset();
}

void UProfileSubsystem::SetEquippedStatBonus(const FItemStats& Bonus)
{
	// M3-005: plain value store. Hardening of the row itself (NaN/negative
	// rejection) belongs to FStatCalculator at recalculation time, so the
	// stored raw row stays the caller's honest input and the snapshot is the
	// single hardened output point.
	EquippedStatBonus = Bonus;
}

const FItemStats& UProfileSubsystem::GetEquippedStatBonus() const
{
	return EquippedStatBonus;
}
