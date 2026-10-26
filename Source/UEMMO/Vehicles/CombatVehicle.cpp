#include "CombatVehicle.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"

/**
 * M5-039: implementation of the hittable vehicle hull. See the header for the
 * frozen contract: the shared component pool (HealthComponent owns life,
 * CombatComponent carries defense and the control-immune reaction policy),
 * every hit through the M5-012 unified entry, and the one idempotent
 * death/destroy sequence (seats -> registry -> destroy event).
 */

DEFINE_LOG_CATEGORY_STATIC(LogUEMMOVehicle, Log, All);

namespace
{
	// The fixed hull query body (see the HullBody member comment): hull
	// dimensions are not in the M5-037 schema, so the box stays a named
	// constant until a mesh card introduces a real hull size.
	const FVector HullBodyHalfExtentCm(90.0f, 160.0f, 80.0f);

	// The registry category the M5-010 header names for vehicle bodies.
	const FName VehicleEntityCategory(TEXT("vehicle"));
}

ACombatVehicle::ACombatVehicle()
{
	PrimaryActorTick.bCanEverTick = false;

	HullBody = CreateDefaultSubobject<UBoxComponent>(TEXT("HullBody"));
	SetRootComponent(HullBody);
	HullBody->SetMobility(EComponentMobility::Movable);
	HullBody->SetBoxExtent(HullBodyHalfExtentCm);
	// QueryOnly: the hull is a passive target - melee overlaps and hitscan
	// object traces address it through the pawn body object type, projectile
	// sweeps resolve against its blocking responses; nothing simulates.
	HullBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	HullBody->SetCollisionObjectType(ECC_Pawn);
	HullBody->SetCollisionResponseToAllChannels(ECR_Block);

	HullHealth = CreateDefaultSubobject<UHealthComponent>(TEXT("HullHealth"));
	HullCombat = CreateDefaultSubobject<UCombatComponent>(TEXT("HullCombat"));
}

// M5-047-lesson: ACombatVehicle intentionally does NOT override
// PostInitializeComponents - the engine dispatches it only for worlds past
// actor initialization (Actor.cpp's AreActorsInitialized gate), and
// automation worlds created via UWorld::CreateWorld never pass that gate.
// The death-sequence binding lives in SetVehicleDefinition instead, which
// runs unconditionally wherever the hull is loaded.

bool ACombatVehicle::SetVehicleDefinition(const FVehicleDefinition& InDefinition, FString* OutErrors)
{
	if (bDestroyFinalized)
	{
		// The terminal state wins (first-terminal-wins idempotency): a dead or
		// destroyed hull never re-opens a health lifecycle.
		if (OutErrors != nullptr)
		{
			*OutErrors = TEXT("the hull is already destroyed; a destroyed vehicle refuses a new definition");
		}
		return false;
	}

	FString Errors;
	if (!ValidateVehicleDefinition(InDefinition, Errors))
	{
		// No partial state: an invalid definition touches nothing (the
		// M5-037 validator owns the field-level reasons).
		if (OutErrors != nullptr)
		{
			*OutErrors = Errors;
		}
		UE_LOG(LogUEMMOVehicle, Warning,
			TEXT("UEMMO ACombatVehicle: refused vehicle definition '%s' (%s)."),
			*InDefinition.VehicleId.ToString(), *Errors);
		return false;
	}

	Definition = InDefinition;
	bHasDefinition = true;

	// The one health owner (interface contract section 5): a lowered max
	// clamps the pool down, a raised max never heals, and ResetHealth opens
	// the fresh death lifecycle the definition implies.
	if (HullHealth != nullptr)
	{
		HullHealth->SetMaxHealth(Definition.MaxHealth);
		HullHealth->ResetHealth();
		// M5-047-lesson: the death-sequence binding lives HERE, not in
		// PostInitializeComponents - the engine dispatches PostInitialize-
		// Components only for worlds past actor initialization (Actor.cpp:
		// the AreActorsInitialized gate), and a world created via
		// UWorld::CreateWorld without InitializeActorsForPlay/BeginPlay never
		// passes that gate, so the binding would never happen in those
		// automation worlds. Every definition load opens a fresh death
		// lifecycle, so the binding refreshes with it; the RemoveAll keeps
		// repeated loads from stacking handlers.
		HullHealth->OnDied.RemoveAll(this);
		HullHealth->OnDied.AddUObject(this, &ACombatVehicle::HandleHealthDied);
	}

	// The shared carrier gets the definition's defense (the default unified
	// calculator reads exactly this component) and the control-immune hull
	// policy (the melee/hitscan pipelines read the overridden policy the same
	// way they read every combatant's).
	if (HullCombat != nullptr)
	{
		HullCombat->SetCombatStats(0.0f, Definition.Defense);
		HullCombat->SetTargetReactionPolicy(MakeHullTargetReaction(Definition));
	}

	UE_LOG(LogUEMMOVehicle, Log,
		TEXT("UEMMO ACombatVehicle: hull '%s' loaded (HP %f, defense %f)."),
		*Definition.VehicleId.ToString(), Definition.MaxHealth, Definition.Defense);
	return true;
}

FTargetReaction ACombatVehicle::MakeHullTargetReaction(const FVehicleDefinition& InDefinition)
{
	// A hull is a machine body: it takes full damage and dies, but no control
	// kind ever applies to it (contract section 5 - control immunity still
	// lets damage through, and a pure-control hit never fakes damage). The
	// per-kind gates stay closed alongside the blanket flag so a resolver that
	// only reads the gates refuses the same way. Poise stays 0 (the blanket
	// immunity already refuses everything; a poise pool would be dead state).
	FTargetReaction Policy;
	Policy.PolicyId = InDefinition.VehicleId;
	Policy.bAllowStagger = false;
	Policy.bAllowLaunch = false;
	Policy.bAllowKnockdown = false;
	Policy.bImmuneControl = true;
	Policy.bImmuneDamage = false;
	Policy.bDeathResistant = false;
	return Policy;
}

FEntityId ACombatVehicle::RegisterCombatEntity(FCombatEntityRegistry& Registry, FName Faction)
{
	// The ResolveUnifiedEntityId guard (M5-013): a cached id is reused only
	// while its record still resolves to this live actor - a record wiped by a
	// world rebuild (or a different registry) re-mints instead of inheriting
	// stale identity.
	if (BoundRegistry == &Registry && IsValidCombatEntityId(EntityId))
	{
		const FCombatEntityRecord* Record = Registry.FindEntity(EntityId);
		if (Record != nullptr && Record->Actor.Get() == this)
		{
			return EntityId;
		}
	}

	FCombatEntityMetadata Metadata;
	// Faction is the wiring's choice (empty = unaligned never trips the
	// friendly-fire filter); the category is the registry header's own
	// vehicle example.
	Metadata.Faction = Faction;
	Metadata.Category = VehicleEntityCategory;

	const FCombatEpoch CurrentEpoch = Registry.GetCurrentEpoch();
	const FEntityId MintedId = Registry.RegisterEntity(this, Metadata, CurrentEpoch);
	if (IsValidCombatEntityId(MintedId))
	{
		BoundRegistry = &Registry;
		EntityId = MintedId;
		RegisteredEpoch = CurrentEpoch;
	}
	else
	{
		UE_LOG(LogUEMMOVehicle, Warning,
			TEXT("UEMMO ACombatVehicle: the registry refused hull '%s' (id stays unset)."),
			*Definition.VehicleId.ToString());
	}
	return MintedId;
}

FSeatOccupancyModel::FSeatEnterGate ACombatVehicle::MakeVehicleValidityGate() const
{
	// Weak capture: the gate outlives nothing - once the hull is gone the
	// predicate answers false instead of dereferencing a dangling actor.
	TWeakObjectPtr<const ACombatVehicle> WeakHull(this);
	return [WeakHull](const FVehicleSeatPolicyContext& Context)
	{
		const ACombatVehicle* Hull = WeakHull.Get();
		// This gate vouches only for its own hull (the wiring binds one gate
		// per vehicle); a request naming another vehicle id is not this
		// hull's business and refuses with the gate's own reason.
		if (Hull == nullptr || Context.Request.VehicleId != Hull->Definition.VehicleId)
		{
			return false;
		}
		return Hull->IsHullAlive() && !Hull->IsPendingKillPending();
	};
}

bool ACombatVehicle::IsHullAlive() const
{
	return HullHealth != nullptr && HullHealth->IsAlive();
}

bool ACombatVehicle::FinalizeDestruction()
{
	// First terminal wins (the HitLedger/scene evaluator idempotency
	// precedent): every later death, destroy or EndPlay lands here and is
	// rejected without touching any state.
	if (bDestroyFinalized)
	{
		return false;
	}
	bDestroyFinalized = true;

	// (a) Seats/mounts first (contract section 8 order): the occupants leave
	// before the entity identity disappears, so the wiring's ClearVehicle sees
	// the still-registered vehicle id. Unset seam = safe no-op (the pure
	// world-entity tests run without a seat model).
	if (SeatVacate)
	{
		SeatVacate(Definition.VehicleId);
	}

	// (b) Unregister from the entity registry, naming the generation that owns
	// the record: a rebuilt world already wiped it and refuses StaleEpoch -
	// the documented safe no-op, never an error path.
	if (BoundRegistry != nullptr && IsValidCombatEntityId(EntityId))
	{
		ECombatRegistryRejectReason Reason = ECombatRegistryRejectReason::None;
		if (!BoundRegistry->UnregisterEntity(EntityId, RegisteredEpoch, &Reason))
		{
			UE_LOG(LogUEMMOVehicle, Verbose,
				TEXT("UEMMO ACombatVehicle: unregister of hull '%s' refused (reason %d) - the record is already gone with its epoch."),
				*Definition.VehicleId.ToString(), static_cast<int32>(Reason));
		}
	}

	// (c) The one-shot destroy event. Observers (wreck presentation, the
	// M5-043 mount teardown, room accounting) resolve the hull through the
	// weak reference / registry id, never through a stored raw pointer.
	FVehicleDestroyedEvent Event;
	Event.VehicleId = Definition.VehicleId;
	Event.EntityId = EntityId;
	Event.Epoch = RegisteredEpoch;
	Event.Vehicle = this;
	OnVehicleDestroyed.Broadcast(Event);

	UE_LOG(LogUEMMOVehicle, Log,
		TEXT("UEMMO ACombatVehicle: hull '%s' destroyed (entity %llu, epoch %llu)."),
		*Definition.VehicleId.ToString(), EntityId, RegisteredEpoch);
	return true;
}

void ACombatVehicle::HandleHealthDied()
{
	// The health owner's at-most-once death signal is the only death trigger;
	// idempotency and ordering live in FinalizeDestruction.
	FinalizeDestruction();
}

void ACombatVehicle::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The destroy cleanup face (contract section 8): an actor destroyed while
	// alive runs the same one sequence; after a death it is the rejected
	// second terminal. Run before Super so the registry/model outlives the
	// observer teardown the base class performs.
	FinalizeDestruction();

	if (HullHealth != nullptr)
	{
		HullHealth->OnDied.RemoveAll(this);
	}

	Super::EndPlay(EndPlayReason);
}
