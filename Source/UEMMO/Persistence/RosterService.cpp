#include "RosterService.h"

#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "RosterSaveGame.h"

namespace
{
	constexpr int32 NoActiveSlot = URosterIndexSaveGame::NoActiveSlot;

	/** The canonical text one roster row digests over (field order frozen). */
	FString RosterRowText(const FRosterEntrySaveRow& Row)
	{
		return FString::Printf(TEXT("%s|%s|%s|%s|%d"),
			*Row.CharacterId.ToString(), *Row.ProfessionId.ToString(),
			*Row.DisplayName, *Row.SavePrefix, Row.bActive ? 1 : 0);
	}

	/** The slot integrity digest over the canonical row text. */
	uint32 RosterEntryDigest(const TArray<FRosterEntrySaveRow>& Entries)
	{
		FString Canonical;
		for (const FRosterEntrySaveRow& Row : Entries)
		{
			Canonical += RosterRowText(Row);
			Canonical += TEXT("\n");
		}
		return FCrc::StrCrc32(*Canonical);
	}
}

uint32 URosterService::ComputeEntryDigestImpl(const TArray<FRosterEntrySaveRow>& Entries)
{
	return RosterEntryDigest(Entries);
}

bool URosterService::Initialize(const FString& InRosterPrefix)
{
	if (InRosterPrefix.IsEmpty())
	{
		return false;
	}
	RosterPrefix = InRosterPrefix;
	SlotNames = { RosterPrefix + TEXT("A"), RosterPrefix + TEXT("B"), RosterPrefix + TEXT("Index") };
	return true;
}

void URosterService::SetStorage(ISaveStorage* InStorage)
{
	StorageOverride = InStorage;
}

const FString& URosterService::GetRosterPrefix() const
{
	return RosterPrefix;
}

ISaveStorage* URosterService::ResolveStorage()
{
	if (StorageOverride)
	{
		return StorageOverride;
	}
	static FGameplayStaticsSaveStorage EngineStorage;
	return &EngineStorage;
}

bool URosterService::HasRosterFiles() const
{
	ISaveStorage* Storage = const_cast<URosterService*>(this)->ResolveStorage();
	return Storage->DoesSlotExist(SlotNames[0]) || Storage->DoesSlotExist(SlotNames[1])
		|| Storage->DoesSlotExist(SlotNames[2]);
}

FString URosterService::DeriveSavePrefix(const FGuid& CharacterId)
{
	// The 32 hex digits are filename-safe and unique per identity; "Char_"
	// namespaces them away from the legacy "Profile_" slots by construction.
	return TEXT("Char_") + CharacterId.ToString(EGuidFormats::Digits);
}

bool URosterService::TryReadRosterSlot(int32 SlotIndex, TArray<FRosterEntrySaveRow>& OutEntries,
	int32& OutGeneration, FString& OutProblem)
{
	USaveGame* Raw = ResolveStorage()->ReadSlot(SlotNames[SlotIndex]);
	if (!Raw)
	{
		OutProblem = FString::Printf(TEXT("slot '%s' is missing or unreadable"), *SlotNames[SlotIndex]);
		return false;
	}
	const URosterSlotSaveGame* Slot = Cast<URosterSlotSaveGame>(Raw);
	if (!Slot)
	{
		OutProblem = FString::Printf(TEXT("slot '%s' is not a URosterSlotSaveGame"), *SlotNames[SlotIndex]);
		return false;
	}
	if (Slot->SchemaVersion != URosterSaveGame::CurrentSchemaVersion)
	{
		OutProblem = FString::Printf(TEXT("slot '%s' carries schema_version %d (this build understands %d)"),
			*SlotNames[SlotIndex], Slot->SchemaVersion, URosterSaveGame::CurrentSchemaVersion);
		return false;
	}
	if (Slot->EntryDigest != ComputeEntryDigestImpl(Slot->Entries))
	{
		OutProblem = FString::Printf(TEXT("slot '%s' failed its integrity digest check"), *SlotNames[SlotIndex]);
		return false;
	}
	if (Slot->EntryCount != Slot->Entries.Num())
	{
		OutProblem = FString::Printf(TEXT("slot '%s' failed its integrity field checks"), *SlotNames[SlotIndex]);
		return false;
	}
	OutEntries = Slot->Entries;
	OutGeneration = Slot->SlotGeneration;
	return true;
}

bool URosterService::WriteRosterSlotAndVerify(int32 SlotIndex, const TArray<FRosterEntrySaveRow>& Entries,
	int32 Generation, FString& OutProblem)
{
	URosterSlotSaveGame* SlotSave = NewObject<URosterSlotSaveGame>(GetTransientPackage());
	SlotSave->SchemaVersion = URosterSaveGame::CurrentSchemaVersion;
	SlotSave->Entries = Entries;
	SlotSave->SlotGeneration = Generation;
	SlotSave->EntryDigest = ComputeEntryDigestImpl(Entries);
	SlotSave->EntryCount = Entries.Num();

	if (!ResolveStorage()->WriteSlot(SlotNames[SlotIndex], SlotSave))
	{
		OutProblem = FString::Printf(TEXT("slot '%s' could not be written"), *SlotNames[SlotIndex]);
		return false;
	}

	// Read-back verification: the same checks a load applies.
	TArray<FRosterEntrySaveRow> ReadBack;
	int32 ReadGeneration = 0;
	FString ReadProblem;
	if (!TryReadRosterSlot(SlotIndex, ReadBack, ReadGeneration, ReadProblem)
		|| ReadGeneration != Generation
		|| ReadBack.Num() != Entries.Num())
	{
		OutProblem = FString::Printf(TEXT("slot '%s' failed the read-back verification: %s"),
			*SlotNames[SlotIndex], *ReadProblem);
		return false;
	}
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (ReadBack[Index].CharacterId != Entries[Index].CharacterId
			|| ReadBack[Index].SavePrefix != Entries[Index].SavePrefix)
		{
			OutProblem = FString::Printf(TEXT("slot '%s' read-back row %d does not match"), *SlotNames[SlotIndex], Index);
			return false;
		}
	}
	return true;
}

bool URosterService::M2_ReadIndex(int32& OutActiveSlot, int32 (&OutGenerations)[2],
	bool& bOutMissing, FString& OutProblem)
{
	bOutMissing = false;
	if (!ResolveStorage()->DoesSlotExist(SlotNames[2]))
	{
		bOutMissing = true;
		OutActiveSlot = NoActiveSlot;
		OutGenerations[0] = OutGenerations[1] = 0;
		return false;
	}
	USaveGame* Raw = ResolveStorage()->ReadSlot(SlotNames[2]);
	const URosterIndexSaveGame* Index = Cast<URosterIndexSaveGame>(Raw);
	if (!Index || Index->SlotGenerations.Num() != 2
		|| (Index->ActiveSlotIndex != 0 && Index->ActiveSlotIndex != 1
			&& Index->ActiveSlotIndex != NoActiveSlot))
	{
		OutProblem = TEXT("the roster index is missing fields or unreadable");
		return false;
	}
	OutActiveSlot = Index->ActiveSlotIndex;
	OutGenerations[0] = Index->SlotGenerations[0];
	OutGenerations[1] = Index->SlotGenerations[1];
	return true;
}

bool URosterService::ReadCommittedEntries(TArray<FRosterEntrySaveRow>& OutEntries,
	FString& OutProblem, bool& bOutNoRoster)
{
	bOutNoRoster = false;
	const bool bAnyFile = ResolveStorage()->DoesSlotExist(SlotNames[0])
		|| ResolveStorage()->DoesSlotExist(SlotNames[1])
		|| ResolveStorage()->DoesSlotExist(SlotNames[2]);
	if (!bAnyFile)
	{
		bOutNoRoster = true;
		OutProblem = TEXT("no roster slot files exist (fresh state)");
		return false;
	}

	int32 ActiveSlot = NoActiveSlot;
	int32 Generations[2] = { 0, 0 };
	bool bIndexMissing = false;
	FString IndexProblem;
	const bool bIndexUsable = M2_ReadIndex(ActiveSlot, Generations, bIndexMissing, IndexProblem);

	TArray<FRosterEntrySaveRow> Read[2];
	int32 ReadGeneration[2] = { 0, 0 };
	FString Problems[2];
	bool bValid[2] = { false, false };
	if (bIndexUsable)
	{
		for (int32 Index = 0; Index < 2; ++Index)
		{
			bValid[Index] = TryReadRosterSlot(Index, Read[Index], ReadGeneration[Index], Problems[Index]);
		}
	}

	if (bIndexUsable && (ActiveSlot == 0 || ActiveSlot == 1) && bValid[ActiveSlot])
	{
		OutEntries = Read[ActiveSlot];
		return true;
	}
	// Corruption fallback: a valid other slot wins.
	const int32 Other = (ActiveSlot == 0) ? 1 : 0;
	if (bValid[Other])
	{
		OutProblem = FString::Printf(TEXT("index/active-slot fallback: %s"),
			bIndexUsable ? *Problems[ActiveSlot] : *IndexProblem);
		OutEntries = Read[Other];
		return true;
	}
	// Both unusable: explicit corruption (the evidence stays on disk).
	OutProblem = FString::Printf(TEXT("roster recovery failed: index=%s slotA=%s slotB=%s"),
		bIndexUsable ? *IndexProblem : TEXT("(none)"),
		*Problems[0], *Problems[1]);
	return false;
}

bool URosterService::CommitRoster(const TArray<FRosterEntrySaveRow>& Entries, FString& OutProblem)
{
	// The transaction mirrors the profile save: read the index fresh, write
	// the INACTIVE slot, verify by read-back, then commit the index. An
	// unreadable EXISTING index refuses the commit entirely; a MISSING index
	// is the legal fresh state (the first commit goes to A).
	int32 ActiveSlot = NoActiveSlot;
	int32 Generations[2] = { 0, 0 };
	bool bIndexMissing = false;
	FString IndexProblem;
	const bool bIndexUsable = M2_ReadIndex(ActiveSlot, Generations, bIndexMissing, IndexProblem);
	if (!bIndexUsable && !bIndexMissing)
	{
		OutProblem = FString::Printf(TEXT("the roster index is corrupt; the commit refuses to guess: %s"), *IndexProblem);
		return false;
	}
	const int32 Staging = (bIndexMissing || ActiveSlot == NoActiveSlot) ? 0 : (1 - ActiveSlot);
	const int32 NextGeneration = FMath::Max(Generations[0], Generations[1]) + 1;

	FString WriteProblem;
	if (!WriteRosterSlotAndVerify(Staging, Entries, NextGeneration, WriteProblem))
	{
		OutProblem = WriteProblem;
		return false;
	}

	URosterIndexSaveGame* Index = NewObject<URosterIndexSaveGame>(GetTransientPackage());
	Index->ActiveSlotIndex = Staging;
	Index->SlotGenerations = { Generations[0], Generations[1] };
	Index->SlotGenerations[Staging] = NextGeneration;
	if (!ResolveStorage()->WriteSlot(SlotNames[2], Index))
	{
		OutProblem = FString::Printf(TEXT("the roster index commit failed; slot %d holds uncommitted data"), Staging);
		return false;
	}
	return true;
}

FRosterLoadOutcome URosterService::LoadRoster()
{
	FRosterLoadOutcome Outcome;
	TArray<FRosterEntrySaveRow> Rows;
	FString Problem;
	bool bNoRoster = false;
	if (!ReadCommittedEntries(Rows, Problem, bNoRoster))
	{
		Outcome.Result = bNoRoster ? ERosterLoadResult::NoRoster : ERosterLoadResult::RecoveryError;
		Outcome.Message = Problem;
		return Outcome;
	}
	Outcome.Result = ERosterLoadResult::Recovered;
	bool bActiveSeen = false;
	for (const FRosterEntrySaveRow& Row : Rows)
	{
		FRosterEntry Entry;
		Entry.CharacterId = Row.CharacterId;
		Entry.ProfessionId = Row.ProfessionId;
		Entry.DisplayName = Row.DisplayName;
		Entry.SavePrefix = Row.SavePrefix;
		Entry.bActive = Row.bActive;
		if (Entry.bActive && !bActiveSeen)
		{
			Outcome.ActiveCharacterId = Entry.CharacterId;
			bActiveSeen = true;
		}
		Outcome.Entries.Add(Entry);
	}
	return Outcome;
}

bool URosterService::SaveInitialCharacterState(const FString& Prefix,
	const FProfileSaveRequest& InitialState, FString& OutProblem)
{
	UProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
	if (!CharService->Initialize(Prefix))
	{
		OutProblem = FString::Printf(TEXT("the character save service rejected the prefix '%s'"), *Prefix);
		return false;
	}
	CharService->SetStorage(ResolveStorage());
	const FProfileSaveOutcome Outcome = CharService->SaveProfile(InitialState);
	if (Outcome.Result != EProfileSaveResult::Success)
	{
		OutProblem = FString::Printf(TEXT("the character's first A/B save failed: %s"), *Outcome.Message);
		return false;
	}
	// Read-back verification through the character's own load path.
	const FProfileLoadOutcome Load = CharService->LoadActiveProfile();
	if (Load.Result != EProfileLoadResult::Success
		|| Load.Snapshot.CharacterId != InitialState.Snapshot.CharacterId)
	{
		OutProblem = FString::Printf(TEXT("the character's first save failed the read-back verification (result %d)"),
			static_cast<int32>(Load.Result));
		return false;
	}
	return true;
}

bool URosterService::CreateCharacterInternal(const FGuid& CharacterId, FName ProfessionId,
	const FString& DisplayName, const FProfileSaveRequest& InitialState, FRosterCreateOutcome& Out)
{
	Out = FRosterCreateOutcome();
	// The committed roster (when any) is the base; a fresh install starts
	// from an empty list. A corrupt roster refuses creation (双角色 guard).
	TArray<FRosterEntrySaveRow> Committed;
	FString Problem;
	bool bNoRoster = false;
	if (!ReadCommittedEntries(Committed, Problem, bNoRoster) && !bNoRoster)
	{
		Out.Message = FString::Printf(TEXT("the roster is corrupt; character creation refuses: %s"), *Problem);
		return false;
	}
	const FString Prefix = DeriveSavePrefix(CharacterId);

	// 1) The pending roster row goes into the STAGING slot (recoverable
	//    evidence; the committed roster and its index stay untouched).
	TArray<FRosterEntrySaveRow> Pending = Committed;
	for (FRosterEntrySaveRow& Row : Pending)
	{
		Row.bActive = false;
	}
	FRosterEntrySaveRow PendingRow;
	PendingRow.CharacterId = CharacterId;
	PendingRow.ProfessionId = ProfessionId;
	PendingRow.DisplayName = DisplayName;
	PendingRow.SavePrefix = Prefix;
	PendingRow.bActive = true;
	Pending.Add(PendingRow);

	int32 ActiveSlot = NoActiveSlot;
	int32 Generations[2] = { 0, 0 };
	bool bIndexMissing = false;
	FString IndexProblem;
	(void)M2_ReadIndex(ActiveSlot, Generations, bIndexMissing, IndexProblem);
	const int32 Staging = (bIndexMissing || ActiveSlot == NoActiveSlot) ? 0 : (1 - ActiveSlot);
	const int32 PendingGeneration = FMath::Max(Generations[0], Generations[1]) + 1;
	FString WriteProblem;
	if (!WriteRosterSlotAndVerify(Staging, Pending, PendingGeneration, WriteProblem))
	{
		Out.Message = FString::Printf(TEXT("the pending roster row could not be staged: %s"), *WriteProblem);
		return false;
	}

	// 2) The character's own A/B/Index save (write + read-back verification).
	FProfileSaveRequest Request = InitialState;
	Request.Snapshot.CharacterId = CharacterId;
	Request.ProfessionState.ProfessionId = ProfessionId;
	if (!SaveInitialCharacterState(Prefix, Request, Problem))
	{
		Out.Message = FString::Printf(TEXT("the character's initial save failed: %s"), *Problem);
		return false;
	}

	// 3) The roster commit (the pending row becomes the committed state).
	if (!CommitRoster(Pending, Problem))
	{
		Out.Message = FString::Printf(TEXT("the roster commit failed: %s"), *Problem);
		return false;
	}
	Out.Result = ERosterCreateResult::Created;
	Out.CharacterId = CharacterId;
	Out.SavePrefix = Prefix;
	return true;
}

FRosterCreateOutcome URosterService::CreateCharacter(FName ProfessionId,
	const FString& DisplayName, const FProfileSaveRequest& InitialState)
{
	FRosterCreateOutcome Out;
	// The service mints the identity (the caller's NewProfile re-syncs from
	// the returned identity afterwards).
	const FGuid Minted = FGuid::NewGuid();
	CreateCharacterInternal(Minted, ProfessionId, DisplayName, InitialState, Out);
	return Out;
}

FRosterImportOutcome URosterService::ImportLegacyProfile(UProfileSaveService& LegacyService,
	UProfileSubsystem* Profile)
{
	FRosterImportOutcome Out;
	// The 双角色 guard: a healthy roster refuses the import outright.
	FRosterLoadOutcome Roster = LoadRoster();
	if (Roster.Result == ERosterLoadResult::Recovered || Roster.Result == ERosterLoadResult::RecoveredFallback)
	{
		Out.Result = ERosterImportResult::RosterAlreadyExists;
		Out.Message = FString::Printf(TEXT("a healthy roster with %d character(s) already exists"), Roster.Entries.Num());
		return Out;
	}
	if (Roster.Result == ERosterLoadResult::RecoveryError)
	{
		// A corrupt roster is NEVER treated as a fresh state: the files stay
		// on disk for diagnosis and the import is not retried this session.
		Out.Result = ERosterImportResult::Failed;
		Out.Message = FString::Printf(TEXT("the roster is corrupt; the import refuses: %s"), *Roster.Message);
		return Out;
	}

	// Probe the legacy data.
	const FProfileLoadOutcome Legacy = LegacyService.LoadActiveProfile();
	if (Legacy.Result == EProfileLoadResult::FailedNoData)
	{
		// Fresh install: the default prototype character (the same shape the
		// M3-029 first-boot path would have created, now through the roster).
		if (Profile == nullptr)
		{
			Out.Result = ERosterImportResult::Failed;
			Out.Message = TEXT("the fresh-install path needs the profile subsystem");
			return Out;
		}
		Profile->NewProfile();
		FProfileSaveRequest Request;
		Request.Snapshot = Profile->GetProfileSnapshot();
		Request.Inventory = Profile->GetInventory();
		// M7-004 fix: the request MUST carry the legacy-equivalent profession
		// state explicitly. The struct default left the fresh payload with
		// ProfessionId None and no rank rows - the exact artifact the first
		// packaged launch wrote over the user's legacy slots.
		Request.ProfessionState = FProfessionSaveState::MakeLegacyPrototype();
		FRosterCreateOutcome Created;
		if (!CreateCharacterInternal(Profile->GetCharacterId(), FName(TEXT("prototype")),
			TEXT("原型战士"), Request, Created))
		{
			Out.Result = ERosterImportResult::Failed;
			Out.Message = Created.Message;
			return Out;
		}
		Out.Result = ERosterImportResult::FreshInstallCreated;
		Out.CharacterId = Created.CharacterId;
		Out.SavePrefix = Created.SavePrefix;
		return Out;
	}
	if (Legacy.Result != EProfileLoadResult::Success
		&& Legacy.Result != EProfileLoadResult::SuccessFallback)
	{
		// The legacy data exists but is unusable: keep the legacy files and
		// the empty roster (retryable later) - never wipe, never guess.
		Out.Result = ERosterImportResult::LegacyRecoveryError;
		Out.Message = FString::Printf(TEXT("the legacy save exists but could not be restored: %s"), *Legacy.Message);
		return Out;
	}

	// The import: the new character slot derives from the ORIGINAL legacy
	// CharacterId (the identity is preserved), the profession state is the
	// legacy-equivalent shape (prototype + the four legacy attacks at rank 1)
	// and every legacy payload field travels verbatim. The legacy A/B/Index
	// bytes stay read-only (this flow never writes the legacy prefix).
	FProfileSaveRequest Request;
	Request.Snapshot = Legacy.Snapshot;
	Request.Inventory = Legacy.Inventory;
	Request.PendingRewards = Legacy.PendingRewards;
	Request.AppliedSettlementIds = Legacy.AppliedSettlementIds;
	Request.EquippedMap = Legacy.EquippedMap;
	Request.ProfessionState = FProfessionSaveState::MakeLegacyPrototype();
	Request.RoomClears = Legacy.RoomClears;

	FRosterCreateOutcome Created;
	if (!CreateCharacterInternal(Legacy.Snapshot.CharacterId, FName(TEXT("prototype")),
		TEXT("原型战士"), Request, Created))
	{
		Out.Result = ERosterImportResult::Failed;
		Out.Message = Created.Message;
		return Out;
	}
	Out.Result = ERosterImportResult::Imported;
	Out.CharacterId = Created.CharacterId;
	Out.SavePrefix = Created.SavePrefix;
	return Out;
}

bool URosterService::SetActiveCharacter(const FGuid& TargetCharacterId)
{
	TArray<FRosterEntrySaveRow> Committed;
	FString Problem;
	bool bNoRoster = false;
	if (!ReadCommittedEntries(Committed, Problem, bNoRoster) || bNoRoster)
	{
		return false;
	}
	FRosterEntrySaveRow* Target = Committed.FindByPredicate(
		[TargetCharacterId](const FRosterEntrySaveRow& Row) { return Row.CharacterId == TargetCharacterId; });
	if (!Target)
	{
		return false;
	}
	// The switching order: the caller saved the CURRENT character first; this
	// entry verifies the target's own save still loads before committing.
	UProfileSaveService* TargetService = NewObject<UProfileSaveService>(GetTransientPackage());
	if (!TargetService->Initialize(Target->SavePrefix))
	{
		return false;
	}
	TargetService->SetStorage(ResolveStorage());
	if (TargetService->LoadActiveProfile().Result != EProfileLoadResult::Success)
	{
		return false;
	}
	for (FRosterEntrySaveRow& Row : Committed)
	{
		Row.bActive = Row.CharacterId == TargetCharacterId;
	}
	return CommitRoster(Committed, Problem);
}

int32 URosterService::DeleteTestSlots()
{
	int32 Deleted = 0;
	for (const FString& Name : SlotNames)
	{
		if (ResolveStorage()->DoesSlotExist(Name) && ResolveStorage()->DeleteSlot(Name))
		{
			++Deleted;
		}
	}
	return Deleted;
}
