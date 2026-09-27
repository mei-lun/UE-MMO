#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"

#include "ProfileSubsystem.generated.h"

/**
 * Read-only view of the profile state, returned as a plain VALUE copy by
 * UProfileSubsystem::GetProfileSnapshot (interface contract section 8). The
 * derived stats are recomputed from the level formulas at copy time - never
 * copied out of a World's HealthComponent - so a snapshot can never fossilize
 * temporary in-combat HP as the character's permanent base. The
 * default-constructed snapshot (Level 0, zero stats, invalid CharacterId)
 * means "no profile yet" (HasProfile() == false); an existing profile always
 * reports Level >= 1 and the exact level-1 formula row (100/0/0).
 */
struct FProfileSnapshot
{
	/** Persistent business identity of the character; invalid when no profile. */
	FGuid CharacterId;

	/** Current level (1..UProfileSubsystem::MaxLevel for an existing profile). */
	int32 Level = 0;

	/** XP toward the next level; compare against GetNextLevelXP(Level). */
	int32 XP = 0;

	/** Derived: GetMaxHPForLevel(Level) - base HP pool before any equipment. */
	int32 MaxHP = 0;

	/** Derived: GetAttackForLevel(Level) - base attack before any equipment. */
	int32 Attack = 0;

	/** Derived: GetDefenseForLevel(Level) - base defense before any equipment. */
	int32 Defense = 0;

	/** Value copy of the backing inventory at snapshot time (insertion order). */
	FInventoryModel Inventory;
};

/**
 * M3-003: local character profile and new-game initial state (interface
 * contract section 8). A UGameInstanceSubsystem, so the profile lives as long
 * as the GameInstance: map switches and ordinary returns to the menu destroy
 * UWorlds but never this subsystem, its CharacterId, its progress or its
 * inventory. The subsystem holds NO World actors and no HealthComponent
 * reference - the World-side combat attributes are (in a later wiring task)
 * read FROM the profile at session start, never written back permanently:
 * temporary in-combat HP must not fossilize into the profile.
 *
 * State: CharacterId (a unique FGuid minted by NewProfile), Level (1..10),
 * XP, the backing FInventoryModel (M3-002), plus placeholder storage for the
 * downstream M3 tasks: Equipment (M3-004 fills the slot->instance mapping),
 * PendingRewards and AppliedSettlementIds (M3-008 fills the reward flow).
 * This card does NOT save or load anything (M3-013 owns persistence), so a
 * process exit still loses the profile by design.
 *
 * Explicit API only:
 * - NewProfile() mints a fresh unique CharacterId and resets Level=1, XP=0,
 *   inventory, equipment and placeholders. Called by the new-game flow.
 * - ResetNewGame() is the equally explicit "start over" entry with identical
 *   semantics. NOTHING calls it automatically: a failed save/load (or any
 *   other error path) must never silently wipe a character - when loading
 *   exists (M3-013) it must surface the failure, not reset.
 * - AddXP(Amount) grows XP monotonically (non-positive amounts are ignored,
 *   the counter saturates instead of overflowing) and cascades through
 *   multiple level-ups while the accumulated XP covers the next requirement.
 *
 * Level formulas (pure, static, directly testable - the initial values are a
 * design choice and later tuning must be recorded): level range 1..10, XP to
 * next level = 100 x current level (0 at max level), MaxHP = 100 + 10 x (L-1),
 * Attack = 0 + 2 x (L-1), Defense = 0 + (L-1). Out-of-range levels are
 * clamped into [1, 10] by every formula, so a bad caller can never
 * synthesize a stat outside the design table.
 *
 * GetProfileSnapshot() hands out a read-only value copy (identity, progress,
 * derived stats, inventory) for UI and the future save layer; mutating the
 * inventory goes through GetInventory() (M3-002 semantics unchanged).
 */
UCLASS()
class UEMMO_API UProfileSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// -- Design constants (interface contract section 8) ----------------------

	/** Closed level range top; there is no level beyond this one. */
	static constexpr int32 MaxLevel = 10;

	/** Level-1 base HP (the GetMaxHPForLevel intercept). */
	static constexpr int32 BaseMaxHP = 100;

	/** Level-1 base attack (the GetAttackForLevel intercept). */
	static constexpr int32 BaseAttack = 0;

	/** Level-1 base defense (the GetDefenseForLevel intercept). */
	static constexpr int32 BaseDefense = 0;

	/** XP requirement per level: 100 x current level. */
	static constexpr int32 XPPerLevelFactor = 100;

	/** MaxHP gain per level above 1. */
	static constexpr int32 MaxHPPerLevel = 10;

	/** Attack gain per level above 1. */
	static constexpr int32 AttackPerLevel = 2;

	/** Defense gain per level above 1. */
	static constexpr int32 DefensePerLevel = 1;

	// -- Lifecycle -------------------------------------------------------------

	/**
	 * Puts the subsystem into the "no profile yet" state (invalid CharacterId,
	 * HasProfile() == false). Deliberately NOT a NewProfile call: an empty
	 * GameInstance must not silently mint a character identity.
	 */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// -- New game entries (explicit API only) ----------------------------------

	/** Mints a fresh unique CharacterId and resets Level=1, XP=0 and all containers. */
	void NewProfile();

	/**
	 * Explicit "start a new game" entry: identical semantics to NewProfile
	 * (fresh unique CharacterId, level 1 restart). Never called by any
	 * automatic path - in particular the future load flow (M3-013) must not
	 * fall back to this on load failure.
	 */
	void ResetNewGame();

	/** True once a profile exists (NewProfile/ResetNewGame ran at least once). */
	bool HasProfile() const;

	// -- Level formulas (pure static functions) --------------------------------

	/**
	 * XP needed to advance FROM the given level to the next: 100 x Level for
	 * 1..9, and 0 at MaxLevel (the max level has no next level to buy).
	 * Out-of-range input clamps into [1, MaxLevel] first.
	 */
	static int32 GetNextLevelXP(int32 Level);

	/** MaxHP at the given level: BaseMaxHP + 10 x (Level-1); clamped input. */
	static int32 GetMaxHPForLevel(int32 Level);

	/** Attack at the given level: BaseAttack + 2 x (Level-1); clamped input. */
	static int32 GetAttackForLevel(int32 Level);

	/** Defense at the given level: BaseDefense + (Level-1); clamped input. */
	static int32 GetDefenseForLevel(int32 Level);

	// -- Progression -------------------------------------------------------------

	/**
	 * Adds XP to the profile (no-op without a profile or for Amount <= 0).
	 * XP only grows: the counter saturates at MAX_int32 instead of wrapping.
	 * While Level < MaxLevel and XP covers GetNextLevelXP(Level), the
	 * requirement is subtracted and the level increases - so one call can
	 * cascade through several levels. Returns true when at least one level-up
	 * happened (UI hook), false otherwise.
	 */
	bool AddXP(int32 Amount);

	// -- Introspection -----------------------------------------------------------

	/** Persistent character identity; invalid until a profile exists. */
	const FGuid& GetCharacterId() const;

	/** Current level (0 when no profile exists yet, else 1..MaxLevel). */
	int32 GetLevel() const;

	/** XP accumulated toward the next level. */
	int32 GetXP() const;

	/**
	 * Backing inventory (M3-002 semantics: TryAdd/Remove return explicit
	 * results and never lose the caller's item on rejection). Mutable so the
	 * gameplay flows can add/remove; NewProfile empties it.
	 */
	FInventoryModel& GetInventory();

	/** Read-only access to the backing inventory. */
	const FInventoryModel& GetInventory() const;

	// -- Read-only snapshot --------------------------------------------------------

	/**
	 * Value copy of the profile for UI and the future save layer: identity,
	 * Level, XP, the three derived stats (recomputed from the formulas, never
	 * read from a World's HealthComponent) and a copy of the inventory.
	 * Returns the default snapshot (Level 0, invalid id) when no profile
	 * exists; the copy never aliases the live state.
	 */
	FProfileSnapshot GetProfileSnapshot() const;

private:
	/**
	 * Shared body of NewProfile/ResetNewGame: mints a fresh unique
	 * CharacterId when requested, restarts Level/XP from the design initial
	 * values and empties every container (inventory and the downstream-task
	 * placeholders alike).
	 */
	void ApplyFreshProfileState(bool bGenerateNewCharacterId);

	/** Persistent identity; minted only by NewProfile/ResetNewGame. */
	FGuid CharacterId;

	/** 0 = no profile yet, else 1..MaxLevel. */
	int32 Level = 0;

	/** XP toward the next level; saturating growth, never wraps. */
	int32 XP = 0;

	/** False until NewProfile/ResetNewGame created the first profile. */
	bool bHasProfile = false;

	/** Backing inventory (M3-002); plain value struct, no UObject references. */
	FInventoryModel Inventory;

	/**
	 * M3-004 placeholder: equipment slot -> inventory InstanceId mapping.
	 * Always empty in this card; NewProfile/ResetNewGame clear it so the
	 * future implementation starts from the same lifecycle.
	 */
	TMap<EItemSlot, FGuid> Equipment;

	/**
	 * M3-008 placeholder: pending reward queue (that task introduces the real
	 * FPendingReward element type; this card only owns the lifecycle - the
	 * array exists, starts empty and is cleared by NewProfile/ResetNewGame).
	 */
	TArray<FGuid> PendingRewards;

	/**
	 * M3-008 placeholder: settlement ids whose rewards were already claimed
	 * (the double-claim idempotency guard). Empty this card.
	 */
	TSet<uint64> AppliedSettlementIds;

	/**
	 * Card-listed placeholder ("AliveIds") with no consumer inside this card's
	 * scope; kept so the state surface matches the task card, and a downstream
	 * task either uses or removes it. Empty this card.
	 */
	TSet<FGuid> AliveIds;
};
