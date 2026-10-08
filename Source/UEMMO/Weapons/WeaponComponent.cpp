#include "WeaponComponent.h"

#include "../Items/ItemDefinition.h"

#include "FirePolicies/SingleFirePolicy.h"

namespace
{
	/** The human-readable text of a registry refusal (the M5-019 named reasons). */
	const TCHAR* LexBindingReject(EWeaponBindingRejectReason Reason)
	{
		switch (Reason)
		{
		case EWeaponBindingRejectReason::InvalidInstance: return TEXT("invalid instance identity");
		case EWeaponBindingRejectReason::UnknownItemDefinition: return TEXT("unknown item definition");
		case EWeaponBindingRejectReason::SlotNotWeapon: return TEXT("item slot is not Weapon");
		case EWeaponBindingRejectReason::NoWeaponMapping: return TEXT("item definition carries no weapon mapping");
		case EWeaponBindingRejectReason::UnknownWeaponDefinition: return TEXT("unknown weapon definition");
		case EWeaponBindingRejectReason::DuplicateInstanceBinding: return TEXT("duplicate active instance binding");
		default: return TEXT("unknown refusal");
		}
	}
}

UWeaponComponent::UWeaponComponent()
	: UActorComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	OwnedDefaultPolicy = MakeUnique<FSingleFirePolicy>();
	ActivePolicy = OwnedDefaultPolicy.Get();
}

bool UWeaponComponent::MountCatalogs(const FCombatCatalog* InWeaponCatalog, const FItemDefinitionCatalog* InItemCatalog, FString* OutError)
{
	// Named refusal + explicit latch, and the previously mounted pair is never
	// swapped: firing stays disabled until a healthy mount replaces the state.
	const auto Refuse = [this, OutError](const TCHAR* Detail)
	{
		bCatalogError = true;
		if (OutError)
		{
			*OutError = Detail;
		}
		return false;
	};

	if (InWeaponCatalog == nullptr || InItemCatalog == nullptr)
	{
		return Refuse(TEXT("MountCatalogs refused: both the weapon catalog and the item definition catalog are required"));
	}
	if (InWeaponCatalog->GetConfigRevision().IsEmpty())
	{
		return Refuse(TEXT("MountCatalogs refused: the weapon catalog was never built (empty ConfigRevision)"));
	}

	WeaponCatalog = InWeaponCatalog;
	ItemCatalog = InItemCatalog;
	MountedRevision = InWeaponCatalog->GetConfigRevision();
	bCatalogError = false;
	return true;
}

FWeaponMountOutcome UWeaponComponent::ApplyEquippedWeapon(const FItemInstance* WeaponInstance)
{
	FWeaponMountOutcome Outcome;

	// Gate 1 - the catalog pair. A latched catalog error (refused mount,
	// absent sources) keeps firing explicitly disabled: no fallback weapon, no
	// fake success ("缺武器不是假成功").
	if (bCatalogError || WeaponCatalog == nullptr || ItemCatalog == nullptr)
	{
		Outcome.Reject = EWeaponMountReject::CatalogUnavailable;
		Outcome.RejectDetail = TEXT("ApplyEquippedWeapon refused: no healthy catalog pair is mounted");
		return Outcome;
	}

	// The switch semantics: the previous binding tears down FIRST (unbind
	// parks the rounds, an open reload window closes without transferring, the
	// generation bumps). A refused request after that leaves the mount
	// explicitly unbound - never half-bound.
	TeardownActiveBinding();

	// The explicit "no weapon" state (empty slot / unequip).
	if (WeaponInstance == nullptr)
	{
		Outcome.bSucceeded = true;
		return Outcome;
	}

	// Resolve the item->weapon chain BEFORE touching the registry, so every
	// mount-level refusal below leaves the registry completely unchanged. A
	// broken chain stays with the registry's own named refusal (the
	// BindingRefused order slot).
	const FItemDefinition* ItemDefinition = ItemCatalog->Find(WeaponInstance->DefinitionId);
	const FWeaponDefinition* WeaponDefinition = nullptr;
	if (ItemDefinition != nullptr && ItemDefinition->Slot == EItemSlot::Weapon && !ItemDefinition->WeaponDefinitionId.IsNone())
	{
		WeaponDefinition = WeaponCatalog->FindWeapon(ItemDefinition->WeaponDefinitionId);
	}

	if (WeaponDefinition != nullptr && WeaponDefinition->FireMode == EWeaponFireMode::Melee)
	{
		// The melee bind must carry exactly the legacy four-attack set: a
		// configured deviation is a named refusal, never silently ignored
		// ("配置不支持的AttackSet明确拒绝"). The X/Z chain itself stays legacy.
		const TArray<FName>& LegacySet = GetLegacyMeleeAttackIds();
		bool bAttackSetSupported = WeaponDefinition->MeleeAttackIds.Num() == LegacySet.Num();
		if (bAttackSetSupported)
		{
			for (const FName LegacyId : LegacySet)
			{
				if (!WeaponDefinition->MeleeAttackIds.Contains(LegacyId))
				{
					bAttackSetSupported = false;
					break;
				}
			}
		}
		if (!bAttackSetSupported)
		{
			Outcome.Reject = EWeaponMountReject::UnsupportedMeleeAttackSet;
			Outcome.RejectDetail = FString::Printf(
				TEXT("ApplyEquippedWeapon refused: melee weapon '%s' carries %d attack ids; exactly the legacy four-attack set is supported"),
				*ItemDefinition->WeaponDefinitionId.ToString(),
				WeaponDefinition->MeleeAttackIds.Num());
			return Outcome;
		}
	}
	else if (WeaponDefinition != nullptr)
	{
		// A ranged weapon must reference an ammo kind the catalog defines. The
		// shared reserve account provisions lazily and EMPTY (supply belongs to
		// the later lineage - mounting never mints ammunition).
		const FAmmoType* AmmoType = WeaponCatalog->FindAmmoType(WeaponDefinition->AmmoId);
		if (AmmoType == nullptr)
		{
			Outcome.Reject = EWeaponMountReject::AmmoUnavailable;
			Outcome.RejectDetail = FString::Printf(
				TEXT("ApplyEquippedWeapon refused: ranged weapon '%s' references unknown ammo kind '%s'"),
				*WeaponDefinition->WeaponId.ToString(),
				*WeaponDefinition->AmmoId.ToString());
			return Outcome;
		}
		if (Ammo.GetReserveRounds(WeaponDefinition->AmmoId) < 0)
		{
			FString AmmoError;
			if (!Ammo.RegisterAmmoType(WeaponDefinition->AmmoId, AmmoType->MaxReserve, 0, &AmmoError))
			{
				Outcome.Reject = EWeaponMountReject::AmmoUnavailable;
				Outcome.RejectDetail = FString::Printf(
					TEXT("ApplyEquippedWeapon refused: the reserve pool of '%s' could not be provisioned (%s)"),
					*WeaponDefinition->AmmoId.ToString(),
					*AmmoError);
				return Outcome;
			}
		}
	}

	// Bind through the M5-019 registry: identity preserved verbatim, named
	// refusal over the whole item->weapon chain.
	const FWeaponBindOutcome Bind = Registry.BindInstance(*WeaponInstance, *ItemCatalog, *WeaponCatalog);
	if (!Bind.bBound)
	{
		Outcome.Reject = EWeaponMountReject::BindingRefused;
		Outcome.RejectDetail = FString::Printf(TEXT("ApplyEquippedWeapon refused: %s (%s)"),
			LexBindingReject(Bind.RejectReason), *Bind.RejectDetail);
		return Outcome;
	}

	// The live magazine account (M5-021) provisions lazily: an initialized
	// slot survives a switch untouched ("切装备不初始化新弹药"), and a first
	// bind starts the slot EMPTY - rounds enter through the reload/supply
	// lineage while the registry record keeps the M5-019 first-bind-full book.
	if (Bind.Record.FireMode != EWeaponFireMode::Melee && WeaponDefinition != nullptr)
	{
		if (Ammo.FindMagazine(Bind.Record.InstanceId) == nullptr)
		{
			FString MagazineError;
			if (WeaponDefinition->MagazineSize < 1 ||
				!Ammo.InitializeInstance(Bind.Record.InstanceId, WeaponDefinition->AmmoId, WeaponDefinition->MagazineSize, 0, &MagazineError))
			{
				// Degenerate magazine configuration: the named last-line refusal
				// rolls the just-made bind back (no half-bound mount).
				Registry.UnbindInstance(Bind.Record.InstanceId);
				Outcome.Reject = EWeaponMountReject::AmmoUnavailable;
				Outcome.RejectDetail = FString::Printf(
					TEXT("ApplyEquippedWeapon refused: the magazine of '%s' could not be provisioned (%s)"),
					*WeaponDefinition->WeaponId.ToString(),
					MagazineError.IsEmpty() ? TEXT("MagazineSize must be a positive value") : *MagazineError);
				return Outcome;
			}
		}
	}

	ActiveInstanceId = Bind.Record.InstanceId;
	Outcome.bSucceeded = true;
	Outcome.bWeaponBound = true;
	Outcome.Record = Bind.Record;
	return Outcome;
}

void UWeaponComponent::NotifyOwnerDied()
{
	// The death teardown: an open reload window closes without transferring
	// and pending callbacks lose their effect (generation bump). The binding
	// identity survives - the revive re-entry re-applies the equipment.
	if (ActiveInstanceId.IsValid())
	{
		Ammo.CancelReload(ActiveInstanceId);
	}
	// M5-022: the per-cycle window closes, the in-flight shot voids (the
	// policy bookkeeping drops with it) and the cooldown resets - the revive
	// re-entry fires without a stale pacing debt.
	CloseActiveReloadWindow();
	if (bShotInFlight)
	{
		ActivePolicy->AbortReservation(LastShotContext);
		bShotInFlight = false;
	}
	NextFireTimeSeconds = 0.0;
	++BindingGeneration;
	bOwnerAlive = false;
}

void UWeaponComponent::NotifyOwnerRevived()
{
	bOwnerAlive = true;
}

const FWeaponBindingRecord* UWeaponComponent::GetActiveBinding() const
{
	return Registry.FindBinding(ActiveInstanceId);
}

bool UWeaponComponent::IsFireAuthorized() const
{
	// Only a bound ranged weapon on a healthy catalog pair with a living
	// wielder authorizes component fire; melee keeps the legacy X/Z chain and
	// never this flag. A catalog error, an unbound mount or a dead wielder all
	// disable firing explicitly.
	if (bCatalogError || WeaponCatalog == nullptr || ItemCatalog == nullptr || !bOwnerAlive)
	{
		return false;
	}
	const FWeaponBindingRecord* Binding = Registry.FindBinding(ActiveInstanceId);
	return Binding != nullptr && Binding->FireMode != EWeaponFireMode::Melee;
}

void UWeaponComponent::TeardownActiveBinding()
{
	if (ActiveInstanceId.IsValid() && Registry.FindBinding(ActiveInstanceId) != nullptr)
	{
		// Unbind parks the magazine rounds; an open reload window closes
		// without transferring ("切换/死亡取消不得转移弹药").
		Registry.UnbindInstance(ActiveInstanceId);
		Ammo.CancelReload(ActiveInstanceId);
	}
	// Every apply boundary bumps the generation: callbacks captured before
	// the boundary must drop their work (IsBindingGenerationCurrent).
	// M5-022: the per-cycle reload window closes, the in-flight shot voids
	// (the policy bookkeeping drops with it) and the fire books reset for the
	// next binding.
	CloseActiveReloadWindow();
	if (bShotInFlight)
	{
		ActivePolicy->AbortReservation(LastShotContext);
		bShotInFlight = false;
	}
	bHasCommittedShot = false;
	LastCommittedLocalSequence = 0;
	NextFireTimeSeconds = 0.0;
	ActiveInstanceId.Invalidate();
	++BindingGeneration;
}

// ---------------------------------------------------------------------------
// M5-022: the single-fire transaction. The gate chain refuses with named
// reasons and zero side effects; the commit path reserves -> deducts ->
// allocates, so a post-deduction allocation failure rolls the round back and
// every observed failure keeps the books untouched.
// ---------------------------------------------------------------------------

FEntityId UWeaponComponent::RegisterFireSource(const FCombatEntityMetadata& Metadata)
{
	// The owner (or a logic-only null actor for a standalone mount) enters the
	// fire-lineage registry of the current generation.
	const FEntityId Minted = FireRegistry.RegisterEntity(GetOwner(), Metadata, FireRegistry.GetCurrentEpoch());
	if (IsValidCombatEntityId(Minted))
	{
		FireSourceEntityId = Minted;
	}
	return Minted;
}

void UWeaponComponent::SetFirePolicy(IFirePolicy* Policy)
{
	ActivePolicy = Policy != nullptr ? Policy : OwnedDefaultPolicy.Get();
}

void UWeaponComponent::SetFireClockProvider(TFunction<double()> Provider)
{
	FireClockProvider = MoveTemp(Provider);
}

void UWeaponComponent::SetMenuOpenPredicate(TFunction<bool()> Predicate)
{
	MenuOpenPredicate = MoveTemp(Predicate);
}

void UWeaponComponent::SetHitStunPredicate(TFunction<bool()> Predicate)
{
	HitStunPredicate = MoveTemp(Predicate);
}

double UWeaponComponent::ResolveFireClockSeconds() const
{
	if (FireClockProvider)
	{
		return FireClockProvider();
	}
	const UWorld* World = GetWorld();
	return World != nullptr ? World->GetTimeSeconds() : 0.0;
}

double UWeaponComponent::GetRemainingReloadSeconds(double NowSeconds) const
{
	// The window lives on the per-cycle model slot (see BeginReload); an
	// invalid slot id reads as 0.0 - the model's "no window" answer.
	return Ammo.GetRemainingReloadSeconds(ActiveReloadSlotId, NowSeconds);
}

void UWeaponComponent::CloseActiveReloadWindow()
{
	if (ActiveReloadSlotId.IsValid())
	{
		// The cancellation moves nothing ("切换/死亡取消不得转移弹药").
		Ammo.CancelReload(ActiveReloadSlotId);
		ActiveReloadSlotId.Invalidate();
	}
}

FFireOutcome UWeaponComponent::TryFire(const FFireIntent& Intent)
{
	FFireOutcome Outcome;

	// Gate 1 - the catalog pair (the M5-020 latch).
	if (bCatalogError || WeaponCatalog == nullptr || ItemCatalog == nullptr)
	{
		Outcome.Reject = EFireReject::CatalogUnavailable;
		Outcome.RejectDetail = TEXT("TryFire refused: no healthy catalog pair is mounted");
		return Outcome;
	}

	// Gate 2 - an active ranged binding; melee keeps the legacy X/Z chain and
	// never fires through this path.
	const FWeaponBindingRecord* Binding = Registry.FindBinding(ActiveInstanceId);
	if (Binding == nullptr)
	{
		Outcome.Reject = EFireReject::NoWeaponBound;
		Outcome.RejectDetail = TEXT("TryFire refused: no weapon binding is active");
		return Outcome;
	}
	if (Binding->FireMode == EWeaponFireMode::Melee)
	{
		Outcome.Reject = EFireReject::NoWeaponBound;
		Outcome.RejectDetail = TEXT("TryFire refused: the bound weapon is melee (the legacy X/Z chain owns it)");
		return Outcome;
	}

	// Gate 3/4/5 - the wielder and the injected state gates (production wiring
	// belongs to M5-033/034; unbound predicates read as clear).
	if (!bOwnerAlive)
	{
		Outcome.Reject = EFireReject::OwnerDead;
		Outcome.RejectDetail = TEXT("TryFire refused: the wielder is dead");
		return Outcome;
	}
	if (MenuOpenPredicate && MenuOpenPredicate())
	{
		Outcome.Reject = EFireReject::MenuOpen;
		Outcome.RejectDetail = TEXT("TryFire refused: the menu is open");
		return Outcome;
	}
	if (HitStunPredicate && HitStunPredicate())
	{
		Outcome.Reject = EFireReject::HitStunned;
		Outcome.RejectDetail = TEXT("TryFire refused: the wielder is hit-stunned");
		return Outcome;
	}

	// Gate 6 - the open reload window.
	if (ActiveReloadSlotId.IsValid() && Ammo.IsReloading(ActiveReloadSlotId))
	{
		Outcome.Reject = EFireReject::Reloading;
		Outcome.RejectDetail = TEXT("TryFire refused: a reload window is open");
		return Outcome;
	}

	// Gate 7 - the pacing cooldown on the injected fire clock.
	const double Now = ResolveFireClockSeconds();
	if (Now < NextFireTimeSeconds)
	{
		Outcome.Reject = EFireReject::CooldownActive;
		Outcome.RejectDetail = FString::Printf(
			TEXT("TryFire refused: the pacing window runs until %.3f (now %.3f)"), NextFireTimeSeconds, Now);
		return Outcome;
	}

	// Gate 8 - the in-flight shot (ReleaseFire ends the semiauto window).
	if (bShotInFlight)
	{
		Outcome.Reject = EFireReject::ShotInProgress;
		Outcome.RejectDetail = TEXT("TryFire refused: a committed shot is still in flight");
		return Outcome;
	}

	// Gate 9 - the intent identity: zero sequence, foreign instance, or a
	// repeated local sequence is the named refusal ("重复意图不多发").
	if (Intent.LocalShotSequence == 0)
	{
		Outcome.Reject = EFireReject::StaleIntent;
		Outcome.RejectDetail = TEXT("TryFire refused: the local shot sequence must be a positive value");
		return Outcome;
	}
	if (Intent.WeaponInstanceId != ActiveInstanceId)
	{
		Outcome.Reject = EFireReject::StaleIntent;
		Outcome.RejectDetail = TEXT("TryFire refused: the intent does not name the active binding");
		return Outcome;
	}
	if (bHasCommittedShot && Intent.LocalShotSequence <= LastCommittedLocalSequence)
	{
		Outcome.Reject = EFireReject::DuplicateIntent;
		Outcome.RejectDetail = FString::Printf(
			TEXT("TryFire refused: the local sequence %llu repeats or regresses the committed %llu"),
			Intent.LocalShotSequence, LastCommittedLocalSequence);
		return Outcome;
	}

	// Gate 10 - the fire book (the M5-019 slot; firing the LAST round is legal
	// and empties the book).
	if (Binding->LoadedRounds <= 0)
	{
		Outcome.Reject = EFireReject::MagazineEmpty;
		Outcome.RejectDetail = TEXT("TryFire refused: the fire book is empty");
		return Outcome;
	}

	// Gate 11 - the weapon definition (catalog drift defense).
	const FWeaponDefinition* Definition = WeaponCatalog->FindWeapon(Binding->WeaponDefinitionId);
	if (Definition == nullptr)
	{
		Outcome.Reject = EFireReject::CatalogUnavailable;
		Outcome.RejectDetail = FString::Printf(
			TEXT("TryFire refused: the bound weapon '%s' no longer resolves in the mounted catalog"),
			*Binding->WeaponDefinitionId.ToString());
		return Outcome;
	}

	// The candidate context; the ShotId stays pending until the allocation.
	FShotContext Candidate;
	Candidate.Epoch = FireRegistry.GetCurrentEpoch();
	Candidate.SourceEntityId = FireSourceEntityId;
	Candidate.LocalShotSequence = Intent.LocalShotSequence;
	Candidate.WeaponInstanceId = Binding->InstanceId;
	Candidate.WeaponDefinitionId = Binding->WeaponDefinitionId;
	Candidate.FireMode = Binding->FireMode;
	Candidate.DamageProfileId = Definition->DamageProfileId;
	Candidate.AmmoId = Definition->AmmoId;
	Candidate.PelletCount = Definition->PelletCount;
	Candidate.ProjectileId = Definition->ProjectileId;
	Candidate.RangeCm = Definition->RangeCm;
	Candidate.SpreadDegrees = Definition->SpreadDegrees;
	Candidate.CommittedAtSeconds = Now;

	// Step 1 - the policy reservation: all projectile/raycast capacity is
	// pre-claimed BEFORE any state moves.
	const FCapacityReservation Reservation = ActivePolicy->ReserveShotCapacity(Candidate);
	if (!Reservation.bGranted)
	{
		Outcome.Reject = EFireReject::ReservationFailed;
		Outcome.RejectDetail = FString::Printf(
			TEXT("TryFire refused: the fire policy withheld the capacity (%s)"), *Reservation.RejectDetail);
		return Outcome;
	}

	// Step 2 - one guarded round deduction (the fire book).
	const int32 Loaded = Binding->LoadedRounds;
	if (!Registry.SetInstanceRounds(ActiveInstanceId, Loaded - 1))
	{
		ActivePolicy->AbortReservation(Candidate);
		Outcome.Reject = EFireReject::AmmoWriteFailed;
		Outcome.RejectDetail = TEXT("TryFire refused: the guarded magazine write refused the deduction");
		return Outcome;
	}

	// Step 3 - the common ActionSequence allocation (the M5-010 allocator of
	// the fire lineage). A refusal rolls the deduction back so the attempt
	// stays side-effect free ("生成预留失败不扣弹/不增sequence").
	const FShotId ShotId = FireRegistry.AllocateActionSequence(FireSourceEntityId, Candidate.Epoch);
	if (!IsValidCombatShotId(ShotId))
	{
		Registry.SetInstanceRounds(ActiveInstanceId, Loaded);
		ActivePolicy->AbortReservation(Candidate);
		Outcome.Reject = EFireReject::SequenceAllocationFailed;
		Outcome.RejectDetail = TEXT("TryFire refused: the fire registry refused the common ActionSequence allocation (register the fire source)");
		return Outcome;
	}

	// Commit: the shot is real. The full state lands BEFORE the broadcast so a
	// reentrant fire call sees the committed shot and refuses.
	Candidate.ShotId = ShotId;
	bShotInFlight = true;
	bHasCommittedShot = true;
	LastCommittedLocalSequence = Intent.LocalShotSequence;
	LastShotContext = Candidate;
	++CommittedShotCount;
	NextFireTimeSeconds = Now + 60.0 / FMath::Max(Definition->FireRateRpm, 1.0f);
	ActivePolicy->CommitShot(Candidate);
	OnShotCommitted.Broadcast(Candidate);

	Outcome.bFired = true;
	Outcome.Shot = Candidate;
	return Outcome;
}

void UWeaponComponent::ReleaseFire()
{
	if (bShotInFlight)
	{
		bShotInFlight = false;
		ActivePolicy->NotifyShotReleased(LastShotContext);
	}
}

FAmmoReloadOutcome UWeaponComponent::BeginReload(double NowSeconds, double DurationSeconds)
{
	FAmmoReloadOutcome Outcome;

	// The entry gates (the model's named vocabulary carries the refusal): an
	// active ranged binding on a living wielder with a healthy catalog.
	const FWeaponBindingRecord* Binding = Registry.FindBinding(ActiveInstanceId);
	if (bCatalogError || WeaponCatalog == nullptr || Binding == nullptr || Binding->FireMode == EWeaponFireMode::Melee)
	{
		Outcome.Error = EAmmoModelError::UnknownInstance;
		Outcome.ErrorDetail = TEXT("BeginReload refused: no active ranged weapon binding");
		return Outcome;
	}
	if (!bOwnerAlive)
	{
		Outcome.Error = EAmmoModelError::UnknownInstance;
		Outcome.ErrorDetail = TEXT("BeginReload refused: the wielder is dead");
		return Outcome;
	}
	if (ActiveReloadSlotId.IsValid() && Ammo.IsReloading(ActiveReloadSlotId))
	{
		Outcome.Error = EAmmoModelError::DuplicateReload;
		Outcome.ErrorDetail = TEXT("BeginReload refused: a reload window is already open");
		return Outcome;
	}

	// The fire book gates: a full book has nothing to fill.
	const int32 Loaded = Binding->LoadedRounds;
	if (Loaded >= Binding->MagazineCapacity)
	{
		Outcome.Error = EAmmoModelError::MagazineFull;
		Outcome.ErrorDetail = TEXT("BeginReload refused: the fire book is full");
		return Outcome;
	}

	// The weapon definition and its reserve pool (catalog drift defense).
	const FWeaponDefinition* Definition = WeaponCatalog->FindWeapon(Binding->WeaponDefinitionId);
	if (Definition == nullptr)
	{
		Outcome.Error = EAmmoModelError::UnknownAmmoType;
		Outcome.ErrorDetail = FString::Printf(
			TEXT("BeginReload refused: the bound weapon '%s' no longer resolves in the mounted catalog"),
			*Binding->WeaponDefinitionId.ToString());
		return Outcome;
	}
	if (Ammo.GetReserveRounds(Definition->AmmoId) <= 0)
	{
		Outcome.Error = EAmmoModelError::NoReserveAmmo;
		Outcome.ErrorDetail = TEXT("BeginReload refused: the shared reserve pool is empty");
		return Outcome;
	}

	// The fresh per-cycle slot: the M5-021 model has no deduction API, so a
	// repeated refill cycle mints a fresh slot identity per window whose
	// capacity is exactly the missing rounds (the stale slots are tiny and
	// bounded by the reload count). The granted transfer is therefore
	// min(capacity - loaded, reserve) - the model's own semantics.
	++ReloadCycle;
	ActiveReloadSlotId = FGuid(static_cast<uint32>(ReloadCycle), static_cast<uint32>(ReloadCycle >> 32), 0x4D35u, 0x0522u);
	if (!Ammo.InitializeInstance(ActiveReloadSlotId, Definition->AmmoId, Binding->MagazineCapacity - Loaded, 0))
	{
		Outcome.Error = EAmmoModelError::InvalidCapacity;
		Outcome.ErrorDetail = TEXT("BeginReload refused: the per-cycle magazine slot was refused");
		ActiveReloadSlotId.Invalidate();
		return Outcome;
	}

	Outcome = Ammo.BeginReload(ActiveReloadSlotId, NowSeconds, DurationSeconds);
	if (!Outcome.bSuccess)
	{
		// The window refused: drop the just-minted slot (no window, no state).
		ActiveReloadSlotId.Invalidate();
		return Outcome;
	}

	// Report the real fire book view, not the per-cycle slot's internals.
	Outcome.LoadedRounds = Loaded;
	return Outcome;
}

FAmmoReloadOutcome UWeaponComponent::CompleteReload()
{
	FAmmoReloadOutcome Outcome;

	// The named idempotent refusal without an open window.
	if (!ActiveReloadSlotId.IsValid() || !Ammo.IsReloading(ActiveReloadSlotId))
	{
		Outcome.Error = EAmmoModelError::NoActiveReload;
		Outcome.ErrorDetail = TEXT("CompleteReload refused: no reload window is open");
		return Outcome;
	}

	Outcome = Ammo.CompleteReload(ActiveReloadSlotId);
	ActiveReloadSlotId.Invalidate();
	if (!Outcome.bSuccess)
	{
		return Outcome;
	}

	// The fire book sync: the refill is the only path that ever raises the
	// M5-019 slot, and the grant cannot exceed the gap the slot was minted for.
	const FWeaponBindingRecord* Binding = Registry.FindBinding(ActiveInstanceId);
	if (Binding == nullptr)
	{
		Outcome.bSuccess = false;
		Outcome.Error = EAmmoModelError::UnknownInstance;
		Outcome.ErrorDetail = TEXT("CompleteReload refused: the fire book vanished mid-reload");
		return Outcome;
	}
	FString SyncError;
	if (!Registry.SetInstanceRounds(ActiveInstanceId, Binding->LoadedRounds + Outcome.RoundsTransferred, &SyncError))
	{
		Outcome.bSuccess = false;
		Outcome.Error = EAmmoModelError::UnknownInstance;
		Outcome.ErrorDetail = FString::Printf(TEXT("CompleteReload refused: the fire book sync failed (%s)"), *SyncError);
		return Outcome;
	}
	Outcome.LoadedRounds = Binding->LoadedRounds;
	return Outcome;
}

void UWeaponComponent::CancelReload(FName Reason)
{
	// The named cancel (contract section 3): best-effort close, nothing
	// transfers. The idempotent no-op case is an already-closed window.
	CloseActiveReloadWindow();
}

bool UWeaponComponent::GetLastShotContext(FShotContext& OutShot) const
{
	if (!bHasCommittedShot)
	{
		return false;
	}
	OutShot = LastShotContext;
	return true;
}
