# -*- coding: utf-8 -*-
"""M7-002 test repair: error propagation, SetStorage, real snapshots, ProfessionId default."""
from pathlib import Path

# 1) UProfileSaveGame.ProfessionId default back to None (ToSaveGame always stamps explicitly)
h = Path('Source/UEMMO/Persistence/ProfileSaveGame.h')
s = h.read_text(encoding='utf-8')
old = '\tUPROPERTY()\n\tFName ProfessionId = FName(TEXT("prototype"));'
if old in s:
    s = s.replace(old, '\tUPROPERTY()\n\tFName ProfessionId;', 1)
    h.write_text(s, encoding='utf-8')
    print('1) ProfessionId default -> None')
else:
    print('1) ProfessionId default already fixed')

# 2) M7 test file fixes
t = Path('Source/UEMMO/Tests/M7SaveMigrationTests.cpp')
ts = t.read_text(encoding='utf-8')

# 2a) real item-instance helper after the digest helper
old = '\tstatic URosterService* NewRosterService(FAutomationTestBase& Test, FMemoryRosterStorage& Storage,'
new = '''\t/**
\t * Deterministic item instance whose snapshot string is the REAL
\t * M3-001 wire form (the serializer validates the version tag, so a
\t * hand-written fake snapshot would be refused before any profession
\t * rule is reached).
\t */
\tstatic FItemInstance M7_002_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
\t\tfloat Attack, float Defense, float MaxHP, int32 ItemLevel)
\t{
\t\tFItemInstance Instance;
\t\tInstance.InstanceId = FGuid(0x7EED0000u + Seed, 0x0772u, 0x0072u, Seed * 19u + 3u);
\t\tInstance.DefinitionId = FName(DefinitionName);
\t\tInstance.RollSeed = static_cast<int64>(0x0772CBA9876543210ULL) + static_cast<int64>(Seed);
\t\tInstance.RolledStats.Attack = Attack;
\t\tInstance.RolledStats.Defense = Defense;
\t\tInstance.RolledStats.MaxHP = MaxHP;
\t\tInstance.Level = ItemLevel;
\t\treturn Instance;
\t}

\tstatic URosterService* NewRosterService(FAutomationTestBase& Test, FMemoryRosterStorage& Storage,'''
assert old in ts, 'helper anchor not found'
ts = ts.replace(old, new, 1)

# 2b) replace the three fake snapshots with real ToStringSnapshot strings
repl = [
 ('Save->InventoryInstanceSnapshots.Add(TEXT("M3-001|weapon_training|v1|seed=7|hp=1|atk=2|def=3|lvl=1"));',
  'Save->InventoryInstanceSnapshots.Add(\n\t\t\tM7_002_MakeInstance(7, TEXT("weapon_training"), 2.0f, 3.0f, 1.0f, 1).ToStringSnapshot());'),
 ('V1->InventoryInstanceSnapshots.Add(TEXT("M3-001|charm_training|v1|seed=3|hp=0|atk=0|def=20|lvl=1"));',
  'V1->InventoryInstanceSnapshots.Add(\n\t\tM7_002_MakeInstance(3, TEXT("charm_training"), 1.0f, 0.0f, 5.0f, 1).ToStringSnapshot());'),
 ('LegacySlot->InventoryInstanceSnapshots.Add(TEXT("M3-001|weapon_training|v1|seed=11|hp=1|atk=2|def=3|lvl=1"));',
  'LegacySlot->InventoryInstanceSnapshots.Add(\n\t\tM7_002_MakeInstance(11, TEXT("weapon_training"), 2.0f, 3.0f, 1.0f, 1).ToStringSnapshot());'),
]
for old, new in repl:
    assert old in ts, old[:60]
    ts = ts.replace(old, new, 1)

# 2c) RunFrom lambda propagates the error out to the caller
old = '''\tauto RunFrom = [](UProfileSaveGame* Save, FProfessionStateOut& Out) -> bool
\t{
\t\tFProfileLoadError Error;
\t\tFProfileSnapshot ScratchSnapshot;
\t\tFInventoryModel ScratchInventory;
\t\tTArray<FPendingReward> ScratchPending;
\t\tTSet<uint64> ScratchApplied;
\t\tTMap<EItemSlot, FGuid> ScratchEquipped;
\t\treturn FProfileSerializer::FromSaveGame(Save, ScratchSnapshot, ScratchInventory,
\t\t\tScratchPending, ScratchApplied, ScratchEquipped, Error,
\t\t\tnullptr, nullptr, nullptr, nullptr, &Out);
\t};'''
new = '''\tauto RunFrom = [](UProfileSaveGame* Save, FProfessionStateOut& Out,
\t\tFProfileLoadError& OutError) -> bool
\t{
\t\tFProfileSnapshot ScratchSnapshot;
\t\tFInventoryModel ScratchInventory;
\t\tTArray<FPendingReward> ScratchPending;
\t\tTSet<uint64> ScratchApplied;
\t\tTMap<EItemSlot, FGuid> ScratchEquipped;
\t\treturn FProfileSerializer::FromSaveGame(Save, ScratchSnapshot, ScratchInventory,
\t\t\tScratchPending, ScratchApplied, ScratchEquipped, OutError,
\t\t\tnullptr, nullptr, nullptr, nullptr, &Out);
\t};'''
assert old in ts, 'RunFrom lambda not found'
ts = ts.replace(old, new, 1)

# 2d) rewrite the five blocks to pass one shared Error per block
old_dup = '''\t\tFProfessionStateOut Out;
\t\tTestFalse(TEXT("a duplicate rank row is refused"), RunFrom(Save, Out));
\t\tFProfileLoadError Error;
\t\tRunFrom(Save, Out);
\t\tTestEqual(TEXT("duplicate rank refused"), Error.Code, EProfileLoadError::DuplicateProfessionState);'''
new_dup = '''\t\tFProfessionStateOut Out;
\t\tFProfileLoadError Error;
\t\tTestFalse(TEXT("a duplicate rank row is refused"), RunFrom(Save, Out, Error));
\t\tTestEqual(TEXT("duplicate rank refused"), Error.Code, EProfileLoadError::DuplicateProfessionState);'''
assert old_dup in ts, 'dup block not found'
ts = ts.replace(old_dup, new_dup, 1)

old_range = '''\t\tFProfessionStateOut Out;
\t\tTestFalse(TEXT("a rank outside 1..8 is refused"), RunFrom(Save, Out));
\t\tFProfileLoadError Error;
\t\tRunFrom(Save, Out);
\t\tTestEqual(TEXT("rank range refused"), Error.Code, EProfileLoadError::InvalidProfessionState);'''
new_range = '''\t\tFProfessionStateOut Out;
\t\tFProfileLoadError Error;
\t\tTestFalse(TEXT("a rank outside 1..8 is refused"), RunFrom(Save, Out, Error));
\t\tTestEqual(TEXT("rank range refused"), Error.Code, EProfileLoadError::InvalidProfessionState);'''
assert old_range in ts, 'range block not found'
ts = ts.replace(old_range, new_range, 1)

old_dang = '''\t\tFProfessionStateOut Out;
\t\tTestFalse(TEXT("a dangling loadout skill is refused"), RunFrom(Save, Out));
\t\tFProfileLoadError Error;
\t\tRunFrom(Save, Out);
\t\tTestEqual(TEXT("dangling loadout refused"), Error.Code, EProfileLoadError::DanglingLoadoutSkill);'''
new_dang = '''\t\tFProfessionStateOut Out;
\t\tFProfileLoadError Error;
\t\tTestFalse(TEXT("a dangling loadout skill is refused"), RunFrom(Save, Out, Error));
\t\tTestEqual(TEXT("dangling loadout refused"), Error.Code, EProfileLoadError::DanglingLoadoutSkill);'''
assert old_dang in ts, 'dangling block not found'
ts = ts.replace(old_dang, new_dang, 1)

old_sp = '''\t\tFProfessionStateOut Out;
\t\tTestFalse(TEXT("a negative UnspentSP is refused"), RunFrom(Save, Out));
\t\tFProfileLoadError Error;
\t\tRunFrom(Save, Out);
\t\tTestEqual(TEXT("negative SP refused"), Error.Code, EProfileLoadError::InvalidProfessionState);'''
new_sp = '''\t\tFProfessionStateOut Out;
\t\tFProfileLoadError Error;
\t\tTestFalse(TEXT("a negative UnspentSP is refused"), RunFrom(Save, Out, Error));
\t\tTestEqual(TEXT("negative SP refused"), Error.Code, EProfileLoadError::InvalidProfessionState);'''
assert old_sp in ts, 'sp block not found'
ts = ts.replace(old_sp, new_sp, 1)

old_valid = '''\t\tUProfileSaveGame* Save = MakeValidV4(nullptr);
\t\tFProfessionStateOut Out;
\t\tTestTrue(TEXT("a valid v4 payload loads"), RunFrom(Save, Out));'''
new_valid = '''\t\tUProfileSaveGame* Save = MakeValidV4(nullptr);
\t\tFProfessionStateOut Out;
\t\tFProfileLoadError ValidError;
\t\tTestTrue(TEXT("a valid v4 payload loads"), RunFrom(Save, Out, ValidError));'''
assert old_valid in ts, 'valid block not found'
ts = ts.replace(old_valid, new_valid, 1)

# 2e) SetStorage fixes
old = '''\tUProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
\tTestTrue(TEXT("the character save service initializes on the derived prefix"),
\t\tCharService->Initialize(Created.SavePrefix));
\tconst FProfileLoadOutcome CharLoad = CharService->LoadActiveProfile();'''
new = '''\tUProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
\tTestTrue(TEXT("the character save service initializes on the derived prefix"),
\t\tCharService->Initialize(Created.SavePrefix));
\tCharService->SetStorage(&Storage);
\tconst FProfileLoadOutcome CharLoad = CharService->LoadActiveProfile();'''
assert old in ts, 'create-load SetStorage anchor not found'
ts = ts.replace(old, new, 1)

old = '''\tTestTrue(TEXT("service A initializes"), ServiceA->Initialize(First.SavePrefix));
\tconst FProfileLoadOutcome LoadA = ServiceA->LoadActiveProfile();'''
new = '''\tTestTrue(TEXT("service A initializes"), ServiceA->Initialize(First.SavePrefix));
\tServiceA->SetStorage(&Storage);
\tconst FProfileLoadOutcome LoadA = ServiceA->LoadActiveProfile();'''
assert old in ts, 'service A anchor not found'
ts = ts.replace(old, new, 1)
old = '''\tTestTrue(TEXT("service B initializes"), ServiceB->Initialize(Second.SavePrefix));
\tconst FProfileLoadOutcome LoadB = ServiceB->LoadActiveProfile();'''
new = '''\tTestTrue(TEXT("service B initializes"), ServiceB->Initialize(Second.SavePrefix));
\tServiceB->SetStorage(&Storage);
\tconst FProfileLoadOutcome LoadB = ServiceB->LoadActiveProfile();'''
assert old in ts, 'isolation SetStorage anchor not found'
ts = ts.replace(old, new, 1)

old = '''\tUProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
\tTestTrue(TEXT("the imported character's service initializes"), CharService->Initialize(Import.SavePrefix));
\tconst FProfileLoadOutcome CharLoad = CharService->LoadActiveProfile();'''
new = '''\tUProfileSaveService* CharService = NewObject<UProfileSaveService>(GetTransientPackage());
\tTestTrue(TEXT("the imported character's service initializes"), CharService->Initialize(Import.SavePrefix));
\tCharService->SetStorage(&RosterStorage);
\tconst FProfileLoadOutcome CharLoad = CharService->LoadActiveProfile();'''
assert old in ts, 'import SetStorage anchor not found'
ts = ts.replace(old, new, 1)

t.write_text(ts, encoding='utf-8')
print('2) test file fixes applied')
