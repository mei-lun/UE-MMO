// M3-003: local character profile and new-game initial state (interface
// contract section 8). Green implementation of the stubbed contract: explicit
// NewProfile/ResetNewGame entries, the pure level formulas, the saturating
// AddXP cascade and the read-only snapshot. No World actor is ever touched -
// the derived stats are recomputed from the level formulas at copy time.

#include "ProfileSubsystem.h"

namespace
{
	// Card-local clamp: the design level range is closed [1, MaxLevel]; every
	// formula treats out-of-range input as the nearest legal level, so a bad
	// caller can never synthesize a stat outside the design table.
	int32 M3_003_ClampLevel(int32 Level)
	{
		return FMath::Clamp(Level, 1, UProfileSubsystem::MaxLevel);
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
	PendingRewards.Reset();
	AppliedSettlementIds.Reset();
	AliveIds.Reset();
}

void UProfileSubsystem::NewProfile()
{
	ApplyFreshProfileState(/*bGenerateNewCharacterId*/ true);
}

void UProfileSubsystem::ResetNewGame()
{
	// Explicit new-game entry only, with identical semantics to NewProfile.
	// Nothing here calls it automatically: when loading exists (M3-013), a
	// failed load must surface the failure - never fall back to a reset.
	ApplyFreshProfileState(/*bGenerateNewCharacterId*/ true);
}

bool UProfileSubsystem::HasProfile() const
{
	return bHasProfile;
}

int32 UProfileSubsystem::GetNextLevelXP(int32 Level)
{
	const int32 ClampedLevel = M3_003_ClampLevel(Level);
	// The max level has no next level to buy: its requirement reads 0.
	return ClampedLevel >= MaxLevel ? 0 : XPPerLevelFactor * ClampedLevel;
}

int32 UProfileSubsystem::GetMaxHPForLevel(int32 Level)
{
	return BaseMaxHP + MaxHPPerLevel * (M3_003_ClampLevel(Level) - 1);
}

int32 UProfileSubsystem::GetAttackForLevel(int32 Level)
{
	return BaseAttack + AttackPerLevel * (M3_003_ClampLevel(Level) - 1);
}

int32 UProfileSubsystem::GetDefenseForLevel(int32 Level)
{
	return BaseDefense + DefensePerLevel * (M3_003_ClampLevel(Level) - 1);
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
	// The derived stats are recomputed from the level formulas at copy time -
	// never copied out of a World's HealthComponent - so a snapshot can never
	// fossilize temporary in-combat HP as the permanent base.
	Snapshot.MaxHP = GetMaxHPForLevel(Level);
	Snapshot.Attack = GetAttackForLevel(Level);
	Snapshot.Defense = GetDefenseForLevel(Level);
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
	PendingRewards.Reset();
	AppliedSettlementIds.Reset();
	AliveIds.Reset();
}
