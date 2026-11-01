#include "SkillLoadoutComponent.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"
#include "Misc/Guid.h"

USkillLoadoutComponent::USkillLoadoutComponent()
{
	// The skill layer observes combat; it never drives the frame loop itself.
	PrimaryComponentTick.bCanEverTick = false;
	for (FName& Slot : SlotSkills)
	{
		Slot = NAME_None;
	}
}

void USkillLoadoutComponent::Initialize(UCombatComponent* InCombat, UProfileSubsystem* InProfile,
	const FProfessionCatalog* InCatalog, const FString& InConfigRevision)
{
	Combat = InCombat;
	Profile = InProfile;
	Catalog = InCatalog;
	ConfigRevision = InConfigRevision;
	RefreshLoadout();
	RefreshMaxMPFromProfile();
}

bool USkillLoadoutComponent::IsWired() const
{
	return Catalog != nullptr && Catalog->IsPublished() && Profile.IsValid();
}

void USkillLoadoutComponent::SetAttackCatalog(UAttackCatalog* InAttackCatalog)
{
	AttackCatalog = InAttackCatalog;
}

void USkillLoadoutComponent::SetClockProvider(TFunction<double()> InProvider)
{
	ClockProvider = MoveTemp(InProvider);
}

void USkillLoadoutComponent::SetFacingProvider(TFunction<int32()> InProvider)
{
	FacingProvider = MoveTemp(InProvider);
}

double USkillLoadoutComponent::NowSeconds() const
{
	return ClockProvider ? ClockProvider() : 0.0;
}

void USkillLoadoutComponent::RefreshLoadout()
{
	for (FName& Slot : SlotSkills)
	{
		Slot = NAME_None;
	}
	if (Catalog == nullptr)
	{
		return;
	}
	// The mapping is the profession's default loadout (Data/Professions/
	// loadouts.json): one row per filled slot, Q stays the weapon slot.
	for (const FLoadoutRow& Row : Catalog->Tables.Loadouts)
	{
		if (Row.SlotIndex >= 1 && Row.SlotIndex <= 8)
		{
			SlotSkills[Row.SlotIndex - 1] = Row.SkillId;
		}
	}
}

void USkillLoadoutComponent::RefreshMaxMPFromProfile()
{
	UProfileSubsystem* P = Profile.Get();
	if (P == nullptr || Catalog == nullptr || !Catalog->IsPublished())
	{
		return;
	}
	int32 OutHP = 0, OutAttack = 0, OutDefense = 0, OutPool = 0;
	if (Catalog->GetBaseStats(P->GetProfessionId(), P->GetLevel(), OutHP, OutAttack, OutDefense, OutPool))
	{
		MaxMP = OutPool;
		// An increased pool never refills; a shrank pool clamps the current.
		CurrentMP = FMath::Min(CurrentMP, MaxMP);
	}
}

void USkillLoadoutComponent::SetCurrentMP(int32 NewMP)
{
	CurrentMP = FMath::Clamp(NewMP, 0, FMath::Max(MaxMP, 0));
}

void USkillLoadoutComponent::RestoreFullMP()
{
	CurrentMP = FMath::Max(MaxMP, 0);
}

FName USkillLoadoutComponent::GetSlotSkill(int32 SlotIndex) const
{
	return (SlotIndex >= 1 && SlotIndex <= 8) ? SlotSkills[SlotIndex - 1] : FName(NAME_None);
}

EProfessionSkillEffectPolicy USkillLoadoutComponent::GetSkillPolicy(FName SkillId) const
{
	const FSkillRow* Row = (Catalog != nullptr) ? Catalog->FindSkill(SkillId) : nullptr;
	return Row ? Row->EffectPolicy : EProfessionSkillEffectPolicy::SingleMeleeHit;
}

float USkillLoadoutComponent::GetCooldownRemaining(FName SkillId) const
{
	const double* Deadline = CooldownDeadlineSeconds.Find(SkillId);
	if (Deadline == nullptr)
	{
		return 0.f;
	}
	const double Remaining = *Deadline - NowSeconds();
	return Remaining > 0.0 ? static_cast<float>(Remaining) : 0.f;
}

bool USkillLoadoutComponent::IsOnCooldown(FName SkillId) const
{
	return GetCooldownRemaining(SkillId) > 0.f;
}

const FSkillRow* USkillLoadoutComponent::FindLauncherSkillRow() const
{
	if (Catalog == nullptr || !Catalog->IsPublished())
	{
		return nullptr;
	}
	for (const TPair<FName, FSkillRow>& Pair : Catalog->Tables.Skills)
	{
		if (Pair.Value.SlotEligibility == FName(TEXT("launcher")))
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

const FSkillLevelRow* USkillLoadoutComponent::FindOwnedRankRow(FName SkillId, int32 OwnedRank) const
{
	if (Catalog == nullptr || !Catalog->IsPublished())
	{
		return nullptr;
	}
	const TArray<FSkillLevelRow>* Levels = Catalog->FindSkillLevels(SkillId);
	if (Levels == nullptr)
	{
		return nullptr;
	}
	for (const FSkillLevelRow& Row : *Levels)
	{
		if (Row.Rank == OwnedRank)
		{
			return &Row;
		}
	}
	return nullptr;
}

FSkillActivationOutcome USkillLoadoutComponent::PrecheckSkill(FName SkillId, double Now) const
{
	FSkillActivationOutcome Out;
	UProfileSubsystem* P = Profile.Get();
	if (Catalog == nullptr || !Catalog->IsPublished() || P == nullptr || !P->GetCharacterId().IsValid())
	{
		Out.Error = ESkillActivationError::InvalidContext;
		Out.Message = TEXT("no active character or no published profession catalog");
		return Out;
	}
	const FSkillRow* Row = Catalog->FindSkill(SkillId);
	if (Row == nullptr)
	{
		Out.Error = ESkillActivationError::InvalidContext;
		Out.Message = FString::Printf(TEXT("skill '%s' is not in the published catalog"), *SkillId.ToString());
		return Out;
	}
	Out.SkillId = Row->SkillId;
	Out.AttackId = Row->AttackId;
	if (Row->ProfessionId != P->GetProfessionId())
	{
		Out.Error = ESkillActivationError::WrongProfession;
		Out.Message = FString::Printf(
			TEXT("skill '%s' belongs to profession '%s' (active '%s')"),
			*SkillId.ToString(), *Row->ProfessionId.ToString(), *P->GetProfessionId().ToString());
		return Out;
	}
	const int32 OwnedRank = P->GetSkillRank(SkillId);
	if (OwnedRank < 1)
	{
		Out.Error = ESkillActivationError::Locked;
		Out.Message = FString::Printf(TEXT("skill '%s' was never learned"), *SkillId.ToString());
		return Out;
	}
	const FSkillLevelRow* RankRow = FindOwnedRankRow(SkillId, OwnedRank);
	if (RankRow == nullptr)
	{
		Out.Error = ESkillActivationError::MissingAsset;
		Out.Message = FString::Printf(
			TEXT("skill '%s' has no rank %d row in the published catalog"), *SkillId.ToString(), OwnedRank);
		return Out;
	}
	if (Row->AttackPending)
	{
		Out.Error = ESkillActivationError::MissingAsset;
		Out.Message = FString::Printf(
			TEXT("skill '%s' is registered with a pending (unwired) attack"), *SkillId.ToString());
		return Out;
	}
	UAttackCatalog* Attacks = AttackCatalog.Get();
	if (Attacks != nullptr && Attacks->Find(Row->AttackId) == nullptr
		&& Row->EffectPolicy != EProfessionSkillEffectPolicy::SelfBuff)
	{
		Out.Error = ESkillActivationError::MissingAttack;
		Out.Message = FString::Printf(
			TEXT("skill '%s' drives attack '%s' which the combat catalog does not define"),
			*SkillId.ToString(), *Row->AttackId.ToString());
		return Out;
	}
	const double Remaining = GetCooldownRemaining(SkillId);
	if (Remaining > 0.f)
	{
		Out.Error = ESkillActivationError::Cooldown;
		Out.Message = FString::Printf(TEXT("skill '%s' is on cooldown for %.2fs"), *SkillId.ToString(), Remaining);
		return Out;
	}
	if (CurrentMP < RankRow->MPCost)
	{
		Out.Error = ESkillActivationError::InsufficientMP;
		Out.Message = FString::Printf(
			TEXT("skill '%s' costs %d MP (current %d)"), *SkillId.ToString(), RankRow->MPCost, CurrentMP);
		return Out;
	}
	Out.MPCost = RankRow->MPCost;
	Out.CooldownSeconds = RankRow->CooldownS;
	return Out;
}

FSkillActivationOutcome USkillLoadoutComponent::TryActivateSkill(int32 SlotIndex)
{
	FSkillActivationOutcome Out;
	const FName SkillId = GetSlotSkill(SlotIndex);
	if (SkillId.IsNone())
	{
		Out.Error = ESkillActivationError::InvalidSlot;
		Out.Message = FString::Printf(TEXT("hotbar slot %d maps no skill"), SlotIndex);
		return Out;
	}
	const double Now = NowSeconds();
	Out = PrecheckSkill(SkillId, Now);
	if (Out.Error != ESkillActivationError::None)
	{
		return Out;
	}

	// The commit: the combat component accepts the attack FIRST; only then
	// are MP paid and the cooldown written (无副作用预检 -> 接受后扣费).
	UCombatComponent* C = Combat.Get();
	if (C == nullptr)
	{
		Out.Error = ESkillActivationError::InvalidContext;
		Out.Message = TEXT("no combat component to start the attack");
		return Out;
	}
	if (C->IsDead() || C->GetSnapshot().ActionState != ECombatActionState::Free)
	{
		Out.Error = ESkillActivationError::Busy;
		Out.Message = TEXT("the combatant is not Free (attacking, stunned, down or dead)");
		return Out;
	}
	const FSkillRow* Row = Catalog->FindSkill(SkillId);
	const int32 OwnedRank = Profile->GetSkillRank(SkillId);
	const FSkillLevelRow* RankRow = FindOwnedRankRow(SkillId, OwnedRank);
	if (Row == nullptr || RankRow == nullptr)
	{
		Out.Error = ESkillActivationError::InvalidContext;
		Out.Message = TEXT("the skill row vanished between the precheck and the commit");
		return Out;
	}

	// The SelfBuff policy never starts an attack: it applies the rank's
	// effect_parameters as a timed flat attack-power bonus and pays through
	// the same MP/cooldown commit (one active buff; a second application
	// replaces the first from the pre-buff base, never stacking).
	if (Row->EffectPolicy == EProfessionSkillEffectPolicy::SelfBuff)
	{
		UCombatComponent* BuffTarget = Combat.Get();
		if (BuffTarget == nullptr)
		{
			Out.Error = ESkillActivationError::InvalidContext;
			Out.Message = TEXT("no combat component to receive the buff");
			return Out;
		}
		const float Bonus = RankRow->EffectParameters.Contains(TEXT("attack_bonus"))
			? RankRow->EffectParameters[TEXT("attack_bonus")]
			: Row->EffectParameters.Contains(TEXT("attack_bonus"))
				? Row->EffectParameters[TEXT("attack_bonus")]
				: 0.f;
		const float Duration = RankRow->EffectParameters.Contains(TEXT("duration_s"))
			? RankRow->EffectParameters[TEXT("duration_s")]
			: Row->EffectParameters.Contains(TEXT("duration_s"))
				? Row->EffectParameters[TEXT("duration_s")]
				: 0.f;
		if (ActiveBuffSkillId == NAME_None)
		{
			ActiveBuffBaseAttackPower = BuffTarget->GetAttackPower();
		}
		ActiveBuffSkillId = Row->SkillId;
		ActiveBuffAttackBonus = Bonus;
		BuffExpirySeconds = Now + static_cast<double>(Duration);
		BuffTarget->SetCombatStats(ActiveBuffBaseAttackPower + Bonus, BuffTarget->GetDefense());
		return CommitActivation(*Row, *RankRow, 0);
	}

	// One-shot damage scale for the instance about to start (per-rank
	// damage_coefficient; 1.0 for unscaled legacy attacks).
	C->SetNextInstanceDamageScale(RankRow->DamageCoefficient);
	const int32 Facing = FacingProvider ? FacingProvider() : 1;
	if (!C->TryStartAttack(Row->AttackId, Facing))
	{
		Out.Error = ESkillActivationError::Busy;
		Out.Message = FString::Printf(
			TEXT("the combat component refused attack '%s'"), *Row->AttackId.ToString());
		return Out;
	}

	return CommitActivation(*Row, *RankRow, C->GetSnapshot().InstanceId);
}

FSkillActivationOutcome USkillLoadoutComponent::PrecheckLauncherSkill()
{
	const FSkillRow* Row = FindLauncherSkillRow();
	if (Row == nullptr)
	{
		FSkillActivationOutcome Out;
		Out.Error = ESkillActivationError::InvalidContext;
		Out.Message = TEXT("the active profession declares no launcher skill");
		return Out;
	}
	return PrecheckSkill(Row->SkillId, NowSeconds());
}

void USkillLoadoutComponent::CommitLauncherPayment(FName StartedAttackId)
{
	const FSkillRow* Row = FindLauncherSkillRow();
	if (Row == nullptr || Row->AttackId != StartedAttackId)
	{
		return;
	}
	UProfileSubsystem* P = Profile.Get();
	const int32 OwnedRank = P ? P->GetSkillRank(Row->SkillId) : 0;
	const FSkillLevelRow* RankRow = FindOwnedRankRow(Row->SkillId, OwnedRank);
	if (RankRow == nullptr)
	{
		return;
	}
	// The buffered press may have consumed its precheck before a concurrent
	// spend; the floor keeps MP non-negative (never a negative pool).
	CurrentMP = FMath::Max(0, CurrentMP - RankRow->MPCost);
	CooldownDeadlineSeconds.Add(Row->SkillId, NowSeconds() + static_cast<double>(RankRow->CooldownS));

	FSkillActivationOutcome Out;
	Out.bAccepted = true;
	Out.Error = ESkillActivationError::None;
	Out.SkillId = Row->SkillId;
	Out.AttackId = Row->AttackId;
	Out.MPCost = RankRow->MPCost;
	Out.CooldownSeconds = RankRow->CooldownS;
	Out.DamageCoefficient = RankRow->DamageCoefficient;
	Out.Message = TEXT("the launcher skill was paid at combat acceptance");
	OnSkillActivated.Broadcast(Out);
}

void USkillLoadoutComponent::RestoreCooldownRemainders(const TMap<FName, float>& Remainders)
{
	const double Now = NowSeconds();
	for (const TPair<FName, float>& Pair : Remainders)
	{
		if (Pair.Value > 0.f)
		{
			CooldownDeadlineSeconds.Add(Pair.Key, Now + static_cast<double>(Pair.Value));
		}
	}
}

void USkillLoadoutComponent::ResetTransientCombatState()
{
	CooldownDeadlineSeconds.Reset();
	RestoreFullMP();
	// An active SelfBuff dies with the run (the combat stats restore first).
	if (ActiveBuffSkillId != NAME_None)
	{
		ExpireActiveBuff();
	}
}

void USkillLoadoutComponent::ExpireActiveBuff()
{
	if (ActiveBuffSkillId == NAME_None)
	{
		return;
	}
	if (UCombatComponent* C = Combat.Get())
	{
		// Restore the pre-buff attack power (an equip change between the
		// apply and the expiry re-runs ApplyProfileFinalStats and overwrites
		// the buff anyway - a documented no-consumer edge).
		C->SetCombatStats(ActiveBuffBaseAttackPower, C->GetDefense());
	}
	ActiveBuffSkillId = NAME_None;
	ActiveBuffAttackBonus = 0.f;
	ActiveBuffBaseAttackPower = 0.f;
	BuffExpirySeconds = 0.0;
}

void USkillLoadoutComponent::TickSkillBuffs()
{
	if (ActiveBuffSkillId != NAME_None && NowSeconds() >= BuffExpirySeconds)
	{
		ExpireActiveBuff();
	}
}

FSkillActivationOutcome USkillLoadoutComponent::CommitActivation(const FSkillRow& SkillRow,
	const FSkillLevelRow& RankRow, uint64 AttackInstanceId)
{
	CurrentMP = FMath::Max(0, CurrentMP - RankRow.MPCost);
	CooldownDeadlineSeconds.Add(SkillRow.SkillId, NowSeconds() + static_cast<double>(RankRow.CooldownS));

	FSkillActivationOutcome Out;
	Out.bAccepted = true;
	Out.Error = ESkillActivationError::None;
	Out.SkillId = SkillRow.SkillId;
	Out.AttackId = SkillRow.AttackId;
	Out.AttackInstanceId = AttackInstanceId;
	Out.MPCost = RankRow.MPCost;
	Out.CooldownSeconds = RankRow.CooldownS;
	Out.DamageCoefficient = RankRow.DamageCoefficient;
	Out.Message = FString::Printf(TEXT("skill '%s' rank %d accepted"),
		*SkillRow.SkillId.ToString(), RankRow.Rank);
	OnSkillActivated.Broadcast(Out);
	return Out;
}
