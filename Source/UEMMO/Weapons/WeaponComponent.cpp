#include "WeaponComponent.h"

#include "../Items/ItemDefinition.h"

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
	ActiveInstanceId.Invalidate();
	++BindingGeneration;
}
