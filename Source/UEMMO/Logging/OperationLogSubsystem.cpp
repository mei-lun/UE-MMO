// M3-023: operation log subsystem (user-requested 2026-09-30). GREEN
// implementation of the header contract:
// - every Log call appends one "[HH:MM:SS.mmm][Category] Message" line to
//   ProjectSavedDir/OperationLogs/OperationLog.log in its own write call
//   (crash safety: a process death can lose at most the line being written),
// - every write first runs the daily-rollover gate: when the injectable
//   provider's date differs from ActiveLogDate (or no day is open yet) the
//   file is deleted and reopened with a fresh "=== Operation log started
//   YYYY-MM-DD (build) ===" header (the user's cross-day clear requirement),
// - RegisterPlayer binds the player's CombatComponent Started/Finished/
//   HitConfirmed and the world's RoomSessionSubsystem OnRunStarted/OnRunEnded
//   through weak lambdas with remembered handles.
//
// M5-053: the shot event-chain face (see the header). The store below is a
// plain logic class over the same subsystem: one bounded LRU map of chains
// keyed by (Epoch, SourceEntityId, ShotId), rows dedup by (stage, pellet,
// target, verdict), Info rows of the per-frame Projectile stage may be
// sampled down, refusal/critical rows are never sampled/dropped and survive
// the chain's end, and every capacity drop increments an auditable counter.

#include "OperationLogSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Logging/LogMacros.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/System/UnifiedHitApplier.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Weapons/WeaponComponent.h"

namespace
{
	// The three fixed categories of the card plus the file layout. Every
	// anonymous-namespace symbol carries the M3_023 prefix so the per-file
	// translation unit can never collide with another module TU.
	const FName M3_023_CategoryInput(TEXT("Input"));
	const FName M3_023_CategoryState(TEXT("State"));
	const FName M3_023_CategoryResponse(TEXT("Response"));
	const TCHAR* M3_023_LogDirectory = TEXT("OperationLogs");
	const TCHAR* M3_023_LogFileName = TEXT("OperationLog.log");
}

// ===========================================================================
// M5-053: the shot event-chain store (header contract; every symbol carries
// the M5_053 prefix like the M3_023 block above).
// ===========================================================================

// NOTE: the store class and its helpers live at GLOBAL scope (no anonymous
// namespace): the header forward-declares FShotChainStore at global scope for
// the raw owning-pointer member, and MSVC (C2888) rejects member definitions
// of a global-scope class inside an anonymous namespace. Every symbol below
// carries the M5_053 prefix so the TU cannot collide.

// Capacity policy (all bounds documented on the header API):
// - 64 ACTIVE chains: the card's example bound. A Begin/auto-open past the
//   cap force-closes the least-recently-used OPEN chain (with its Cleanup
//   row) instead of growing without bound - a mount that dies mid-shot
//   without its ReleaseFire cannot leak an open chain forever.
// - 192 TRACKED chains: ended chains stay queryable inside this retention
//   window; the oldest ENDED chain is hard-dropped first, so completed
//   chains survive as long as possible before eviction.
// - 256 rows per chain: a full chain drops further Info rows (counted) but
//   never a refusal/critical row - dedup bounds refusal repeats and the
//   ledger bounds the distinct keys, so an error chain cannot be deleted.
constexpr int32 M5_053_MaxActiveChains = 64;
constexpr int32 M5_053_MaxTrackedChains = 192;
constexpr int32 M5_053_MaxEventsPerChain = 256;

/** The (Epoch, Source, Shot) chain identity (the event key's pellet-free prefix). */
struct FM5_053_ChainKey
{
	FCombatEpoch Epoch = InvalidCombatEpoch;
	FEntityId Source = InvalidCombatEntityId;
	FShotId Shot = InvalidCombatShotId;

	bool operator==(const FM5_053_ChainKey& Other) const
	{
		return Epoch == Other.Epoch && Source == Other.Source && Shot == Other.Shot;
	}
};

inline uint32 GetTypeHash(const FM5_053_ChainKey& Key)
{
	// Unqualified calls: FName's hash is a class-scope friend only ADL
	// finds (the M5-050 lesson); ::-qualified lookup misses it.
	uint32 Hash = GetTypeHash(Key.Epoch);
	Hash = HashCombine(Hash, GetTypeHash(Key.Source));
	Hash = HashCombine(Hash, GetTypeHash(Key.Shot));
	return Hash;
}

/**
 * The dedup identity of one row: repeated identical (stage, pellet,
 * target, verdict) calls merge into the first row's RepeatCount instead of
 * duplicating it. The verdict is part of the key, so two DIFFERENT
 * refusals on the same (stage, pellet, target) stay two traceable rows.
 */
struct FM5_053_DedupKey
{
	EShotChainStage Stage = EShotChainStage::ShotCommitted;
	FPelletIndex PelletIndex = InvalidCombatPelletIndex;
	FTargetId TargetId = InvalidCombatTargetId;
	FName Result;

	bool operator==(const FM5_053_DedupKey& Other) const
	{
		return Stage == Other.Stage && PelletIndex == Other.PelletIndex
			&& TargetId == Other.TargetId && Result == Other.Result;
	}
};

inline uint32 GetTypeHash(const FM5_053_DedupKey& Key)
{
	uint32 Hash = GetTypeHash(static_cast<uint8>(Key.Stage));
	Hash = HashCombine(Hash, GetTypeHash(Key.PelletIndex));
	Hash = HashCombine(Hash, GetTypeHash(Key.TargetId));
	Hash = HashCombine(Hash, GetTypeHash(Key.Result));
	return Hash;
}

/** Opening metadata (the committing caller's snapshot; empty for auto-opened chains). */
struct FM5_053_ChainMeta
{
	FName WeaponDefinitionId;
	FString ConfigRevision;
	FString DriverName;
	FString VehicleName;
};

struct FM5_053_Chain
{
	FM5_053_ChainKey Key;
	FM5_053_ChainMeta Meta;
	bool bEnded = false;
	EShotChainEndReason EndReason = EShotChainEndReason::StillOpen;
	int32 DroppedInfoCount = 0;
	FDateTime FirstRecordedAt;
	FDateTime LastRecordedAt;
	TArray<FShotChainEvent> Events;
	TMap<FM5_053_DedupKey, int32> DedupRowIndex;
	TMap<EShotChainStage, uint64> StageOccurrences;
};

/**
 * The chain store: a bounded LRU map (front of LruOrder = most recent)
 * owning the full row policy. Plain logic - no UObject - so the subsystem
 * owns it through a raw owning pointer (see the member note in the header)
 * and the game-thread-only convention of this subsystem applies.
 */
class FShotChainStore
{
public:
	/**
	 * Fired for every open->ended transition (released or force-closed) so
	 * the subsystem can write the text-file summary line. Raw member
	 * callback: the owning subsystem outlives the store and resets it in
	 * its destructor before its own teardown continues.
	 */
	TFunction<void(const FM5_053_Chain&)> OnChainEnded;

	/** Sampling stride for Info rows of sample-eligible stages (1 = record all). */
	int32 InfoSampleStride = 1;

	void Reset()
	{
		Chains.Empty();
		LruOrder.Empty();
		ActiveCount = 0;
	}

	int32 NumTracked() const { return Chains.Num(); }
	int32 NumActive() const { return ActiveCount; }

	static bool IsUsableKey(const FM5_053_ChainKey& Key)
	{
		return IsValidCombatEpoch(Key.Epoch) && IsValidCombatEntityId(Key.Source) && IsValidCombatShotId(Key.Shot);
	}

	const FM5_053_Chain* FindChain(const FM5_053_ChainKey& Key) const
	{
		return Chains.Find(Key);
	}

	/**
	 * Appends one stage row under the full policy: an unknown key auto-
	 * opens a chain (enforcing both caps first), a repeated verdict merges
	 * into the existing row, Info rows of sample-eligible stages pass the
	 * sampling gate and a full chain drops Info rows. Refusal/critical
	 * rows bypass every drop gate and are recorded even on an ended chain.
	 * Returns the recorded/merged row; null when dropped or refused.
	 */
	const FShotChainEvent* AppendEvent(const FM5_053_ChainKey& Key, const FShotStageRecord& Record,
		const FM5_053_ChainMeta& Meta, FDateTime Now)
	{
		if (!IsUsableKey(Key))
		{
			return nullptr;
		}
		FM5_053_Chain* Chain = Chains.Find(Key);
		if (Chain == nullptr)
		{
			Chain = OpenChainInternal(Key, Meta, Now);
		}
		else
		{
			Touch(Key);
		}
		if (Chain->bEnded)
		{
			// An ended chain keeps accepting refusal/critical rows (a late
			// ledger refusal after EndShot stays traceable) but never grows
			// further Info rows.
			if (Record.Severity == EShotChainSeverity::Info)
			{
				++Chain->DroppedInfoCount;
				return nullptr;
			}
		}
		const FM5_053_DedupKey Dedup{Record.Stage, Record.PelletIndex, Record.TargetId, Record.Result};
		if (const int32* ExistingIndex = Chain->DedupRowIndex.Find(Dedup))
		{
			FShotChainEvent& Merged = Chain->Events[*ExistingIndex];
			++Merged.RepeatCount;
			Chain->LastRecordedAt = Now;
			return &Merged;
		}
		// Sampling gate: Info rows of the high-frequency stages only; the
		// occurrence counter runs over distinct verdicts (dedup ran first),
		// so merged repeats never consume sampling slots.
		if (Record.Severity == EShotChainSeverity::Info
			&& IsSampleEligibleStage(Record.Stage) && InfoSampleStride > 1)
		{
			const uint64 Occurrence = ++Chain->StageOccurrences.FindOrAdd(Record.Stage);
			if (((Occurrence - 1) % static_cast<uint64>(InfoSampleStride)) != 0)
			{
				++Chain->DroppedInfoCount;
				return nullptr;
			}
		}
		// Per-chain overflow gate: Info rows drop (counted), refusal/
		// critical rows always append - the "no dropped error chains" rule.
		if (Chain->Events.Num() >= M5_053_MaxEventsPerChain
			&& Record.Severity == EShotChainSeverity::Info)
		{
			++Chain->DroppedInfoCount;
			return nullptr;
		}
		FShotChainEvent& Row = Chain->Events.AddDefaulted_GetRef();
		Row.Epoch = Key.Epoch;
		Row.SourceEntityId = Key.Source;
		Row.ShotId = Key.Shot;
		Row.PelletIndex = Record.PelletIndex;
		Row.TargetId = Record.TargetId;
		Row.Stage = Record.Stage;
		Row.Severity = Record.Severity;
		Row.Result = Record.Result;
		Row.Detail = Record.Detail;
		Row.NumericValue = Record.NumericValue;
		Row.bHasNumericValue = Record.bHasNumericValue;
		Row.ConfigRevision = Chain->Meta.ConfigRevision;
		Row.RecordedAt = Now;
		Row.RepeatCount = 1;
		Chain->DedupRowIndex.Add(Dedup, Chain->Events.Num() - 1);
		if (Chain->Events.Num() == 1)
		{
			Chain->FirstRecordedAt = Now;
		}
		Chain->LastRecordedAt = Now;
		return &Row;
	}

	/** Closes an open chain with its Cleanup row; false when unknown or already ended. */
	bool EndChain(const FM5_053_ChainKey& Key, EShotChainEndReason Reason, FDateTime Now)
	{
		FM5_053_Chain* Chain = Chains.Find(Key);
		if (Chain == nullptr || Chain->bEnded)
		{
			return false;
		}
		ForceEnd(*Chain, Reason, Now);
		return true;
	}

private:
	// The per-frame projectile lifecycle is the only high-frequency stage;
	// every other stage is discrete by construction and records all Info.
	static bool IsSampleEligibleStage(EShotChainStage Stage)
	{
		return Stage == EShotChainStage::Projectile;
	}

	FM5_053_Chain* OpenChainInternal(const FM5_053_ChainKey& Key, const FM5_053_ChainMeta& Meta, FDateTime Now)
	{
		// Cap 1 (active chains): force-close the least-recently-used OPEN
		// chain, cleanup row included, before a new chain opens.
		while (ActiveCount >= M5_053_MaxActiveChains)
		{
			FM5_053_Chain* OldestActive = FindOldest(/*bEnded*/ false);
			if (OldestActive == nullptr)
			{
				break; // defensive: the counter can never disagree with the map
			}
			ForceEnd(*OldestActive, EShotChainEndReason::ActiveEvicted, Now);
		}
		// Cap 2 (tracked chains): hard-drop the oldest ENDED chain first
		// (it was already summarized); ended chains evict before open ones.
		while (Chains.Num() >= M5_053_MaxTrackedChains)
		{
			const FM5_053_Chain* OldestEnded = FindOldest(/*bEnded*/ true);
			if (OldestEnded == nullptr)
			{
				break; // defensive: MaxTracked > MaxActive keeps one available
			}
			const FM5_053_ChainKey DroppedKey = OldestEnded->Key; // copy: the map mutates below
			DropChain(DroppedKey);
		}
		FM5_053_Chain& Chain = Chains.Add(Key);
		Chain.Key = Key;
		Chain.Meta = Meta;
		++ActiveCount;
		LruOrder.Insert(Key, 0);
		return &Chain;
	}

	void ForceEnd(FM5_053_Chain& Chain, EShotChainEndReason Reason, FDateTime Now)
	{
		if (Chain.bEnded)
		{
			return;
		}
		// The Cleanup row lands BEFORE the ended flip, so it is never
		// rejected by the post-end Info gate.
		FShotChainEvent& Row = Chain.Events.AddDefaulted_GetRef();
		Row.Epoch = Chain.Key.Epoch;
		Row.SourceEntityId = Chain.Key.Source;
		Row.ShotId = Chain.Key.Shot;
		Row.PelletIndex = InvalidCombatPelletIndex;
		Row.TargetId = InvalidCombatTargetId;
		Row.Stage = EShotChainStage::Cleanup;
		Row.Severity = EShotChainSeverity::Info;
		Row.Result = Reason == EShotChainEndReason::Released ? FName(TEXT("Released")) : FName(TEXT("ActiveEvicted"));
		Row.Detail = TEXT("chain closed: the recorded rows stay queryable inside the retention window");
		Row.ConfigRevision = Chain.Meta.ConfigRevision;
		Row.RecordedAt = Now;
		Row.RepeatCount = 1;
		Chain.LastRecordedAt = Now;
		Chain.bEnded = true;
		Chain.EndReason = Reason;
		--ActiveCount;
		Touch(Chain.Key);
		if (OnChainEnded)
		{
			OnChainEnded(Chain);
		}
	}

	void Touch(const FM5_053_ChainKey& Key)
	{
		for (int32 Index = 0; Index < LruOrder.Num(); ++Index)
		{
			if (LruOrder[Index] == Key)
			{
				LruOrder.RemoveAt(Index);
				LruOrder.Insert(Key, 0);
				return;
			}
		}
	}

	FM5_053_Chain* FindOldest(bool bEnded)
	{
		for (int32 Index = LruOrder.Num() - 1; Index >= 0; --Index)
		{
			if (FM5_053_Chain* Chain = Chains.Find(LruOrder[Index]))
			{
				if (Chain->bEnded == bEnded)
				{
					return Chain;
				}
			}
		}
		return nullptr;
	}

	void DropChain(const FM5_053_ChainKey& Key)
	{
		if (const FM5_053_Chain* Chain = Chains.Find(Key))
		{
			if (!Chain->bEnded)
			{
				--ActiveCount;
			}
		}
		Chains.Remove(Key);
		LruOrder.RemoveSingle(Key);
	}

	TMap<FM5_053_ChainKey, FM5_053_Chain> Chains;
	TArray<FM5_053_ChainKey> LruOrder;
	int32 ActiveCount = 0;
};

// -- Refusal-reason name helpers (plain enums carry no reflection; the
//    names double as the refusal rows' Result verdicts, so repeated
//    identical refusals dedup while different reasons stay separate rows).

const TCHAR* M5_053_HitDecisionName(EHitDecision Decision)
{
	switch (Decision)
	{
		case EHitDecision::Apply: return TEXT("Apply");
		case EHitDecision::Block_Invulnerable: return TEXT("Block_Invulnerable");
		case EHitDecision::Block_Immune: return TEXT("Block_Immune");
		case EHitDecision::Block_Ignore: return TEXT("Block_Ignore");
	}
	return TEXT("Unknown");
}

const TCHAR* M5_053_RefuseReasonName(EUnifiedHitRefuseReason Reason)
{
	switch (Reason)
	{
		case EUnifiedHitRefuseReason::None: return TEXT("None");
		case EUnifiedHitRefuseReason::UnusableKey: return TEXT("UnusableKey");
		case EUnifiedHitRefuseReason::InvalidTargetActor: return TEXT("InvalidTargetActor");
		case EUnifiedHitRefuseReason::MissingHealthComponent: return TEXT("MissingHealthComponent");
		case EUnifiedHitRefuseReason::TargetDead: return TEXT("TargetDead");
		case EUnifiedHitRefuseReason::GetUpProtection: return TEXT("GetUpProtection");
		case EUnifiedHitRefuseReason::SameFaction: return TEXT("SameFaction");
		case EUnifiedHitRefuseReason::LedgerRefused: return TEXT("LedgerRefused");
		case EUnifiedHitRefuseReason::DamageContextRefused: return TEXT("DamageContextRefused");
	}
	return TEXT("Unknown");
}

const TCHAR* M5_053_LedgerReasonName(EHitLedgerRejectReason Reason)
{
	switch (Reason)
	{
		case EHitLedgerRejectReason::None: return TEXT("None");
		case EHitLedgerRejectReason::UnusableKey: return TEXT("UnusableKey");
		case EHitLedgerRejectReason::StaleEpoch: return TEXT("StaleEpoch");
		case EHitLedgerRejectReason::UnknownSource: return TEXT("UnknownSource");
		case EHitLedgerRejectReason::UnknownTarget: return TEXT("UnknownTarget");
		case EHitLedgerRejectReason::DuplicateEvent: return TEXT("DuplicateEvent");
		case EHitLedgerRejectReason::DuplicateControl: return TEXT("DuplicateControl");
		case EHitLedgerRejectReason::ShotAlreadyEnded: return TEXT("ShotAlreadyEnded");
		case EHitLedgerRejectReason::CapacityFull: return TEXT("CapacityFull");
		case EHitLedgerRejectReason::UnboundRegistry: return TEXT("UnboundRegistry");
	}
	return TEXT("Unknown");
}

const TCHAR* M5_053_DamageBlockName(EDamageBlockReason Reason)
{
	switch (Reason)
	{
		case EDamageBlockReason::None: return TEXT("None");
		case EDamageBlockReason::NonFiniteInput: return TEXT("NonFiniteInput");
		case EDamageBlockReason::NegativeInput: return TEXT("NegativeInput");
		case EDamageBlockReason::NonPositiveBaseDamage: return TEXT("NonPositiveBaseDamage");
		case EDamageBlockReason::OverflowedResult: return TEXT("OverflowedResult");
	}
	return TEXT("Unknown");
}

const TCHAR* M5_053_EndReasonName(EShotChainEndReason Reason)
{
	switch (Reason)
	{
		case EShotChainEndReason::StillOpen: return TEXT("StillOpen");
		case EShotChainEndReason::Released: return TEXT("Released");
		case EShotChainEndReason::ActiveEvicted: return TEXT("ActiveEvicted");
	}
	return TEXT("Unknown");
}

// NOTE: this anonymous namespace intentionally stays open past the store
// class: the UOperationLogSubsystem member definitions below (through
// GetNumActiveShotChains) consume the complete FShotChainStore type. It
// closes right after the last such use.

void UOperationLogSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LogFilePath = FPaths::Combine(FPaths::ProjectSavedDir(), M3_023_LogDirectory, M3_023_LogFileName);
	// No day is open yet: the first write opens the current (provider) day.
	ActiveLogDate = FDateTime();
	// M5-053: the chain store dies with the subsystem; every open->ended
	// transition echoes one summary line into the text file face above.
	M5_053_ChainStore = new FShotChainStore();
	M5_053_ChainStore->OnChainEnded = [this](const FM5_053_Chain& Chain)
	{
		LogResponse(FString::Printf(
			TEXT("ShotChain End: epoch=%llu source=%llu shot=%llu events=%d dropped=%d reason=%s"),
			Chain.Key.Epoch, Chain.Key.Source, Chain.Key.Shot,
			Chain.Events.Num(), Chain.DroppedInfoCount,
			M5_053_EndReasonName(Chain.EndReason)));
	};
}

void UOperationLogSubsystem::Deinitialize()
{
	// World teardown drops the delegates with their owners anyway; the explicit
	// unbind only keeps the handles tidy for a still-alive world.
	M3_023_UnbindAll();
	// M5-053: chains are in-memory facts of one game-instance lifetime (the
	// text file already received the per-chain end summaries), so the store is
	// dropped here and never outlives the subsystem.
	if (M5_053_ChainStore != nullptr)
	{
		M5_053_ChainStore->Reset();
		delete M5_053_ChainStore;
		M5_053_ChainStore = nullptr;
	}
	NowProvider = nullptr;
	Super::Deinitialize();
}

UOperationLogSubsystem::~UOperationLogSubsystem()
{
	// Defined here (not inline) because only this translation unit sees the
	// complete FShotChainStore type the raw member deletes.
	if (M5_053_ChainStore != nullptr)
	{
		delete M5_053_ChainStore;
		M5_053_ChainStore = nullptr;
	}
}

UOperationLogSubsystem* UOperationLogSubsystem::FindForContext(const UObject* Context)
{
	if (Context == nullptr)
	{
		return nullptr;
	}
	// 1) Outer chain: game instance subsystems (and everything created below
	//    them, e.g. UObject services owned by a subsystem) resolve directly.
	UObject* MutableContext = const_cast<UObject*>(Context);
	for (UObject* Outer = MutableContext->GetOuter(); Outer != nullptr; Outer = Outer->GetOuter())
	{
		if (UGameInstance* GameInstance = Cast<UGameInstance>(Outer))
		{
			return GameInstance->GetSubsystem<UOperationLogSubsystem>();
		}
	}
	// 2) World path: actors, components and world subsystems reach the game
	//    instance through their world. A world-less context (bare test
	//    objects, CDOs) stops here and the caller skips the log.
	if (UWorld* World = MutableContext->GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			return GameInstance->GetSubsystem<UOperationLogSubsystem>();
		}
	}
	return nullptr;
}

void UOperationLogSubsystem::SetNowProvider(TFunction<FDateTime()> InProvider)
{
	NowProvider = MoveTempIfPossible(InProvider);
}

void UOperationLogSubsystem::ResetNowProvider()
{
	NowProvider = nullptr;
}

void UOperationLogSubsystem::Log(FName Category, const FString& Message)
{
	// The now source first (the provider decides which day this write belongs
	// to; default is the local wall clock, matching the user's calendar).
	const FDateTime Now = NowProvider ? NowProvider() : FDateTime::Now();
	M3_023_EnsureDailyFile(Now);
	const FString Line = FString::Printf(TEXT("[%02d:%02d:%02d.%03d][%s] %s"),
		Now.GetHour(), Now.GetMinute(), Now.GetSecond(), Now.GetMillisecond(),
		*Category.ToString(), *Message);
	M3_023_AppendLine(Line);
}

void UOperationLogSubsystem::LogInput(const FString& Message)
{
	Log(M3_023_CategoryInput, Message);
}

void UOperationLogSubsystem::LogState(const FString& Message)
{
	Log(M3_023_CategoryState, Message);
}

void UOperationLogSubsystem::LogResponse(const FString& Message)
{
	Log(M3_023_CategoryResponse, Message);
}

// -- M5-053: the shot event-chain face -----------------------------------------

bool UOperationLogSubsystem::BeginShotChain(const FShotContext& Shot, const FString& ConfigRevision,
	const FString& DriverName, const FString& VehicleName)
{
	if (M5_053_ChainStore == nullptr)
	{
		return false;
	}
	const FM5_053_ChainKey Key{Shot.Epoch, Shot.SourceEntityId, Shot.ShotId};
	if (!FShotChainStore::IsUsableKey(Key))
	{
		return false;
	}
	FM5_053_ChainMeta Meta;
	Meta.WeaponDefinitionId = Shot.WeaponDefinitionId;
	Meta.ConfigRevision = ConfigRevision;
	Meta.DriverName = DriverName;
	Meta.VehicleName = VehicleName;
	FShotStageRecord Record;
	Record.Stage = EShotChainStage::ShotCommitted;
	Record.PelletIndex = InvalidCombatPelletIndex;
	Record.TargetId = InvalidCombatTargetId;
	Record.Severity = EShotChainSeverity::Info;
	Record.Result = FName(TEXT("Committed"));
	Record.Detail = FString::Printf(TEXT("weapon=%s pellets=%d fire_mode=%d local_seq=%llu"),
		*Shot.WeaponDefinitionId.ToString(), Shot.PelletCount,
		static_cast<int32>(Shot.FireMode), Shot.LocalShotSequence);
	// A re-begin of a live chain merges into the existing ShotCommitted row
	// (first commit wins, the metadata is never replaced).
	M5_053_ChainStore->AppendEvent(Key, Record, Meta, NowProvider ? NowProvider() : FDateTime::Now());
	return true;
}

bool UOperationLogSubsystem::LogShotStage(FCombatEpoch Epoch, FEntityId Source, FShotId Shot, const FShotStageRecord& Record)
{
	if (M5_053_ChainStore == nullptr)
	{
		return false;
	}
	const FM5_053_ChainKey Key{Epoch, Source, Shot};
	// Auto-open carries no commit metadata: the shot's committing caller owns
	// the revision/driver/vehicle snapshot, a pure hit-pipeline caller does not.
	FM5_053_ChainMeta Meta;
	return M5_053_ChainStore->AppendEvent(Key, Record, Meta, NowProvider ? NowProvider() : FDateTime::Now()) != nullptr;
}

void UOperationLogSubsystem::LogUnifiedHitOutcome(const FUnifiedHitRequest& Request, const FUnifiedHitOutcome& Outcome)
{
	if (M5_053_ChainStore == nullptr)
	{
		return;
	}
	const FCombatEventKey Key = Request.MakeEventKey();
	if (!IsUsableCombatEventKey(Key) || !IsValidCombatPelletIndex(Request.PelletIndex))
	{
		// Without a usable event key there is no chain identity; the entry's
		// own UnusableKey refusal stays at the call site (documented limit).
		return;
	}
	const FM5_053_ChainKey ChainKey{Key.Epoch, Key.EntityId, Key.ShotId};
	const FDateTime Now = NowProvider ? NowProvider() : FDateTime::Now();
	FM5_053_ChainMeta Meta; // empty: auto-open, the committing caller owns the revision

	FShotStageRecord Record;
	Record.PelletIndex = Key.PelletIndex;
	Record.TargetId = Key.TargetId;

	if (Outcome.bWasBlocked)
	{
		// One refusal row per face combination; the Result verdict is the
		// applier-face reason itself, so repeated identical refusals dedup
		// while a different reason on the same key stays a separate row.
		Record.Stage = EShotChainStage::Admission;
		Record.Severity = EShotChainSeverity::Refusal;
		Record.Result = FName(M5_053_RefuseReasonName(Outcome.RefuseReason));
		Record.NumericValue = static_cast<double>(static_cast<uint8>(Outcome.RefuseReason));
		Record.bHasNumericValue = true;
		Record.Detail = FString::Printf(TEXT("decision=%s refuse=%s ledger=%s damage_block=%s"),
			M5_053_HitDecisionName(Outcome.Decision),
			M5_053_RefuseReasonName(Outcome.RefuseReason),
			M5_053_LedgerReasonName(Outcome.LedgerReason),
			M5_053_DamageBlockName(Outcome.DamageBlockReason));
		M5_053_ChainStore->AppendEvent(ChainKey, Record, Meta, Now);
		return;
	}

	// Admission: the unified entry approved the hit (the filter faces passed).
	Record.Stage = EShotChainStage::Admission;
	Record.Severity = EShotChainSeverity::Info;
	Record.Result = FName(TEXT("Approved"));
	Record.NumericValue = 0.0;
	Record.bHasNumericValue = false;
	Record.Detail = FString::Printf(TEXT("decision=Apply immune=%d"), Outcome.bWasImmune ? 1 : 0);
	M5_053_ChainStore->AppendEvent(ChainKey, Record, Meta, Now);

	// Hit: the pellet connected on this target (the filter let it through).
	Record.Stage = EShotChainStage::Hit;
	Record.Result = FName(TEXT("Connected"));
	Record.Detail = FString::Printf(TEXT("profile=%s"), *Request.Attack.DamageProfileId.ToString());
	M5_053_ChainStore->AppendEvent(ChainKey, Record, Meta, Now);

	// Damage: the applied value is the numeric payload (the row carries it even
	// for an immune connect, where exactly 0 were applied).
	Record.Stage = EShotChainStage::Damage;
	Record.Result = Outcome.bWasImmune ? FName(TEXT("Immune")) : FName(TEXT("Applied"));
	Record.NumericValue = static_cast<double>(Outcome.DamageApplied);
	Record.bHasNumericValue = true;
	Record.Detail = FString::Printf(TEXT("applied=%.1f"), static_cast<double>(Outcome.DamageApplied));
	M5_053_ChainStore->AppendEvent(ChainKey, Record, Meta, Now);

	if (Outcome.Control.AnyAccepted())
	{
		// Reaction: the control face (control consumes once per shot/target
		// upstream, so multi-pellet shots log the reaction exactly once here).
		Record.Stage = EShotChainStage::Reaction;
		Record.Severity = EShotChainSeverity::Info;
		Record.Result = FName(TEXT("Accepted"));
		Record.NumericValue = 0.0;
		Record.bHasNumericValue = false;
		Record.Detail = FString::Printf(TEXT("stagger=%d launch=%d knockdown=%d"),
			Outcome.Control.bStagger ? 1 : 0,
			Outcome.Control.bLaunch ? 1 : 0,
			Outcome.Control.bKnockdown ? 1 : 0);
		M5_053_ChainStore->AppendEvent(ChainKey, Record, Meta, Now);
	}

	if (Outcome.bTargetDied)
	{
		// Death: never sampled, never dropped (critical severity).
		Record.Stage = EShotChainStage::Death;
		Record.Severity = EShotChainSeverity::Critical;
		Record.Result = FName(TEXT("Died"));
		Record.NumericValue = 0.0;
		Record.bHasNumericValue = false;
		Record.Detail = TEXT("target reached zero health from this hit");
		M5_053_ChainStore->AppendEvent(ChainKey, Record, Meta, Now);
	}
}

bool UOperationLogSubsystem::EndShotChain(FCombatEpoch Epoch, FEntityId Source, FShotId Shot)
{
	if (M5_053_ChainStore == nullptr)
	{
		return false;
	}
	return M5_053_ChainStore->EndChain(FM5_053_ChainKey{Epoch, Source, Shot},
		EShotChainEndReason::Released, NowProvider ? NowProvider() : FDateTime::Now());
}

TArray<FShotChainEvent> UOperationLogSubsystem::GetShotChainEvents(FCombatEpoch Epoch, FEntityId Source, FShotId Shot) const
{
	TArray<FShotChainEvent> Rows;
	if (M5_053_ChainStore != nullptr)
	{
		if (const FM5_053_Chain* Chain = M5_053_ChainStore->FindChain(FM5_053_ChainKey{Epoch, Source, Shot}))
		{
			Rows = Chain->Events;
		}
	}
	return Rows;
}

bool UOperationLogSubsystem::FindShotChainSummary(FCombatEpoch Epoch, FEntityId Source, FShotId Shot, FShotChainSummary& OutSummary) const
{
	if (M5_053_ChainStore == nullptr)
	{
		return false;
	}
	const FM5_053_Chain* Chain = M5_053_ChainStore->FindChain(FM5_053_ChainKey{Epoch, Source, Shot});
	if (Chain == nullptr)
	{
		return false;
	}
	OutSummary = FShotChainSummary();
	OutSummary.Epoch = Chain->Key.Epoch;
	OutSummary.SourceEntityId = Chain->Key.Source;
	OutSummary.ShotId = Chain->Key.Shot;
	OutSummary.WeaponDefinitionId = Chain->Meta.WeaponDefinitionId;
	OutSummary.ConfigRevision = Chain->Meta.ConfigRevision;
	OutSummary.DriverName = Chain->Meta.DriverName;
	OutSummary.VehicleName = Chain->Meta.VehicleName;
	OutSummary.bEnded = Chain->bEnded;
	OutSummary.EndReason = Chain->EndReason;
	OutSummary.EventCount = Chain->Events.Num();
	OutSummary.DroppedInfoCount = Chain->DroppedInfoCount;
	OutSummary.FirstRecordedAt = Chain->FirstRecordedAt;
	OutSummary.LastRecordedAt = Chain->LastRecordedAt;
	return true;
}

void UOperationLogSubsystem::SetInfoSampleStride(int32 Stride)
{
	if (M5_053_ChainStore != nullptr)
	{
		M5_053_ChainStore->InfoSampleStride = FMath::Max(1, Stride);
	}
}

int32 UOperationLogSubsystem::GetInfoSampleStride() const
{
	return M5_053_ChainStore != nullptr ? M5_053_ChainStore->InfoSampleStride : 1;
}

int32 UOperationLogSubsystem::GetNumTrackedShotChains() const
{
	return M5_053_ChainStore != nullptr ? M5_053_ChainStore->NumTracked() : 0;
}

int32 UOperationLogSubsystem::GetNumActiveShotChains() const
{
	return M5_053_ChainStore != nullptr ? M5_053_ChainStore->NumActive() : 0;
}

bool UOperationLogSubsystem::RegisterPlayer(APrototypeCharacter* Player)
{
	if (Player == nullptr)
	{
		return false;
	}
	// A re-register (a re-possessed pawn, or the test's explicit call on top
	// of BeginPlay's automatic one) replaces the previous binding set, so
	// exactly one binding set exists at any time.
	M3_023_UnbindAll();
	RegisteredPlayerPtr = Player;

	// Weak-lambda style: every handler re-checks this subsystem through a
	// TWeakObjectPtr, so a subsystem destroyed before its bound components
	// (a game instance shutdown ahead of a world teardown) only ever skips
	// the row instead of dereferencing a dead object.
	TWeakObjectPtr<UOperationLogSubsystem> WeakSelf(this);
	if (UCombatComponent* Combat = Player->GetCombat())
	{
		RegisteredCombatPtr = Combat;
		CombatStartedHandle = Combat->OnStarted.AddLambda([WeakSelf](FName AttackId, uint64 InstanceId)
		{
			if (UOperationLogSubsystem* Self = WeakSelf.Get())
			{
				Self->Log(M3_023_CategoryState, FString::Printf(
					TEXT("Combat Started: %s #%llu"), *AttackId.ToString(), InstanceId));
			}
		});
		CombatFinishedHandle = Combat->OnFinished.AddLambda([WeakSelf](FName AttackId, uint64 InstanceId)
		{
			if (UOperationLogSubsystem* Self = WeakSelf.Get())
			{
				Self->Log(M3_023_CategoryState, FString::Printf(
					TEXT("Combat Finished: %s #%llu"), *AttackId.ToString(), InstanceId));
			}
		});
		CombatHitHandle = Combat->OnHitConfirmed.AddLambda([WeakSelf](const FCombatHit& Hit)
		{
			if (UOperationLogSubsystem* Self = WeakSelf.Get())
			{
				const AActor* Target = Hit.Target.Get();
				Self->Log(M3_023_CategoryResponse, FString::Printf(
					TEXT("Combat HitConfirmed: %s #%llu damage=%.0f target=%s"),
					*Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage,
					Target != nullptr ? *Target->GetName() : TEXT("none")));
			}
		});
	}
	if (UWorld* World = Player->GetWorld())
	{
		if (URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>())
		{
			RoomSessionPtr = Session;
			RoomStartedHandle = Session->OnRunStarted().AddLambda([WeakSelf]()
			{
				if (UOperationLogSubsystem* Self = WeakSelf.Get())
				{
					Self->M3_023_LogRoomRunStarted();
				}
			});
			RoomEndedHandle = Session->OnRunEnded().AddLambda([WeakSelf](const FRoomResult& Result)
			{
				if (UOperationLogSubsystem* Self = WeakSelf.Get())
				{
					Self->Log(M3_023_CategoryState, FString::Printf(
						TEXT("Room RunEnded: run=%llu cleared=%d killed=%d elapsed=%.2f settlement=%llu"),
						Result.RunId, Result.bCleared ? 1 : 0, Result.KilledCount,
						Result.ElapsedSeconds, Result.SettlementId));
				}
			});
		}
	}
	return true;
}

void UOperationLogSubsystem::UnregisterPlayer()
{
	M3_023_UnbindAll();
}

bool UOperationLogSubsystem::IsPlayerRegistered() const
{
	return RegisteredPlayerPtr.IsValid();
}

const FString& UOperationLogSubsystem::GetLogFilePath() const
{
	return LogFilePath;
}

const FDateTime& UOperationLogSubsystem::GetActiveLogDate() const
{
	return ActiveLogDate;
}

bool UOperationLogSubsystem::M3_023_EnsureDailyFile(const FDateTime& Now)
{
	const FDateTime Today = Now.GetDate();
	if (ActiveLogDate == Today)
	{
		return true;
	}
	// Day change (or the first write): the previous day's log is cleared and
	// the new day opens with a fresh header. The directory is ensured first
	// (a fresh Saved directory may not carry it yet).
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LogFilePath), /*Tree*/ true);
	IFileManager::Get().Delete(*LogFilePath);
	ActiveLogDate = Today;
	M3_023_WriteRolloverHeader(Now);
	return true;
}

void UOperationLogSubsystem::M3_023_WriteRolloverHeader(const FDateTime& Now)
{
	const FString DateText = FString::Printf(TEXT("%04d-%02d-%02d"),
		Now.GetYear(), Now.GetMonth(), Now.GetDay());
	FString BuildId = FApp::GetBuildVersion();
	if (BuildId.IsEmpty())
	{
		BuildId = TEXT("dev");
	}
	M3_023_AppendLine(FString::Printf(TEXT("=== Operation log started %s (%s) ==="), *DateText, *BuildId));
}

void UOperationLogSubsystem::M3_023_AppendLine(const FString& Line)
{
	if (LogFilePath.IsEmpty())
	{
		return;
	}
	// One write call per line. ForceUTF8WithoutBOM instead of ForceUTF8: the
	// engine writes the BOM on EVERY SaveStringToFile call with FILEWRITE_Append
	// (Engine FileHelper.cpp serializes it unconditionally for ForceUTF8), so a
	// per-line append would sprinkle a BOM between the lines; without-BOM keeps
	// the file clean UTF-8. EvenIfReadOnly keeps the write alive beside a
	// read-only attribute flip.
	FString WithTerminator = Line;
	WithTerminator += LINE_TERMINATOR;
	FFileHelper::SaveStringToFile(WithTerminator, *LogFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(),
		FILEWRITE_Append | FILEWRITE_EvenIfReadOnly);
}

void UOperationLogSubsystem::M3_023_UnbindAll()
{
	// Weak references only: a destroyed player/world already took its delegate
	// objects with it, so the unbind skips expired pointers safely.
	if (UCombatComponent* Combat = RegisteredCombatPtr.Get())
	{
		Combat->OnStarted.Remove(CombatStartedHandle);
		Combat->OnFinished.Remove(CombatFinishedHandle);
		Combat->OnHitConfirmed.Remove(CombatHitHandle);
	}
	if (URoomSessionSubsystem* Session = RoomSessionPtr.Get())
	{
		Session->OnRunStarted().Remove(RoomStartedHandle);
		Session->OnRunEnded().Remove(RoomEndedHandle);
	}
	RegisteredPlayerPtr = nullptr;
	RegisteredCombatPtr = nullptr;
	RoomSessionPtr = nullptr;
	CombatStartedHandle.Reset();
	CombatFinishedHandle.Reset();
	CombatHitHandle.Reset();
	RoomStartedHandle.Reset();
	RoomEndedHandle.Reset();
}

void UOperationLogSubsystem::M3_023_LogRoomRunStarted()
{
	const URoomSessionSubsystem* Session = RoomSessionPtr.Get();
	if (Session == nullptr)
	{
		return;
	}
	Log(M3_023_CategoryState, FString::Printf(
		TEXT("Room RunStarted: run=%llu room=%s seed=%d settlement=%llu"),
		Session->GetRunId(), *Session->GetRoomId().ToString(), Session->GetSeed(),
		Session->GetSettlementId()));
}
