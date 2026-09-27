// M3-014: A/B dual-slot profile save transaction with index commit
// (interface contract section 8). GREEN-PHASE implementation replacing the
// two red stubs (A: always-fail, B: fake success without disk effects).
//
// Transaction shape (see ProfileSaveService.h for the full contract):
// - BeginSave copies the request ONCE (the consistent snapshot) and validates
//   the payload with the M3-013 serializer BEFORE any disk write; a second
//   BeginSave while a save is in progress merges the NEWEST snapshot over
//   the queued one (documented choice: merge-over-queue - the superseded
//   state is stale the moment the newer state exists, and nothing stacks).
// - ProcessPendingSave reads the index fresh from storage, writes the new
//   payload (generation = max(indexed generations) + 1) into the INACTIVE
//   slot, reads it back and verifies generation + integrity fields + the
//   M3-013 FromSaveGame validation, and only then commits the index. Every
//   failure leaves the index and the old active slot untouched; the failed
//   slot holds uncommitted content that the next save simply overwrites.
// - LoadActiveProfile reads the index then the active slot; on a
//   missing/corrupt index or an unusable active slot it falls back to a
//   valid slot (higher generation wins when scanning); nothing valid means
//   an explicit failure (FailedNoData when truly nothing is stored, else
//   FailedCorrupt) with no partially restored data.
//
// Reentrancy note: ProcessPendingSave consumes the in-progress guard at its
// start and works on a local copy of the snapshot, so a (theoretical,
// storage-callback-driven) reentrant BeginSave during the transaction would
// queue a NEW snapshot for the next process call instead of corrupting the
// running one. The service is synchronous and game-thread only.

#include "ProfileSaveService.h"

#include "ProfileSerializer.h"

#include "Kismet/GameplayStatics.h"
#include "Misc/Crc.h"
#include "UObject/UObjectGlobals.h"

// -- Storage implementations ---------------------------------------------------

bool FGameplayStaticsSaveStorage::WriteSlot(const FString& SlotName, USaveGame* Data)
{
	if (!Data)
	{
		return false;
	}
	return UGameplayStatics::SaveGameToSlot(Data, SlotName, 0);
}

USaveGame* FGameplayStaticsSaveStorage::ReadSlot(const FString& SlotName)
{
	return UGameplayStatics::LoadGameFromSlot(SlotName, 0);
}

bool FGameplayStaticsSaveStorage::DeleteSlot(const FString& SlotName)
{
	return UGameplayStatics::DeleteGameInSlot(SlotName, 0);
}

bool FGameplayStaticsSaveStorage::DoesSlotExist(const FString& SlotName)
{
	return UGameplayStatics::DoesSaveGameExist(SlotName, 0);
}

// -- Lifecycle ------------------------------------------------------------------

bool UProfileSaveService::Initialize(const FString& InSlotPrefix)
{
	FString Trimmed = InSlotPrefix;
	Trimmed.TrimStartAndEndInline();
	if (Trimmed.IsEmpty())
	{
		// An empty prefix would produce generic "A"/"B"/"Index" slot names
		// that could collide with unrelated saves - never acceptable.
		return false;
	}
	SlotPrefix = Trimmed;
	SlotNames[0] = SlotPrefix + TEXT("A");
	SlotNames[1] = SlotPrefix + TEXT("B");
	IndexSlotName = SlotPrefix + TEXT("Index");
	return true;
}

void UProfileSaveService::SetStorage(ISaveStorage* InStorage)
{
	Storage = InStorage;
	OwnedStorage.Reset();
}

bool UProfileSaveService::IsSaveInProgress() const
{
	return bSaveInProgress;
}

const FString& UProfileSaveService::GetSlotPrefix() const
{
	return SlotPrefix;
}

ISaveStorage* UProfileSaveService::ResolveStorage()
{
	if (!Storage)
	{
		OwnedStorage = MakeUnique<FGameplayStaticsSaveStorage>();
		Storage = OwnedStorage.Get();
	}
	return Storage;
}

// -- Snapshot / payload helpers ---------------------------------------------------

uint32 UProfileSaveService::M3_014_ComputePayloadDigest(const UProfileSaveGame& Save)
{
	// Canonical ordering of every base payload field; a single flipped field
	// changes the digest. The applied-ids ARRAY is hashed in stored order
	// (serialization preserves array order, so a reload recomputes the same
	// digest regardless of TSet iteration order at write time).
	FString Canonical;
	Canonical += FString::Printf(TEXT("schema=%d|id=%s|level=%d|xp=%d|volume=%s|"),
		Save.SchemaVersion, *Save.CharacterId.ToString(), Save.Level, Save.XP,
		*FString::SanitizeFloat(Save.MasterVolume));
	for (const FString& Snapshot : Save.InventoryInstanceSnapshots)
	{
		Canonical += FString::Printf(TEXT("item=%s|"), *Snapshot);
	}
	for (const FEquippedSlotSavePair& Pair : Save.EquippedIds)
	{
		Canonical += FString::Printf(TEXT("equip=%u:%s|"), Pair.Slot, *Pair.InstanceId.ToString());
	}
	for (const FPendingRewardSaveEntry& Entry : Save.PendingRewards)
	{
		Canonical += FString::Printf(TEXT("draft=%llu:%d|"), Entry.SettlementId, Entry.XP);
		for (const FString& Item : Entry.ItemSnapshots)
		{
			Canonical += FString::Printf(TEXT("draftitem=%s|"), *Item);
		}
	}
	for (const uint64 AppliedId : Save.AppliedSettlementIds)
	{
		Canonical += FString::Printf(TEXT("applied=%llu|"), AppliedId);
	}
	return FCrc::StrCrc32(*Canonical);
}

UProfileSlotSaveGame* UProfileSaveService::M3_014_BuildSlotSave(const FProfileSaveRequest& Request, int32 Generation)
{
	UProfileSaveGame* Payload = FProfileSerializer::ToSaveGame(Request.Snapshot, Request.Inventory,
		Request.PendingRewards, Request.AppliedSettlementIds, Request.EquippedMap);
	if (!Payload)
	{
		return nullptr;
	}
	UProfileSlotSaveGame* SlotSave = NewObject<UProfileSlotSaveGame>(GetTransientPackage());
	// Copy every reflected UProfileSaveGame property (including inherited
	// members) into the subclass: schema-robust - a future payload field
	// added to UProfileSaveGame travels through this copy automatically.
	for (TFieldIterator<FProperty> It(UProfileSaveGame::StaticClass()); It; ++It)
	{
		FProperty* Property = *It;
		Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(SlotSave),
			Property->ContainerPtrToValuePtr<void>(Payload));
	}
	SlotSave->SlotGeneration = Generation;
	SlotSave->PayloadInstanceCount = SlotSave->InventoryInstanceSnapshots.Num();
	SlotSave->CharacterIdSummary = SlotSave->CharacterId.ToString();
	SlotSave->PayloadDigest = M3_014_ComputePayloadDigest(*SlotSave);
	return SlotSave;
}

bool UProfileSaveService::M3_014_ReadIndex(int32& OutActiveSlot, int32 (&OutGenerations)[2], bool& bOutMissing, FString& OutProblem)
{
	bOutMissing = false;
	ISaveStorage* ActiveStorage = ResolveStorage();
	USaveGame* Raw = ActiveStorage->ReadSlot(IndexSlotName);
	if (!Raw)
	{
		if (ActiveStorage->DoesSlotExist(IndexSlotName))
		{
			OutProblem = TEXT("the index slot exists but failed to load");
			return false;
		}
		// Missing index = legal fresh state (first save goes to slot A).
		bOutMissing = true;
		OutActiveSlot = UProfileIndexSaveGame::NoActiveSlot;
		OutGenerations[0] = 0;
		OutGenerations[1] = 0;
		return true;
	}

	UProfileIndexSaveGame* Index = Cast<UProfileIndexSaveGame>(Raw);
	const bool bValidShape = Index != nullptr
		&& Index->SlotGenerations.Num() == 2
		&& (Index->ActiveSlotIndex == UProfileIndexSaveGame::NoActiveSlot
			|| Index->ActiveSlotIndex == 0
			|| Index->ActiveSlotIndex == 1);
	if (!bValidShape)
	{
		OutProblem = TEXT("the index slot content is not a valid UProfileIndexSaveGame");
		return false;
	}
	OutActiveSlot = Index->ActiveSlotIndex;
	OutGenerations[0] = Index->SlotGenerations[0];
	OutGenerations[1] = Index->SlotGenerations[1];
	return true;
}

void UProfileSaveService::M3_014_RecoverStateFromSlots(int32& OutEffectiveActive, int32 (&OutGenerations)[2])
{
	ISaveStorage* ActiveStorage = ResolveStorage();
	int32 SlotGenerations[2] = { 0, 0 };
	bool bValid[2] = { false, false };
	for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		const UProfileSlotSaveGame* Slot = Cast<UProfileSlotSaveGame>(ActiveStorage->ReadSlot(SlotNames[SlotIndex]));
		if (Slot
			&& Slot->PayloadDigest == M3_014_ComputePayloadDigest(*Slot)
			&& Slot->PayloadInstanceCount == Slot->InventoryInstanceSnapshots.Num()
			&& Slot->CharacterIdSummary == Slot->CharacterId.ToString())
		{
			bValid[SlotIndex] = true;
			SlotGenerations[SlotIndex] = Slot->SlotGeneration;
		}
	}
	if (bValid[0] && bValid[1])
	{
		// Both payloads are valid: the higher generation was committed last
		// (every commit writes max+1 into the newly active slot), so it is
		// the effective active save. A tie resolves to slot A.
		OutEffectiveActive = (SlotGenerations[0] >= SlotGenerations[1]) ? 0 : 1;
	}
	else if (bValid[0] || bValid[1])
	{
		OutEffectiveActive = bValid[0] ? 0 : 1;
	}
	else
	{
		OutEffectiveActive = UProfileIndexSaveGame::NoActiveSlot;
	}
	OutGenerations[0] = SlotGenerations[0];
	OutGenerations[1] = SlotGenerations[1];
}

bool UProfileSaveService::M3_014_VerifyReadBack(const USaveGame* RawLoaded, int32 ExpectedGeneration,
	const UProfileSaveGame& SourcePayload, FString& OutProblem)
{
	if (!RawLoaded)
	{
		OutProblem = TEXT("the freshly written slot did not load back");
		return false;
	}
	const UProfileSlotSaveGame* Slot = Cast<UProfileSlotSaveGame>(RawLoaded);
	if (!Slot)
	{
		OutProblem = TEXT("the loaded slot object is not a UProfileSlotSaveGame");
		return false;
	}
	if (Slot->SlotGeneration != ExpectedGeneration)
	{
		OutProblem = FString::Printf(TEXT("the loaded slot carries generation %d, expected %d"),
			Slot->SlotGeneration, ExpectedGeneration);
		return false;
	}
	if (Slot->PayloadInstanceCount != Slot->InventoryInstanceSnapshots.Num())
	{
		OutProblem = FString::Printf(TEXT("the loaded slot records %d inventory instances but carries %d snapshots"),
			Slot->PayloadInstanceCount, Slot->InventoryInstanceSnapshots.Num());
		return false;
	}
	if (Slot->CharacterIdSummary != Slot->CharacterId.ToString())
	{
		OutProblem = TEXT("the loaded slot's CharacterId summary does not match its CharacterId");
		return false;
	}
	const uint32 LoadedDigest = M3_014_ComputePayloadDigest(*Slot);
	if (Slot->PayloadDigest != LoadedDigest)
	{
		OutProblem = TEXT("the loaded slot's stored integrity digest does not match its payload (integrity check failed)");
		return false;
	}
	if (LoadedDigest != M3_014_ComputePayloadDigest(SourcePayload))
	{
		OutProblem = TEXT("the loaded slot payload differs from the captured snapshot");
		return false;
	}

	// Full serializer gate: the read-back must pass the M3-013 validation and
	// restore cleanly into scratch containers before it may go active.
	FProfileSnapshot ScratchSnapshot;
	FInventoryModel ScratchInventory;
	TArray<FPendingReward> ScratchPending;
	TSet<uint64> ScratchApplied;
	TMap<EItemSlot, FGuid> ScratchEquipped;
	FProfileLoadError Error;
	if (!FProfileSerializer::FromSaveGame(Slot, ScratchSnapshot, ScratchInventory, ScratchPending,
		ScratchApplied, ScratchEquipped, Error))
	{
		OutProblem = FString::Printf(TEXT("the loaded slot failed the serializer validation: %s"), *Error.Message);
		return false;
	}
	return true;
}

// -- Save transaction -----------------------------------------------------------

FProfileSaveOutcome UProfileSaveService::BeginSave(const FProfileSaveRequest& Request)
{
	FProfileSaveOutcome Outcome;
	if (SlotPrefix.IsEmpty())
	{
		Outcome.Result = EProfileSaveResult::FailedCapture;
		Outcome.Message = TEXT("the service was not initialized with a slot prefix");
		return Outcome;
	}
	if (bSaveInProgress)
	{
		// Merge semantics: the newest request replaces the queued snapshot;
		// exactly one transaction stays queued (nothing stacks).
		PendingRequest = Request;
		Outcome.Result = EProfileSaveResult::Merged;
		Outcome.Message = TEXT("a save was already in progress; the newest snapshot replaced the queued one");
		return Outcome;
	}

	// Capture the consistent snapshot: one deep value copy of every input.
	PendingRequest = Request;

	// Refuse invalid profile state BEFORE any disk write. The serializer
	// trusts its input, so the capture builds the payload and runs the same
	// validation the load path will apply. The CharacterId check is service
	// policy: an unnamed profile is an upstream bug, not a savable state.
	if (!PendingRequest.Snapshot.CharacterId.IsValid())
	{
		PendingRequest = FProfileSaveRequest();
		Outcome.Result = EProfileSaveResult::FailedCapture;
		Outcome.Message = TEXT("the profile snapshot carries no valid CharacterId");
		return Outcome;
	}
	UProfileSlotSaveGame* Probe = M3_014_BuildSlotSave(PendingRequest, 0);
	if (!Probe)
	{
		PendingRequest = FProfileSaveRequest();
		Outcome.Result = EProfileSaveResult::FailedCapture;
		Outcome.Message = TEXT("the save payload could not be built");
		return Outcome;
	}
	FProfileLoadError Error;
	if (!FProfileSerializer::Validate(Probe, Error))
	{
		PendingRequest = FProfileSaveRequest();
		Outcome.Result = EProfileSaveResult::FailedCapture;
		Outcome.Message = FString::Printf(TEXT("the profile state was rejected before saving: %s"), *Error.Message);
		return Outcome;
	}

	bSaveInProgress = true;
	Outcome.Result = EProfileSaveResult::Queued;
	return Outcome;
}

FProfileSaveOutcome UProfileSaveService::ProcessPendingSave()
{
	FProfileSaveOutcome Outcome;
	if (SlotPrefix.IsEmpty() || !bSaveInProgress)
	{
		Outcome.Result = EProfileSaveResult::FailedNoPendingSave;
		Outcome.Message = TEXT("ProcessPendingSave without a queued snapshot");
		return Outcome;
	}

	// Consume the transaction: the guard is cleared and the snapshot moves to
	// a local (see the reentrancy note in the file header).
	FProfileSaveRequest Request = MoveTemp(PendingRequest);
	PendingRequest = FProfileSaveRequest();
	bSaveInProgress = false;

	// Step 1: fresh index state from storage (the index file is the single
	// source of truth for which slot is active and what generations exist).
	int32 ActiveSlot = UProfileIndexSaveGame::NoActiveSlot;
	int32 Generations[2] = { 0, 0 };
	bool bIndexMissing = false;
	FString IndexProblem;
	if (!M3_014_ReadIndex(ActiveSlot, Generations, bIndexMissing, IndexProblem))
	{
		// An existing but unreadable index means the service cannot know
		// which slot holds the only good copy - refuse instead of guessing.
		Outcome.Result = EProfileSaveResult::FailedIndexState;
		Outcome.Message = FString::Printf(TEXT("the activity index is unreadable (%s); refusing to pick a slot to overwrite"), *IndexProblem);
		return Outcome;
	}
	if (bIndexMissing)
	{
		// The index file is gone, but each slot payload self-describes with
		// its integrity digest and generation: recover the effective state
		// from the payloads so the transaction never overwrites the
		// surviving newer save and the generation stays monotonic (a plain
		// fresh-state restart here would write slot A again and could
		// destroy the only good copy on a later failed read-back).
		M3_014_RecoverStateFromSlots(ActiveSlot, Generations);
	}

	// Step 2: target = the INACTIVE slot (fresh state writes slot A first);
	// generation = max(indexed generations) + 1, monotonic across fallbacks.
	const int32 TargetSlot = (ActiveSlot == 0) ? 1 : 0;
	const int32 NewGeneration = FMath::Max(Generations[0], Generations[1]) + 1;

	// Step 2b: M3-015 write guard. StartupLoad may have armed slots this
	// build must never overwrite (a future-schema save belonging to a newer
	// build, or corrupt evidence of an unrecoverable startup state). Refusing
	// here keeps the guarded file byte-identical; a save never weakens the
	// guard (only the next StartupLoad re-arms it).
	if (const FString* GuardReason = M3_015_NoOverwriteSlots.Find(SlotNames[TargetSlot]))
	{
		Outcome.Result = EProfileSaveResult::FailedGuardedSlot;
		Outcome.Message = FString::Printf(TEXT("the save was refused: slot '%s' is under the startup write guard (%s); the file stays untouched"),
			*SlotNames[TargetSlot], **GuardReason);
		return Outcome;
	}

	// Step 3: build the transaction payload from the captured snapshot.
	UProfileSlotSaveGame* SlotSave = M3_014_BuildSlotSave(Request, NewGeneration);
	if (!SlotSave)
	{
		Outcome.Result = EProfileSaveResult::FailedCapture;
		Outcome.Message = TEXT("the save payload could not be built from the captured snapshot");
		return Outcome;
	}

	// Step 4: write the inactive slot. On failure the active slot and the
	// index are untouched.
	ISaveStorage* ActiveStorage = ResolveStorage();
	if (!ActiveStorage->WriteSlot(SlotNames[TargetSlot], SlotSave))
	{
		Outcome.Result = EProfileSaveResult::FailedWriteSlot;
		Outcome.Message = FString::Printf(TEXT("writing the inactive slot '%s' failed (storage refused)"), *SlotNames[TargetSlot]);
		return Outcome;
	}

	// Step 5: read back and verify (generation, integrity fields, serializer
	// validation). On failure the written slot is uncommitted content that
	// the next save overwrites; the index was not touched.
	USaveGame* LoadedBack = ActiveStorage->ReadSlot(SlotNames[TargetSlot]);
	FString VerifyProblem;
	if (!M3_014_VerifyReadBack(LoadedBack, NewGeneration, *SlotSave, VerifyProblem))
	{
		Outcome.Result = EProfileSaveResult::FailedReadBack;
		Outcome.Message = FString::Printf(TEXT("the read-back verification of '%s' failed: %s"), *SlotNames[TargetSlot], *VerifyProblem);
		return Outcome;
	}

	// Step 6: COMMIT - the index is the only file that activates the new
	// slot. It keeps the other slot's last committed generation.
	UProfileIndexSaveGame* IndexSave = NewObject<UProfileIndexSaveGame>(GetTransientPackage());
	IndexSave->ActiveSlotIndex = TargetSlot;
	IndexSave->SlotGenerations.Add(Generations[0]);
	IndexSave->SlotGenerations.Add(Generations[1]);
	IndexSave->SlotGenerations[TargetSlot] = NewGeneration;
	if (!ActiveStorage->WriteSlot(IndexSlotName, IndexSave))
	{
		Outcome.Result = EProfileSaveResult::FailedWriteIndex;
		Outcome.Message = FString::Printf(TEXT("committing the activity index failed; the written slot '%s' stays inactive and the old active save remains loadable"), *SlotNames[TargetSlot]);
		return Outcome;
	}

	Outcome.Result = EProfileSaveResult::Success;
	Outcome.Generation = NewGeneration;
	Outcome.WrittenSlotIndex = TargetSlot;
	return Outcome;
}

FProfileSaveOutcome UProfileSaveService::SaveProfile(const FProfileSaveRequest& Request)
{
	FProfileSaveOutcome BeginOutcome = BeginSave(Request);
	if (BeginOutcome.Result == EProfileSaveResult::FailedCapture)
	{
		return BeginOutcome;
	}
	return ProcessPendingSave();
}

// -- Load path --------------------------------------------------------------------

FProfileLoadOutcome UProfileSaveService::LoadActiveProfile()
{
	FProfileLoadOutcome Outcome;
	Outcome.Result = EProfileLoadResult::FailedNoData;
	Outcome.SlotIndex = -1;

	if (SlotPrefix.IsEmpty())
	{
		Outcome.Message = TEXT("the service was not initialized with a slot prefix");
		return Outcome;
	}

	// One slot load attempt: integrity fields + full serializer restore.
	auto TryLoadSlot = [this](int32 SlotIndex, FProfileLoadOutcome& Out, FString& OutProblem) -> bool
	{
		USaveGame* Raw = ResolveStorage()->ReadSlot(SlotNames[SlotIndex]);
		if (!Raw)
		{
			OutProblem = FString::Printf(TEXT("slot '%s' is missing or unreadable"), *SlotNames[SlotIndex]);
			return false;
		}
		const UProfileSlotSaveGame* Slot = Cast<UProfileSlotSaveGame>(Raw);
		if (!Slot)
		{
			OutProblem = FString::Printf(TEXT("slot '%s' is not a UProfileSlotSaveGame"), *SlotNames[SlotIndex]);
			return false;
		}
		if (Slot->PayloadDigest != M3_014_ComputePayloadDigest(*Slot))
		{
			OutProblem = FString::Printf(TEXT("slot '%s' failed its integrity digest check"), *SlotNames[SlotIndex]);
			return false;
		}
		if (Slot->PayloadInstanceCount != Slot->InventoryInstanceSnapshots.Num()
			|| Slot->CharacterIdSummary != Slot->CharacterId.ToString())
		{
			OutProblem = FString::Printf(TEXT("slot '%s' failed its integrity field checks"), *SlotNames[SlotIndex]);
			return false;
		}
		FProfileLoadError Error;
		if (!FProfileSerializer::FromSaveGame(Slot, Out.Snapshot, Out.Inventory, Out.PendingRewards,
			Out.AppliedSettlementIds, Out.EquippedMap, Error))
		{
			OutProblem = FString::Printf(TEXT("slot '%s' failed the serializer validation: %s"), *SlotNames[SlotIndex], *Error.Message);
			return false;
		}
		Out.Generation = Slot->SlotGeneration;
		Out.SlotIndex = SlotIndex;
		return true;
	};

	int32 ActiveSlot = UProfileIndexSaveGame::NoActiveSlot;
	int32 Generations[2] = { 0, 0 };
	bool bIndexMissing = false;
	FString IndexProblem;
	const bool bIndexUsable = M3_014_ReadIndex(ActiveSlot, Generations, bIndexMissing, IndexProblem);

	if (bIndexUsable && (ActiveSlot == 0 || ActiveSlot == 1))
	{
		FString ActiveProblem;
		if (TryLoadSlot(ActiveSlot, Outcome, ActiveProblem))
		{
			Outcome.Result = EProfileLoadResult::Success;
			Outcome.Message = FString::Printf(TEXT("loaded the active slot '%s' (generation %d)"),
				*SlotNames[ActiveSlot], Outcome.Generation);
			return Outcome;
		}
		// The active slot is unusable: fall back to the other slot.
		const int32 OtherSlot = 1 - ActiveSlot;
		FString OtherProblem;
		if (TryLoadSlot(OtherSlot, Outcome, OtherProblem))
		{
			Outcome.Result = EProfileLoadResult::SuccessFallback;
			Outcome.Message = FString::Printf(TEXT("the index points at '%s' but it failed (%s); recovered '%s' (generation %d)"),
				*SlotNames[ActiveSlot], *ActiveProblem, *SlotNames[OtherSlot], Outcome.Generation);
			return Outcome;
		}
		Outcome.Result = EProfileLoadResult::FailedCorrupt;
		Outcome.SlotIndex = -1;
		Outcome.Generation = 0;
		Outcome.Message = FString::Printf(TEXT("no loadable slot: '%s': %s; '%s': %s"),
			*SlotNames[ActiveSlot], *ActiveProblem, *SlotNames[OtherSlot], *OtherProblem);
		return Outcome;
	}

	// Index missing or corrupt: scan both slots, prefer the higher generation.
	const bool bScanReason = bIndexMissing;
	FProfileLoadOutcome Candidates[2];
	FString Problems[2];
	bool bValid[2] = { false, false };
	for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		bValid[SlotIndex] = TryLoadSlot(SlotIndex, Candidates[SlotIndex], Problems[SlotIndex]);
	}
	int32 BestSlot = -1;
	for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		if (bValid[SlotIndex] && (BestSlot == -1 || Candidates[SlotIndex].Generation > Candidates[BestSlot].Generation))
		{
			BestSlot = SlotIndex;
		}
	}
	if (BestSlot != -1)
	{
		Outcome = MoveTemp(Candidates[BestSlot]);
		Outcome.Result = EProfileLoadResult::SuccessFallback;
		Outcome.Message = FString::Printf(TEXT("the index is %s (%s); recovered '%s' (generation %d) by scan"),
			bScanReason ? TEXT("missing") : TEXT("corrupt"), *IndexProblem, *SlotNames[BestSlot], Outcome.Generation);
		return Outcome;
	}

	const bool bNothingStored = !ResolveStorage()->DoesSlotExist(SlotNames[0])
		&& !ResolveStorage()->DoesSlotExist(SlotNames[1])
		&& !ResolveStorage()->DoesSlotExist(IndexSlotName);
	if (bNothingStored)
	{
		Outcome.Result = EProfileLoadResult::FailedNoData;
		Outcome.Message = FString::Printf(TEXT("no profile save exists under prefix '%s' (no index, no slots)"), *SlotPrefix);
		return Outcome;
	}
	Outcome.Result = EProfileLoadResult::FailedCorrupt;
	Outcome.Message = FString::Printf(TEXT("no loadable slot and the index is %s: slot A: %s; slot B: %s"),
		bScanReason ? TEXT("missing") : TEXT("corrupt"),
		*Problems[0], *Problems[1]);
	return Outcome;
}

FStartupLoadOutcome UProfileSaveService::StartupLoad()
{
	FStartupLoadOutcome Outcome;
	Outcome.SlotIndex = -1;

	if (SlotPrefix.IsEmpty())
	{
		Outcome.Result = EStartupLoadResult::RecoveryError;
		Outcome.Summary = TEXT("the save service was not initialized with a slot prefix");
		return Outcome;
	}

	// Re-arm the write guard: every startup pass re-evaluates from scratch.
	M3_015_NoOverwriteSlots.Reset();

	// Classify both slots (schema gate first, then the integrity envelope,
	// then the serializer restore). A future-schema slot is reported AND
	// write-guarded no matter which result the load reaches: this older build
	// must never overwrite a newer build's save.
	FM3_015_SlotRead Reads[2];
	for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		M3_015_ClassifySlot(SlotIndex, Reads[SlotIndex]);
		if (Reads[SlotIndex].bExists && !Reads[SlotIndex].bLoaded)
		{
			// The recovery report always names every existing unusable slot,
			// even when a fallback succeeded (the user must see what broke).
			Outcome.CorruptedSlotReports.Add(
				FString::Printf(TEXT("'%s': %s"), *SlotNames[SlotIndex], *Reads[SlotIndex].Problem));
		}
		if (Reads[SlotIndex].bFutureSchema)
		{
			Outcome.FutureSchemaSlotNames.Add(SlotNames[SlotIndex]);
			M3_015_NoOverwriteSlots.Add(SlotNames[SlotIndex],
				FString::Printf(TEXT("the slot holds a schema version %d save written by a newer game build (this build understands %d)"),
					Reads[SlotIndex].SchemaVersion, UProfileSaveGame::CurrentSchemaVersion));
		}
	}

	// Index state (M3_014_ReadIndex: a missing index is the legal fresh
	// signal; a readable index can also record "nothing committed yet").
	int32 ActiveSlot = UProfileIndexSaveGame::NoActiveSlot;
	int32 Generations[2] = { 0, 0 };
	bool bIndexMissing = false;
	FString IndexProblem;
	const bool bIndexUsable = M3_014_ReadIndex(ActiveSlot, Generations, bIndexMissing, IndexProblem);
	if (!bIndexUsable)
	{
		Outcome.IndexProblem = IndexProblem;
	}

	// Why the index did not deliver a usable active slot (report text).
	auto IndexStateText = [&]() -> FString
	{
		if (!bIndexUsable)
		{
			return FString::Printf(TEXT("the index is unreadable (%s)"), *IndexProblem);
		}
		if (bIndexMissing)
		{
			return TEXT("the index is missing");
		}
		if (ActiveSlot == UProfileIndexSaveGame::NoActiveSlot)
		{
			return TEXT("the index records no committed slot");
		}
		return FString::Printf(TEXT("the index points at '%s'"), *SlotNames[ActiveSlot]);
	};

	// Future-schema note appended to every summary that ships data (the
	// report must state that the guard is armed).
	auto GuardedSuffix = [&Outcome]() -> const TCHAR*
	{
		return Outcome.FutureSchemaSlotNames.Num() > 0
			? TEXT("; a future-schema save stays guarded: the save path refuses to overwrite it")
			: TEXT("");
	};

	// Arms the evidence guard for every existing-but-unusable NON-future-
	// schema slot. Only called on the RecoveryError paths: with a valid
	// recoverable state the M3-014 transaction keeps overwriting the failed
	// slot (pinned behavior), but when NOTHING is recoverable the bad files
	// are the only evidence and must not be touched by any save.
	auto ArmEvidenceGuard = [&Reads, &Outcome, this]()
	{
		for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
		{
			if (Reads[SlotIndex].bExists && !Reads[SlotIndex].bLoaded && !Reads[SlotIndex].bFutureSchema)
			{
				M3_015_NoOverwriteSlots.Add(SlotNames[SlotIndex],
					FString::Printf(TEXT("the slot file is unusable startup evidence (%s)"), *Reads[SlotIndex].Problem));
			}
		}
	};

	// Copies one classified slot's restored data into the outcome.
	auto CopyLoadedData = [&](const FM3_015_SlotRead& Source)
	{
		Outcome.Snapshot = Source.Snapshot;
		Outcome.Inventory = Source.Inventory;
		Outcome.PendingRewards = Source.PendingRewards;
		Outcome.AppliedSettlementIds = Source.AppliedSettlementIds;
		Outcome.EquippedMap = Source.EquippedMap;
		Outcome.Generation = Source.Generation;
	};

	// 1) The index names a real active slot: try it, then the other one.
	if (bIndexUsable && (ActiveSlot == 0 || ActiveSlot == 1))
	{
		if (Reads[ActiveSlot].bLoaded)
		{
			Outcome.Result = EStartupLoadResult::Recovered;
			Outcome.SlotIndex = ActiveSlot;
			CopyLoadedData(Reads[ActiveSlot]);
			Outcome.Summary = FString::Printf(TEXT("startup load: loaded the active slot '%s' (generation %d)%s"),
				*SlotNames[ActiveSlot], Outcome.Generation, GuardedSuffix());
			return Outcome;
		}
		const int32 OtherSlot = 1 - ActiveSlot;
		if (Reads[OtherSlot].bLoaded)
		{
			Outcome.Result = EStartupLoadResult::RecoveredFallback;
			Outcome.SlotIndex = OtherSlot;
			CopyLoadedData(Reads[OtherSlot]);
			Outcome.FallbackReason = FString::Printf(TEXT("the index points at '%s' but it is unusable (%s); recovered '%s' (generation %d)"),
				*SlotNames[ActiveSlot], *Reads[ActiveSlot].Problem, *SlotNames[OtherSlot], Outcome.Generation);
			Outcome.Summary = FString::Printf(TEXT("startup load: %s%s"), *Outcome.FallbackReason, GuardedSuffix());
			return Outcome;
		}

		// Both candidates are unusable: explicit recovery error - the corrupt
		// files are evidence and every slot write is refused from here on.
		Outcome.Result = EStartupLoadResult::RecoveryError;
		ArmEvidenceGuard();
		Outcome.Summary = FString::Printf(TEXT("startup load: recovery failed (%s) - no profile was restored and the corrupt saves stay untouched"),
			*IndexStateText());
		for (const FString& Problem : Outcome.CorruptedSlotReports)
		{
			Outcome.Summary += FString::Printf(TEXT("; %s"), *Problem);
		}
		Outcome.Summary += GuardedSuffix();
		return Outcome;
	}

	// 2) Index missing/unreadable/no-commit: scan both slot payloads, highest
	//    generation wins (a tie resolves to A, matching the M3-014 recovery).
	int32 BestSlot = -1;
	for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
	{
		if (Reads[SlotIndex].bLoaded && (BestSlot == -1 || Reads[SlotIndex].Generation > Reads[BestSlot].Generation))
		{
			BestSlot = SlotIndex;
		}
	}
	if (BestSlot != -1)
	{
		Outcome.Result = EStartupLoadResult::RecoveredFallback;
		Outcome.SlotIndex = BestSlot;
		CopyLoadedData(Reads[BestSlot]);
		Outcome.FallbackReason = FString::Printf(TEXT("%s; recovered '%s' (generation %d) by generation scan"),
			*IndexStateText(), *SlotNames[BestSlot], Outcome.Generation);
		Outcome.Summary = FString::Printf(TEXT("startup load: %s%s"), *Outcome.FallbackReason, GuardedSuffix());
		return Outcome;
	}

	// 3) Nothing usable: distinguish a fresh install from corruption. ONLY
	//    when nothing at all is stored (no index, no slot files) the result
	//    is NoSaveFound - creating a new profile is the CALLER's decision.
	const bool bNothingStored = !ResolveStorage()->DoesSlotExist(SlotNames[0])
		&& !ResolveStorage()->DoesSlotExist(SlotNames[1])
		&& !ResolveStorage()->DoesSlotExist(IndexSlotName);
	if (bNothingStored)
	{
		Outcome.Result = EStartupLoadResult::NoSaveFound;
		Outcome.Summary = FString::Printf(TEXT("startup load: no profile save exists under prefix '%s' (no index, no slot files); creating a new profile is the caller's decision"), *SlotPrefix);
		return Outcome;
	}

	// Data exists but nothing is loadable: keep the evidence, surface the
	// error, refuse every slot write.
	Outcome.Result = EStartupLoadResult::RecoveryError;
	ArmEvidenceGuard();
	Outcome.Summary = FString::Printf(TEXT("startup load: recovery failed (%s) - no profile was restored and the corrupt saves stay untouched"),
		*IndexStateText());
	for (const FString& Problem : Outcome.CorruptedSlotReports)
	{
		Outcome.Summary += FString::Printf(TEXT("; %s"), *Problem);
	}
	Outcome.Summary += GuardedSuffix();
	return Outcome;
}

void UProfileSaveService::M3_015_ClassifySlot(int32 SlotIndex, FM3_015_SlotRead& OutRead)
{
	OutRead = FM3_015_SlotRead();
	ISaveStorage* ActiveStorage = ResolveStorage();
	if (!ActiveStorage->DoesSlotExist(SlotNames[SlotIndex]))
	{
		OutRead.Problem = FString::Printf(TEXT("the slot file '%s' does not exist"), *SlotNames[SlotIndex]);
		return;
	}
	OutRead.bExists = true;

	USaveGame* Raw = ActiveStorage->ReadSlot(SlotNames[SlotIndex]);
	if (!Raw)
	{
		OutRead.Problem = FString::Printf(TEXT("the slot file '%s' exists but failed to load"), *SlotNames[SlotIndex]);
		return;
	}
	const UProfileSlotSaveGame* Slot = Cast<UProfileSlotSaveGame>(Raw);
	if (!Slot)
	{
		OutRead.Problem = FString::Printf(TEXT("the slot file '%s' is not a UProfileSlotSaveGame"), *SlotNames[SlotIndex]);
		return;
	}

	// SCHEMA GATE FIRST: a newer build may store a different digest canonical
	// form, so this build cannot even trust its integrity check on a future-
	// schema payload. Version detection must precede every other gate -
	// otherwise the old build would classify the newer save as "just corrupt"
	// and (worse) overwrite it as a failed slot. Version above this build's =
	// future schema (guarded, never overwritten); anything else unknown is
	// left to the serializer gate below (which rejects it as UnsupportedSchema).
	OutRead.SchemaVersion = Slot->SchemaVersion;
	if (Slot->SchemaVersion > UProfileSaveGame::CurrentSchemaVersion)
	{
		OutRead.bFutureSchema = true;
		OutRead.Problem = FString::Printf(TEXT("schema version %d was written by a newer game build (this build understands %d); refusing to load and guarding the file from overwrite"),
			Slot->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion);
		return;
	}

	// The M3-014 integrity envelope: stored digest and the two field checks.
	if (Slot->PayloadDigest != M3_014_ComputePayloadDigest(*Slot))
	{
		OutRead.Problem = FString::Printf(TEXT("the stored integrity digest of '%s' does not match its payload (integrity check failed)"), *SlotNames[SlotIndex]);
		return;
	}
	if (Slot->PayloadInstanceCount != Slot->InventoryInstanceSnapshots.Num()
		|| Slot->CharacterIdSummary != Slot->CharacterId.ToString())
	{
		OutRead.Problem = FString::Printf(TEXT("the stored integrity field checks of '%s' failed (instance count / character id summary)"), *SlotNames[SlotIndex]);
		return;
	}

	// Full serializer gate: restores into the report's data fields on success
	// and rejects every field-level corruption the envelope cannot see.
	FProfileLoadError Error;
	if (!FProfileSerializer::FromSaveGame(Slot, OutRead.Snapshot, OutRead.Inventory, OutRead.PendingRewards,
		OutRead.AppliedSettlementIds, OutRead.EquippedMap, Error))
	{
		OutRead.Problem = FString::Printf(TEXT("the save of '%s' failed the serializer validation: %s"), *SlotNames[SlotIndex], *Error.Message);
		return;
	}
	OutRead.bLoaded = true;
	OutRead.Generation = Slot->SlotGeneration;
}

int32 UProfileSaveService::DeleteTestSlots()
{
	if (SlotPrefix.IsEmpty())
	{
		return 0;
	}
	// Exactly the three names this service built from its own prefix - no
	// other slot is ever reachable here.
	const FString OwnedNames[3] = { SlotNames[0], SlotNames[1], IndexSlotName };
	ISaveStorage* ActiveStorage = ResolveStorage();
	int32 Removed = 0;
	for (const FString& Name : OwnedNames)
	{
		if (ActiveStorage->DoesSlotExist(Name) && ActiveStorage->DeleteSlot(Name))
		{
			++Removed;
		}
	}
	return Removed;
}
