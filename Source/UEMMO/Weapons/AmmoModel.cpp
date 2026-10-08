#include "AmmoModel.h"

// M5-021: the magazine, shared reserve and reload model. See AmmoModel.h for
// the frozen guarantees: per-instance magazines keyed by ItemInstanceId,
// shared reserve pools keyed by ammo id, a two-phase reload that moves rounds
// exactly once on completion, and named refusals that leave the model
// completely unchanged.

namespace
{
	/** Fills one refusal outcome with its named cause. */
	FAmmoReloadOutcome MakeRefusal(EAmmoModelError Code, const FString& Detail)
	{
		FAmmoReloadOutcome Outcome;
		Outcome.bSuccess = false;
		Outcome.Error = Code;
		Outcome.ErrorDetail = Detail;
		return Outcome;
	}
}

bool FAmmoModel::RegisterAmmoType(FName AmmoId, int32 MaxReserve, int32 InitialReserve, FString* OutError)
{
	// Identity: an invalid id text cannot be referenced by weapon definitions
	// or save data.
	if (!IsValidDefinitionIdText(AmmoId.ToString()))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("InvalidAmmoId: AmmoId must be 1-64 characters of a-z0-9_ (got '%s')"),
				*AmmoId.ToString());
		}
		return false;
	}

	// Reserve bounds: a negative cap would silently mint ammunition; the
	// upper bounds keep source-table typos visible. The initial stock must fit
	// inside its own cap.
	if (MaxReserve < 0 || InitialReserve < 0)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("NegativeRounds: MaxReserve and InitialReserve must be non-negative (got MaxReserve=%d, InitialReserve=%d)"),
				MaxReserve, InitialReserve);
		}
		return false;
	}
	if (MaxReserve > MaxAmmoReserve)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("BeyondReserveLimit: MaxReserve must be 0..%d (got %d)"),
				MaxAmmoReserve, MaxReserve);
		}
		return false;
	}
	if (InitialReserve > MaxReserve)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("BeyondReserveLimit: InitialReserve must be 0..MaxReserve (got InitialReserve=%d, MaxReserve=%d)"),
				InitialReserve, MaxReserve);
		}
		return false;
	}

	// One pool per ammo kind: a duplicate registration would silently reset
	// the shared stock of a kind already in use.
	if (ReservePools.Contains(AmmoId))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("DuplicateRegistration: ammo kind '%s' is already registered"),
				*AmmoId.ToString());
		}
		return false;
	}

	FAmmoReservePool Pool;
	Pool.MaxReserve = MaxReserve;
	Pool.Rounds = InitialReserve;
	ReservePools.Add(AmmoId, Pool);
	return true;
}

bool FAmmoModel::InitializeInstance(const FGuid& InstanceId, FName AmmoId, int32 MagazineCapacity, int32 InitialLoaded, FString* OutError)
{
	// Identity: the all-zero guid is not an item instance identity.
	if (!InstanceId.IsValid())
	{
		if (OutError)
		{
			*OutError = TEXT("InvalidInstance: the instance id must not be the all-zero guid");
		}
		return false;
	}

	// Cross reference: the magazine must name a registered reserve pool kind.
	if (!ReservePools.Contains(AmmoId))
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("UnknownAmmoType: ammo kind '%s' is not registered"),
				*AmmoId.ToString());
		}
		return false;
	}

	// Capacity: a magazine that cannot hold a round is meaningless; the upper
	// bound keeps caller typos visible.
	if (MagazineCapacity < 1 || MagazineCapacity > MaxAmmoMagazineSize)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("InvalidCapacity: MagazineCapacity must be 1..%d (got %d)"),
				MaxAmmoMagazineSize, MagazineCapacity);
		}
		return false;
	}

	// Load bounds: the explicit initial load must fit the magazine.
	if (InitialLoaded < 0)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("NegativeRounds: InitialLoaded must be non-negative (got %d)"), InitialLoaded);
		}
		return false;
	}
	if (InitialLoaded > MagazineCapacity)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("BeyondCapacity: InitialLoaded must be 0..MagazineCapacity (got InitialLoaded=%d, MagazineCapacity=%d)"),
				InitialLoaded, MagazineCapacity);
		}
		return false;
	}

	// One slot per instance: re-initialization would silently mint or reset
	// the magazine of an instance already in play.
	if (Magazines.Contains(InstanceId))
	{
		if (OutError)
		{
			*OutError = TEXT("DuplicateInstance: this item instance already holds an initialized magazine");
		}
		return false;
	}

	FMagazineState Magazine;
	Magazine.InstanceId = InstanceId;
	Magazine.AmmoId = AmmoId;
	Magazine.Capacity = MagazineCapacity;
	Magazine.LoadedRounds = InitialLoaded;
	Magazines.Add(InstanceId, Magazine);
	return true;
}

FAmmoReloadOutcome FAmmoModel::BeginReload(const FGuid& InstanceId, double NowSeconds, double DurationSeconds)
{
	// The magazine must exist before a reload can open.
	const FMagazineState* Magazine = Magazines.Find(InstanceId);
	if (!Magazine)
	{
		return MakeRefusal(EAmmoModelError::UnknownInstance, TEXT("UnknownInstance: this item instance holds no initialized magazine"));
	}

	// Window inputs: both clock values must be finite and the duration
	// non-negative - a NaN deadline or a negative window is a caller bug.
	if (!FMath::IsFinite(NowSeconds) || !FMath::IsFinite(DurationSeconds) || DurationSeconds < 0.0)
	{
		return MakeRefusal(EAmmoModelError::InvalidDuration, TEXT("InvalidDuration: NowSeconds must be finite and DurationSeconds non-negative"));
	}

	// One window per instance: the duplicate open would orphan the first
	// deadline.
	if (ActiveReloads.Contains(InstanceId))
	{
		return MakeRefusal(EAmmoModelError::DuplicateReload, TEXT("DuplicateReload: a reload window is already open for this instance"));
	}

	// Nothing to refill: the acceptance rule "满弹匣不换弹".
	if (Magazine->LoadedRounds >= Magazine->Capacity)
	{
		return MakeRefusal(EAmmoModelError::MagazineFull,
			FString::Printf(TEXT("MagazineFull: the magazine already holds %d/%d rounds"),
				Magazine->LoadedRounds, Magazine->Capacity));
	}

	// Nothing to grant: the acceptance rule "无备弹不换弹".
	const FAmmoReservePool* Pool = ReservePools.Find(Magazine->AmmoId);
	if (!Pool || Pool->Rounds <= 0)
	{
		return MakeRefusal(EAmmoModelError::NoReserveAmmo,
			FString::Printf(TEXT("NoReserveAmmo: the shared reserve of '%s' has no rounds left"),
				*Magazine->AmmoId.ToString()));
	}

	FAmmoReloadState Reload;
	Reload.EndTimeSeconds = NowSeconds + DurationSeconds;
	ActiveReloads.Add(InstanceId, Reload);

	// Opening the window moves nothing; report the untouched accounts.
	FAmmoReloadOutcome Outcome;
	Outcome.bSuccess = true;
	Outcome.LoadedRounds = Magazine->LoadedRounds;
	Outcome.ReserveRounds = Pool->Rounds;
	return Outcome;
}

FAmmoReloadOutcome FAmmoModel::CompleteReload(const FGuid& InstanceId)
{
	// The magazine must exist before a completion can land.
	const FMagazineState* Magazine = Magazines.Find(InstanceId);
	if (!Magazine)
	{
		return MakeRefusal(EAmmoModelError::UnknownInstance, TEXT("UnknownInstance: this item instance holds no initialized magazine"));
	}

	// Exactly one grant per window: the closed window refuses the repeated
	// completion callback ("重复完成回调不赠弹").
	if (!ActiveReloads.Contains(InstanceId))
	{
		return MakeRefusal(EAmmoModelError::NoActiveReload, TEXT("NoActiveReload: no reload window is open for this instance"));
	}

	// The pool of the instance's own ammo kind - registered pools and
	// initialized magazines are 1:1 bound, so this lookup holds by invariant.
	FAmmoReservePool& Pool = ReservePools.FindChecked(Magazine->AmmoId);

	// The one atomic transfer: granted = min(space, reserve). BeginReload
	// refused a full magazine and an empty pool, so granted is positive here.
	const int32 SpaceInMagazine = Magazine->Capacity - Magazine->LoadedRounds;
	const int32 Granted = FMath::Min(SpaceInMagazine, Pool.Rounds);
	Pool.Rounds -= Granted;
	Magazines[InstanceId].LoadedRounds += Granted;
	ActiveReloads.Remove(InstanceId);

	FAmmoReloadOutcome Outcome;
	Outcome.bSuccess = true;
	Outcome.RoundsTransferred = Granted;
	Outcome.LoadedRounds = Magazines[InstanceId].LoadedRounds;
	Outcome.ReserveRounds = Pool.Rounds;
	return Outcome;
}

bool FAmmoModel::CancelReload(const FGuid& InstanceId, FString* OutError)
{
	// The cancellation path (equipment switch / death) only closes the window;
	// every refusal leaves the accounts untouched.
	if (!Magazines.Contains(InstanceId))
	{
		if (OutError)
		{
			*OutError = TEXT("UnknownInstance: this item instance holds no initialized magazine");
		}
		return false;
	}
	if (!ActiveReloads.Contains(InstanceId))
	{
		if (OutError)
		{
			*OutError = TEXT("NoActiveReload: no reload window is open for this instance");
		}
		return false;
	}
	ActiveReloads.Remove(InstanceId);
	return true;
}

const FMagazineState* FAmmoModel::FindMagazine(const FGuid& InstanceId) const
{
	return Magazines.Find(InstanceId);
}

int32 FAmmoModel::GetReserveRounds(FName AmmoId) const
{
	const FAmmoReservePool* Pool = ReservePools.Find(AmmoId);
	return Pool ? Pool->Rounds : -1;
}

double FAmmoModel::GetRemainingReloadSeconds(const FGuid& InstanceId, double NowSeconds) const
{
	const FAmmoReloadState* Reload = ActiveReloads.Find(InstanceId);
	if (!Reload)
	{
		return 0.0;
	}
	// Clamped at the deadline: a passed deadline reads 0, the completion
	// callback stays the caller's duty.
	return FMath::Max(0.0, Reload->EndTimeSeconds - NowSeconds);
}

bool FAmmoModel::IsReloading(const FGuid& InstanceId) const
{
	return ActiveReloads.Contains(InstanceId);
}

int32 FAmmoModel::NumMagazines() const
{
	return Magazines.Num();
}

int32 FAmmoModel::NumAmmoTypes() const
{
	return ReservePools.Num();
}

void FAmmoModel::Reset()
{
	Magazines.Reset();
	ActiveReloads.Reset();
	ReservePools.Reset();
}
