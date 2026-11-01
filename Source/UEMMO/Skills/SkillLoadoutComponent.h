#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/Function.h"

#include "../Profession/ProfessionCatalog.h"
#include "../Profile/ProfileSubsystem.h"

#include "SkillTypes.h"

#include "SkillLoadoutComponent.generated.h"

class UCombatComponent;
class UAttackCatalog;

/**
 * Broadcast after a COMMITTED skill activation (interface contract section 6:
 * the event carries the character id, skill id, attack instance id and the
 * config revision; presentation and the operation log consume it).
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnSkillActivated, const FSkillActivationOutcome& /*Outcome*/);

/**
 * M7-003: the skill chain's loadout half (02 文档 §6). One component on the
 * player character holds ONLY the run-transient skill state - the 8-slot
 * mapping of the active profession, the current MP and the per-skill cooldown
 * deadlines; whether a skill is learned, its rank numbers and its costs are
 * always queried from the profile and the published profession catalog (不造
 * 第二份永久状态). The chain is:
 *
 *   input action -> FSkillIntent -> THIS component (precheck) ->
 *   UCombatComponent::TryStartAttack (accept) -> pay MP + write cooldown.
 *
 * The precheck has no side effects; MP and cooldown are written only after
 * the combat component accepted the attack (被受击打断的技能默认不退款 - the
 * payment happens at acceptance, so an interrupt after it is the design).
 *
 * The Z launcher keeps its existing buffered-input path (the cancel chains
 * depend on it): the press prechecks MP/cooldown/learned here and is refused
 * BEFORE entering the buffer; the payment happens on the combat component's
 * OnStarted of the launcher attack, which is exactly the "combat accepted"
 * moment. Hotbar skills start synchronously and pay right after acceptance.
 *
 * Cooldowns and MP live on the injected clock (the same pause-aware
 * World->GetTimeSeconds the combat input uses); they are run-transient and
 * never saved (大厅存档默认不保存战斗中冷却).
 */
UCLASS(ClassGroup = (Skills), meta = (BlueprintSpawnableComponent))
class UEMMO_API USkillLoadoutComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USkillLoadoutComponent();

	/**
	 * Binds the collaborators. Combat and profile may be null (bare test
	 * components) - every activation then refuses with InvalidContext. The
	 * catalog pointer is unowned; the caller guarantees its lifetime (the
	 * catalog subsystem owns the published value). Safe to re-call.
	 */
	void Initialize(UCombatComponent* InCombat, UProfileSubsystem* InProfile,
		const FProfessionCatalog* InCatalog, const FString& InConfigRevision);

	/**
	 * Binds the combat attack catalog (the runtime truth for MissingAttack:
	 * the profession tables gate attack ids at load time, but the combat
	 * catalog is what actually starts attacks). Unbound = no attack check.
	 */
	void SetAttackCatalog(UAttackCatalog* InAttackCatalog);

	/**
	 * Injects the cooldown/MP clock (the pause-aware game clock, one call per
	 * game frame before any activation; the character binds it from the World
	 * clock). Unbound, the clock reads 0 - cooldowns never expire, which is
	 * the correct "no clock yet" reading for bare test components.
	 */
	void SetClockProvider(TFunction<double()> InProvider);

	/**
	 * Injects the facing source for the started attack (+1 right / -1 left,
	 * the TryStartAttack mirror convention). Unbound, activations face +1.
	 */
	void SetFacingProvider(TFunction<int32()> InProvider);

	/**
	 * Re-reads the active profession's loadout mapping from the published
	 * catalog (the 8-slot map is configuration, not player state - the
	 * default loadout rows fill the slots). Called on spawn and after any
	 * profession/roster switch.
	 */
	void RefreshLoadout();

	/**
	 * Re-reads the snapshot's MaxMP (the profession curve's pool) and clamps
	 * the current MP down to it when the pool shrank. An increased pool never
	 * refills the current MP (等级提升增加 MaxMP 时不自动回满).
	 */
	void RefreshMaxMPFromProfile();

	/** Run-transient MP state (the pool itself is the profile's MaxMP). */
	int32 GetCurrentMP() const { return CurrentMP; }
	void SetCurrentMP(int32 NewMP);
	void RestoreFullMP();

	/**
	 * The full precheck + commit for one hotbar slot (1..8). Returns the
	 * explicit outcome; on acceptance the attack has STARTED, MP was paid and
	 * the cooldown was written. On refusal nothing changed.
	 */
	FSkillActivationOutcome TryActivateSkill(int32 SlotIndex);

	/**
	 * The Z-launcher precheck (NO side effects - the payment happens on the
	 * combat OnStarted). The input layer refuses the buffered press when this
	 * returns a refusal outcome.
	 */
	FSkillActivationOutcome PrecheckLauncherSkill();

	/**
	 * The launcher payment: called from the combat OnStarted handler with the
	 * attack id that started. Pays ONLY the launcher skill's own attack
	 * (a legacy combo chain into the launcher also passes here; the payment
	 * floors MP at 0 instead of refunding or blocking, so the legacy chain
	 * never breaks at an empty pool - the fresh Z press is what the economy
	 * gate refuses). Writes the cooldown deadline.
	 */
	void CommitLauncherPayment(FName StartedAttackId);

	/**
	 * Clears the run-transient skill state (cooldowns) and refills MP - the
	 * room-start/load semantics, matching the health pool's ResetHealth.
	 */
	void ResetTransientCombatState();

	/**
	 * Expires an active SelfBuff at its deadline (called per game frame from
	 * the owner's tick, on the same clock). No-op without an active buff.
	 */
	void TickSkillBuffs();

	/** True while a SelfBuff modification is applied to the combat stats. */
	bool HasActiveBuff() const { return ActiveBuffSkillId != NAME_None; }

private:
	/** Restores the pre-buff combat stats and clears the buff state. */
	void ExpireActiveBuff();

public:

	/** Run-transient cooldown queries for the HUD (seconds; 0 = ready). */
	float GetCooldownRemaining(FName SkillId) const;
	bool IsOnCooldown(FName SkillId) const;

	/** The skill id mapped to a hotbar slot (NAME_None = empty slot). */
	FName GetSlotSkill(int32 SlotIndex) const;

	/** The effect policy of a skill (SingleMeleeHit when unknown). */
	EProfessionSkillEffectPolicy GetSkillPolicy(FName SkillId) const;

	/** The active profession's config revision (empty when unpublished). */
	const FString& GetConfigRevision() const { return ConfigRevision; }

	/** The published catalog (null before Initialize; HUD rank-row reads). */
	const FProfessionCatalog* GetCatalog() const { return Catalog; }

	/**
	 * True once Initialize bound a published catalog AND a profile - the
	 * input layer gates on this so a bare test character keeps the legacy
	 * counting-only/legacy-launcher behavior verbatim.
	 */
	bool IsWired() const;

	/** Broadcast after every committed activation (success only). */
	FOnSkillActivated OnSkillActivated;

private:
	/** Shared precheck for hotbar + launcher (the doc's rejection reasons). */
	FSkillActivationOutcome PrecheckSkill(FName SkillId, double NowSeconds) const;

	/**
	 * Commits one accepted activation: deducts MP (floored at 0), writes the
	 * cooldown deadline and broadcasts. Returns the filled outcome.
	 */
	FSkillActivationOutcome CommitActivation(const FSkillRow& SkillRow,
		const FSkillLevelRow& RankRow, uint64 AttackInstanceId);

	/** The skill the buffered launcher press drives (launcher_z), or null. */
	const FSkillRow* FindLauncherSkillRow() const;

	/** Resolves the rank row of the character's current rank of the skill. */
	const FSkillLevelRow* FindOwnedRankRow(FName SkillId, int32 OwnedRank) const;

	/** Clock read (0.0 while unbound). */
	double NowSeconds() const;

	/** The 8-slot map of the active profession (NAME_None = empty). */
	FName SlotSkills[8];

	/** Run-transient MP (the pool is the profile snapshot's MaxMP). */
	int32 CurrentMP = 0;
	int32 MaxMP = 0;

	/** Per-skill cooldown deadlines on the injected clock (absent = ready). */
	TMap<FName, double> CooldownDeadlineSeconds;

	/** Collaborators (weak: the owner character owns the strong chain). */
	TWeakObjectPtr<UCombatComponent> Combat;
	TWeakObjectPtr<UProfileSubsystem> Profile;
	TWeakObjectPtr<UAttackCatalog> AttackCatalog;
	const FProfessionCatalog* Catalog = nullptr;

	/** The revision the catalog was published from (rides the events). */
	FString ConfigRevision;

	/** Injected clock (empty = reads 0). */
	TFunction<double()> ClockProvider;

	/** Injected facing source (empty = +1). */
	TFunction<int32()> FacingProvider;

	// -- M7-003: the SelfBuff policy state (one active buff; none of the
	// six fighter skills uses it - the policy is implemented and tested
	// directly so the whitelist is real, not reserved).

	FName ActiveBuffSkillId = NAME_None;
	float ActiveBuffAttackBonus = 0.f;
	float ActiveBuffBaseAttackPower = 0.f;
	double BuffExpirySeconds = 0.0;
};
