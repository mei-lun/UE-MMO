#include "CombatReferenceValidator.h"

namespace
{
	/**
	 * One definition site for the unknown-reference message shape. It matches
	 * the M5-005 mechanism (CombatDataTableParser.cpp) so operators see a
	 * single format: table, row id, field, referenced table, missing id.
	 */
	FString MakeUnknownReferenceError(const TCHAR* SourceTable, const FName RowId, const TCHAR* Field,
		const TCHAR* ReferencedTable, const FName MissingId)
	{
		return FString::Printf(TEXT("%s: row '%s': field '%s' references unknown %s id '%s'"),
			SourceTable, *RowId.ToString(), Field, ReferencedTable, *MissingId.ToString());
	}

	/**
	 * Defense-in-depth duplicate row id detection over one candidate table.
	 * The parser already refuses duplicate ids at the file level; this guards
	 * the candidate struct, which other code paths can assemble too. Empty
	 * (NAME_None) ids are skipped - local validators own the empty-id rule.
	 */
	template <typename RowType>
	void CollectDuplicateRowIds(const TArray<RowType>& Rows, FName RowType::*IdMember,
		const TCHAR* Table, const TCHAR* Field, TArray<FString>& OutProblems)
	{
		TSet<FName> SeenIds;
		for (const RowType& Row : Rows)
		{
			const FName Id = Row.*IdMember;
			if (!Id.IsNone())
			{
				if (SeenIds.Contains(Id))
				{
					OutProblems.Add(FString::Printf(TEXT("%s: row '%s': field '%s' duplicates an earlier row id inside table '%s'"),
						Table, *Id.ToString(), Field, Table));
				}
				else
				{
					SeenIds.Add(Id);
				}
			}
		}
	}

	TSet<FName> MakeDamageProfileIdSet(const TArray<FDamageProfile>& Rows)
	{
		TSet<FName> Ids;
		for (const FDamageProfile& Row : Rows)
		{
			Ids.Add(Row.DamageProfileId);
		}
		return Ids;
	}

	TSet<FName> MakeAttackReactionIdSet(const TArray<FAttackReaction>& Rows)
	{
		TSet<FName> Ids;
		for (const FAttackReaction& Row : Rows)
		{
			Ids.Add(Row.ReactionId);
		}
		return Ids;
	}

	TSet<FName> MakeAmmoIdSet(const TArray<FAmmoType>& Rows)
	{
		TSet<FName> Ids;
		for (const FAmmoType& Row : Rows)
		{
			Ids.Add(Row.AmmoId);
		}
		return Ids;
	}

	TSet<FName> MakeProjectileIdSet(const TArray<FProjectileDefinition>& Rows)
	{
		TSet<FName> Ids;
		for (const FProjectileDefinition& Row : Rows)
		{
			Ids.Add(Row.ProjectileId);
		}
		return Ids;
	}

	/**
	 * weapons rows: the mode-appropriate damage profile, ammo and projectile
	 * references resolve, and the melee chain is wired end to end - the
	 * melee_attack_ids are exactly the complete legacy four-attack set and
	 * every listed attack id has both its attack_reactions row (the
	 * reaction_id reference) and its damage_profiles row (the magnitudes).
	 */
	void ValidateWeaponReferences(const FParsedCombatConfig& Config, TArray<FString>& OutProblems)
	{
		const TSet<FName> DamageProfileIds = MakeDamageProfileIdSet(Config.DamageProfiles);
		const TSet<FName> AmmoIds = MakeAmmoIdSet(Config.AmmoTypes);
		const TSet<FName> ProjectileIds = MakeProjectileIdSet(Config.Projectiles);
		const TSet<FName> ReactionIds = MakeAttackReactionIdSet(Config.AttackReactions);
		const TArray<FName>& LegacyAttackIds = GetLegacyMeleeAttackIds();

		for (const FWeaponDefinition& Weapon : Config.Weapons)
		{
			const FName RowId = Weapon.WeaponId;

			if (!Weapon.DamageProfileId.IsNone() && !DamageProfileIds.Contains(Weapon.DamageProfileId))
			{
				OutProblems.Add(MakeUnknownReferenceError(TEXT("weapons"), RowId,
					TEXT("damage_profile_id"), TEXT("damage_profiles"), Weapon.DamageProfileId));
			}
			if (!Weapon.AmmoId.IsNone() && !AmmoIds.Contains(Weapon.AmmoId))
			{
				OutProblems.Add(MakeUnknownReferenceError(TEXT("weapons"), RowId,
					TEXT("ammo_id"), TEXT("ammo_types"), Weapon.AmmoId));
			}
			if (!Weapon.ProjectileId.IsNone() && !ProjectileIds.Contains(Weapon.ProjectileId))
			{
				OutProblems.Add(MakeUnknownReferenceError(TEXT("weapons"), RowId,
					TEXT("projectile_id"), TEXT("projectiles"), Weapon.ProjectileId));
			}

			// The complete legacy set is a melee-only rule (other modes must
			// carry no attack ids at all - a local 004 validator rule).
			if (Weapon.FireMode == EWeaponFireMode::Melee)
			{
				TSet<FName> ListedAttackIds;
				for (const FName AttackId : Weapon.MeleeAttackIds)
				{
					if (!LegacyAttackIds.Contains(AttackId))
					{
						OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'melee_attack_ids' attack id '%s' is not part of the complete legacy four-attack set (light_01, light_02, launcher, aerial_01)"),
							*RowId.ToString(), *AttackId.ToString()));
					}
					if (!AttackId.IsNone())
					{
						if (ListedAttackIds.Contains(AttackId))
						{
							OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'melee_attack_ids' lists attack id '%s' more than once"),
								*RowId.ToString(), *AttackId.ToString()));
						}
						else
						{
							ListedAttackIds.Add(AttackId);
						}
					}
				}
				for (const FName LegacyId : LegacyAttackIds)
				{
					if (!ListedAttackIds.Contains(LegacyId))
					{
						OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'melee_attack_ids' must contain the complete legacy four-attack set (missing '%s')"),
							*RowId.ToString(), *LegacyId.ToString()));
					}
				}
			}

			// Every listed attack id (any mode that illegally carries one is
			// a local-validity problem; the references still must resolve)
			// resolves into attack_reactions and damage_profiles.
			TSet<FName> ResolvedAttackIds;
			for (const FName AttackId : Weapon.MeleeAttackIds)
			{
				if (AttackId.IsNone() || ResolvedAttackIds.Contains(AttackId))
				{
					continue;
				}
				ResolvedAttackIds.Add(AttackId);
				if (!ReactionIds.Contains(AttackId))
				{
					OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'melee_attack_ids' attack '%s' has no attack_reactions row (reaction_id reference missing)"),
						*RowId.ToString(), *AttackId.ToString()));
				}
				if (!DamageProfileIds.Contains(AttackId))
				{
					OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'melee_attack_ids' attack '%s' has no damage_profiles row (attack damage profile missing)"),
						*RowId.ToString(), *AttackId.ToString()));
				}
			}
		}
	}

	/** projectiles rows: the per-hit and per-explosion damage profiles resolve. */
	void ValidateProjectileReferences(const FParsedCombatConfig& Config, TArray<FString>& OutProblems)
	{
		const TSet<FName> DamageProfileIds = MakeDamageProfileIdSet(Config.DamageProfiles);

		for (const FProjectileDefinition& Projectile : Config.Projectiles)
		{
			const FName RowId = Projectile.ProjectileId;

			if (!Projectile.DamageProfileId.IsNone() && !DamageProfileIds.Contains(Projectile.DamageProfileId))
			{
				OutProblems.Add(MakeUnknownReferenceError(TEXT("projectiles"), RowId,
					TEXT("damage_profile_id"), TEXT("damage_profiles"), Projectile.DamageProfileId));
			}
			if (!Projectile.ExplosionDamageProfileId.IsNone() && !DamageProfileIds.Contains(Projectile.ExplosionDamageProfileId))
			{
				OutProblems.Add(MakeUnknownReferenceError(TEXT("projectiles"), RowId,
					TEXT("explosion_damage_profile_id"), TEXT("damage_profiles"), Projectile.ExplosionDamageProfileId));
			}
		}
	}

	/**
	 * presentations rows (two-argument entry): every reaction_id resolves
	 * into attack_reactions, every attack reaction keeps exactly one
	 * presentation mapping, and presentation ids stay unique.
	 */
	void ValidatePresentationReferences(const FParsedCombatConfig& Config,
		const TArray<FCombatPresentation>& Presentations, TArray<FString>& OutProblems)
	{
		const TSet<FName> ReactionIds = MakeAttackReactionIdSet(Config.AttackReactions);

		TSet<FName> MappedReactionIds;
		TSet<FName> SeenPresentationIds;
		for (const FCombatPresentation& Presentation : Presentations)
		{
			const FName RowId = Presentation.PresentationId;

			if (!Presentation.ReactionId.IsNone() && !ReactionIds.Contains(Presentation.ReactionId))
			{
				OutProblems.Add(MakeUnknownReferenceError(TEXT("presentations"), RowId,
					TEXT("reaction_id"), TEXT("attack_reactions"), Presentation.ReactionId));
			}

			if (!Presentation.ReactionId.IsNone())
			{
				if (MappedReactionIds.Contains(Presentation.ReactionId))
				{
					OutProblems.Add(FString::Printf(TEXT("presentations: row '%s': field 'reaction_id' duplicates the presentation mapping of attack reaction '%s'"),
						*RowId.ToString(), *Presentation.ReactionId.ToString()));
				}
				else
				{
					MappedReactionIds.Add(Presentation.ReactionId);
				}
			}

			if (!RowId.IsNone())
			{
				if (SeenPresentationIds.Contains(RowId))
				{
					OutProblems.Add(FString::Printf(TEXT("presentations: row '%s': field 'presentation_id' duplicates an earlier row id inside table 'presentations'"),
						*RowId.ToString()));
				}
				else
				{
					SeenPresentationIds.Add(RowId);
				}
			}
		}

		for (const FAttackReaction& Reaction : Config.AttackReactions)
		{
			if (Reaction.ReactionId.IsNone() || MappedReactionIds.Contains(Reaction.ReactionId))
			{
				continue;
			}
			OutProblems.Add(FString::Printf(TEXT("attack_reactions: row '%s': field 'reaction_id' has no presentations mapping (exactly one presentation per attack reaction is required)"),
				*Reaction.ReactionId.ToString()));
		}
	}
}

TArray<FString> ValidateAllReferences(const FParsedCombatConfig& Config)
{
	TArray<FString> Problems;

	CollectDuplicateRowIds(Config.DamageProfiles, &FDamageProfile::DamageProfileId,
		TEXT("damage_profiles"), TEXT("damage_profile_id"), Problems);
	CollectDuplicateRowIds(Config.AttackReactions, &FAttackReaction::ReactionId,
		TEXT("attack_reactions"), TEXT("reaction_id"), Problems);
	CollectDuplicateRowIds(Config.TargetReactions, &FTargetReaction::PolicyId,
		TEXT("target_reactions"), TEXT("policy_id"), Problems);
	CollectDuplicateRowIds(Config.Weapons, &FWeaponDefinition::WeaponId,
		TEXT("weapons"), TEXT("weapon_id"), Problems);
	CollectDuplicateRowIds(Config.AmmoTypes, &FAmmoType::AmmoId,
		TEXT("ammo_types"), TEXT("ammo_id"), Problems);
	CollectDuplicateRowIds(Config.Projectiles, &FProjectileDefinition::ProjectileId,
		TEXT("projectiles"), TEXT("projectile_id"), Problems);

	ValidateWeaponReferences(Config, Problems);
	ValidateProjectileReferences(Config, Problems);

	return Problems;
}

TArray<FString> ValidateAllReferences(const FParsedCombatConfig& Config, const TArray<FCombatPresentation>& Presentations)
{
	TArray<FString> Problems = ValidateAllReferences(Config);

	CollectDuplicateRowIds(Presentations, &FCombatPresentation::PresentationId,
		TEXT("presentations"), TEXT("presentation_id"), Problems);
	ValidatePresentationReferences(Config, Presentations, Problems);

	return Problems;
}
