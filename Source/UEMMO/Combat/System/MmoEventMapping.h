#pragma once

/**
 * ===========================================================================
 * M5-052: MMO event mapping boundary - DOCUMENTATION ONLY.
 * ===========================================================================
 *
 * This header contains NOTHING but comments. It declares no type, no
 * function, no constant and includes nothing. It is the in-source boundary
 * document that states, for every M5 combat event/type/enum, where the value
 * may live in a future MMO deployment: pure single-player local, needs
 * server confirmation before any protocol use, or server authoritative.
 * The executable contracts stay in the headers they were frozen in
 * (CombatEventTypes.h - M5-002, DamageResolver.h - M5-011,
 * UnifiedHitApplier.h - M5-012); this file never restates their definitions
 * and adds no behavior.
 *
 * This is NOT a network implementation and NOT a protocol proposal with
 * fixed fields. M4-000 (Docs/Tasks/M4-000.md, DEFERRED) stays the single
 * entry point for future server integration: the user's existing server is
 * the authoritative source of interface facts. Nothing here assumes an UE
 * Dedicated Server, UE replication, a login protocol or a transport; every
 * server-side field name below is a "to be provided" placeholder that M4-000
 * collection must fill (protocol format, transport, auth, ids, map
 * lifecycle, time/tick units, authority, skill numbering, hit/launch
 * messages, inventory/settlement idempotency, save boundary).
 *
 * Boundary labels used throughout (exactly one primary label per item):
 * - [单机本地]  Local only: exists and dies inside this client build; must
 *               never be sent as an authoritative fact or persisted into a
 *               server payload.
 * - [需服务端确认] Needs server confirmation: the local semantics are
 *               stable but the server-side counterpart, field format or
 *               ownership is unknown until M4-000 collection provides it.
 * - [服务端权威] Server authoritative: in the future MMO the SERVER owns
 *               the final decision; the local value is at best a prediction
 *               the server may override, at worst untrusted input.
 *
 * Ground rules carried over from the M5-052 card (all restated per item
 * below, never weakened):
 * - A client GUID, a client collision result or a client-reported hit
 *   location is never trusted authorization. A future damage request
 *   carries inputs, never final_damage: the server re-resolves the damage
 *   from its own authoritative context.
 * - Actor weak references (TWeakObjectPtr) and local clocks
 *   (GameClockSeconds, FCombatTick/EpochTick, injected session clocks)
 *   never enter a proposed persistence or protocol payload. All units and
 *   all dedup scopes are stated explicitly below.
 * - Value snapshots only: every M5 event/key type is a pointer-free,
 *   trivially copyable value (pinned by static_asserts in their own
 *   headers), which is exactly what makes them safe candidates for logs and
 *   - after server confirmation - protocol payloads.
 *
 * ===========================================================================
 * SECTION 1 - M5-002 key types (CombatEventTypes.h): per-type boundary
 * ===========================================================================
 *
 * | Type                  | Boundary                              | Mapping notes |
 * |-----------------------|---------------------------------------|---------------|
 *
 * FCombatEpoch (uint64)
 *   [单机本地] as a value: minted once per CombatComponent lifetime by the
 *   local registry (010), from 1, 0 invalid. [需服务端确认] the server-side
 *   counterpart: a future server needs its own room/instance generation
 *   (or reconnect generation) so that a stale client epoch is rejected as a
 *   whole; whether the client epoch maps 1:1 onto that server generation or
 *   is purely local with server-side re-minting is UNPROVIDED. [服务端权威]
 *   generation invalidation on leave-room/retry/reconnect: only the server
 *   may declare a generation dead for everyone; the local rule (World
 *   EndPlay invalidates old epochs) is the single-player shadow of that.
 *
 * FEntityId (uint64)
 *   [单机本地] as a value: allocated by the local world entity registry
 *   (010) per World, from 1, 0 invalid, never a pointer value. Legacy
 *   GetUniqueID compatibility is a local bridge only. [需服务端确认] the
 *   server's character/instance id rules (M4-000 item 3) and the
 *   correspondence table local registry id <-> server entity id; the local
 *   uint64 must not be presented to a server as a persistent identity.
 *
 * FShotId (uint64)
 *   [单机本地] as an allocation: the COMMON per-(Epoch, SourceEntity)
 *   ActionSequence slot from the 010 registry (melee/firearms/vehicles
 *   share one monotonic sequence; module-local counters only associate).
 *   [需服务端确认] who allocates the server-side action sequence (client
 *   proposes vs server assigns), and whether the server keeps its own
 *   per-source monotonic sequence for ordering and dedup.
 *
 * FPelletIndex (uint8)
 *   [单机本地] entirely: a 0..N-1 discriminator inside one shot (0 for
 *   single-pellet, 255 invalid sentinel). [需服务端确认] whether a future
 *   protocol carries pellets as sub-events of one shot message or as
 *   separate messages under one shot id; the local rule that per-pellet
 *   damage is distinct while per-shot control is pellet-insensitive must
 *   survive any such mapping.
 *
 * FTargetId (uint64)
 *   [单机本地] as a value: the 010 registry's id of the damaged entity
 *   (0 invalid). Same confirmation needs as FEntityId; the legacy
 *   static_cast<uint64>(GetUniqueID()) source is a local bridge until the
 *   registry replaces it, never a server identity.
 *
 * FCombatTick (uint32)
 *   [单机本地] absolutely: one component's own monotonic tick counter. Not
 *   a clock, not seconds, not comparable across components, never
 *   comparable with GameClockSeconds. It MUST NOT be sent to a server as a
 *   time base, deadline or ordering key and MUST NOT enter a persistence
 *   payload. The server's clock/tick unit is an M4-000 collection item
 *   (item 5); no local-to-server time conversion exists or may be invented
 *   here.
 *
 * FCombatEventKey (five-tuple: Epoch, EntityId, ShotId, PelletIndex,
 * TargetId; operator== / GetTypeHash local)
 *   [单机本地] as the dedup surface: five equal fields mean "the same hit";
 *   the damage dedup scope is the FULL five-tuple, the control dedup scope
 *   is the pellet-free prefix (Epoch, EntityId, ShotId, TargetId) - both
 *   scopes are frozen semantics any mapping must preserve. [需服务端确认]
 *   whether this five-tuple (or its stable derived EventId) becomes the
 *   hit idempotency token of a future protocol, or the server re-keys hits
 *   with its own id scheme. [服务端权威] the FINAL duplicate decision: the
 *   local FHitLedger verdict is a client-side face only; a server replaying
 *   or reordering messages needs its own idempotency surface (see Section
 *   6) and its verdict overrides the local one.
 *
 * Invalid sentinels and validity helpers (InvalidCombatEpoch /
 * InvalidCombatEntityId / InvalidCombatShotId / InvalidCombatTargetId = 0,
 * InvalidCombatPelletIndex = 255; IsValidCombatEpoch / IsValidCombatEntityId
 * / IsValidCombatShotId / IsValidCombatTargetId / IsValidCombatPelletIndex /
 * IsUsableCombatEventKey)
 *   [单机本地] entirely: local encoding hygiene (0 and 255 reserved, all
 *   four id fields populated before a key enters a ledger). A future
 *   protocol MAY reuse the same sentinel discipline on the wire, but that
 *   is a server-confirmation item, not an established fact.
 *
 * EHitDecision : uint8 { Apply=0, Block_Invulnerable=1, Block_Immune=2,
 * Block_Ignore=3 } (append-only numbering)
 *   [单机本地] as the local unified entry's verdict enum, including its
 *   local semantics: Apply = applied exactly once; Block_Invulnerable =
 *   get-up window refused the whole hit; Block_Immune = policy immunity;
 *   Block_Ignore = filtered before evaluation (stale epoch, unknown
 *   entity, duplicate key, filtered source). [需服务端确认] the server's
 *   verdict vocabulary and its mapping onto these four values (does the
 *   server distinguish invulnerable vs immune vs ignore?), so client logs
 *   and server receipts can be correlated without renumbering. [服务端权威]
 *   the future authoritative verdict itself: a client-side Block_* is a
 *   local refusal, never a reportable authoritative outcome; a client-side
 *   Apply under server authority is a prediction awaiting confirmation.
 *
 * Ownership ladder / two clocks / space comments frozen in
 * CombatEventTypes.h (Profile=GameInstance, World/session=UWorld,
 * CombatComponent=Pawn lifetimes; GameClockSeconds vs EpochTick; X lateral,
 * Y depth, Z height, facing +-1 mirrors X only)
 *   [单机本地] as written; they define WHICH values could ever cross the
 *   boundary (value snapshots at World or CombatComponent lifetime) and
 *   which never can (Actor pointers, local clocks). The units they pin
 *   (health points, seconds, cm/s, cm) are the units any future protocol
 *   must state explicitly per field - server confirmation required.
 *
 * ===========================================================================
 * SECTION 2 - M5-011 damage resolution (DamageResolver.h): boundary
 * ===========================================================================
 *
 * ResolveDamage(const FDamageProfile&, const FTargetReaction&, AttackPower,
 * Defense) -> FDamageOutcome
 *   [单机本地] as implemented: a pure function - no World, no Actor, no
 *   mutable state, no RNG - reproducing the M1-019 formula byte for byte
 *   (max(1, round((base + AttackPower * coefficient) * 100 / (100 + max(0,
 *   Defense))))), with immunity resolving to exactly 0 and illegal contexts
 *   refused with a reason. [服务端权威] the FUTURE final damage: in the MMO
 *   the server re-resolves damage from ITS authoritative context (its
 *   attack power, its defense, its policy tables) and its number is the
 *   truth; a client request carries the inputs (profile id, resolved
 *   attacker stat snapshot, target id) and NEVER a proposed final_damage -
 *   a client-computed FinalDamage is a prediction/local display value that
 *   the server is free to overwrite. [需服务端确认] whether the server
 *   executes the same formula and reads the same configuration revision
 *   (see Section 8, ConfigRevision), i.e. whether the local resolver
 *   becomes a mirrored prediction of a server-side calculator or stays
 *   purely presentational.
 *
 * FDamageOutcome { FinalDamage (health points, >=1 when applied, exactly 0
 * when refused or immune), bWasImmune, bWasBlocked, bControlAccepted,
 * bStaggerAccepted, bLaunchAccepted, bControlFacesPoiseThreshold,
 * BlockReason }
 *   [单机本地] as a value snapshot. Boundary-relevant fields:
 *   - FinalDamage: local prediction only under future server authority
 *     (never submitted as a result); unit is health points.
 *   - bControlFacesPoiseThreshold: a fact about the target's PoiseMax>0;
 *     poise pool depletion is caller-side state [单机本地]; whether poise
 *     lives server-side in the MMO [需服务端确认].
 *   - All control acceptance bits are the local half of the negotiation
 *     whose future authoritative half lives server-side (Section 5).
 *
 * EDamageBlockReason : uint8 { None=0, NonFiniteInput=1, NegativeInput=2,
 * NonPositiveBaseDamage=3, OverflowedResult=4 } (append-only)
 *   [单机本地] the local refusal vocabulary of an illegal context. A future
 *   server needs an equivalent refusal vocabulary for the requests IT
 *   refuses (bad context, unknown profile, mismatched revision) [需服务端
 *   确认]; local enum numbers must not be assumed to match server error
 *   codes.
 *
 * ===========================================================================
 * SECTION 3 - M5-012 unified hit entry (UnifiedHitApplier.h): boundary
 * ===========================================================================
 *
 * FUnifiedHitRequest { Epoch, AttackerEntityId, ShotId, PelletIndex,
 * TargetEntityId, TargetActor (TWeakObjectPtr<AActor>), Attack
 * (FDamageProfile), TargetReaction (FTargetReaction), AttackPower (float,
 * pre-resolved by caller), HitLocation (FVector, world cm, X lateral/Y
 * depth/Z height), MakeEventKey() }
 *   [单机本地] as implemented. Per-field mapping notes for a FUTURE
 *   protocol (all placeholders until M4-000 collection):
 *   - Epoch/AttackerEntityId/ShotId/PelletIndex/TargetEntityId: the
 *     five-tuple key fields (Section 1 boundaries apply unchanged).
 *   - TargetActor: [单机本地] a resolve-once weak reference. It MUST NEVER
 *     be serialized, persisted or transmitted; a future message names the
 *     target by its (server-confirmed) entity id only, and the receiver
 *     resolves the actor locally through the registry.
 *   - Attack (FDamageProfile: DamageProfileId, BaseDamage, AttackCoefficient,
 *     HitStunSeconds [s], KnockbackCmPerSecond [cm/s], LaunchCmPerSecond
 *     [cm/s], HitStopSeconds [s]): [需服务端确认] whether a request carries
 *     the whole profile value or only the profile id for the server to look
 *     up in its own tables. Carrying values is the anti-tamper worst case;
 *     carrying ids plus a ConfigRevision is the natural mapping.
 *   - TargetReaction (FTargetReaction policy: allow gates, air-cycle caps,
 *     poise, immunity flags): [需服务端确认] the server owns target policy
 *     for its own entities; a client should never upload its target's
 *     policy as a fact.
 *   - AttackPower: caller-resolved local stat. [服务端权威] the future
 *     attack power comes from the server's authoritative character state;
 *     a client-reported power is untrusted input the server may discard.
 *   - HitLocation: [单机本地] a client-reported world-space claim. A future
 *     server must treat it as an UNTRUSTED spatial claim to validate
 *     (reachability, geometry, timing), never as authorization - the
 *     client's collision result proves nothing.
 *   - NOT present in the implemented request: ConfigRevision,
 *     SimulationTick, weapon/projectile instance FGuids (those live in the
 *     section-2 discussion draft and in 022/027's planned contracts). Any
 *     future protocol field list must be built from the implemented
 *     request plus explicitly confirmed additions, not from the draft.
 *
 * FUnifiedHitControlOutcome { bStagger, bLaunch, bKnockdown, AnyAccepted() }
 *   [单机本地] the local control application verdict after the target
 *   reaction gates (super armor = damage without control; shot control
 *   accepted once per pellet-free (Epoch, Source, Shot, Target) via the
 *   ledger). [需服务端确认] which control outcomes the server adjudicates
 *   authoritatively (knockdown/launch windows drive gameplay state, so
 *   server authority is the working assumption for the future MMO; the
 *   local decision then becomes client-side prediction with rollback).
 *
 * FUnifiedHitOutcome { Decision, DamageApplied (health points actually
 * removed), bWasImmune, bWasBlocked, bTargetDied, Control, DamageBlockReason,
 * LedgerReason, RefuseReason, WasApplied(), BridgesLegacyHitConfirmed(),
 * TakesPureControlPath() }
 *   [单机本地] the local result of one submission. [服务端权威] in the
 *   future MMO the authoritative outcome is the server's receipt (its
 *   damage number, its death fact, its control verdict); this local
 *   outcome is the prediction the receipt reconciles. The two bridging
 *   helpers are presentation-routing rules for the LOCAL legacy
 *   OnHitConfirmed path [单机本地] and never become protocol semantics:
 *   the server states its own presentation events (M4-000 item 8).
 *   bTargetDied: [单机本地] a fact about the local HealthComponent; death
 *   is a server-authoritative fact in the MMO (including death-resistance
 *   capping: the server re-evaluates the >=1 HP floor against its own
 *   policy).
 *
 * EUnifiedHitRefuseReason : uint8 { None=0, UnusableKey=1,
 * InvalidTargetActor=2, MissingHealthComponent=3, TargetDead=4,
 * GetUpProtection=5, SameFaction=6, LedgerRefused=7,
 * DamageContextRefused=8 } (append-only)
 *   [单机本地] the applier face's refusal vocabulary (validation order:
 *   everything refused BEFORE the ledger key is consumed, so a refusal
 *   never poisons a later instance). [需服务端确认] the server-side
 *   equivalent rejection reasons for hits it refuses; local numbers are
 *   not server error codes.
 *
 * IUnifiedHitDamageCalculator / GetDefaultUnifiedHitCalculator /
 * ApplyUnifiedHit(const FUnifiedHitRequest&, FHitLedger&[, calculator])
 *   [单机本地] the game-thread single entry and its injectable damage
 *   seam (default: M5-011 resolver + legacy target-side defense lookup).
 *   The MMO reading of this architecture: the client entry stays the local
 *   submission point whose verdict is prediction, and the FUTURE
 *   authoritative analogue is a server-side "receive hit claim -> validate
 *   identity/context -> dedup -> resolve damage -> apply -> receipt"
 *   pipeline [服务端权威]. [需服务端确认] every field that pipeline reads
 *   and returns; nothing here assumes the server runs UE or this code.
 *
 * ===========================================================================
 * SECTION 4 - Supporting faces consumed by the entry (context rows)
 * ===========================================================================
 *
 * FDamageProfile / FTargetReaction (M5-003 value types)
 *   [单机本地] as loaded from the local validated catalog (007, immutable
 *   per ConfigRevision). [需服务端确认] the server's configuration source
 *   and revision discipline for the same tables; both sides resolving
 *   different table content while exchanging ids would silently corrupt
 *   every mapping below.
 *
 * FHitLedger / EHitLedgerRejectReason : uint8 { None=0, UnusableKey=1,
 * StaleEpoch=2, UnknownSource=3, UnknownTarget=4, DuplicateEvent=5,
 * DuplicateControl=6, ShotAlreadyEnded=7, CapacityFull=8,
 * UnboundRegistry=9 } (append-only) and the ledger's dedup scopes
 *   [单机本地] as implemented: World-lifetime idempotency face - event
 *   dedup on the full five-tuple, control dedup on the pellet-free prefix,
 *   capacity bounding ACTIVE shot instances (default 256) with
 *   refuse-never-evict, EndShot tombstones that keep refusing late
 *   re-sends, Reset on session end/world rebuild. [需服务端确认] the
 *   server's hit idempotency surface: duplicate messages across retries
 *   and reconnects must be idempotent SERVER-side; the local ledger alone
 *   is not sufficient protection, and the local capacity semantics
 *   (refuse new attacks when full) need a server-side analogue or an
 *   explicit decision that capacity stays client-only.
 *
 * DeriveCombatEventId (FNV-1a 64 over the five key fields, member order,
 * fixed byte order)
 *   [单机本地] the stable local EventId derivation (never pointer-derived).
 *   [需服务端确认] whether a future protocol uses this derivation, and if
 *   so that both sides agree on field order and byte order - or the server
 *   provides its own event id and this stays log-only.
 *
 * ===========================================================================
 * SECTION 5 - Future server responsibilities per gameplay domain
 * (planning statements, not protocol facts; M4-000 collection decides)
 * ===========================================================================
 *
 * Movement (local: CharacterMovement + depth play on Y, no Y lock)
 *   [需服务端确认] the client-prediction/server-authority boundary, the
 *   position report rate and format, and how Y-depth positioning is
 *   validated server-side. Today movement is fully local [单机本地].
 *
 * Fire (planned 022 fire transaction / 027 projectile reservations)
 *   [需服务端确认] who owns ammo/capacity/state so a shot commits exactly
 *   once; the local TryFire is an all-or-nothing transaction whose server
 *   analogue must keep that property [服务端权威 for ammunition truth in
 *   the MMO working assumption].
 *
 * Hit claim (this entry's request face)
 *   [需服务端确认] message semantics for "client claims a hit"; [服务端
 *   权威] the server validates identity, range/timing plausibility,
 *   faction and dedup, then decides. A client hit claim is never
 *   self-executing.
 *
 * Damage (M5-011 resolver face)
 *   [服务端权威] final damage: server re-resolves from its context;
 *   requests carry inputs, never results (see Section 2).
 *
 * Control (stagger/launch/knockdown)
 *   [需服务端确认] authoritative owner of control state (get-up protection
 *   windows, air-cycle caps, poise). Working assumption: server
 *   authoritative for state-driving controls, client-predicted for feel;
 *   the local once-per-shot control dedup must hold server-side too.
 *
 * Death (HealthComponent ownership)
 *   [服务端权威] the death fact and the death-resistance floor; local
 *   OnDied-once-per-lifetime remains the local component semantic.
 *
 * Loot/pickup (M3 drops/rewards, planned 007 catalog adaptation)
 *   [服务端权威] grant decisions; [需服务端确认] idempotency keys for
 *   grant/claim retries (the local SettlementId + AppliedSettlementIds
 *   pattern is the client-side precedent, not the server contract).
 *
 * ===========================================================================
 * SECTION 6 - Local value -> future server field mapping (TO BE PROVIDED)
 * ===========================================================================
 *
 * No server field below exists yet. Each row names the local value, the
 * placeholder for the server-side counterpart, and the collection source
 * that must fill it (M4-000 item numbers from Docs/Tasks/M4-000.md).
 * Nothing here may be implemented as a wire format before the user's
 * server interface facts arrive.
 *
 * | Local value                     | Future server field (placeholder)            | Status / collector |
 * |---------------------------------|----------------------------------------------|--------------------|
 * | FCombatEpoch                    | room/instance/reconnect generation id        | 需服务端确认 / M4-000 #4 #2 |
 * | FEntityId / FTargetId           | server entity & character/instance id rules  | 需服务端确认 / M4-000 #3 |
 * | FShotId (ActionSequence)        | server action/shot sequence per source       | 需服务端确认 / M4-000 #7 #8 |
 * | FPelletIndex                    | sub-event discriminator within a shot        | 需服务端确认 / M4-000 #8 |
 * | FCombatTick / SimulationTick    | (nothing - must NOT be sent as time)         | 单机本地禁上送 / M4-000 #5 |
 * | GameClockSeconds                | server clock unit & sync requirement         | 需服务端确认 / M4-000 #5 |
 * | FCombatEventKey (+EventId)      | hit idempotency token                        | 需服务端确认 / M4-000 #8 #9 |
 * | EHitDecision / refuse reasons   | server verdict & error-code vocabulary       | 需服务端确认 / M4-000 #8 |
 * | FDamageProfile (id vs value)    | skill/attack numbering + table version       | 需服务端确认 / M4-000 #7 |
 * | ConfigRevision (catalog layer)  | server table/content version field           | 需服务端确认 / M4-000 #7 #10 |
 * | FUnifiedHitOutcome (receipt)    | authoritative hit/damage receipt             | 服务端权威 / M4-000 #6 #8 |
 * | Death fact (bTargetDied)        | authoritative death event                    | 服务端权威 / M4-000 #8 |
 * | Loot grants (SettlementId face) | grant/claim idempotency keys                 | 服务端权威 / M4-000 #9 |
 * | Movement state                  | authority boundary & report rate             | 需服务端确认 / M4-000 #6 |
 *
 * NOTE on ConfigRevision: the implemented FUnifiedHitRequest does not carry
 * it; it lives at the catalog layer (007 GetConfigRevision) and in the
 * section-2 discussion-draft FHitRequest. Any future hit message that must
 * be revision-checked gets it as an explicitly confirmed addition.
 *
 * ===========================================================================
 * SECTION 7 - Protocol guarantees a future MMO needs (requirements, not
 * solutions; transport and format unknown until M4-000 provides them)
 * ===========================================================================
 *
 * - Latency: local Apply is at best a prediction. The boundary must allow
 *   reconciliation in BOTH directions: server receipts override local
 *   numbers (damage, death), and server refusals must be expressible as
 *   local rollbacks (Block_* + refuse reasons give the local faces to
 *   roll back through).
 * - Duplication: at-least-once delivery is the safe assumption. The
 *   five-tuple key discipline (event dedup full tuple, control dedup
 *   pellet-free, tombstones keep refusing ended shots, refuse-never-evict)
 *   is the local template; the server needs its own idempotency surface
 *   with at least the same strength (Section 4).
 * - Reordering: no local cross-component ordering exists to inherit
 *   (EpochTick is per-component, GameClockSeconds is a local clock). A
 *   future protocol must define its own ordering rule (server receive
 *   order or per-source sequence) and both sides must agree that hit
 *   outcomes do not depend on arrival order.
 * - Disconnect/reconnect: the local precedent is total generation
 *   invalidation (World EndPlay -> epochs dead, registry cleared, ledger
 *   Reset). The server must own the reconnect generation bump and state
 *   which in-flight claims survive; the client re-derives everything from
 *   a clean World/session state.
 * - Config version mismatch: a revision mismatch must REFUSE the affected
 *   requests loudly (the local catalog refuses missing required assets and
 *   stamps one revision everywhere); silently applying across versions
 *   would corrupt damage/control semantics on both sides. The refusal
 *   vocabulary for version mismatch is a server-confirmation item.
 *
 * ===========================================================================
 * SECTION 8 - Hard prohibitions (carry into any future protocol work)
 * ===========================================================================
 *
 * - No TWeakObjectPtr<AActor>, no raw Actor/World pointer, no UObject
 *   reference in a payload (value snapshots only - pinned by static_asserts
 *   in the owning headers).
 * - No FCombatTick, no GameClockSeconds, no injected session clock value
 *   sent as a server time base; no cross-clock conversion may be invented.
 * - No client-computed FinalDamage submitted as a result; no client GUID,
 *   collision result or HitLocation treated as authorization.
 * - No UE Dedicated Server / UE replication assumption, no login protocol
 *   invention: M4-000 stays the only entry point, the user's existing
 *   server stays the authority on every interface fact, and unmeasured
 *   compatibility is never claimed.
 * - Nothing in this file is executable or testable network behavior; any
 *   future implementation card must restate its own contract and tests.
 */
