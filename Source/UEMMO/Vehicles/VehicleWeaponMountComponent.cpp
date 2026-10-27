#include "VehicleWeaponMountComponent.h"

#include "SeatModel.h"
#include "../Weapons/FirePolicies/SingleFirePolicy.h"

namespace
{
	/**
	 * M5-048-lesson: the mount's capacity policy for the turret weapon. The
	 * weapon component's OWNED default is the single-fire policy, which grants
	 * hitscan shots only — the shipped explosive turret (mode=projectile)
	 * would be refused at the reservation. This mirrors the pawn-side
	 * FWeaponDeliveryFirePolicy (PrototypeCharacter.cpp file-local): projectile
	 * candidates grant PelletCount projectile slots, hitscan candidates grant
	 * raycast slots. Pacing-free by design: the component's fire-rate cooldown
	 * owns the cadence, and the release/abort faces are no-ops (no held slot).
	 */
	class FMountDeliveryFirePolicy final : public IFirePolicy
	{
	public:
		FCapacityReservation ReserveShotCapacity(const FShotContext& Candidate) override
		{
			FCapacityReservation Out;
			switch (Candidate.FireMode)
			{
			case EWeaponFireMode::Projectile:
				Out.bGranted = true;
				Out.ProjectileSlots = Candidate.PelletCount;
				break;
			case EWeaponFireMode::Hitscan:
				Out.bGranted = true;
				Out.RaycastSlots = Candidate.PelletCount;
				break;
			default:
				Out.RejectDetail = TEXT("melee weapons deliver through the legacy attack chain, not the fire transaction");
				break;
			}
			return Out;
		}

		void CommitShot(const FShotContext&) override {}
		void AbortReservation(const FShotContext&) override {}
		void NotifyShotReleased(const FShotContext&) override {}
	};

	/**
	 * Why the forwarded WeaponComponent::TryFire refused (the M5-022 named
	 * reasons) - the mount surfaces the cause verbatim instead of re-mapping
	 * it, so one log line keeps its meaning across the handheld and turret
	 * faces.
	 */
	const TCHAR* LexFireReject(EFireReject Reason)
	{
		switch (Reason)
		{
		case EFireReject::None: return TEXT("accepted");
		case EFireReject::NoWeaponBound: return TEXT("no ranged weapon bound");
		case EFireReject::CatalogUnavailable: return TEXT("catalog unavailable");
		case EFireReject::OwnerDead: return TEXT("the turret weapon is dead");
		case EFireReject::MenuOpen: return TEXT("menu open");
		case EFireReject::HitStunned: return TEXT("hit-stunned");
		case EFireReject::Reloading: return TEXT("a reload window is open");
		case EFireReject::CooldownActive: return TEXT("cooldown active");
		case EFireReject::MagazineEmpty: return TEXT("magazine empty");
		case EFireReject::DuplicateIntent: return TEXT("duplicate intent");
		case EFireReject::ShotInProgress: return TEXT("shot in flight");
		case EFireReject::StaleIntent: return TEXT("stale intent");
		case EFireReject::ReservationFailed: return TEXT("capacity reservation refused");
		case EFireReject::SequenceAllocationFailed: return TEXT("ActionSequence allocation refused");
		case EFireReject::AmmoWriteFailed: return TEXT("magazine write refused");
		default: return TEXT("unknown fire refusal");
		}
	}
}

UVehicleWeaponMountComponent::UVehicleWeaponMountComponent()
	: UActorComponent()
{
	// The mount is an API surface: every clock, input and delivery decision
	// enters through the M5-044 wiring, nothing ticks here.
	PrimaryComponentTick.bCanEverTick = false;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

FVehicleMountConfigOutcome UVehicleWeaponMountComponent::Configure(const FVehicleMountDefinition& MountDefinition, UWeaponComponent* InTurretWeapon)
{
	FVehicleMountConfigOutcome Outcome;

	// Gate 1 - the turret weapon component. Nothing downstream can resolve
	// without it (the weapon/ammo/angles all live on or behind it).
	if (InTurretWeapon == nullptr)
	{
		Outcome.Reject = EVehicleMountConfigReject::WeaponComponentMissing;
		Outcome.RejectDetail = TEXT("Configure refused: the turret weapon component is required (no weapon, no fire transaction)");
		return Outcome;
	}

	// Gate 2 - the vehicle body. The pivot offset (LocationCm) is relative to
	// the vehicle origin; a mount without an owner actor has no origin to hang
	// the turret from and no actor to register as the fire source.
	if (GetOwner() == nullptr)
	{
		Outcome.Reject = EVehicleMountConfigReject::OwnerActorMissing;
		Outcome.RejectDetail = TEXT("Configure refused: the mount has no owner actor (the turret pivot and the fire source need the vehicle body)");
		return Outcome;
	}

	// Gate 3 - the mount row itself (the M5-037 validator: ids present, pivot
	// finite and bounded, pitch clamp finite inside +/-89 with Min <= Max).
	FString DefinitionErrors;
	if (!ValidateVehicleMountDefinition(MountDefinition, DefinitionErrors))
	{
		Outcome.Reject = EVehicleMountConfigReject::MountDefinitionInvalid;
		Outcome.RejectDetail = FString::Printf(TEXT("Configure refused: the mount row failed validation (%s)"), *DefinitionErrors);
		return Outcome;
	}

	// Gate 4 - a healthy catalog pair on the turret weapon component (the
	// M5-020 latch). There is no fallback catalog and no default weapon.
	const FCombatCatalog* WeaponCatalog = InTurretWeapon->GetMountedWeaponCatalog();
	if (InTurretWeapon->IsCatalogError() || WeaponCatalog == nullptr)
	{
		Outcome.Reject = EVehicleMountConfigReject::CatalogUnavailable;
		Outcome.RejectDetail = TEXT("Configure refused: the turret weapon component carries no healthy mounted catalog pair");
		return Outcome;
	}

	// Gate 5 - the row's weapon id must resolve in the catalog the component
	// actually mounted ("缺武器" is a named, id-carrying refusal).
	const FWeaponDefinition* WeaponDefinition = WeaponCatalog->FindWeapon(MountDefinition.TurretWeaponId);
	if (WeaponDefinition == nullptr)
	{
		Outcome.Reject = EVehicleMountConfigReject::WeaponDefinitionMissing;
		Outcome.RejectDetail = FString::Printf(
			TEXT("Configure refused: the mount's TurretWeaponId '%s' does not resolve in the turret weapon component's mounted catalog"),
			*MountDefinition.TurretWeaponId.ToString());
		return Outcome;
	}

	// Gate 6 - a turret fires ranged weapons only (melee keeps the legacy X/Z
	// pawn chain; a melee turret would have no TryFire transaction at all).
	if (WeaponDefinition->FireMode == EWeaponFireMode::Melee)
	{
		Outcome.Reject = EVehicleMountConfigReject::MeleeTurretWeapon;
		Outcome.RejectDetail = FString::Printf(
			TEXT("Configure refused: the turret weapon '%s' is melee (turret mounts fire ranged weapons only)"),
			*WeaponDefinition->WeaponId.ToString());
		return Outcome;
	}

	// Gate 7 - the weapon's ammo kind must resolve in the same catalog (the
	// shared reserve/magazine lineage is the only ammunition path). A binding
	// through ApplyEquippedWeapon would refuse this first; the gate protects
	// the direct-Configure path where no binding exists yet.
	const FAmmoType* AmmoType = WeaponCatalog->FindAmmoType(WeaponDefinition->AmmoId);
	if (AmmoType == nullptr)
	{
		Outcome.Reject = EVehicleMountConfigReject::AmmoUnavailable;
		Outcome.RejectDetail = FString::Printf(
			TEXT("Configure refused: the turret weapon '%s' references unknown ammo kind '%s'"),
			*WeaponDefinition->WeaponId.ToString(),
			*WeaponDefinition->AmmoId.ToString());
		return Outcome;
	}

	// Commit: replace the previous configuration in place. The fire-source
	// registration is NOT touched here (RefreshVehicleFireSource owns it) so a
	// re-Configure never churns the fire lineage on its own.
	ConfiguredMount = MountDefinition;
	ConfiguredWeapon = *WeaponDefinition;
	TurretWeapon = InTurretWeapon;
	bConfigured = true;

	// M5-048-lesson: the turret weapon's mode-appropriate fire policy. The
	// component's owned default is the single-fire policy, which grants
	// hitscan shots only — a projectile turret (the shipped explosive row)
	// would be refused at the capacity reservation. Subscribe once and set
	// the policy per binding; Finalize unsubscribes.
	TurretWeapon->OnBindingApplied.RemoveAll(this);
	TurretWeapon->OnBindingApplied.AddUObject(this, &UVehicleWeaponMountComponent::HandleTurretBindingApplied);
	HandleTurretBindingApplied();

	Outcome.bConfigured = true;
	return Outcome;
}

void UVehicleWeaponMountComponent::HandleTurretBindingApplied()
{
	if (TurretWeapon == nullptr || !bConfigured || bFinalized)
	{
		return;
	}
	const FWeaponBindingRecord* Binding = TurretWeapon->GetActiveBinding();
	const FWeaponDefinition* Definition = (Binding != nullptr && TurretWeapon->GetMountedWeaponCatalog() != nullptr)
		? TurretWeapon->GetMountedWeaponCatalog()->FindWeapon(Binding->WeaponDefinitionId)
		: nullptr;
	if (Definition != nullptr && Definition->FireMode == EWeaponFireMode::Projectile)
	{
		if (TurretDeliveryPolicy == nullptr)
		{
			// Mirrors the pawn-side FWeaponDeliveryFirePolicy (the projectile
			// capacity grant lives in PrototypeCharacter.cpp as a file-local
			// class the mount cannot reach).
			TurretDeliveryPolicy = MakeUnique<FMountDeliveryFirePolicy>();
		}
		TurretWeapon->SetFirePolicy(TurretDeliveryPolicy.Get());
	}
	else
	{
		// Hitscan shots are granted by the component's owned single-fire
		// policy (the default); melee is refused at the transaction gate.
		TurretDeliveryPolicy.Reset();
		TurretWeapon->SetFirePolicy(nullptr);
	}
}

FString UVehicleWeaponMountComponent::GetAimClampSummary() const
{
	if (!bConfigured)
	{
		return TEXT("turret aim clamp: not configured");
	}
	// The M5-037 mount schema has no yaw field: horizontal traverse is free
	// (full circle) and only the pitch is clamped. A future yaw field changes
	// this summary together with the gate, never silently.
	return FString::Printf(
		TEXT("turret aim clamp: yaw free full-circle traverse (the M5-037 mount schema has no yaw limit field); pitch clamped to [%s, %s] degrees"),
		*FString::SanitizeFloat(ConfiguredMount.MinPitchDegrees),
		*FString::SanitizeFloat(ConfiguredMount.MaxPitchDegrees));
}

// ---------------------------------------------------------------------------
// Operator state (the frozen seat-state seam)
// ---------------------------------------------------------------------------

void UVehicleWeaponMountComponent::SetOperatorSeatSource(const FSeatOccupancyModel* SeatModel, FName InVehicleId, FName InSeatId)
{
	OperatorSeatModel = SeatModel;
	OperatorVehicleId = InVehicleId;
	OperatorSeatId = InSeatId;
}

void UVehicleWeaponMountComponent::SetOperatorPresenceProvider(TFunction<bool()> Provider)
{
	OperatorPresenceProvider = MoveTemp(Provider);
}

void UVehicleWeaponMountComponent::SetOperatorFactionProvider(TFunction<FName()> Provider)
{
	OperatorFactionProvider = MoveTemp(Provider);
}

void UVehicleWeaponMountComponent::SetOperatorFaction(FName Faction)
{
	// The fixed-faction convenience: one faction for as long as the wiring
	// leaves it set (the faction provider path overwrites it again).
	OperatorFactionProvider = [Faction]() { return Faction; };
}

void UVehicleWeaponMountComponent::SetVehicleFaction(FName Faction)
{
	VehicleFaction = Faction;
}

void UVehicleWeaponMountComponent::NotifyOperatorChanged()
{
	// The fire-source registration follows the operator so the existing
	// M5-026 faction filter reads the CURRENT operator faction (contract
	// section 8). No registration yet / finalized / mid-flight: the refresh
	// is either a no-op or explicitly skipped (see RefreshVehicleFireSource).
	if (!bConfigured || bFinalized || TurretWeapon == nullptr)
	{
		return;
	}
	RefreshVehicleFireSource(nullptr);
}

FVehicleMountOperatorState UVehicleWeaponMountComponent::ResolveOperator() const
{
	FVehicleMountOperatorState State;

	// Presence: the direct override wins (it exists for wiring without a seat
	// model); the injected seat model is the production source; with neither,
	// nobody is at the gun (fire stays forbidden - the safe default).
	if (OperatorPresenceProvider)
	{
		State.bPresent = OperatorPresenceProvider();
	}
	else if (OperatorSeatModel != nullptr)
	{
		FGuid OccupantId;
		State.bPresent = OperatorSeatModel->FindSeatOccupant(OperatorVehicleId, OperatorSeatId, OccupantId);
		State.OccupantId = OccupantId;
	}

	if (OperatorFactionProvider)
	{
		State.Faction = OperatorFactionProvider();
	}
	return State;
}

FName UVehicleWeaponMountComponent::ResolveFilterFaction() const
{
	// Contract section 8: the friendly-fire filter uses the operator faction
	// while an operator holds the seat, the vehicle faction without one. Both
	// empty = unaligned (never trips the existing filter - the M5-039 hull
	// precedent for a world without a faction source).
	const FVehicleMountOperatorState Operator = ResolveOperator();
	if (Operator.bPresent)
	{
		return Operator.Faction;
	}
	return VehicleFaction;
}

// ---------------------------------------------------------------------------
// Fire lineage (the vehicle is the firing source; the operator faction filters)
// ---------------------------------------------------------------------------

bool UVehicleWeaponMountComponent::RefreshVehicleFireSource(FString* OutError)
{
	if (OutError != nullptr)
	{
		OutError->Reset();
	}
	if (!bConfigured || bFinalized || TurretWeapon == nullptr)
	{
		if (OutError != nullptr)
		{
			*OutError = TEXT("RefreshVehicleFireSource refused: the mount is not configured (or already finalized)");
		}
		return false;
	}

	FCombatEntityRegistry& FireRegistry = TurretWeapon->GetFireRegistry();
	const FEntityId CurrentId = TurretWeapon->GetFireSourceEntityId();
	const FCombatEntityRecord* CurrentRecord = IsValidCombatEntityId(CurrentId)
		? FireRegistry.FindEntity(CurrentId)
		: nullptr;

	// The filter faction of THIS moment: operator when present, else vehicle
	// (contract section 8). The registration is what makes the existing
	// M5-026 faction filter (FindEntity(SourceEntityId)->Metadata.Faction)
	// read the operator faction without any new filter code.
	const FName DesiredFaction = ResolveFilterFaction();

	// A live, current-epoch registration with the desired faction is the
	// no-op face (the common case: repeated calls between operator changes).
	if (CurrentRecord != nullptr
		&& CurrentRecord->Epoch == FireRegistry.GetCurrentEpoch()
		&& CurrentRecord->Metadata.Faction == DesiredFaction)
	{
		VehicleFireSourceEntityId = CurrentId;
		return true;
	}

	// Mid-flight re-registration would orphan the committed shot's source
	// attribution; the wiring re-calls NotifyOperatorChanged after
	// ReleaseMountShot (an operator change without a faction change never
	// reaches this branch anyway).
	if (TurretWeapon->IsShotInFlight() && CurrentRecord != nullptr)
	{
		return true;
	}

	// Drop the previous registration first (a faction change keeps the live
	// actor; a world rebuild already wiped the record - that UnregisterEntity
	// refusal is the safe stale no-op). RegisterFireSource then re-enters the
	// vehicle with the fresh faction; a new fire-lineage id restarts its
	// sequence space at 1, which stays collision-free because the source id
	// is part of every event key (M5-010 semantics).
	if (CurrentRecord != nullptr)
	{
		FireRegistry.UnregisterEntity(CurrentId, FireRegistry.GetCurrentEpoch());
	}

	FCombatEntityMetadata SourceMetadata;
	SourceMetadata.Faction = DesiredFaction;
	SourceMetadata.Category = FName(TEXT("vehicle_turret"));
	const FEntityId Minted = TurretWeapon->RegisterFireSource(SourceMetadata);
	if (!IsValidCombatEntityId(Minted))
	{
		// The registration refused (the refusal reason stays on the registry
		// face); the mount stays explicitly unregistered - no fake source id.
		VehicleFireSourceEntityId = InvalidCombatEntityId;
		if (OutError != nullptr)
		{
			*OutError = FString::Printf(
				TEXT("RefreshVehicleFireSource refused: the turret weapon's fire registry refused the vehicle '%s' registration (owner actor or epoch face)"),
				*ConfiguredMount.VehicleId.ToString());
		}
		return false;
	}

	VehicleFireSourceEntityId = Minted;
	return true;
}

// ---------------------------------------------------------------------------
// The fire face
// ---------------------------------------------------------------------------

bool UVehicleWeaponMountComponent::EvaluateFireGates(const FVehicleMountAim& Aim, EVehicleMountFireReject& OutReject, FString& OutDetail) const
{
	// Gate 1 - the terminal state wins over everything (a destroyed vehicle
	// never fires again, not even to report a richer refusal).
	if (bFinalized)
	{
		OutReject = EVehicleMountFireReject::MountFinalized;
		OutDetail = TEXT("the mount is finalized (vehicle destroyed); it never fires again");
		return false;
	}

	// Gate 2 - the configuration.
	if (!bConfigured || TurretWeapon == nullptr)
	{
		OutReject = EVehicleMountFireReject::NotConfigured;
		OutDetail = TEXT("the mount is not configured (call Configure with a mount row and the turret weapon component)");
		return false;
	}

	// Gate 3 - the turret binding: the weapon component must hold an active
	// RANGED binding of exactly the row's TurretWeaponId. The bind itself is
	// the standard ApplyEquippedWeapon chain (no parallel mounting path); a
	// rebound/unequipped turret weapon is a named refusal, never a silent
	// fallback to whatever else happens to be bound.
	const FWeaponBindingRecord* Binding = TurretWeapon->GetActiveBinding();
	if (Binding == nullptr
		|| Binding->FireMode == EWeaponFireMode::Melee
		|| Binding->WeaponDefinitionId != ConfiguredMount.TurretWeaponId)
	{
		OutReject = EVehicleMountFireReject::TurretWeaponNotBound;
		OutDetail = FString::Printf(
			TEXT("the turret weapon holds no active ranged binding of '%s' (bind the turret weapon through ApplyEquippedWeapon first)"),
			*ConfiguredMount.TurretWeaponId.ToString());
		return false;
	}

	// Gate 4 - the fire lineage: a registered vehicle source that is a live
	// record of the CURRENT fire-registry generation. A world rebuild (old
	// epoch) wipes the record; the mount refuses with the named reason and
	// the wiring re-registers (RefreshVehicleFireSource) - the retry/
	// leave-room face of the M5-039 hull precedent.
	FCombatEntityRegistry& FireRegistry = TurretWeapon->GetFireRegistry();
	const FEntityId SourceId = TurretWeapon->GetFireSourceEntityId();
	if (!IsValidCombatEntityId(SourceId))
	{
		OutReject = EVehicleMountFireReject::FireSourceMissing;
		OutDetail = TEXT("no vehicle fire source is registered (call RefreshVehicleFireSource)");
		return false;
	}
	if (!FireRegistry.IsKnownEntityInEpoch(SourceId, FireRegistry.GetCurrentEpoch()))
	{
		OutReject = EVehicleMountFireReject::FireSourceStale;
		OutDetail = TEXT("the registered fire source is not a live record of the current fire-registry epoch (world rebuild; call RefreshVehicleFireSource)");
		return false;
	}

	// Gate 5 - the operator ("无操作者禁止玩家开火"): without a resolved
	// operator the player fire path is forbidden outright, zero side effects.
	if (!ResolveOperator().bPresent)
	{
		OutReject = EVehicleMountFireReject::NoOperator;
		OutDetail = TEXT("no operator occupies the operator seat (player fire is forbidden without an operator)");
		return false;
	}

	// Gate 6 - the aim must be finite (a NaN/Inf aim poisons every rotation
	// the delivery would build from it).
	if (!FMath::IsFinite(Aim.YawDegrees) || !FMath::IsFinite(Aim.PitchDegrees))
	{
		OutReject = EVehicleMountFireReject::AimInvalid;
		OutDetail = FString::Printf(
			TEXT("the aim yaw/pitch must be finite (got yaw=%s, pitch=%s)"),
			*FString::SanitizeFloat(Aim.YawDegrees), *FString::SanitizeFloat(Aim.PitchDegrees));
		return false;
	}

	// Gate 7 - the pitch clamp (REFUSE, never clamp into range). The yaw is
	// free today (no yaw field in the M5-037 schema; YawOutOfRange stays
	// reserved for that schema change) - see GetAimClampSummary.
	if (Aim.PitchDegrees < ConfiguredMount.MinPitchDegrees || Aim.PitchDegrees > ConfiguredMount.MaxPitchDegrees)
	{
		OutReject = EVehicleMountFireReject::PitchOutOfRange;
		OutDetail = FString::Printf(
			TEXT("the pitch %s is outside the turret clamp [%s, %s] degrees"),
			*FString::SanitizeFloat(Aim.PitchDegrees),
			*FString::SanitizeFloat(ConfiguredMount.MinPitchDegrees),
			*FString::SanitizeFloat(ConfiguredMount.MaxPitchDegrees));
		return false;
	}

	return true;
}

FVehicleMountFireCheck UVehicleWeaponMountComponent::CanFire(const FVehicleMountAim& Aim) const
{
	FVehicleMountFireCheck Check;
	Check.bCanFire = EvaluateFireGates(Aim, Check.Reject, Check.RejectDetail);
	return Check;
}

FVehicleMountFireOutcome UVehicleWeaponMountComponent::TryFire(const FVehicleMountAim& Aim)
{
	FVehicleMountFireOutcome Outcome;

	// The shared gate chain (named, side-effect free).
	if (!EvaluateFireGates(Aim, Outcome.Reject, Outcome.RejectDetail))
	{
		return Outcome;
	}

	// The forwarded transaction: the SAME WeaponComponent::TryFire a handheld
	// weapon uses - one binding, one magazine, one reserve pool, one common
	// ActionSequence allocator. The mount adds its own monotonic local
	// sequence (association only; strictly increasing across binding
	// generations is always admissible) and the aim snapshot for the M5-044
	// delivery.
	const FWeaponBindingRecord* Binding = TurretWeapon->GetActiveBinding();
	FFireIntent Intent;
	Intent.WeaponInstanceId = Binding->InstanceId;
	Intent.LocalShotSequence = NextLocalShotSequence;
	const FFireOutcome FireOutcome = TurretWeapon->TryFire(Intent);
	if (!FireOutcome.bFired)
	{
		// The transaction refused: surface the named cause verbatim (never
		// swallowed, never re-mapped) - the mount adds no second gate chain
		// behind its own.
		Outcome.Reject = EVehicleMountFireReject::WeaponRefused;
		Outcome.RejectDetail = FString::Printf(
			TEXT("the turret weapon transaction refused (%s): %s"),
			LexFireReject(FireOutcome.Reject), *FireOutcome.RejectDetail);
		return Outcome;
	}

	// Commit: the shot is real (round deducted, sequence minted, cooldown
	// started - all inside the weapon transaction). The context's source is
	// the VEHICLE fire source (shot attribution, contract section 8); the
	// operator stays this component's separate record.
	++NextLocalShotSequence;
	++MountedShotCount;
	LastCommittedAim = Aim;
	bHasCommittedAim = true;

	Outcome.bFired = true;
	Outcome.Shot = FireOutcome.Shot;
	return Outcome;
}

void UVehicleWeaponMountComponent::ReleaseMountShot()
{
	if (TurretWeapon != nullptr)
	{
		TurretWeapon->ReleaseFire();
	}
}

bool UVehicleWeaponMountComponent::GetLastCommittedAim(FVehicleMountAim& OutAim) const
{
	// The aim snapshot the M5-044 delivery aims the shot's pellets with.
	if (!bHasCommittedAim)
	{
		return false;
	}
	OutAim = LastCommittedAim;
	return true;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

bool UVehicleWeaponMountComponent::Finalize()
{
	// First terminal wins (the HitLedger/scene evaluator precedent): a second
	// call is the rejected no-op with no side effects.
	if (bFinalized)
	{
		return false;
	}
	bFinalized = true;

	// The weapon component's own cancel faces ("对齐 WeaponComponent 的取消
	// 面"): the named reload cancel closes any open window without
	// transferring rounds, and the death teardown voids the in-flight shot
	// (policy reservation aborted), resets the cooldown and bumps the binding
	// generation so stale callbacks drop their work.
	if (TurretWeapon != nullptr)
	{
		TurretWeapon->CancelReload(FName(TEXT("VehicleMountFinalized")));
		TurretWeapon->NotifyOwnerDied();
	}

	// The weak-reference drop: the injected seat model pointer and every
	// provider/closure die here, so nothing can reach the wiring's objects
	// past the vehicle teardown. The configuration snapshot stays readable
	// for diagnostics (plain values, no references).
	if (TurretWeapon != nullptr)
	{
		TurretWeapon->OnBindingApplied.RemoveAll(this);
		TurretWeapon->SetFirePolicy(nullptr);
	}
	TurretDeliveryPolicy.Reset();
	OperatorSeatModel = nullptr;
	OperatorPresenceProvider = TFunction<bool()>();
	OperatorFactionProvider = TFunction<FName()>();
	TurretWeapon = nullptr;
	return true;
}

void UVehicleWeaponMountComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Actor destruction (vehicle destroy, room teardown, world EndPlay) routes
	// through the same cleanup face as an explicit vehicle-destroy call.
	Finalize();
	Super::EndPlay(EndPlayReason);
}
