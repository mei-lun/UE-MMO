#pragma once
#include "CoreMinimal.h"
#include "Delegates/DelegateCombinations.h"
#include "GameFramework/Character.h"
#include "Character/PlanarMovement.h"
#include "Combat/CombatInputBuffer.h"
// M5-020: the pawn owns the session catalog VALUES the weapon component mounts
// (non-owning pointers there), so the full value types are needed here.
#include "Combat/Data/CombatCatalog.h"
// M5-033: the pawn owns the input-layer fire wiring - the delivery bridge
// (hitscan executor seam, projectile world service, motion policies) and the
// pacing policies it registers into the weapon component.
#include "Combat/System/CombatEventTypes.h"
#include "Combat/System/HitLedger.h"
#include "Items/ItemDefinition.h"
#include "Projectiles/HitscanExecutor.h"
#include "Projectiles/LinearProjectilePolicy.h"
#include "Projectiles/ProjectileWorldService.h"
#include "Templates/Function.h"
#include "Weapons/FirePolicies/AutomaticFirePolicy.h"
#include "Weapons/FirePolicies/BurstFirePolicy.h"
#include "PrototypeCharacter.generated.h"

class ACombatProjectile;
class USideViewCameraComponent;
class UCombatComponent;
class UCombatPresentationComponent;
class UHealthComponent;
class UProfileSubsystem;
class URoomSessionSubsystem;
class UTrainingResetService;
class UWeaponComponent;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;
struct FItemStats;
struct FShotContext;
struct FWeaponMountOutcome;

/** Broadcast exactly once per player death lifecycle, when the health pool dies. */
DECLARE_MULTICAST_DELEGATE(FOnPlayerDied);

/** M0 movement scaffold. M1-012 routes combat intents into the combat component; M1-040 remaps the keys to the DNF layout. */
UCLASS()
class UEMMO_API APrototypeCharacter : public ACharacter, public IHitscanTargetIdentity
{
    GENERATED_BODY()
public:
    APrototypeCharacter();
    virtual void Tick(float DeltaSeconds) override;

    /** M1-012: the combat subobject every submitted intent is buffered into. */
    UCombatComponent* GetCombat() const { return Combat; }

    /**
     * M2-004: broadcast exactly once per death lifecycle, when the pawn's
     * health pool reaches 0. A reset opens a new lifecycle, so a revived pawn
     * broadcasts again on its next death (the UHealthComponent lifecycle
     * contract, interface contract section 5).
     */
    FOnPlayerDied PlayerDied;

    /**
     * M2-004: the pawn's own health pool (MaxHP 100 per the card). Owning it
     * from spawn makes the pawn selectable by the M1-018 target query, so the
     * M2-003 enemy attack pipeline lands real damage on it, and it carries
     * the death lifecycle the PlayerDied broadcast follows.
     */
    UHealthComponent* GetHealth() const { return Health; }

    /**
     * M1-012: public combat intent entry (M1-040 keys: X = Light, Z =
     * Launcher). PressedAt
     * is read from the input game clock (World GetTimeSeconds: advances with
     * normal game time only, so pause and hit stop do not advance it) and the
     * sequence comes from a character-level counter starting at 1. Push
     * rejections (duplicate/regressing sequence) are silently ignored.
     */
    void SubmitCombatInput(ECombatInput Action);

    /** Explicit-clock variant for early tests (interface contract section 2). */
    void SubmitCombatInput(ECombatInput Action, double PressedAt);

    /**
     * Standard pawn input setup (binding point for movement, jump, reset and
     * the M1-012 combat actions). Public on this class so the automation test
     * can drive a second setup call and prove the idempotency guard.
     */
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    /**
     * M1-027: the reset entry is the public reset surface (M1-040: bound to
     * F2; the old R key moved to skill slot 4). With a registered
     * UTrainingResetService the press routes into exactly one unified session
     * reset (player + every registered enemy; the service owns the player
     * physics reset, so nothing runs twice); without a service the M0 local
     * reset stays (bare scaffolding, test worlds). Public because it is the
     * input binding target and the automation tests drive it directly.
     */
    void ResetPosition();

    /**
     * M1-027: the local half of the unified reset - physics (zero velocity,
     * teleport back to the captured spawn location and spawn rotation, drop
     * pending launch/impulse forces and re-ground the movement mode) plus the
     * M2-004 value half for the pawn itself: full health pool (fresh death
     * lifecycle, so a dead pawn is revived and can die again), combat
     * teardown with the dead flag dropped, and the locomotion animation
     * re-attached. Called by UTrainingResetService (phase 2) and as the
     * service-less fallback of ResetPosition; it never triggers a session
     * reset itself, so the two entry points cannot recurse into each other.
     */
    void ApplyTrainingRoomReset();

    /**
     * M1-027: injects the session reset service the F2-key path routes into
     * (the room owner / automation tests register the participants on the
     * service and hand it to its pawn here). Passing null restores the M0
     * fallback behavior.
     */
    void SetTrainingResetService(UTrainingResetService* InService);

    /**
     * M1-023: the Jump entry (Enhanced Input Started binding; M1-040 keys: C
     * primary with Space retained as the alias). Public so the
     * automation tests can drive the exact binding target: the press is
     * buffered into the combat component (never a direct state-bypassing
     * jump); the component's state decides Free jump vs launcher jump-cancel
     * vs keep-buffered.
     */
    void StartJump();

    /**
     * M1-040: DNF skill-slot entry (slot 1..8 = Q W E R A S D F in that
     * order). The press only lands in a per-slot counter plus a Verbose log -
     * no combat effect yet (M2+ owns skill execution). Out-of-range slots are
     * ignored. Public because it is the input binding target and the
     * automation tests drive it directly.
     */
    void SubmitSkillSlot(int32 SlotIndex);

    /**
     * M3-010: the player-side equip/unequip entry. NewEquippedBonus REPLACES
     * the whole equipped stat bonus row of the profile (a zero row means
     * "nothing equipped", the M3-005 SetEquippedStatBonus semantics), so the
     * same entry serves equip and unequip. The request is REFUSED (returns
     * false, nothing changes) while a room run is Running - equipping is a
     * non-combat operation, the card requires exiting the room first - and
     * without an existing profile (there is nothing to write into). On
     * acceptance the fresh profile snapshot re-applies with the no-heal clamp
     * semantics (a lowered MaxHP clamps CurrentHP, a raised one never heals).
     * Public because it is the equipping surface the future equipment flow
     * and the automation tests drive directly.
     */
    bool TryEquipStatBonus(const FItemStats& NewEquippedBonus);

    /** M1-040: how often skill slot 1..8 was pressed since spawn (0 out of range). */
    int32 GetSkillSlotPressCount(int32 SlotIndex) const;

    /**
     * M5-020: the production weapon mount (M5 interface contract section 1,
     * owner 019..025). BeginPlay and every accepted equipment push re-resolve
     * the equipped weapon-slot item through it; the M5-022 fire scheduling and
     * the M5-043 vehicle mount consume the same component later.
     */
    UWeaponComponent* GetWeaponMount() const { return WeaponMount; }

    /**
     * M5-020: the equipment-changed re-entry - re-resolves the equipped
     * weapon-slot item from the local HUD's real equipment model and applies
     * it to the weapon mount (an empty slot applies the explicit "no weapon"
     * state). Called by BeginPlay (the map-travel rebind point), by the
     * accepted tail of TryEquipStatBonus (the production equip/unequip push,
     * so every equipment change re-loads the mount) and by the unified reset.
     * False (and no state change) without a HUD or a profile - a bare test
     * world keeps an unbound mount, never a fake bind. The production item
     * source carries no weapon mappings yet (the M5-019 lineage), so a
     * production equip is an explicit named refusal until that table lands.
     */
    bool RefreshWeaponMountFromEquipment();

    /**
     * M5-033: the Q Started binding target - the weapon-fire trigger. With a
     * bound ranged weapon that authorizes fire (alive, catalog healthy, no
     * menu open, no hit stun, no reload window) the press holds the trigger,
     * arms the burst policy (M5-024) and polls the fire transaction once
     * (the press-edge shot); without one the press keeps the M1-040
     * skill-slot 1 intent unchanged. Public because it is the input binding
     * target and the automation tests drive it directly.
     */
    void OnFireInputPressed();

    /** M5-033: the Q Completed binding target - drops the trigger (the burst policy keeps a started burst running). */
    void OnFireInputReleased();

    /**
     * M5-033: the T Started binding target - opens the reload window of the
     * active ranged weapon through the component's real reload entry (the
     * input layer owns the deadline poll that completes it). Inert without a
     * bound ranged weapon. Public because it is the input binding target.
     */
    void OnReloadInputPressed();

    /**
     * M5-033: the input-layer weapon wiring pass - selects the fire policy
     * from the active binding's weapon definition (delivery policy for
     * projectile weapons, burst policy for multi-round hitscan bursts,
     * automatic hold policy otherwise), resets the input-layer windows and
     * refreshes the stale-callback generation. Called by the accepted tail of
     * RefreshWeaponMountFromEquipment; the automation tests call it directly
     * after their own component-level binds (the production items source has
     * no weapon mappings yet, so the HUD/profile bind path refuses).
     */
    void ApplyWeaponBindWiring();

    /**
     * M5-033: injects the input-layer clock (the reload deadline and any
     * future input-layer pacing read it). Unbound, the owner world's
     * GetTimeSeconds stays the clock. The automation tests inject a shared
     * rig clock so pacing is exact.
     */
    void SetWeaponInputClockProvider(TFunction<double()> Provider);

    /** M5-033: true between the fire-authorized Q press and its release. */
    bool IsWeaponFireHeld() const { return bWeaponFireHeld; }

    /** M5-033: true while the input layer polls for the reload deadline. */
    bool IsWeaponReloadPending() const { return bWeaponReloadPending; }

    /** M5-033: live projectile pellets the delivery bridge currently advances. */
    int32 GetLiveWeaponProjectileCount() const { return LiveWeaponProjectiles.Num(); }

    /**
     * M1-040: the eight runtime skill-slot actions in slot order (index 0 is
     * slot 1 = Q, index 7 is slot 8 = F), read-only for the automation tests'
     * mapping-table assertions.
     */
    const TArray<TObjectPtr<UInputAction>>& GetSkillSlotActions() const { return SkillSlotActions; }

    /**
     * M1-028: the runtime mapping context built by EnsureCombatInputActions,
     * read-only. The automation smoke asserts the F1 debug-toggle mapping
     * exists on it exactly once.
     */
    const UInputMappingContext* GetRuntimeInputMappingContext() const { return Mapping; }

    /**
     * M1-028: the F1 press entry (Enhanced Input Started binding). Toggles the
     * local HUD's combat debug overlay through the possessing player
     * controller; pure display, no combat state change. Public because it is
     * the input binding target.
     */
    void OnDebugTogglePressed();

protected:
    virtual void BeginPlay() override;
private:
    void MoveHorizontal(const FInputActionValue& Value);
    void MoveDepth(const FInputActionValue& Value);
    void EndJump();
    /**
     * M3-023: the F2 Started binding logs the key press into the operation
     * log and then routes into ResetPosition. The split exists so the
     * fall-out-of-world recoveries (which call ResetPosition directly from
     * Tick) stay unlogged as the non-key events they are.
     */
    void OnResetPressed();
    /**
     * M2-004: the death half of the health wiring (bound to Health->OnDied in
     * BeginPlay): marks the combat component dead (refuses attacks and
     * movement, cancels the running attack), takes the mesh out of the
     * locomotion AnimBP drive so the death pose is not overridden, and
     * broadcasts PlayerDied exactly once per lifecycle.
     */
    void HandlePlayerDied();
    /**
     * M3-010: re-applies the profile's final stats to this pawn's combat
     * attributes: snapshot MaxHP -> Health SetMaxHealth (no-heal clamp) and
     * snapshot Attack/Defense -> Combat SetCombatStats. A no-op without a
     * resolved profile subsystem or without an existing profile (the component
     * defaults stay). Never heals by itself: the only pool refills are the
     * pawn's spawn load in BeginPlay and the run-start handler's explicit
     * ResetHealth - an ordinary equipment change keeps the current pool.
     */
    void ApplyProfileFinalStats();
    /**
     * M3-010: the RoomSessionSubsystem OnRunStarted handler (bound in
     * BeginPlay): one accepted StartRoom re-loads the profile final stats
     * (the "enter the dungeon" load point) and restores the pool to the
     * CURRENT max via ResetHealth - a fresh run opens with a full pool, while
     * ordinary equipment changes never heal.
     */
    void HandleRoomRunStarted();
    /**
     * M5-020: builds the session catalog values once (the combat catalog from
     * the six Data/CombatSystem source tables via the M5-005 loader and the
     * M5-007 builder; the item catalog from the production items source) and
     * mounts them into the weapon component. Only reached with a HUD and a
     * profile present (bare test worlds never parse anything). Any failure
     * (missing source, parse/build/mount refusal) latches the component's
     * explicit catalog-error state: firing stays disabled, never a fallback
     * weapon. Idempotent: the second call returns the first result.
     */
    bool EnsureWeaponCatalogsMounted();
    // M1-012: builds the mapping context and actions exactly once (guarded by
    // Mapping != nullptr); re-setup (re-possess) reuses them.
    void EnsureCombatInputActions();
    // M5-033: the per-frame input-layer duties (hold/burst polling, the
    // reload deadline poll and the live-projectile motion advance).
    void TickWeaponInput(float DeltaSeconds);
    // M5-033: one fire-transaction poll (hold frame or burst sub-shot): the
    // local TryFire plus the same-frame ReleaseFire (the M5-023 hold-mode
    // convention - hitscan/projectile commits resolve instantly).
    void PollWeaponFire();
    // M5-033: the delivery bridge (the OnShotCommitted consumer 026..028
    // delegated to this card): plans the shot pattern and delivers it through
    // the M5-026 executor (hitscan) or the M5-027/028 projectile service.
    void HandleWeaponShotCommitted(const FShotContext& Shot);
    // M5-033: the BeginPlay wiring pass (gates, fire source, ledger bind, the
    // shot subscription); guarded to run exactly once per pawn.
    void WireWeaponMount();
    // M5-033: the injected input-layer clock seconds (provider, world, 0.0).
    double ResolveWeaponInputClockSeconds() const;
    // M5-033: the IHitscanTargetIdentity seam - the pawn resolves a hit actor
    // to its fire-registry entity id, lazily registering hostile bodies that
    // carry a health pool (walls stay unregistered environment).
    virtual FEntityId ResolveTargetEntityId(const AActor& HitActor) const override;
    void OnCombatLightPressed();
    void OnCombatLauncherPressed();
    // M1-040: per-slot Started handlers (one member per action keeps the
    // member-pointer binding form, so ClearBindingsForObject keeps covering
    // every skill-slot binding on a re-setup).
    void OnSkillSlot1Pressed();
    void OnSkillSlot2Pressed();
    void OnSkillSlot3Pressed();
    void OnSkillSlot4Pressed();
    void OnSkillSlot5Pressed();
    void OnSkillSlot6Pressed();
    void OnSkillSlot7Pressed();
    void OnSkillSlot8Pressed();
    // M1-030: single component owning the fixed side-view rig and the ground
    // anchor follow (replaces the M0 CameraBoom/Camera pair).
    UPROPERTY(VisibleAnywhere) TObjectPtr<USideViewCameraComponent> CameraRig;
    // M1-012: combat lifecycle component (M1-011); intents buffer here.
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCombatComponent> Combat;
    // M1-032: owns attack montage playback; follows Combat's
    // Started/Finished events plus a per-tick snapshot fallback.
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCombatPresentationComponent> CombatPresentation;
    // M5-020: the production weapon mount (session binding registry + ammo
    // model + mounted catalogs live inside).
    UPROPERTY(VisibleAnywhere) TObjectPtr<UWeaponComponent> WeaponMount;
    // M2-004: the pawn's own health pool (MaxHP 100), created as a default
    // subobject exactly like the M2-002 enemies carry theirs. Damage must go
    // through its ApplyDamage; its OnDied event drives HandlePlayerDied.
    UPROPERTY(VisibleAnywhere) TObjectPtr<UHealthComponent> Health;
    UPROPERTY() TObjectPtr<UInputMappingContext> Mapping;
    UPROPERTY() TObjectPtr<UInputAction> HorizontalAction;
    UPROPERTY() TObjectPtr<UInputAction> DepthAction;
    UPROPERTY() TObjectPtr<UInputAction> JumpAction;
    UPROPERTY() TObjectPtr<UInputAction> ResetAction;
    // M1-012: combat actions (J = Light, K = Launcher); created with the M0
    // runtime-action pattern, only the Started event is bound.
    UPROPERTY(Transient) TObjectPtr<UInputAction> CombatLightAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> CombatLauncherAction;
    // M1-028: F1 debug-overlay toggle action (same runtime-action pattern).
    UPROPERTY(Transient) TObjectPtr<UInputAction> DebugToggleAction;
    // M1-040: the eight DNF skill-slot actions in slot order (Q W E R A S D F
    // = slot 1..8); created with the same runtime-action pattern.
    UPROPERTY(Transient) TArray<TObjectPtr<UInputAction>> SkillSlotActions;
    FVector SpawnLocation;
    // M1-027: the facing captured with the spawn point; the unified reset
    // restores it alongside the position (interface contract section 6).
    FRotator SpawnRotation = FRotator::ZeroRotator;
    // M1-027: the registered session reset service (weak: a destroyed service
    // falls the R-key path back to the M0 local reset); empty = fallback.
    TWeakObjectPtr<UTrainingResetService> TrainingResetService;
    // M2-016: the room session subsystem the Tick injects the session clock
    // into, resolved through the owning world and held weakly (re-resolved
    // when the pointer expired or the world changed; a world-less pawn skips
    // the injection entirely).
    TWeakObjectPtr<URoomSessionSubsystem> RoomSessionPtr;
    // M3-010: the profile subsystem resolved once in BeginPlay through the
    // owning game instance and held weakly (the subsystem outlives the pawn;
    // a game-instance-less pawn - bare test worlds - keeps the component
    // defaults and no equip surface, the documented graceful degradation).
    TWeakObjectPtr<UProfileSubsystem> ProfilePtr;
    // M5-020: the session catalog VALUES the weapon component mounts (the
    // component holds non-owning pointers into these; both die with the pawn,
    // so the pointers cannot dangle). Built lazily by
    // EnsureWeaponCatalogsMounted exactly once per pawn.
    FCombatCatalog WeaponCatalogValue;
    FItemDefinitionCatalog WeaponItemCatalogValue;
    bool bWeaponCatalogsBuilt = false;
    bool bWeaponCatalogsMounted = false;
    // M1-029: accumulated planar axis input; applied centrally in Tick.
    UE::UEMMO::Tasks::M1_029::FPlanarAxisState PlanarAxes;
    // M3-023: the move-axis DIRECTION last logged (0 = released). The
    // continuous axis values are never logged per frame - only the
    // transitions (release, press, sign flip) produce one input row each.
    float LastLoggedMoveX = 0.0f;
    float LastLoggedMoveY = 0.0f;
    // M1-012: next combat input sequence; strictly increases per submitted intent.
    uint64 NextCombatInputSequence = 1;
    // M1-040: per-slot press counters (slot 1..8 at index 0..7); the skill
    // slots have no combat effect yet, so this counter is their surface.
    int32 SkillSlotPressCounts[8] = {};
    // M2-004: one PlayerDied broadcast per death lifecycle; cleared by the
    // unified reset (revive), so a revived pawn can die and broadcast again.
    bool bPlayerDiedBroadcast = false;
    // M2-004: the locomotion AnimBP class captured at death so the revive can
    // re-attach it (death detaches the animation drive; reset restores it).
    UPROPERTY(Transient) TObjectPtr<UClass> SavedAnimInstanceClass = nullptr;
    // -- M5-033: the input-layer weapon wiring -----------------------------
    // The T reload action (the Q trigger stays SkillSlotActions[0], rebound
    // onto the fire handlers; the skill-slot 1 intent keeps its fallback).
    UPROPERTY(Transient) TObjectPtr<UInputAction> WeaponReloadAction;
    // True between the fire-authorized Q press and its release.
    bool bWeaponFireHeld = false;
    // The caller-local monotonic shot counter feeding FFireIntent (association
    // only; the common ShotId comes from the fire registry).
    uint64 NextLocalShotSequence = 1;
    // The binding generation the delivery bridge's stale-callback checks key on.
    uint64 BoundWeaponGeneration = 0;
    // The input-layer reload window: opened by T (through the component's real
    // BeginReload), completed by the Tick deadline poll. The model's reload
    // bookkeeping keys on the component's private per-cycle slot id, so the
    // input layer tracks the deadline itself (the M5-021 model exposes no
    // external reload-window query keyed by weapon instance).
    bool bWeaponReloadPending = false;
    double WeaponReloadDeadlineSeconds = 0.0;
    // The injected input-layer clock; empty = the owner world clock.
    TFunction<double()> WeaponInputClockProvider;
    // The pawn-owned pacing/delivery policies registered through SetFirePolicy
    // by ApplyWeaponBindWiring (the card owns the instances, per the M5-024
    // contract "the input layer M5-033 owns the instance").
    TUniquePtr<FAutomaticFirePolicy> AutoFirePolicy;
    TUniquePtr<FBurstFirePolicy> BurstFirePolicy;
    // The concrete delivery policy is defined in the .cpp (pawn-local); the
    // member stores the interface the component consumes.
    TUniquePtr<IFirePolicy> DeliveryFirePolicy;
    // The world-lifetime projectile service (M5-027) and the pawn's production
    // spawner; created in BeginPlay / lazily at the first commit.
    TUniquePtr<FProjectileWorldService> ProjectileService;
    TUniquePtr<IProjectileSpawner> ProjectileSpawner;
    // One live pellet: its motion policy (the M5-028 linear reference policy)
    // plus the bound actor; advanced once per pawn tick until finished.
    struct FLiveWeaponProjectile
    {
        FLinearProjectilePolicy Policy;
        TWeakObjectPtr<ACombatProjectile> Actor;
    };
    TArray<FLiveWeaponProjectile> LiveWeaponProjectiles;
    // The pawn's hit ledger, bound to the mount's fire registry (the bridge
    // the WeaponComponent class comment documents as the future unified
    // adjudication injection point).
    FHitLedger ShotHitLedger;
    // The hit-actor -> fire-registry entity id cache backing the
    // IHitscanTargetIdentity seam; mutable (the lazy registration happens
    // inside the const resolver mid-delivery).
    mutable TMap<TWeakObjectPtr<const AActor>, FEntityId> ShotTargetEntityIds;
    // The WireWeaponMount once-guard.
    bool bWeaponMountWired = false;
};
