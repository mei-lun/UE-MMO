#include "Misc/AutomationTest.h"

#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

// M5-010 acceptance tests: the world-level entity identity registry and the
// bounded idempotent hit ledger. Pure logic - no World is created and no
// actor is spawned here. The only UObject ever created is one bare transient
// AActor (M3-023 precedent) used to prove the weak-reference resolve path;
// every other record registers a null actor, which the registry accepts as a
// logic-only identity so the idempotency rules stay testable without a world.
//
// Acceptance mapping:
// - RegistryRegisterResolve     : weak-ref actor + faction/category registers to a
//                                   unique FEntityId; resolve and lookup round trip.
// - RegistryStaleEpochReject    : a world rebuild mints a new epoch; unregister,
//                                   allocate and register requests that carry the
//                                   old epoch are refused (even for ids that exist
//                                   again in the new generation).
// - RegistryCommonActionSequence: one monotonic ActionSequence per (Epoch, Source);
//                                   melee/firearm/vehicle local counters all equal
//                                   to 1 still draw distinct public sequences.
// - LedgerDuplicateKeyReject    : a repeated FCombatEventKey is refused; the stable
//                                   EventId derives from the complete key.
// - LedgerKeyIsolation          : two sources / two pellets / two epochs never
//                                   collide; unload returns the ledger to baseline.
// - LedgerUnregisterAndStaleReject: hit events from an unregistered actor or an old
//                                   epoch are refused with explicit reasons.
// - LedgerShotControlDedup      : multi-pellet shots damage repeatedly but one shot
//                                   accepts control on one target exactly once.
// - LedgerCapacityEndBaseline   : a full capacity refuses new instances without
//                                   evicting active ones; ending/unloading returns
//                                   the ledger to its baseline.
// - MeleeFirearmDistinctPublicKeys: the card's melee(InstanceId=1) vs firearm
//                                   (ShotSequence=1) scenario end to end.

static_assert(std::is_trivially_copyable_v<FCombatLedgerShotKey>, "FCombatLedgerShotKey must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FCombatShotControlKey>, "FCombatShotControlKey must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FCombatLedgerShotKey::Epoch)>, "FCombatLedgerShotKey must stay pointer-free");
static_assert(!std::is_pointer_v<decltype(FCombatShotControlKey::TargetId)>, "FCombatShotControlKey must stay pointer-free");
static_assert(!std::is_pointer_v<decltype(FCombatEntityRecord::Actor)>, "the registry stores a weak reference, never a raw actor pointer");
static_assert(std::is_same_v<std::underlying_type_t<EHitLedgerRejectReason>, uint8>, "ledger reject reasons stay an append-only uint8 enum");
static_assert(std::is_same_v<std::underlying_type_t<ECombatRegistryRejectReason>, uint8>, "registry reject reasons stay an append-only uint8 enum");

namespace
{
	FCombatEntityMetadata MakeMetadata(const TCHAR* Faction, const TCHAR* Category)
	{
		FCombatEntityMetadata Metadata;
		Metadata.Faction = Faction;
		Metadata.Category = Category;
		return Metadata;
	}
}

// ---------------------------------------------------------------------------
// RegistryRegisterResolve
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010RegistryRegisterResolve,
	"UEMMO.Tasks.M5_010.RegistryRegisterResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010RegistryRegisterResolve::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	TestEqual(TEXT("a fresh registry models the first live world generation"), Registry.GetCurrentEpoch(), static_cast<FCombatEpoch>(1));

	const FCombatEntityMetadata PlayerMeta = MakeMetadata(TEXT("Player"), TEXT("Pawn"));
	const FCombatEntityMetadata EnemyMeta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));

	// Null-actor registrations are logic-only identities (tests and future
	// headless models); each registration mints the next id of the generation.
	const FEntityId First = Registry.RegisterEntity(nullptr, PlayerMeta, Registry.GetCurrentEpoch());
	const FEntityId Second = Registry.RegisterEntity(nullptr, EnemyMeta, Registry.GetCurrentEpoch());
	TestFalse(TEXT("the first registration is not the invalid sentinel"), First == InvalidCombatEntityId);
	TestFalse(TEXT("the second registration is not the invalid sentinel"), Second == InvalidCombatEntityId);
	TestEqual(TEXT("ids are minted from 1 in order"), First, static_cast<FEntityId>(1));
	TestEqual(TEXT("ids are unique per registration"), Second, static_cast<FEntityId>(2));
	TestEqual(TEXT("two records are tracked"), Registry.GetNumRegisteredEntities(), 2);

	// The record round trips the registration payload and the minting epoch.
	const FCombatEntityRecord* FirstRecord = Registry.FindEntity(First);
	if (TestNotNull(TEXT("a registered entity is found again"), FirstRecord))
	{
		TestEqual(TEXT("the record keeps its id"), FirstRecord->EntityId, First);
		TestEqual(TEXT("the record keeps its minting epoch"), FirstRecord->Epoch, static_cast<FCombatEpoch>(1));
		TestTrue(TEXT("the record keeps the faction"), FirstRecord->Metadata.Faction == PlayerMeta.Faction);
		TestTrue(TEXT("the record keeps the category"), FirstRecord->Metadata.Category == PlayerMeta.Category);
	}

	TestTrue(TEXT("a known entity answers true"), Registry.IsKnownEntity(First));
	TestFalse(TEXT("an unknown id answers false"), Registry.IsKnownEntity(999));
	TestTrue(TEXT("a known entity is known in its own epoch"), Registry.IsKnownEntityInEpoch(First, 1));
	TestFalse(TEXT("a current entity is not known in a future epoch"), Registry.IsKnownEntityInEpoch(First, 2));

	// Weak-reference resolve: an unknown id resolves to null; a registered
	// actor resolves to the same object (one bare transient AActor, M3-023
	// precedent - no world is involved).
	TWeakObjectPtr<AActor> Unresolved = Registry.ResolveEntity(999);
	TestTrue(TEXT("an unknown id resolves to null"), Unresolved.Get() == nullptr);

	AActor* BareActor = NewObject<AActor>(GetTransientPackage());
	const FEntityId ActorId = Registry.RegisterEntity(BareActor, PlayerMeta, Registry.GetCurrentEpoch());
	TestEqual(TEXT("the actor registration mints the next id"), ActorId, static_cast<FEntityId>(3));
	TWeakObjectPtr<AActor> Resolved = Registry.ResolveEntity(ActorId);
	TestTrue(TEXT("a registered actor resolves to the same object"), Resolved.Get() == BareActor);

	// A live actor cannot claim a second identity in the same generation.
	ECombatRegistryRejectReason Reason = ECombatRegistryRejectReason::None;
	const FEntityId Duplicate = Registry.RegisterEntity(BareActor, PlayerMeta, Registry.GetCurrentEpoch(), &Reason);
	TestFalse(TEXT("a duplicate live actor registration is refused"), IsValidCombatEntityId(Duplicate));
	TestEqual(TEXT("the duplicate refusal names the reason"), Reason, ECombatRegistryRejectReason::DuplicateActor);
	TestEqual(TEXT("the refused registration created no record"), Registry.GetNumRegisteredEntities(), 3);

	// Null actors carry no object identity, so logic-only records may repeat.
	const FEntityId LogicOnly = Registry.RegisterEntity(nullptr, EnemyMeta, Registry.GetCurrentEpoch());
	TestEqual(TEXT("a second null-actor record is accepted"), LogicOnly, static_cast<FEntityId>(4));
	TestEqual(TEXT("four records are tracked"), Registry.GetNumRegisteredEntities(), 4);
	return true;
}

// ---------------------------------------------------------------------------
// RegistryStaleEpochReject
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010RegistryStaleEpochReject,
	"UEMMO.Tasks.M5_010.RegistryStaleEpochReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010RegistryStaleEpochReject::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	const FCombatEntityMetadata Meta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));

	const FEntityId StaleEntity = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	TestEqual(TEXT("the generation starts with one registered entity"), StaleEntity, static_cast<FEntityId>(1));
	TestEqual(TEXT("the first allocation of the generation"), Registry.AllocateActionSequence(StaleEntity, Registry.GetCurrentEpoch()), static_cast<FShotId>(1));

	// World rebuild: the next generation is minted, every record and every
	// per-source sequence counter of the old generation is dropped.
	const FCombatEpoch RebuiltEpoch = Registry.BeginNextWorldEpoch();
	TestEqual(TEXT("the rebuild mints the next epoch"), RebuiltEpoch, static_cast<FCombatEpoch>(2));
	TestEqual(TEXT("the rebuild cleared every record"), Registry.GetNumRegisteredEntities(), 0);
	TestFalse(TEXT("the old id is no longer known"), Registry.IsKnownEntity(StaleEntity));

	// Every request that still carries the old epoch is refused, whatever the
	// id it names.
	ECombatRegistryRejectReason Reason = ECombatRegistryRejectReason::None;
	TestFalse(TEXT("an old-epoch unregister is refused"), Registry.UnregisterEntity(StaleEntity, 1, &Reason));
	TestEqual(TEXT("the stale unregister names the reason"), Reason, ECombatRegistryRejectReason::StaleEpoch);
	TestFalse(TEXT("an old-epoch allocation is refused"), IsValidCombatShotId(Registry.AllocateActionSequence(StaleEntity, 1, &Reason)));
	TestEqual(TEXT("the stale allocation names the reason"), Reason, ECombatRegistryRejectReason::StaleEpoch);
	TestFalse(TEXT("an old-epoch registration is refused"), IsValidCombatEntityId(Registry.RegisterEntity(nullptr, Meta, 1, &Reason)));
	TestEqual(TEXT("the stale registration names the reason"), Reason, ECombatRegistryRejectReason::StaleEpoch);

	// A rebuilt world is a fresh identity space: ids restart at 1 and the
	// per-(Epoch, Source) sequence counter starts over.
	const FEntityId NewEntity = Registry.RegisterEntity(nullptr, Meta, RebuiltEpoch, &Reason);
	TestEqual(TEXT("the new generation reuses the id space from 1"), NewEntity, static_cast<FEntityId>(1));
	TestEqual(TEXT("a fresh registration carries no rejection"), Reason, ECombatRegistryRejectReason::None);
	TestEqual(TEXT("the new generation's first allocation"), Registry.AllocateActionSequence(NewEntity, RebuiltEpoch), static_cast<FShotId>(1));

	// The teeth of the stale-epoch rule: id 1 exists again, but an unregister
	// request that still carries epoch 1 must be refused while the live
	// generation's record survives untouched.
	Reason = ECombatRegistryRejectReason::None;
	TestFalse(TEXT("an old-epoch unregister is refused even for a re-minted id"), Registry.UnregisterEntity(NewEntity, 1, &Reason));
	TestEqual(TEXT("the stale unregister still names StaleEpoch"), Reason, ECombatRegistryRejectReason::StaleEpoch);
	TestTrue(TEXT("the live record survived the stale request"), Registry.IsKnownEntity(NewEntity));
	TestFalse(TEXT("the live entity is not known in the stale epoch"), Registry.IsKnownEntityInEpoch(NewEntity, 1));

	// A current-epoch unregister of an unknown id names UnknownEntity instead.
	TestFalse(TEXT("an unregister of an unknown id is refused"), Registry.UnregisterEntity(999, RebuiltEpoch, &Reason));
	TestEqual(TEXT("the unknown unregister names the reason"), Reason, ECombatRegistryRejectReason::UnknownEntity);

	// The proper unregister removes the record; later requests for the id are
	// refused as unknown in the same epoch.
	TestTrue(TEXT("a current-epoch unregister is accepted"), Registry.UnregisterEntity(NewEntity, RebuiltEpoch, &Reason));
	TestEqual(TEXT("the accepted unregister carries no rejection"), Reason, ECombatRegistryRejectReason::None);
	TestEqual(TEXT("the unregister emptied the generation"), Registry.GetNumRegisteredEntities(), 0);
	TestFalse(TEXT("an allocation for an unregistered source is refused"), IsValidCombatShotId(Registry.AllocateActionSequence(NewEntity, RebuiltEpoch, &Reason)));
	TestEqual(TEXT("the unregistered allocation names the reason"), Reason, ECombatRegistryRejectReason::UnknownEntity);
	return true;
}

// ---------------------------------------------------------------------------
// RegistryCommonActionSequence
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010RegistryCommonActionSequence,
	"UEMMO.Tasks.M5_010.RegistryCommonActionSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010RegistryCommonActionSequence::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	const FCombatEntityMetadata MeleeMeta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));

	const FEntityId Source = Registry.RegisterEntity(nullptr, MeleeMeta, Registry.GetCurrentEpoch());
	TestEqual(TEXT("the source is registered"), Source, static_cast<FEntityId>(1));

	// One common monotonic sequence per (Epoch, Source): melee (local
	// AttackInstanceId=1), firearms (local ShotSequence=1) and vehicle weapons
	// (local counter=1) all draw from the same allocator, so capabilities with
	// identical local counters still produce distinct public keys.
	const FShotId MeleeSequence = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	const FShotId FirearmSequence = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	const FShotId VehicleSequence = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	TestEqual(TEXT("melee draws sequence 1"), MeleeSequence, static_cast<FShotId>(1));
	TestEqual(TEXT("firearm draws sequence 2"), FirearmSequence, static_cast<FShotId>(2));
	TestEqual(TEXT("vehicle draws sequence 3"), VehicleSequence, static_cast<FShotId>(3));
	TestTrue(TEXT("identical local counters produce different public sequences"), MeleeSequence != FirearmSequence);

	// A second source counts independently.
	const FEntityId OtherSource = Registry.RegisterEntity(nullptr, MakeMetadata(TEXT("Player"), TEXT("Pawn")), Registry.GetCurrentEpoch());
	TestEqual(TEXT("the second source's first allocation"), Registry.AllocateActionSequence(OtherSource, Registry.GetCurrentEpoch()), static_cast<FShotId>(1));

	// Unknown, unregistered and non-current-epoch callers are refused.
	ECombatRegistryRejectReason Reason = ECombatRegistryRejectReason::None;
	TestFalse(TEXT("an allocation for an unknown source is refused"), IsValidCombatShotId(Registry.AllocateActionSequence(999, Registry.GetCurrentEpoch(), &Reason)));
	TestEqual(TEXT("the unknown source names the reason"), Reason, ECombatRegistryRejectReason::UnknownEntity);
	Reason = ECombatRegistryRejectReason::None;
	TestFalse(TEXT("an allocation outside the current epoch is refused"), IsValidCombatShotId(Registry.AllocateActionSequence(Source, 2, &Reason)));
	TestEqual(TEXT("the non-current epoch names the reason"), Reason, ECombatRegistryRejectReason::StaleEpoch);

	TestTrue(TEXT("the second source unregisters"), Registry.UnregisterEntity(OtherSource, Registry.GetCurrentEpoch()));
	Reason = ECombatRegistryRejectReason::None;
	TestFalse(TEXT("an unregistered source can no longer mint sequences"), IsValidCombatShotId(Registry.AllocateActionSequence(OtherSource, Registry.GetCurrentEpoch(), &Reason)));
	TestEqual(TEXT("the unregistered source names the reason"), Reason, ECombatRegistryRejectReason::UnknownEntity);
	return true;
}

// ---------------------------------------------------------------------------
// LedgerDuplicateKeyReject
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010LedgerDuplicateKeyReject,
	"UEMMO.Tasks.M5_010.LedgerDuplicateKeyReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010LedgerDuplicateKeyReject::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	FHitLedger Ledger;
	Ledger.BindRegistry(&Registry);
	TestTrue(TEXT("the bound registry is reachable"), Ledger.GetBoundRegistry() == &Registry);

	const FCombatEntityMetadata Meta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));
	const FEntityId Source = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId Target = Registry.RegisterEntity(nullptr, MakeMetadata(TEXT("Player"), TEXT("Pawn")), Registry.GetCurrentEpoch());
	const FShotId Shot = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());

	const FCombatEventKey Key{Registry.GetCurrentEpoch(), Source, Shot, 0, Target};
	EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the first occurrence of a key is accepted"), Ledger.TryRecordHitEvent(Key, Reason));
	TestEqual(TEXT("the accepted event carries no rejection"), Reason, EHitLedgerRejectReason::None);
	TestEqual(TEXT("one event is recorded"), Ledger.GetNumRecordedEvents(), 1);

	// The dedup face: an exact re-send of the same five-tuple is refused and
	// leaves the ledger unchanged.
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("a repeated key is refused"), Ledger.TryRecordHitEvent(Key, Reason));
	TestEqual(TEXT("the repeated key names DuplicateEvent"), Reason, EHitLedgerRejectReason::DuplicateEvent);
	TestEqual(TEXT("the refused repeat recorded nothing"), Ledger.GetNumRecordedEvents(), 1);
	TestTrue(TEXT("the recorded key is present"), Ledger.HasRecordedEvent(Key));

	FCombatEventKey OtherKey = Key;
	OtherKey.PelletIndex = 1;
	TestFalse(TEXT("an unrecorded key is absent"), Ledger.HasRecordedEvent(OtherKey));

	// The stable EventId derives from the complete key: value-identical keys
	// derive the same id, any field change derives a different one.
	TestEqual(TEXT("the same key derives the same EventId"), DeriveCombatEventId(Key), DeriveCombatEventId(Key));
	TestTrue(TEXT("a changed key derives a different EventId"), DeriveCombatEventId(Key) != DeriveCombatEventId(OtherKey));
	return true;
}

// ---------------------------------------------------------------------------
// LedgerKeyIsolation
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010LedgerKeyIsolation,
	"UEMMO.Tasks.M5_010.LedgerKeyIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010LedgerKeyIsolation::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	FHitLedger Ledger;
	Ledger.BindRegistry(&Registry);

	const FCombatEntityMetadata Meta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));
	const FEntityId SourceA = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId SourceB = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId TargetA = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId TargetB = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	// Per-source allocators: two sources both mint their own first sequence.
	const FShotId ShotA = Registry.AllocateActionSequence(SourceA, Registry.GetCurrentEpoch());
	const FShotId ShotB = Registry.AllocateActionSequence(SourceB, Registry.GetCurrentEpoch());
	TestEqual(TEXT("each source starts its own sequence"), ShotA, static_cast<FShotId>(1));
	TestEqual(TEXT("each source starts its own sequence (second source)"), ShotB, static_cast<FShotId>(1));

	// Two sources: identical sequence, pellet and target - the EntityId keeps
	// the keys apart.
	const FCombatEventKey SourceAKey{1, SourceA, ShotA, 0, TargetA};
	const FCombatEventKey SourceBKey{1, SourceB, ShotB, 0, TargetA};
	EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the first source's event is accepted"), Ledger.TryRecordHitEvent(SourceAKey, Reason));
	TestTrue(TEXT("the second source's identical-looking event is accepted"), Ledger.TryRecordHitEvent(SourceBKey, Reason));
	TestEqual(TEXT("two isolated events are recorded"), Ledger.GetNumRecordedEvents(), 2);
	TestTrue(TEXT("different sources yield different keys"), SourceAKey != SourceBKey);
	TestTrue(TEXT("different sources yield different hashes"), GetTypeHash(SourceAKey) != GetTypeHash(SourceBKey));

	// Two pellets: one shot damages the same target through several pellets,
	// each with its own event key.
	FCombatEventKey PelletOneKey = SourceAKey;
	PelletOneKey.PelletIndex = 1;
	TestTrue(TEXT("the second pellet is a separate damage event"), Ledger.TryRecordHitEvent(PelletOneKey, Reason));
	TestEqual(TEXT("three events are recorded"), Ledger.GetNumRecordedEvents(), 3);

	// Two targets: the same shot and pellet on another target is its own event.
	FCombatEventKey OtherTargetKey = SourceAKey;
	OtherTargetKey.TargetId = TargetB;
	TestTrue(TEXT("the other target is a separate damage event"), Ledger.TryRecordHitEvent(OtherTargetKey, Reason));
	TestEqual(TEXT("four events are recorded"), Ledger.GetNumRecordedEvents(), 4);

	// Two epochs: a rebuild re-mints the same numeric ids; the numerically
	// identical key in the new epoch is a different event and the stale
	// original is refused instead of merged.
	Registry.BeginNextWorldEpoch();
	const FCombatEpoch NewEpoch = Registry.GetCurrentEpoch();
	const FEntityId SourceA2 = Registry.RegisterEntity(nullptr, Meta, NewEpoch);
	const FEntityId TargetA2 = Registry.RegisterEntity(nullptr, Meta, NewEpoch);
	TestEqual(TEXT("the rebuilt generation re-mints source id 1"), SourceA2, static_cast<FEntityId>(1));
	TestEqual(TEXT("the rebuilt generation re-mints target id 2"), TargetA2, static_cast<FEntityId>(2));

	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("the old-epoch original is refused, not merged"), Ledger.TryRecordHitEvent(SourceAKey, Reason));
	TestEqual(TEXT("the old-epoch key names StaleEpoch"), Reason, EHitLedgerRejectReason::StaleEpoch);

	const FCombatEventKey NewEpochKey{NewEpoch, SourceA2, ShotA, 0, TargetA2};
	TestTrue(TEXT("the new-epoch twin is accepted as its own event"), Ledger.TryRecordHitEvent(NewEpochKey, Reason));
	TestEqual(TEXT("the new-epoch twin is a fifth distinct event"), Ledger.GetNumRecordedEvents(), 5);
	TestTrue(TEXT("different epochs yield different keys"), SourceAKey != NewEpochKey);
	TestTrue(TEXT("different epochs yield different hashes"), GetTypeHash(SourceAKey) != GetTypeHash(NewEpochKey));

	// Unload: everything returns to the baseline (empty) state.
	Ledger.Reset();
	TestEqual(TEXT("the unload cleared every event"), Ledger.GetNumRecordedEvents(), 0);
	TestEqual(TEXT("the unload cleared every shot"), Ledger.GetNumTrackedShots(), 0);
	TestEqual(TEXT("the unload cleared the active count"), Ledger.GetNumActiveShots(), 0);
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("a fresh generation can record again after the unload"), Ledger.TryRecordHitEvent(NewEpochKey, Reason));
	TestEqual(TEXT("the re-recorded key carries no rejection"), Reason, EHitLedgerRejectReason::None);
	return true;
}

// ---------------------------------------------------------------------------
// LedgerUnregisterAndStaleReject
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010LedgerUnregisterAndStaleReject,
	"UEMMO.Tasks.M5_010.LedgerUnregisterAndStaleReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010LedgerUnregisterAndStaleReject::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	FHitLedger Ledger;
	Ledger.BindRegistry(&Registry);

	const FCombatEntityMetadata Meta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));
	const FEntityId Source = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	FEntityId Target = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FShotId Shot = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());

	const FCombatEventKey BaseKey{Registry.GetCurrentEpoch(), Source, Shot, 0, Target};
	EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the baseline event is accepted"), Ledger.TryRecordHitEvent(BaseKey, Reason));

	// Unregistering the target refuses further events naming it - and the
	// already-recorded key stays recorded (unregistering is not ledger cleanup;
	// end/unload is).
	TestTrue(TEXT("the target unregisters"), Registry.UnregisterEntity(Target, Registry.GetCurrentEpoch()));
	FCombatEventKey AfterTargetGone = BaseKey;
	AfterTargetGone.PelletIndex = 1;
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("a hit event for an unregistered target is refused"), Ledger.TryRecordHitEvent(AfterTargetGone, Reason));
	TestEqual(TEXT("the unregistered target names UnknownTarget"), Reason, EHitLedgerRejectReason::UnknownTarget);
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("shot control for an unregistered target is refused"), Ledger.TryAcceptShotControl(AfterTargetGone, Reason));
	TestEqual(TEXT("the control refusal names UnknownTarget"), Reason, EHitLedgerRejectReason::UnknownTarget);
	TestTrue(TEXT("the earlier recorded event stays recorded"), Ledger.HasRecordedEvent(BaseKey));

	// A re-registered target mints a fresh id; only events naming the live id
	// are accepted again.
	Target = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	TestEqual(TEXT("the re-registered target mints the next id"), Target, static_cast<FEntityId>(3));
	FCombatEventKey RebornTargetKey{Registry.GetCurrentEpoch(), Source, Shot, 1, Target};
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("an event for the re-registered target is accepted"), Ledger.TryRecordHitEvent(RebornTargetKey, Reason));

	// Unregistering the source refuses its events as UnknownSource.
	TestTrue(TEXT("the source unregisters"), Registry.UnregisterEntity(Source, Registry.GetCurrentEpoch()));
	FCombatEventKey AfterSourceGone = RebornTargetKey;
	AfterSourceGone.PelletIndex = 2;
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("a hit event for an unregistered source is refused"), Ledger.TryRecordHitEvent(AfterSourceGone, Reason));
	TestEqual(TEXT("the unregistered source names UnknownSource"), Reason, EHitLedgerRejectReason::UnknownSource);

	// A rebuilt world refuses old-epoch keys even when the numeric ids exist
	// again in the new generation.
	Registry.BeginNextWorldEpoch();
	const FCombatEpoch NewEpoch = Registry.GetCurrentEpoch();
	const FEntityId NewSource = Registry.RegisterEntity(nullptr, Meta, NewEpoch);
	const FEntityId NewTarget = Registry.RegisterEntity(nullptr, Meta, NewEpoch);
	const FCombatEventKey StaleEpochKey{1, NewSource, Shot, 0, NewTarget};
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("an old-epoch hit event is refused"), Ledger.TryRecordHitEvent(StaleEpochKey, Reason));
	TestEqual(TEXT("the old-epoch key names StaleEpoch"), Reason, EHitLedgerRejectReason::StaleEpoch);

	// An unusable (half-empty) key never reaches the ledger, bound or not.
	FCombatEventKey ZeroedKey;
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("a half-empty key is refused"), Ledger.TryRecordHitEvent(ZeroedKey, Reason));
	TestEqual(TEXT("the half-empty key names UnusableKey"), Reason, EHitLedgerRejectReason::UnusableKey);

	// A ledger without a bound registry refuses usable requests explicitly.
	FHitLedger UnboundLedger;
	const FCombatEventKey UsableKey{NewEpoch, NewSource, 1, 0, NewTarget};
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("an unbound ledger refuses the request"), UnboundLedger.TryRecordHitEvent(UsableKey, Reason));
	TestEqual(TEXT("the unbound ledger names UnboundRegistry"), Reason, EHitLedgerRejectReason::UnboundRegistry);
	return true;
}

// ---------------------------------------------------------------------------
// LedgerShotControlDedup
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010LedgerShotControlDedup,
	"UEMMO.Tasks.M5_010.LedgerShotControlDedup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010LedgerShotControlDedup::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	FHitLedger Ledger;
	Ledger.BindRegistry(&Registry);

	const FCombatEntityMetadata Meta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));
	const FEntityId Source = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId TargetA = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId TargetB = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FShotId Shot = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());

	// Shotgun: pellet 0 and pellet 1 of one shot both damage the same target
	// (two distinct event keys, two damage events).
	const FCombatEventKey PelletZeroKey{Registry.GetCurrentEpoch(), Source, Shot, 0, TargetA};
	const FCombatEventKey PelletOneKey{Registry.GetCurrentEpoch(), Source, Shot, 1, TargetA};
	EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("pellet 0 damages the target"), Ledger.TryRecordHitEvent(PelletZeroKey, Reason));
	TestTrue(TEXT("pellet 1 damages the target again"), Ledger.TryRecordHitEvent(PelletOneKey, Reason));
	TestEqual(TEXT("two pellet events are recorded"), Ledger.GetNumRecordedEvents(), 2);

	// The control key drops the pellet: the shot's launch/knockdown control is
	// deduped once per (Epoch, Source, Shot, Target).
	TestTrue(TEXT("the control key drops the pellet"), MakeCombatShotControlKey(PelletZeroKey) == MakeCombatShotControlKey(PelletOneKey));
	TestTrue(TEXT("the shot key drops the pellet too"), MakeCombatLedgerShotKey(PelletZeroKey) == MakeCombatLedgerShotKey(PelletOneKey));
	FCombatEventKey OtherShotKey = PelletOneKey;
	OtherShotKey.ShotId = Shot + 1;
	TestTrue(TEXT("a different shot still yields a different control key"), MakeCombatShotControlKey(PelletOneKey) != MakeCombatShotControlKey(OtherShotKey));

	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the shot accepts control once"), Ledger.TryAcceptShotControl(PelletZeroKey, Reason));
	TestEqual(TEXT("the accepted control carries no rejection"), Reason, EHitLedgerRejectReason::None);
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("the same shot refuses control again through another pellet"), Ledger.TryAcceptShotControl(PelletOneKey, Reason));
	TestEqual(TEXT("the second control names DuplicateControl"), Reason, EHitLedgerRejectReason::DuplicateControl);
	TestEqual(TEXT("exactly one control was accepted"), Ledger.GetNumAcceptedControls(), 1);
	TestTrue(TEXT("the accepted control is found pellet-insensitively"), Ledger.HasAcceptedControl(PelletOneKey));

	// A second target of the same shot is controlled independently.
	const FCombatEventKey OtherTargetKey{Registry.GetCurrentEpoch(), Source, Shot, 0, TargetB};
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the same shot controls the other target separately"), Ledger.TryAcceptShotControl(OtherTargetKey, Reason));
	TestEqual(TEXT("two controls are accepted"), Ledger.GetNumAcceptedControls(), 2);

	// The tracked shot state reflects both surfaces.
	FCombatLedgerShotState State;
	TestTrue(TEXT("the shot instance is tracked"), Ledger.FindTrackedShot(Registry.GetCurrentEpoch(), Source, Shot, State));
	TestFalse(TEXT("the tracked shot is still active"), State.bEnded);
	TestEqual(TEXT("the shot recorded two pellet events"), State.RecordedEventCount, 2);
	TestEqual(TEXT("the shot accepted two controls"), State.AcceptedControlCount, 2);
	TestEqual(TEXT("exactly one shot instance is active"), Ledger.GetNumActiveShots(), 1);
	TestEqual(TEXT("exactly one shot instance is tracked"), Ledger.GetNumTrackedShots(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// LedgerCapacityEndBaseline
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010LedgerCapacityEndBaseline,
	"UEMMO.Tasks.M5_010.LedgerCapacityEndBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010LedgerCapacityEndBaseline::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	FHitLedger Ledger;
	Ledger.BindRegistry(&Registry);
	Ledger.SetCapacity(2);
	TestEqual(TEXT("the capacity is configurable"), Ledger.GetCapacity(), 2);

	const FCombatEntityMetadata Meta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));
	const FEntityId Source = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId Target = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FShotId ShotOne = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	const FShotId ShotTwo = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	const FShotId ShotThree = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	const FCombatEventKey ShotOneHit{Registry.GetCurrentEpoch(), Source, ShotOne, 0, Target};
	const FCombatEventKey ShotTwoHit{Registry.GetCurrentEpoch(), Source, ShotTwo, 0, Target};
	const FCombatEventKey ShotThreeHit{Registry.GetCurrentEpoch(), Source, ShotThree, 0, Target};

	// The capacity bounds active shot instances: the first two attacks open
	// instances, the third is refused as a new instance.
	EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the first attack opens an instance"), Ledger.TryRecordHitEvent(ShotOneHit, Reason));
	TestTrue(TEXT("the second attack opens an instance"), Ledger.TryRecordHitEvent(ShotTwoHit, Reason));
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("the third attack is refused at full capacity"), Ledger.TryRecordHitEvent(ShotThreeHit, Reason));
	TestEqual(TEXT("the refused attack names CapacityFull"), Reason, EHitLedgerRejectReason::CapacityFull);
	TestEqual(TEXT("two instances stay active"), Ledger.GetNumActiveShots(), 2);

	// Active instances are never evicted: an already-open shot keeps taking
	// new pellet events while the capacity is full.
	FCombatEventKey ShotOnePellet = ShotOneHit;
	ShotOnePellet.PelletIndex = 1;
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("an active shot keeps accepting pellet events"), Ledger.TryRecordHitEvent(ShotOnePellet, Reason));
	TestEqual(TEXT("three events are recorded"), Ledger.GetNumRecordedEvents(), 3);

	// A control on an active shot still works at full capacity (same instance).
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("an active shot still accepts its control"), Ledger.TryAcceptShotControl(ShotTwoHit, Reason));
	TestEqual(TEXT("one control is accepted"), Ledger.GetNumAcceptedControls(), 1);

	// Ending a shot tombstones the instance and clears exactly its keys.
	TestTrue(TEXT("the first shot ends"), Ledger.EndShot(Registry.GetCurrentEpoch(), Source, ShotOne));
	TestEqual(TEXT("the ended shot's events returned to the pool"), Ledger.GetNumRecordedEvents(), 1);
	TestEqual(TEXT("one instance stays active"), Ledger.GetNumActiveShots(), 1);
	TestEqual(TEXT("the ended shot is still tracked as a tombstone"), Ledger.GetNumTrackedShots(), 2);
	TestFalse(TEXT("the ended shot's events are cleared"), Ledger.HasRecordedEvent(ShotOneHit));
	TestFalse(TEXT("the ended shot's second pellet is cleared"), Ledger.HasRecordedEvent(ShotOnePellet));

	// A late re-send of an ended shot's key is refused - the tombstone keeps
	// the idempotency guarantee past the shot's lifetime.
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("an ended shot refuses a late re-send"), Ledger.TryRecordHitEvent(ShotOneHit, Reason));
	TestEqual(TEXT("the late re-send names ShotAlreadyEnded"), Reason, EHitLedgerRejectReason::ShotAlreadyEnded);

	// Freeing the ended instance lets a new attack in.
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the third attack is accepted after capacity freed"), Ledger.TryRecordHitEvent(ShotThreeHit, Reason));

	// Ending unknown, stale-epoch or already-ended shots is refused.
	TestFalse(TEXT("an unknown shot cannot end"), Ledger.EndShot(Registry.GetCurrentEpoch(), Source, 999));
	TestFalse(TEXT("an ended shot cannot end twice"), Ledger.EndShot(Registry.GetCurrentEpoch(), Source, ShotOne));
	TestFalse(TEXT("a shot of another epoch cannot end"), Ledger.EndShot(Registry.GetCurrentEpoch() + 1, Source, ShotTwo));

	// Ending the remaining shots clears their events and controls and returns
	// the ledger to its baseline.
	TestTrue(TEXT("the second shot ends"), Ledger.EndShot(Registry.GetCurrentEpoch(), Source, ShotTwo));
	TestFalse(TEXT("the ended shot's control is cleared"), Ledger.HasAcceptedControl(ShotTwoHit));
	TestEqual(TEXT("all controls returned to the pool"), Ledger.GetNumAcceptedControls(), 0);
	TestTrue(TEXT("the third shot ends"), Ledger.EndShot(Registry.GetCurrentEpoch(), Source, ShotThree));
	TestEqual(TEXT("every event returned to the pool"), Ledger.GetNumRecordedEvents(), 0);
	TestEqual(TEXT("every control returned to the pool"), Ledger.GetNumAcceptedControls(), 0);
	TestEqual(TEXT("no instance stays active"), Ledger.GetNumActiveShots(), 0);
	TestEqual(TEXT("tombstones remain tracked"), Ledger.GetNumTrackedShots(), 3);

	// Unload: even the tombstones are dropped - the full baseline.
	Ledger.Reset();
	TestEqual(TEXT("the unload dropped the tombstones"), Ledger.GetNumTrackedShots(), 0);
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("after the unload the ledger records again from a clean state"), Ledger.TryRecordHitEvent(ShotOneHit, Reason));
	TestEqual(TEXT("the clean-state record carries no rejection"), Reason, EHitLedgerRejectReason::None);
	return true;
}

// ---------------------------------------------------------------------------
// MeleeFirearmDistinctPublicKeys
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_010MeleeFirearmDistinctPublicKeys,
	"UEMMO.Tasks.M5_010.MeleeFirearmDistinctPublicKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_010MeleeFirearmDistinctPublicKeys::RunTest(const FString& Parameters)
{
	FCombatEntityRegistry Registry;
	FHitLedger Ledger;
	Ledger.BindRegistry(&Registry);

	const FCombatEntityMetadata Meta = MakeMetadata(TEXT("Enemy"), TEXT("Pawn"));
	const FEntityId Source = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());
	const FEntityId Target = Registry.RegisterEntity(nullptr, Meta, Registry.GetCurrentEpoch());

	// The card scenario: one source attacks with a melee attack whose local
	// AttackInstanceId is 1, then with a firearm shot whose local ShotSequence
	// is also 1. Both draw one common ActionSequence from the registry, so the
	// public keys differ even though both local counters equal 1.
	const FShotId MeleePublicSequence = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	const FShotId FirearmPublicSequence = Registry.AllocateActionSequence(Source, Registry.GetCurrentEpoch());
	TestEqual(TEXT("the melee attack draws the first public sequence"), MeleePublicSequence, static_cast<FShotId>(1));
	TestEqual(TEXT("the firearm shot draws the next public sequence"), FirearmPublicSequence, static_cast<FShotId>(2));
	TestTrue(TEXT("identical local sequences produce different public keys"), MeleePublicSequence != FirearmPublicSequence);

	const FCombatEventKey MeleeKey{Registry.GetCurrentEpoch(), Source, MeleePublicSequence, 0, Target};
	const FCombatEventKey FirearmKey{Registry.GetCurrentEpoch(), Source, FirearmPublicSequence, 0, Target};
	EHitLedgerRejectReason Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the melee hit is accepted once"), Ledger.TryRecordHitEvent(MeleeKey, Reason));
	TestTrue(TEXT("the firearm hit is accepted once"), Ledger.TryRecordHitEvent(FirearmKey, Reason));
	TestEqual(TEXT("two hits are recorded"), Ledger.GetNumRecordedEvents(), 2);

	// Each side's re-send is refused on its own public key.
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("the melee re-send is refused"), Ledger.TryRecordHitEvent(MeleeKey, Reason));
	TestEqual(TEXT("the melee re-send names DuplicateEvent"), Reason, EHitLedgerRejectReason::DuplicateEvent);
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("the firearm re-send is refused"), Ledger.TryRecordHitEvent(FirearmKey, Reason));
	TestEqual(TEXT("the firearm re-send names DuplicateEvent"), Reason, EHitLedgerRejectReason::DuplicateEvent);
	TestEqual(TEXT("no extra event was recorded"), Ledger.GetNumRecordedEvents(), 2);

	// The two public keys also carry independent shot control.
	Reason = EHitLedgerRejectReason::None;
	TestTrue(TEXT("the melee shot's control is accepted"), Ledger.TryAcceptShotControl(MeleeKey, Reason));
	TestTrue(TEXT("the firearm shot's control is accepted separately"), Ledger.TryAcceptShotControl(FirearmKey, Reason));
	Reason = EHitLedgerRejectReason::None;
	TestFalse(TEXT("the melee shot's control is accepted only once"), Ledger.TryAcceptShotControl(MeleeKey, Reason));
	TestEqual(TEXT("the melee control re-send names DuplicateControl"), Reason, EHitLedgerRejectReason::DuplicateControl);
	return true;
}

#endif
