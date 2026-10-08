// M5-019: session weapon binding implementation (WeaponBinding.h).
//
// The registry is a plain value container: resolution walks the
// item_definition_id chain (FItemInstance -> FItemDefinition ->
// FWeaponDefinition), refusals name their reason and leave the registry
// untouched, and the per-instance magazine slot is keyed by ItemInstanceId so
// same-definition instances never share rounds. Unbinding parks the rounds
// and rebinding restores them; persistence belongs to M5-046.
#include "WeaponBinding.h"

FWeaponBindOutcome FWeaponBindingRegistry::BindInstance(const FItemInstance& Instance, const FItemDefinitionCatalog& ItemDefinitions, const FCombatCatalog& WeaponCatalog)
{
	FWeaponBindOutcome Outcome;

	// The binding preserves the instance identity, so a usable identity is a
	// precondition: a valid FGuid (the all-zero guid is never a legal
	// identity) and a non-empty kind reference.
	if (!Instance.InstanceId.IsValid())
	{
		Outcome.RejectReason = EWeaponBindingRejectReason::InvalidInstance;
		Outcome.RejectDetail = TEXT("InstanceId must be a valid FGuid (the all-zero guid is not a legal identity)");
		return Outcome;
	}
	if (Instance.DefinitionId.IsNone())
	{
		Outcome.RejectReason = EWeaponBindingRejectReason::InvalidInstance;
		Outcome.RejectDetail = TEXT("DefinitionId must be a non-empty identifier");
		return Outcome;
	}

	// One active binding per instance; rebinding goes through UnbindInstance
	// (which parks the magazine) - a second live bind is refused, never
	// silently replaced.
	if (Bindings.Contains(Instance.InstanceId))
	{
		Outcome.RejectReason = EWeaponBindingRejectReason::DuplicateInstanceBinding;
		Outcome.RejectDetail = FString::Printf(TEXT("instance '%s' already holds an active binding; unbind it first"),
			*Instance.InstanceId.ToString());
		return Outcome;
	}

	// The item kind must exist - there is no implicit definition and no
	// fallback item.
	const FItemDefinition* ItemDefinition = ItemDefinitions.Find(Instance.DefinitionId);
	if (ItemDefinition == nullptr)
	{
		Outcome.RejectReason = EWeaponBindingRejectReason::UnknownItemDefinition;
		Outcome.RejectDetail = FString::Printf(TEXT("item definition '%s' is not registered in the item definition catalog"),
			*Instance.DefinitionId.ToString());
		return Outcome;
	}

	// Only weapon-slot items bind weapon behavior; armor and accessory
	// instances keep their M3 meaning and are refused here.
	if (ItemDefinition->Slot != EItemSlot::Weapon)
	{
		Outcome.RejectReason = EWeaponBindingRejectReason::SlotNotWeapon;
		Outcome.RejectDetail = FString::Printf(TEXT("item definition '%s' has slot %d; only Weapon-slot items bind weapon behavior"),
			*Instance.DefinitionId.ToString(),
			static_cast<int32>(ItemDefinition->Slot));
		return Outcome;
	}

	// The weapon-slot item must carry the explicit mapping; the binding layer
	// never guesses one.
	if (ItemDefinition->WeaponDefinitionId.IsNone())
	{
		Outcome.RejectReason = EWeaponBindingRejectReason::NoWeaponMapping;
		Outcome.RejectDetail = FString::Printf(TEXT("weapon item '%s' carries no WeaponDefinitionId mapping"),
			*Instance.DefinitionId.ToString());
		return Outcome;
	}

	// The mapped weapon kind must resolve in the combat catalog - a broken
	// mapping is a loud refusal, never a reroute to the training sword.
	const FWeaponDefinition* Weapon = WeaponCatalog.FindWeapon(ItemDefinition->WeaponDefinitionId);
	if (Weapon == nullptr)
	{
		Outcome.RejectReason = EWeaponBindingRejectReason::UnknownWeaponDefinition;
		Outcome.RejectDetail = FString::Printf(TEXT("weapon id '%s' of item '%s' does not resolve in the combat catalog"),
			*ItemDefinition->WeaponDefinitionId.ToString(),
			*Instance.DefinitionId.ToString());
		return Outcome;
	}

	// All checks passed: record the binding with the identity copied verbatim
	// and the per-instance magazine slot (parked rounds restored, a first bind
	// starts full; melee has no magazine and stays 0).
	FWeaponBindingRecord Record;
	Record.InstanceId = Instance.InstanceId;
	Record.ItemDefinitionId = Instance.DefinitionId;
	Record.WeaponDefinitionId = Weapon->WeaponId;
	Record.FireMode = Weapon->FireMode;
	Record.Level = Instance.Level;
	Record.RollSeed = Instance.RollSeed;
	Record.MagazineCapacity = Weapon->MagazineSize;

	if (const int32* ParkedRounds = DetachedMagazines.Find(Record.InstanceId))
	{
		Record.LoadedRounds = *ParkedRounds;
		DetachedMagazines.Remove(Record.InstanceId);
	}
	else
	{
		Record.LoadedRounds = Weapon->MagazineSize;
	}

	Bindings.Add(Record.InstanceId, Record);

	Outcome.bBound = true;
	Outcome.Record = Record;
	return Outcome;
}

bool FWeaponBindingRegistry::UnbindInstance(const FGuid& InstanceId)
{
	FWeaponBindingRecord Record;
	if (!Bindings.RemoveAndCopyValue(InstanceId, Record))
	{
		return false;
	}

	// Park the rounds so a rebind of the same instance keeps its magazine
	// (switching equipment does not initialize new ammo).
	DetachedMagazines.Add(Record.InstanceId, Record.LoadedRounds);
	return true;
}

const FWeaponBindingRecord* FWeaponBindingRegistry::FindBinding(const FGuid& InstanceId) const
{
	return Bindings.Find(InstanceId);
}

int32 FWeaponBindingRegistry::NumBindings() const
{
	return Bindings.Num();
}

bool FWeaponBindingRegistry::SetInstanceRounds(const FGuid& InstanceId, int32 NewLoadedRounds, FString* OutError)
{
	FWeaponBindingRecord* Record = Bindings.Find(InstanceId);
	if (Record == nullptr)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("instance '%s' holds no active binding"), *InstanceId.ToString());
		}
		return false;
	}
	if (Record->MagazineCapacity == 0)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("weapon '%s' of instance '%s' has no magazine (melee)"),
				*Record->WeaponDefinitionId.ToString(),
				*InstanceId.ToString());
		}
		return false;
	}
	if (NewLoadedRounds < 0 || NewLoadedRounds > Record->MagazineCapacity)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("loaded rounds must be 0..%d (got %d) for instance '%s'"),
				Record->MagazineCapacity,
				NewLoadedRounds,
				*InstanceId.ToString());
		}
		return false;
	}

	Record->LoadedRounds = NewLoadedRounds;
	return true;
}

void FWeaponBindingRegistry::Reset()
{
	Bindings.Reset();
	DetachedMagazines.Reset();
}
