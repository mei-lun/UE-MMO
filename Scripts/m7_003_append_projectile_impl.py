# -*- coding: utf-8 -*-
"""M7-003: append the skill projectile implementation to the character cpp."""
from pathlib import Path

p = Path('Source/UEMMO/PrototypeCharacter.cpp')
s = p.read_text(encoding='utf-8')
assert 'HandleSkillProjectileActivated(const FSkillActivationOutcome& Outcome)\n{' not in s, 'already appended'

impl = '''

// -- M7-003: the skill projectile path (throwsand) ------------------------------

void APrototypeCharacter::HandleSkillProjectileActivated(const FSkillActivationOutcome& Outcome)
{
    UWorld* World = GetWorld();
    if (World == nullptr || SkillLoadout == nullptr
        || SkillLoadout->GetSkillPolicy(Outcome.SkillId) != EProfessionSkillEffectPolicy::Projectile)
    {
        return;
    }
    // The sand projectile definition loads through the SAME strict parser the
    // combat catalog build uses (Data/CombatSystem/projectiles.json); a
    // missing/invalid row refuses the cast with zero side effects (the MP and
    // the cooldown were already paid - the cast animation still plays, only
    // the delivery is absent, which the missing-definition log makes visible).
    static const FString ProjectilesPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/CombatSystem/projectiles.json"));
    FString Raw;
    if (!FFileHelper::LoadFileToString(Raw, *ProjectilesPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: projectiles.json unreadable; the sand cast delivers nothing."));
        return;
    }
    TSharedPtr<FCombatJsonValue> ParsedRoot;
    FString ParseError;
    if (!FCombatDataTableParser::Parse(Raw, ParsedRoot, ParseError) || !ParsedRoot.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: projectiles.json parse failed (%s)."), *ParseError);
        return;
    }
    const TArray<TSharedPtr<FCombatJsonValue>>* Rows = nullptr;
    if (!ParsedRoot->TryGetArray(TEXT("rows"), Rows) || Rows == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: projectiles.json has no rows array."));
        return;
    }
    FProjectileDefinition Definition;
    bool bFound = false;
    for (const TSharedPtr<FCombatJsonValue>& RowValue : *Rows)
    {
        FString RowText;
        if (!RowValue.IsValid() || !FCombatJsonWriter::Write(RowValue, RowText))
        {
            continue;
        }
        FProjectileDefinition Candidate;
        FString RowError;
        if (FCombatDataTableParser::ParseProjectileDefinition(RowText, Candidate, RowError)
            && Candidate.ProjectileId == FName(TEXT("sand_throw")))
        {
            Definition = Candidate;
            bFound = true;
            break;
        }
    }
    if (!bFound)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: no sand_throw projectile row; the sand cast delivers nothing."));
        return;
    }

    // The per-attacker registry pair (the M5-013 placement): the owner enters
    // once per registry lifetime, every cast allocates its own shot id.
    if (!SkillProjectileIdentity.IsValid())
    {
        SkillProjectileIdentity = MakeUnique<FSkillProjectileIdentity>(this, &SkillProjectileRegistry);
    }
    if (!IsValidCombatEntityId(SkillProjectileSourceEntityId))
    {
        FCombatEntityMetadata SourceMetadata;
        SourceMetadata.Faction = M5_033_PlayerFaction;
        SourceMetadata.Category = M5_033_PlayerCategory;
        SkillProjectileSourceEntityId = SkillProjectileRegistry.RegisterEntity(
            this, SourceMetadata, SkillProjectileRegistry.GetCurrentEpoch());
    }
    if (!IsValidCombatEntityId(SkillProjectileSourceEntityId))
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: the skill registry refused the owner; the sand cast delivers nothing."));
        return;
    }
    const FCombatEpoch Epoch = SkillProjectileRegistry.GetCurrentEpoch();
    const FShotId ShotId = SkillProjectileRegistry.AllocateActionSequence(SkillProjectileSourceEntityId, Epoch);
    if (!IsValidCombatShotId(ShotId))
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: the skill registry refused the shot; the sand cast delivers nothing."));
        return;
    }

    // The spawn mirrors the weapon pellet setup (the same actor, motion
    // policy and unified entry): origin at a chest-height forward offset,
    // direction = the planar facing, speed/gravity/lifetime from the row.
    const FVector Origin = (Combat != nullptr ? Combat->GetDebugFeetLocation() : GetActorLocation())
        + FVector(60.f, 0.f, 80.f);
    const float Facing = (GetActorRotation().Yaw > -90.0f && GetActorRotation().Yaw < 90.0f) ? 1.f : -1.f;
    const FVector Direction = FVector(Facing, 0.f, 0.f);
    FActorSpawnParameters Params;
    Params.Owner = this;
    ACombatProjectile* Actor = World->SpawnActor<ACombatProjectile>(
        ACombatProjectile::StaticClass(), Origin, FRotator::ZeroRotator, Params);
    if (Actor == nullptr)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: the sand pellet actor failed to spawn."));
        return;
    }
    Actor->Context.Epoch = Epoch;
    Actor->Context.SourceEntityId = SkillProjectileSourceEntityId;
    Actor->Context.ShotId = ShotId;
    Actor->Context.PelletIndex = 0;
    Actor->Context.WeaponInstanceId = FGuid();
    Actor->Context.ProjectileInstanceId = FGuid::NewGuid();
    Actor->Context.ConfigRevision = SkillLoadout->GetConfigRevision();
    Actor->Context.ProjectileId = Definition.ProjectileId;
    Actor->LaunchSpeedCmS = Definition.SpeedCmS;
    Actor->MotionGravityScale = Definition.Motion == EProjectileMotion::Parabolic ? 1.0f : 0.0f;
    if (Actor->Movement != nullptr)
    {
        Actor->Movement->InitialSpeed = Definition.SpeedCmS;
        Actor->Movement->MaxSpeed = Definition.SpeedCmS;
        Actor->Movement->ProjectileGravityScale = Actor->MotionGravityScale;
    }
    Actor->SetLifeSpan(Definition.LifetimeS);
    Actor->FaceFlightDirection(Direction);

    // The damage profile: the attack row's base scaled by the rank
    // coefficient (the skill's damage_numbers are the attack's, retuned per
    // rank), carried verbatim by the pellet (the policy never re-reads data).
    FProjectileHitContext HitContext;
    HitContext.Registry = &SkillProjectileRegistry;
    HitContext.Ledger = &SkillProjectileLedger;
    HitContext.Identity = SkillProjectileIdentity.Get();
    HitContext.AttackProfile.DamageProfileId = Outcome.AttackId;
    HitContext.AttackProfile.BaseDamage = 14.f * Outcome.DamageCoefficient;
    HitContext.AttackProfile.AttackCoefficient = 1.0f;
    HitContext.AttackProfile.HitStunSeconds = 0.22f;
    HitContext.PierceCount = Definition.PierceCount;
    HitContext.SourceActor = this;

    FLiveWeaponProjectile Entry;
    Entry.Actor = Actor;
    Entry.Policy = MakeUnique<FLinearProjectilePolicy>();
    Entry.ShotEpoch = Epoch;
    Entry.ShotSourceEntityId = SkillProjectileSourceEntityId;
    Entry.ShotId = ShotId;
    if (Entry.Policy.IsValid() && Entry.Policy->Begin(*Actor, Direction, HitContext))
    {
        LiveSkillProjectiles.Add(MoveTemp(Entry));
        FPendingSkillShotEnd Pending;
        Pending.Epoch = Epoch;
        Pending.SourceEntityId = SkillProjectileSourceEntityId;
        Pending.ShotId = ShotId;
        Pending.LivePellets = 1;
        PendingSkillShotEnds.Add(Pending);
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M7-003: sand pellet committed (shot %llu, speed %.0f)."),
            static_cast<uint64>(ShotId), Definition.SpeedCmS);
    }
    else
    {
        Actor->Destroy();
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-003: the sand pellet policy refused its binding; actor retired."));
    }
}

void APrototypeCharacter::TickSkillProjectiles(float DeltaSeconds)
{
    for (int32 Index = LiveSkillProjectiles.Num() - 1; Index >= 0; --Index)
    {
        FLiveWeaponProjectile& Entry = LiveSkillProjectiles[Index];
        ACombatProjectile* Actor = Entry.Actor.Get();
        if (Actor == nullptr)
        {
            RetireSkillProjectilePellet(Index);
            continue;
        }
        Entry.Policy->AdvanceMotion(DeltaSeconds);
    }
}

void APrototypeCharacter::RetireSkillProjectilePellet(int32 LiveIndex)
{
    if (!LiveSkillProjectiles.IsValidIndex(LiveIndex))
    {
        return;
    }
    const FCombatEpoch PelletEpoch = LiveSkillProjectiles[LiveIndex].ShotEpoch;
    const FEntityId PelletSource = LiveSkillProjectiles[LiveIndex].ShotSourceEntityId;
    const FShotId PelletShotId = LiveSkillProjectiles[LiveIndex].ShotId;
    LiveSkillProjectiles.RemoveAtSwap(LiveIndex);

    for (int32 PendingIndex = PendingSkillShotEnds.Num() - 1; PendingIndex >= 0; --PendingIndex)
    {
        FPendingSkillShotEnd& Pending = PendingSkillShotEnds[PendingIndex];
        if (Pending.Epoch != PelletEpoch || Pending.SourceEntityId != PelletSource || Pending.ShotId != PelletShotId)
        {
            continue;
        }
        --Pending.LivePellets;
        if (Pending.LivePellets <= 0)
        {
            SkillProjectileLedger.EndShot(Pending.Epoch, Pending.SourceEntityId, Pending.ShotId);
            PendingSkillShotEnds.RemoveAtSwap(PendingIndex);
        }
        break;
    }
}
'''
s = s.rstrip() + impl
p.write_text(s, encoding='utf-8')
print('appended', len(impl), 'chars')
