#pragma once

#include "CoreMinimal.h"

/**
 * The three level-derived base attributes as one plain row (integers on
 * purpose: the design table is integral). Equipment bonuses (M3-005) add on
 * top through FStatCalculator - which M3-005 MIGRATED to Items/StatCalculator.h
 * (the seed that lived at the bottom of this header was moved there verbatim
 * and deleted here); this struct is only the curve's own output and never
 * carries equipment or in-combat state.
 */
struct FLevelBaseStats
{
	/** Base HP pool at the level, before any equipment. */
	int32 MaxHP = 0;

	/** Base attack at the level, before any equipment. */
	int32 Attack = 0;

	/** Base defense at the level, before any equipment. */
	int32 Defense = 0;
};

/**
 * M3-006: the level/XP growth curve of the research prototype (interface
 * contract section 8), extracted from UProfileSubsystem (M3-003) into this
 * pure static module so the profile, the future reward flow (M3-008) and the
 * future stat wiring (M3-010) share ONE implementation. No World, no UObject,
 * no clock: every member is a static pure function over plain integers,
 * directly testable without any fixture.
 *
 * Design table (the initial values are a design choice; later tuning must be
 * recorded in the task report):
 * - Level range 1..10 (closed; MinLevel/MaxLevel below).
 * - XP to advance FROM a level: 100 x current level (0 at the max level -
 *   the top level has no next level to buy).
 * - MaxHP   = 100 + 10 x (Level - 1)  -> 100..190
 * - Attack  =   0 +  2 x (Level - 1)  ->   0..18
 * - Defense =   0 +  1 x (Level - 1)  ->   0.. 9
 *
 * Out-of-range levels are clamped into [MinLevel, MaxLevel] by every formula,
 * so a bad caller can never synthesize a stat outside the design table (the
 * clamp semantics are documented here and asserted by the M3-006 tests).
 *
 * AddExperience is the canonical multi-level gain against the curve: it
 * consumes the per-level requirement in a loop (one call can cross several
 * level boundaries), retains the remainder below the next requirement
 * (overflow is never discarded), clamps the remaining XP to 0 at the max
 * level (XP has no meaning at the top) and rejects non-positive amounts
 * without any state change.
 *
 * Deliberate divergence note: UProfileSubsystem::AddXP keeps its M3-003
 * counter semantics (XP only grows monotonically, saturates at MAX_int32 and
 * still accumulates at the max level - locked by the unmodifiable M3-003
 * tests), while this pure curve clamps XP to 0 at the max level. The profile
 * delegates its formulas to this module but owns that counter difference.
 */
struct FExperienceCurve
{
	// -- Design constants (interface contract section 8) ----------------------

	/** Closed level range bottom. */
	static constexpr int32 MinLevel = 1;

	/** Closed level range top; there is no level beyond this one. */
	static constexpr int32 MaxLevel = 10;

	/** XP requirement per level: 100 x current level. */
	static constexpr int32 XPPerLevelFactor = 100;

	/** Level-1 base HP (the GetMaxHPForLevel intercept). */
	static constexpr int32 BaseMaxHP = 100;

	/** MaxHP gain per level above 1. */
	static constexpr int32 MaxHPPerLevel = 10;

	/** Level-1 base attack (the GetAttackForLevel intercept). */
	static constexpr int32 BaseAttack = 0;

	/** Attack gain per level above 1. */
	static constexpr int32 AttackPerLevel = 2;

	/** Level-1 base defense (the GetDefenseForLevel intercept). */
	static constexpr int32 BaseDefense = 0;

	/** Defense gain per level above 1. */
	static constexpr int32 DefensePerLevel = 1;

	// -- Level formulas (pure static functions, clamped inputs) ----------------

	/**
	 * XP needed to advance FROM the given level to the next: 100 x Level for
	 * 1..9, and 0 at MaxLevel. Out-of-range input clamps into
	 * [MinLevel, MaxLevel] first.
	 */
	static int32 GetNextLevelXP(int32 Level);

	/** MaxHP at the given level: BaseMaxHP + MaxHPPerLevel x (Level-1). */
	static int32 GetMaxHPForLevel(int32 Level);

	/** Attack at the given level: BaseAttack + AttackPerLevel x (Level-1). */
	static int32 GetAttackForLevel(int32 Level);

	/** Defense at the given level: BaseDefense + DefensePerLevel x (Level-1). */
	static int32 GetDefenseForLevel(int32 Level);

	/** The three level-derived base stats as one row (all formulas clamped). */
	static FLevelBaseStats GetBaseStatsForLevel(int32 Level);

	// -- Multi-level XP gain -----------------------------------------------------

	/**
	 * Canonical XP gain against the curve. The starting (Level, XP) state is
	 * normalized first (level clamped into [MinLevel, MaxLevel], negative XP
	 * read as 0); a non-positive Amount is rejected with NO state change
	 * (OutNewLevel/OutRemainingXP receive the unchanged normalized state) and
	 * a false return. Positive amounts saturate at MAX_int32 instead of
	 * wrapping, then the requirement is consumed per crossed boundary in a
	 * loop, so one call can cascade through several levels; the remainder
	 * below the next requirement is retained, and a result at MaxLevel clamps
	 * the remaining XP to 0. Returns true when at least one level-up happened.
	 */
	static bool AddExperience(int32 Level, int32 XP, int32 Amount,
		int32& OutNewLevel, int32& OutRemainingXP);
};

// M3-005 migration note: FStatCalculator used to be seeded here and now lives
// in Items/StatCalculator.h (its FLevelBaseStats overload moved verbatim).
// Include Items/StatCalculator.h directly when the calculator is needed; this
// header deliberately does not include it back (no reverse dependency).
