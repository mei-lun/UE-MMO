#include "PrototypeCharacter.h"
#include "Animation/AnimInstance.h"
#include "Character/AttackMovementGate.h"
#include "Character/SideViewCameraComponent.h"
#include "Combat/AttackCatalog.h"
#include "Combat/CombatComponent.h"
#include "Combat/CombatPresentationComponent.h"
#include "Combat/HealthComponent.h"
#include "Combat/Data/CombatDataTableParser.h"
#include "Combat/System/TestRoomConfigDriver.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Items/ItemInstance.h"
#include "Logging/OperationLogSubsystem.h"
#include "PrototypeHUD.h"
#include "Profile/ProfileSubsystem.h"
#include "Room/RoomSessionSubsystem.h"
#include "Room/TrainingResetService.h"
#include "UObject/ConstructorHelpers.h"
#include "Weapons/WeaponComponent.h"

using namespace UE::UEMMO::Tasks::M1_029;

namespace
{
    /**
     * M3-023: logs one input row per DIRECTION change of a planar move axis
     * (any release, press or sign flip). The continuous axis value is never
     * logged per frame (the card's volume rule); the direction is the only
     * discrete fact a keyboard axis produces. LastDirection keeps the last
     * LOGGED direction (0 = released); same-direction magnitude changes are
     * silently absorbed.
     */
    void M3_023_LogMoveAxisChange(APrototypeCharacter& Character, float& LastDirection, float NewValue,
        const TCHAR* AxisLetter, const TCHAR* PositiveKey, const TCHAR* NegativeKey)
    {
        const bool bWasActive = LastDirection != 0.0f;
        const bool bIsActive = NewValue != 0.0f;
        const bool bFlipped = bWasActive && bIsActive && ((LastDirection > 0.0f) != (NewValue > 0.0f));
        if (bWasActive == bIsActive && !bFlipped)
        {
            return;
        }
        UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(&Character);
        if (OpLog == nullptr)
        {
            LastDirection = NewValue;
            return;
        }
        if (!bIsActive)
        {
            OpLog->LogInput(FString::Printf(TEXT("Move %s released"), AxisLetter));
        }
        else
        {
            OpLog->LogInput(FString::Printf(TEXT("Move %s %s (%s)"),
                AxisLetter, NewValue > 0.0f ? TEXT("+") : TEXT("-"),
                NewValue > 0.0f ? PositiveKey : NegativeKey));
        }
        LastDirection = NewValue;
    }

    /**
     * Single choke point for all planar movement: every frame the accumulated
     * axis state is converted into movement input and facing here. M1-013: the
     * combat gate plugs in exactly here (CanAcceptMovement/CanTurn via
     * AttackMovementGate): an in-flight attack or death scales the active
     * planar input to zero and locks facing. Nothing else in this class feeds
     * movement input, and the movement component itself is never disabled, so
     * external impulses (LaunchCharacter, knockback) still apply.
     */
    void ApplyPlanarMovement(APrototypeCharacter& Character, const FPlanarAxisState& Axes)
    {
        if (UE::UEMMO::Tasks::M1_013::ComputeAllowedMoveScale(Character.GetCombat()) > 0.0f)
        {
            const FVector PlanarVelocity = ComputePlanarVelocity(Axes);
            UCharacterMovementComponent* Movement = Character.GetCharacterMovement();
            const float Speed = PlanarVelocity.Size2D();
            if (Speed > UE_SMALL_NUMBER && Movement && Movement->MaxWalkSpeed > UE_SMALL_NUMBER)
            {
                // Normalized direction plus an input scale of speed / MaxWalkSpeed makes
                // CharacterMovement clamp its walk speed to the per-axis planar speed
                // (analog input modifier), keeping normal acceleration, braking and collision.
                Character.AddMovementInput(PlanarVelocity / Speed, Speed / Movement->MaxWalkSpeed);
            }
        }
        // M1-013: facing stays locked while attacking or dead, so reversed input
        // neither flips the locked facing nor swings the camera. Only horizontal
        // input changes facing otherwise; depth input (the Up/Down arrows)
        // never flips it.
        if (UE::UEMMO::Tasks::M1_013::CanFlipFacing(Character.GetCombat()))
        {
            if (Axes.AxisX > 0.0f)
            {
                Character.SetActorRotation(FRotator(0.f, 0.f, 0.f));
            }
            else if (Axes.AxisX < 0.0f)
            {
                Character.SetActorRotation(FRotator(0.f, 180.f, 0.f));
            }
        }
    }
}

APrototypeCharacter::APrototypeCharacter()
{
    PrimaryActorTick.bCanEverTick = true;
    GetCapsuleComponent()->InitCapsuleSize(36.f, 96.f);
    bUseControllerRotationYaw = false;
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->bConstrainToPlane = false;
    GetCharacterMovement()->MaxWalkSpeed = 420.f;
    GetCharacterMovement()->JumpZVelocity = 620.f;
    GetCharacterMovement()->GravityScale = 1.5f;
    GetCharacterMovement()->AirControl = 0.35f;
    GetCharacterMovement()->BrakingDecelerationWalking = 2400.f;
    GetMesh()->SetRelativeLocation(FVector(0, 0, -96));
    GetMesh()->SetRelativeRotation(FRotator(0, -90, 0));
    GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<USkeletalMesh> MeshAsset(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
    if (MeshAsset.Succeeded()) GetMesh()->SetSkeletalMesh(MeshAsset.Object);
    // M1-031: the generated AnimBP owns all animation state (locomotion
    // BlendSpace plus jump/fall/land poses). The M0 per-frame PlayAnimation
    // override is gone, so idle/move switching never restarts from time zero.
    static ConstructorHelpers::FClassFinder<UAnimInstance> AnimBPClass(TEXT("/Game/UEMMO/Animation/ABP_Prototype.ABP_Prototype_C"));
    if (AnimBPClass.Succeeded())
    {
        GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
        GetMesh()->SetAnimInstanceClass(AnimBPClass.Class);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO: /Game/UEMMO/Animation/ABP_Prototype missing; run Scripts/Editor/create_locomotion_assets.py."));
    }
    // M1-030: the fixed side-view framing (yaw -90, pitch -18, FOV 55, arm
    // 1700 cm) and the ground-anchor follow moved into SideViewCameraComponent;
    // those values are preserved as the component's defaults.
    CameraRig = CreateDefaultSubobject<USideViewCameraComponent>(TEXT("SideViewCameraRig"));
    CameraRig->SetupAttachment(RootComponent);
    // M1-012: combat lifecycle component (M1-011); combat intents buffer here.
    Combat = CreateDefaultSubobject<UCombatComponent>(TEXT("Combat"));
    // M2-004: the pawn's own health pool (MaxHP 100, the HealthComponent
    // default). Owning it from spawn makes the pawn a full combatant: the
    // M1-018 target query selects it (the M2-003 enemy attacks land real
    // damage), the M1-020 victim-side stun path applies, and its death
    // lifecycle drives the PlayerDied broadcast (BeginPlay wiring).
    Health = CreateDefaultSubobject<UHealthComponent>(TEXT("PlayerHealth"));
    // M1-032: attack montage playback owner; sources are injected in BeginPlay
    // (after the catalog is attached) because the presenter needs the exact
    // UCombatComponent instance this pawn ticks.
    CombatPresentation = CreateDefaultSubobject<UCombatPresentationComponent>(TEXT("CombatPresentation"));
    // M5-020: the production weapon mount (session binding registry + ammo
    // model + the catalogs mounted in BeginPlay).
    WeaponMount = CreateDefaultSubobject<UWeaponComponent>(TEXT("WeaponMount"));
}

void APrototypeCharacter::BeginPlay()
{
    Super::BeginPlay();
    SpawnLocation = GetActorLocation();
    // M1-027: the unified reset restores this facing alongside the spawn
    // position (interface contract section 6: the reset covers facing).
    SpawnRotation = GetActorRotation();
    // M1-012 catalog wiring point: build the read-only catalog once from the
    // DefaultGame.ini references (the same loading path M1-010/M1-011 use) and
    // inject it into the combat component. On failure combat intents still
    // buffer; TryStartAttack then rejects every id with its own diagnostic.
    UAttackCatalog* Catalog = NewObject<UAttackCatalog>(this);
    FText CatalogError;
    if (Catalog->InitializeFromConfig(CatalogError))
    {
        Combat->InitializeFromCatalog(Catalog);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO: attack catalog unavailable (%s); combat intents buffer without one."), *CatalogError.ToString());
    }
    // M1-032: hand attack playback ownership to the presentation component: it
    // follows Combat's Started/Finished events and re-syncs from the snapshot
    // every tick (Reset/interrupt paths stop the montage and return to the
    // locomotion AnimBP). Null-safe on both arguments by contract.
    CombatPresentation->SetSources(Combat, GetMesh());
    // M1-023: the combat component cannot jump itself; both the Free-state
    // jump and the launcher jump-cancel consume a buffered jump intent into
    // this handler, which performs the real ACharacter::Jump synchronously in
    // the consume path. The component is owned by this pawn, so the raw this
    // capture never outlives the handler.
    Combat->SetJumpRequestHandler([this]()
    {
        Jump();
    });
    // M1-024: the combat component routes a buffered Light by the air state -
    // an airborne J starts aerial_01, a grounded J keeps light_01. The
    // component owns no movement, so the owner binds its own airborne
    // predicate (the movement component's IsFalling, the check the engine
    // derives the falling state from); the component is owned by this pawn,
    // so the raw this capture never outlives the handler.
    Combat->SetAirStateProvider([this]()
    {
        const UCharacterMovementComponent* Movement = GetCharacterMovement();
        return Movement != nullptr && Movement->IsFalling();
    });
    // M1-041: the facing source of the input-driven attack start (the last
    // un-wired component input, M1-H01's "facing passes 0"). M1-029's planar
    // flip writes only yaw 0 (right) or yaw 180 (left) into the actor and
    // locks it while attacking, so the yaw half-plane is exactly the facing
    // the movement code last locked in; right = +1, left = -1 (the
    // TryStartAttack mirror convention). The component is owned by this pawn,
    // so the raw this capture never outlives the handler.
    Combat->SetFacingProvider([this]()
    {
        const float Yaw = GetActorRotation().Yaw;
        return (Yaw > -90.0f && Yaw < 90.0f) ? 1 : -1;
    });
    // M2-004: the death wiring. The health pool fires OnDied exactly once per
    // death lifecycle (HealthComponent contract); the handler marks the
    // combat component dead, stops the current attack and animation, and
    // broadcasts PlayerDied exactly once per lifecycle. Health and Combat
    // live and die with this pawn (subobjects), so the captured lambda can
    // never dangle (the AMeleeEnemy M2-002 precedent).
    if (Health != nullptr)
    {
        Health->OnDied.AddLambda([this]()
        {
            HandlePlayerDied();
        });
    }
    // M3-010: the growth wiring. The profile subsystem is resolved exactly
    // once here through the owning game instance (the subsystem outlives the
    // pawn, so a weak reference suffices); a GameInstance-less pawn - a bare
    // test world - keeps the component defaults and an inert equip entry, the
    // documented graceful degradation. The snapshot's final stats load
    // immediately: MaxHP onto the health pool bound (the no-heal clamp) and
    // Attack/Defense into the damage formula entries. The pawn's first load
    // (spawn / entering the dungeon) opens the pool FULL at the new max, the
    // same entry a new run uses; ordinary equipment changes never heal.
    if (UWorld* World = GetWorld())
    {
        if (UGameInstance* GameInstance = World->GetGameInstance())
        {
            ProfilePtr = GameInstance->GetSubsystem<UProfileSubsystem>();
        }
    }
    ApplyProfileFinalStats();
    if (Health != nullptr)
    {
        Health->ResetHealth();
    }
    // M5-020: the equipment load point (interface contract section 7: the
    // WeaponComponent re-binds at pawn BeginPlay / map travel). Resolves the
    // equipped weapon slot through the local HUD's real equipment model; a
    // bare world (no HUD/profile) skips silently - an unbound mount, never a
    // fake bind.
    RefreshWeaponMountFromEquipment();
    // M3-010: the run-start hook. One accepted StartRoom (the "enter the
    // dungeon" moment, also the re-entry after LeaveRoom) re-loads the fresh
    // snapshot and restores the pool to the CURRENT max - a new run always
    // opens full. The subsystem is a World subsystem that already exists at
    // pawn BeginPlay; the captured raw this dies with the pawn, and the
    // subsystem only broadcasts while its world lives, so the binding can
    // never dangle (the M2-004 death-binding precedent).
    if (UWorld* World = GetWorld())
    {
        URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
        if (Session != nullptr)
        {
            RoomSessionPtr = Session;
            Session->OnRunStarted().AddLambda([this]()
            {
                HandleRoomRunStarted();
            });
        }
    }
    // M3-023: register this pawn with the operation log subsystem (the
    // GameInstance-level log auto-subscribes the combat Started/Finished/
    // HitConfirmed events and this world's room run events). Without a game
    // instance (bare test worlds) there is no log subsystem and the pawn runs
    // unlogged, the same graceful degradation as the profile wiring above.
    if (UGameInstance* GameInstance = GetGameInstance())
    {
        if (UOperationLogSubsystem* OpLog = GameInstance->GetSubsystem<UOperationLogSubsystem>())
        {
            OpLog->RegisterPlayer(this);
        }
    }
    UE_LOG(LogTemp, Display, TEXT("UEMMO: prototype character ready; X/Y movement enabled."));
}

void APrototypeCharacter::HandlePlayerDied()
{
    // Exactly once per death lifecycle: the health pool fires OnDied once per
    // lifecycle, and this guard additionally absorbs any stray repeat, so
    // PlayerDied stays singular until the unified reset reopens the lifecycle.
    if (bPlayerDiedBroadcast)
    {
        return;
    }
    bPlayerDiedBroadcast = true;
    // Death stops control (interface contract section 4: death has the
    // highest priority): the dead flag refuses every new attack and the
    // movement gate (M1-013 CanAcceptMovement) reads false. SetDead alone
    // leaves an in-flight attack running by its M1-011 contract, so the
    // running instance is cancelled explicitly (idempotent; no Finished
    // event - a death cancel is an interruption).
    if (Combat != nullptr)
    {
        Combat->SetDead(true);
        Combat->CancelCurrentAttack(FName(TEXT("PlayerDied")));
    }
    // M5-020: the death teardown of the weapon mount - the open reload window
    // closes without transferring rounds and pending fire/reload callbacks
    // lose their effect (generation bump). The binding identity survives: the
    // unified reset re-applies the equipment and restores the parked magazine.
    if (WeaponMount != nullptr)
    {
        WeaponMount->NotifyOwnerDied();
    }
    // Minimal death presentation: drop any running montage and detach the
    // locomotion AnimBP so the death pose is not overridden by idle/walk
    // blending (the mesh rests in its last pose). The unified reset
    // re-attaches the captured class (SavedAnimInstanceClass).
    if (USkeletalMeshComponent* MeshComponent = GetMesh())
    {
        if (UAnimInstance* Anim = MeshComponent->GetAnimInstance())
        {
            Anim->Montage_Stop(0.0f);
            SavedAnimInstanceClass = Anim->GetClass();
        }
        MeshComponent->SetAnimInstanceClass(nullptr);
    }
    // Broadcast last, so every observer reads the post-death state (combat
    // dead, animation detached).
    PlayerDied.Broadcast();
}

bool APrototypeCharacter::TryEquipStatBonus(const FItemStats& NewEquippedBonus)
{
    // M3-010: the player-side equip/unequip entry. Without a profile there is
    // nothing to write into: refused (the future new-game flow creates the
    // profile first).
    UProfileSubsystem* Profile = ProfilePtr.Get();
    if (Profile == nullptr || !Profile->HasProfile())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UEMMO: equip request refused - no character profile exists yet."));
        return false;
    }
    // The card's first-version rule: equipping is a non-combat operation, so a
    // Running room run refuses every equip/unequip request ("exit the room
    // first"). The gate sits exactly here, BEFORE the profile row is written,
    // so a refused request cannot even half-apply. No session (or an
    // unresolved one) means no run can be Running: the gate passes.
    const URoomSessionSubsystem* Session = RoomSessionPtr.Get();
    if (Session != nullptr && Session->GetState() == ERoomSessionState::Running)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UEMMO: equip request refused - the room run is Running; leave the room before changing equipment."));
        return false;
    }
    // Accepted: the row REPLACES the stored equipped bonus wholesale (the
    // M3-005 SetEquippedStatBonus semantics; a zero row means "nothing
    // equipped") and the fresh snapshot re-applies immediately with the
    // no-heal clamp - an equipment change NEVER restores lost health.
    Profile->SetEquippedStatBonus(NewEquippedBonus);
    ApplyProfileFinalStats();
    // M5-020: the equipment-changed hook. This entry is the production
    // equip/unequip push (the HUD flow calls it after the slot mapping moved),
    // so every accepted change re-loads the weapon mount from the real
    // equipment model. The Running gate above already guaranteed the
    // no-combat-unequip rule for both the stats and the mount. A refused
    // re-load logs its named reason and leaves the previous binding torn down
    // or kept exactly as the component decides - never a silent half state.
    RefreshWeaponMountFromEquipment();
    return true;
}

bool APrototypeCharacter::EnsureWeaponCatalogsMounted()
{
    if (bWeaponCatalogsBuilt)
    {
        return bWeaponCatalogsMounted;
    }
    bWeaponCatalogsBuilt = true;

    // The combat catalog: the six Data/CombatSystem source tables through the
    // M5-005 loader, adapted into the M5-007 candidate struct (the M5-009
    // recomputation pattern) and built once per pawn. The cross-table
    // reference gate is deliberately NOT re-run here: it is M5-006's release
    // gate against the shipped sample-table gap (a recorded divergence), so
    // the mount gate is the strict per-table parse plus the builder's own
    // integrity; a per-weapon ammo-kind check happens at bind time instead.
    FCombatDataTableSet Tables;
    TArray<FString> LoadProblems;
    if (!LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), Tables, LoadProblems))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UEMMO M5-020: the combat source tables are unavailable (%s); weapon firing stays explicitly disabled."),
            *FString::Join(LoadProblems, TEXT(" | ")));
        return false;
    }
    FParsedCombatConfig Parsed;
    Parsed.SchemaVersion = 1;
    for (const TPair<FName, FDamageProfile>& Row : Tables.DamageProfiles)
    {
        Parsed.DamageProfiles.Add(Row.Value);
    }
    for (const TPair<FName, FAttackReaction>& Row : Tables.AttackReactions)
    {
        Parsed.AttackReactions.Add(Row.Value);
    }
    for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions)
    {
        Parsed.TargetReactions.Add(Row.Value);
    }
    for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons)
    {
        Parsed.Weapons.Add(Row.Value);
    }
    for (const TPair<FName, FAmmoType>& Row : Tables.AmmoTypes)
    {
        Parsed.AmmoTypes.Add(Row.Value);
    }
    for (const TPair<FName, FProjectileDefinition>& Row : Tables.Projectiles)
    {
        Parsed.Projectiles.Add(Row.Value);
    }
    FString BuildErrors;
    if (!FCombatCatalog::BuildFromParsed(Parsed, WeaponCatalogValue, BuildErrors))
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UEMMO M5-020: the combat catalog build refused (%s); weapon firing stays explicitly disabled."), *BuildErrors);
        return false;
    }

    // The item catalog: the production items source (the M5-020A HUD
    // precedent). No legacy staging fallback here on purpose: a missing or
    // unparsable source leaves the catalog EMPTY, so every bind refuses with
    // its named reason ("缺武器不是假成功") instead of binding a stand-in.
    TArray<FTestRoomItemRow> ItemRows;
    FString SourceError;
    if (ACombatTestRoomDriver::LoadProductionItems(ItemRows, SourceError))
    {
        for (const FTestRoomItemRow& Row : ItemRows)
        {
            FItemDefinition Definition;
            Definition.DefinitionId = Row.DefinitionId;
            Definition.DisplayName = Row.DisplayName;
            Definition.Slot = Row.Slot == TEXT("Weapon") ? EItemSlot::Weapon
                : Row.Slot == TEXT("Armor") ? EItemSlot::Armor
                : Row.Slot == TEXT("Accessory") ? EItemSlot::Accessory
                : EItemSlot::Weapon;
            Definition.BaseStats.Attack = Row.Attack;
            Definition.BaseStats.Defense = Row.Defense;
            Definition.BaseStats.MaxHP = Row.MaxHP;
            Definition.Rarity = static_cast<EItemRarity>(FMath::Clamp(Row.Rarity, 1, 3));
            // WeaponDefinitionId stays None: the shipped items source carries
            // no weapon mappings yet (the M5-019 report records the table
            // carrier of that field as future lineage work).
            FString AddError;
            if (!WeaponItemCatalogValue.AddDefinition(Definition, &AddError))
            {
                UE_LOG(LogTemp, Warning, TEXT("UEMMO M5-020: item definition '%s' refused (%s)."),
                    *Row.DefinitionId.ToString(), *AddError);
            }
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UEMMO M5-020: the items source is unavailable (%s); weapon binds refuse with their named reason."), *SourceError);
    }

    FString MountError;
    bWeaponCatalogsMounted = (WeaponMount != nullptr)
        && WeaponMount->MountCatalogs(&WeaponCatalogValue, &WeaponItemCatalogValue, &MountError);
    if (!bWeaponCatalogsMounted)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("UEMMO M5-020: the weapon catalog mount refused (%s); firing stays explicitly disabled."), *MountError);
    }
    return bWeaponCatalogsMounted;
}

bool APrototypeCharacter::RefreshWeaponMountFromEquipment()
{
    UWeaponComponent* Mount = GetWeaponMount();
    if (Mount == nullptr)
    {
        return false;
    }
    // The real equipment model lives on the local HUD (the M3-012 production
    // flow maps the slots there and pushes the stat row through
    // TryEquipStatBonus, whose accepted tail re-enters here). No HUD (bare
    // test worlds) or no profile: skip - an unbound mount, never a fake bind.
    const APlayerController* LocalController = Cast<APlayerController>(GetController());
    const APrototypeHUD* Hud = (LocalController != nullptr) ? Cast<APrototypeHUD>(LocalController->GetHUD()) : nullptr;
    UProfileSubsystem* Profile = ProfilePtr.Get();
    if (Hud == nullptr || Profile == nullptr || !Profile->HasProfile())
    {
        UE_LOG(LogTemp, Verbose, TEXT("UEMMO M5-020: weapon mount refresh skipped (no local HUD or profile yet)."));
        return false;
    }
    // The session catalogs (built from the production sources once; a failure
    // latches the mount's explicit disable state, so the apply below refuses
    // with its named reason instead of binding a fallback).
    EnsureWeaponCatalogsMounted();

    const FGuid* EquippedId = Hud->PeekInventoryEquipment().GetEquippedId(EItemSlot::Weapon);
    const FItemInstance* WeaponInstance = nullptr;
    if (EquippedId != nullptr)
    {
        for (const FItemInstance& Instance : Profile->GetInventory().GetAll())
        {
            if (Instance.InstanceId == *EquippedId)
            {
                WeaponInstance = &Instance;
                break;
            }
        }
        if (WeaponInstance == nullptr)
        {
            // A dangling slot mapping (the inventory lost the instance behind
            // the equipment's back): apply the explicit no-weapon state.
            UE_LOG(LogTemp, Warning,
                TEXT("UEMMO M5-020: the equipped weapon instance '%s' is not in the inventory; the mount applies no-weapon."),
                *EquippedId->ToString());
        }
    }
    const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(WeaponInstance);
    if (!Outcome.bSucceeded)
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO M5-020: the weapon mount refused the equipped item (reason %d): %s"),
            static_cast<int32>(Outcome.Reject), *Outcome.RejectDetail);
    }
    else
    {
        UE_LOG(LogTemp, Display, TEXT("UEMMO M5-020: weapon mount applied (bound=%d, revision=%s, generation=%llu)."),
            Outcome.bWeaponBound ? 1 : 0, *Mount->GetMountedRevision(), Mount->GetBindingGeneration());
    }
    return Outcome.bSucceeded;
}

void APrototypeCharacter::ApplyProfileFinalStats()
{
    // M3-010: re-applies the profile's final stats to the pawn's combat
    // attributes. No profile (or no subsystem): the component defaults stay.
    // The pool itself is only clamped (SetMaxHealth), never healed here - the
    // two explicit pool refills are the pawn's spawn load and the run-start
    // handler, both via ResetHealth.
    UProfileSubsystem* Profile = ProfilePtr.Get();
    if (Profile == nullptr || !Profile->HasProfile())
    {
        return;
    }
    const FProfileSnapshot Snapshot = Profile->GetProfileSnapshot();
    if (Health != nullptr)
    {
        // Lowered MaxHP clamps CurrentHP down; raised MaxHP never heals
        // (FStatCalculator::ClampHealthOnMaxChange semantics, M3-005).
        Health->SetMaxHealth(static_cast<float>(Snapshot.MaxHP));
    }
    if (Combat != nullptr)
    {
        Combat->SetCombatStats(static_cast<float>(Snapshot.Attack), static_cast<float>(Snapshot.Defense));
    }
}

void APrototypeCharacter::HandleRoomRunStarted()
{
    // M3-010: the accepted StartRoom is the "enter the dungeon" load point.
    // The fresh snapshot re-applies (equipment changed between runs is
    // reflected) and the pool restores to the CURRENT max - a new run always
    // opens full, while ordinary equipment changes never heal. Works without
    // a profile too: the load is a no-op and the unchanged max is refilled.
    ApplyProfileFinalStats();
    if (Health != nullptr)
    {
        Health->ResetHealth();
    }
}

void APrototypeCharacter::EnsureCombatInputActions()
{
    if (Mapping != nullptr)
    {
        // M1-012: actions and the mapping context are built exactly once per
        // pawn; a re-setup (re-possess) reuses them so nothing accumulates.
        return;
    }
    Mapping = NewObject<UInputMappingContext>(this);
    auto MakeAxis = [this](const TCHAR* Name, FKey Positive, FKey Negative)
    {
        UInputAction* Action = NewObject<UInputAction>(this, Name);
        Action->ValueType = EInputActionValueType::Axis1D;
        Action->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
        Mapping->MapKey(Action, Positive);
        FEnhancedActionKeyMapping& NegativeMapping = Mapping->MapKey(Action, Negative);
        NegativeMapping.Modifiers.Add(NewObject<UInputModifierNegate>(Mapping));
        return Action;
    };
    // M1-040: DNF movement keys. The planar sign convention is unchanged
    // (Right/Down positive, Left/Up negated), only the keys moved: the arrow
    // keys replace A/D (X) and W/S (Y depth); WASD became skill slots.
    HorizontalAction = MakeAxis(TEXT("MoveHorizontal"), EKeys::Right, EKeys::Left);
    DepthAction = MakeAxis(TEXT("MoveDepth"), EKeys::Down, EKeys::Up);
    JumpAction = NewObject<UInputAction>(this, TEXT("Jump"));
    ResetAction = NewObject<UInputAction>(this, TEXT("Reset"));
    // M1-040: C is the primary jump key; the Space alias is retained on the
    // same action to soften the remap (both feed the M1-023 buffered jump).
    Mapping->MapKey(JumpAction, EKeys::C);
    Mapping->MapKey(JumpAction, EKeys::SpaceBar);
    // M1-040: F2 resets; R left the reset binding for skill slot 4 (the WASD
    // skill-slot move removes the old R collision for free). M3-023: the
    // binding routes through the key-logging wrapper OnResetPressed, so only
    // real key presses log (fall-out-of-world recoveries stay unlogged).
    Mapping->MapKey(ResetAction, EKeys::F2);
    // M1-040: combat intents (X = Light, Z = Launcher; J/K removed without
    // alias). Runtime-created actions keep the M0 pattern: no uasset IMC/IA,
    // keys mapped in code. Only the Started event is bound, so holding a key
    // never repeats and releasing never enqueues.
    CombatLightAction = NewObject<UInputAction>(this, TEXT("CombatLight"));
    CombatLauncherAction = NewObject<UInputAction>(this, TEXT("CombatLauncher"));
    Mapping->MapKey(CombatLightAction, EKeys::X);
    Mapping->MapKey(CombatLauncherAction, EKeys::Z);
    // M1-040: the eight DNF skill-slot actions (Q W E R A S D F = slot 1..8),
    // same runtime-action pattern, created exactly once like every action.
    SkillSlotActions.Reset(8);
    for (int32 Slot = 1; Slot <= 8; ++Slot)
    {
        SkillSlotActions.Add(NewObject<UInputAction>(this, *FString::Printf(TEXT("SkillSlot%d"), Slot)));
    }
    // M1-040: the eight DNF skill slots in slot order (Q W E R A S D F = slot
    // 1..8). A/D/W/S/R left the movement/reset bindings and now only produce
    // SkillSlot intents (no combat effect yet, M2+ placeholder).
    const FKey SkillSlotKeys[8] = { EKeys::Q, EKeys::W, EKeys::E, EKeys::R, EKeys::A, EKeys::S, EKeys::D, EKeys::F };
    for (int32 Slot = 1; Slot <= 8; ++Slot)
    {
        Mapping->MapKey(SkillSlotActions[Slot - 1], SkillSlotKeys[Slot - 1]);
    }
    // M1-028: F1 toggles the HUD combat debug overlay. F1 collides with no
    // other mapping in the M1-040 DNF layout (arrows, X, Z, C, Space, F2 and
    // the Q/W/E/R/A/S/D/F skill slots), and the guard above keeps the
    // mapping and action single even on a re-setup.
    DebugToggleAction = NewObject<UInputAction>(this, TEXT("DebugToggle"));
    Mapping->MapKey(DebugToggleAction, EKeys::F1);
}

void APrototypeCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    UEnhancedInputComponent* Input = CastChecked<UEnhancedInputComponent>(PlayerInputComponent);
    // M1-012 duplicate-binding guard: EnhancedInput stacks a delegate per
    // BindAction call, so re-running setup against the same component would
    // double-trigger. Clearing this pawn's bindings first makes the bind block
    // below idempotent.
    Input->ClearBindingsForObject(this);
    EnsureCombatInputActions();
    Input->BindAction(HorizontalAction, ETriggerEvent::Triggered, this, &APrototypeCharacter::MoveHorizontal);
    Input->BindAction(HorizontalAction, ETriggerEvent::Completed, this, &APrototypeCharacter::MoveHorizontal);
    Input->BindAction(DepthAction, ETriggerEvent::Triggered, this, &APrototypeCharacter::MoveDepth);
    Input->BindAction(DepthAction, ETriggerEvent::Completed, this, &APrototypeCharacter::MoveDepth);
    Input->BindAction(JumpAction, ETriggerEvent::Started, this, &APrototypeCharacter::StartJump);
    Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &APrototypeCharacter::EndJump);
    Input->BindAction(ResetAction, ETriggerEvent::Started, this, &APrototypeCharacter::OnResetPressed);
    Input->BindAction(CombatLightAction, ETriggerEvent::Started, this, &APrototypeCharacter::OnCombatLightPressed);
    Input->BindAction(CombatLauncherAction, ETriggerEvent::Started, this, &APrototypeCharacter::OnCombatLauncherPressed);
    // M1-028: F1 routes to the HUD debug overlay toggle (Started only: one
    // press flips the flag exactly once).
    Input->BindAction(DebugToggleAction, ETriggerEvent::Started, this, &APrototypeCharacter::OnDebugTogglePressed);
    // M1-040: the eight DNF skill slots only record a per-slot press counter;
    // no combat effect yet (M2+ placeholder). Started only: one intent per
    // press, releasing never enqueues.
    Input->BindAction(SkillSlotActions[0], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot1Pressed);
    Input->BindAction(SkillSlotActions[1], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot2Pressed);
    Input->BindAction(SkillSlotActions[2], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot3Pressed);
    Input->BindAction(SkillSlotActions[3], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot4Pressed);
    Input->BindAction(SkillSlotActions[4], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot5Pressed);
    Input->BindAction(SkillSlotActions[5], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot6Pressed);
    Input->BindAction(SkillSlotActions[6], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot7Pressed);
    Input->BindAction(SkillSlotActions[7], ETriggerEvent::Started, this, &APrototypeCharacter::OnSkillSlot8Pressed);
    if (APlayerController* PC = Cast<APlayerController>(Controller))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            // M1-012: the context object is a per-pawn singleton (see
            // EnsureCombatInputActions); registering it twice is pointless, so
            // a re-setup never re-adds it.
            if (!Subsystem->HasMappingContext(Mapping))
            {
                Subsystem->AddMappingContext(Mapping, 0);
            }
        }
    }
}

void APrototypeCharacter::MoveHorizontal(const FInputActionValue& Value)
{
    // Enhanced Input already sums the mapped keys (D and negated A) into one net
    // axis; Triggered carries it while held and Completed carries 0 on release.
    // The value is only recorded here; movement is applied centrally in Tick.
    PlanarAxes.SetAxisX(Value.Get<float>());
    // M3-023: one input row per direction change (press/release/flip) of the
    // horizontal arrow keys (Right/Left); the continuous magnitude never logs.
    M3_023_LogMoveAxisChange(*this, LastLoggedMoveX, PlanarAxes.AxisX,
        TEXT("X"), TEXT("Right"), TEXT("Left"));
}
void APrototypeCharacter::MoveDepth(const FInputActionValue& Value)
{
    PlanarAxes.SetAxisY(Value.Get<float>());
    // M3-023: one input row per direction change of the depth arrow keys
    // (Down/Up); the continuous magnitude never logs.
    M3_023_LogMoveAxisChange(*this, LastLoggedMoveY, PlanarAxes.AxisY,
        TEXT("Y"), TEXT("Down"), TEXT("Up"));
}
void APrototypeCharacter::StartJump()
{
    // M1-023: a jump key (M1-040: C primary, Space alias) is a combat intent,
    // not a state bypass. The press is buffered like X/Z and the component's
    // state decides: Free consumes it
    // into the BeginPlay-bound jump request on the next combat tick (same
    // frame, M0 instant-jump experience), the launcher cancel window consumes
    // it into a jump cancel, everything else keeps it buffered until its
    // 150 ms lifetime expires. Without a combat component the M0 direct jump
    // stays.
    if (Combat != nullptr)
    {
        SubmitCombatInput(ECombatInput::Jump);
        return;
    }
    Jump();
}
void APrototypeCharacter::EndJump()
{
    // M3-023: the Completed event is the release half of the jump key
    // (C primary, Space alias); one input row per release.
    if (UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(this))
    {
        OpLog->LogInput(TEXT("Released C/Space (Jump)"));
    }
    StopJumping();
}
void APrototypeCharacter::OnCombatLightPressed() { SubmitCombatInput(ECombatInput::Light); }
void APrototypeCharacter::OnCombatLauncherPressed() { SubmitCombatInput(ECombatInput::Launcher); }
// M1-040: one trivial forwarder per slot keeps the member-pointer binding
// form (ClearBindingsForObject keeps covering every binding) while the slot
// identity travels as the literal the handler forwards.
void APrototypeCharacter::OnSkillSlot1Pressed() { SubmitSkillSlot(1); }
void APrototypeCharacter::OnSkillSlot2Pressed() { SubmitSkillSlot(2); }
void APrototypeCharacter::OnSkillSlot3Pressed() { SubmitSkillSlot(3); }
void APrototypeCharacter::OnSkillSlot4Pressed() { SubmitSkillSlot(4); }
void APrototypeCharacter::OnSkillSlot5Pressed() { SubmitSkillSlot(5); }
void APrototypeCharacter::OnSkillSlot6Pressed() { SubmitSkillSlot(6); }
void APrototypeCharacter::OnSkillSlot7Pressed() { SubmitSkillSlot(7); }
void APrototypeCharacter::OnSkillSlot8Pressed() { SubmitSkillSlot(8); }
void APrototypeCharacter::SubmitSkillSlot(int32 SlotIndex)
{
    // M1-040: the DNF skill slots have no combat effect yet (M2+ owns skill
    // execution); the press only lands in the per-slot counter so the wiring
    // stays observable. Out-of-range slots are ignored.
    if (SlotIndex < 1 || SlotIndex > 8)
    {
        return;
    }
    // M3-023: one discrete input row per skill-slot press (the card's
    // QWERASDF keys; slots have no combat effect yet, M2+ placeholder).
    if (UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(this))
    {
        static const TCHAR* M3_023_SkillSlotKeyNames[8] =
            { TEXT("Q"), TEXT("W"), TEXT("E"), TEXT("R"), TEXT("A"), TEXT("S"), TEXT("D"), TEXT("F") };
        OpLog->LogInput(FString::Printf(TEXT("Pressed %s (SkillSlot%d)"),
            M3_023_SkillSlotKeyNames[SlotIndex - 1], SlotIndex));
    }
    ++SkillSlotPressCounts[SlotIndex - 1];
    UE_LOG(LogTemp, Verbose, TEXT("UEMMO: skill slot %d pressed (no effect; M2+ placeholder)."), SlotIndex);
}
int32 APrototypeCharacter::GetSkillSlotPressCount(int32 SlotIndex) const
{
    return (SlotIndex >= 1 && SlotIndex <= 8) ? SkillSlotPressCounts[SlotIndex - 1] : 0;
}
void APrototypeCharacter::OnDebugTogglePressed()
{
    // M1-028: F1 is not a combat intent. The press only flips the local HUD's
    // debug overlay flag (pure display); combat state, queries and damage are
    // untouched. Without a player controller / prototype HUD it is a no-op.
    // M3-023: the discrete F1 press is logged before the HUD toggle.
    if (UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(this))
    {
        OpLog->LogInput(TEXT("Pressed F1 (DebugToggle)"));
    }
    if (APlayerController* PC = Cast<APlayerController>(Controller))
    {
        if (APrototypeHUD* HUD = Cast<APrototypeHUD>(PC->GetHUD()))
        {
            HUD->ToggleCombatDebugOverlay();
        }
    }
}
void APrototypeCharacter::OnResetPressed()
{
    // M3-023: the F2 Started binding logs the key press here; ResetPosition
    // stays the single unified-reset entry (its fall-out-of-world callers
    // from Tick are not key presses and stay unlogged).
    if (UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(this))
    {
        OpLog->LogInput(TEXT("Pressed F2 (Reset)"));
    }
    ResetPosition();
}

void APrototypeCharacter::SubmitCombatInput(ECombatInput Action)
{
	// M3-031: the press stamp must live in the SAME clock the consumer judges
	// buffered lifetimes with - the combat component's injected input game
	// clock (interface contract section 2). Reading the raw world clock here
	// desynchronized the two domains while a local hit stop pins the
	// component's clock: the press stamped AHEAD of the component's "now"
	// (a future-dated entry) and every press buffered across the stop paid
	// the frozen span out of its 150 ms lifetime - the M3-031 red evidence.
	// The component clock reads 0.0 only before its first injection (bare
	// test worlds), where the world clock stays the best available stamp.
	double PressedAt = 0.0;
	const UWorld* World = GetWorld();
	const double WorldSeconds = World ? World->GetTimeSeconds() : 0.0;
	if (Combat != nullptr)
	{
		const double ComponentClock = Combat->GetInputClockSeconds();
		PressedAt = (ComponentClock > 0.0) ? ComponentClock : WorldSeconds;
	}
	else
	{
		PressedAt = WorldSeconds;
	}
	// Input game clock (interface contract section 2): World GetTimeSeconds
	// advances with normal game time only, so pause and hit stop do not
	// advance it. World-less callers (early tests) read 0.0.
	SubmitCombatInput(Action, PressedAt);
}
void APrototypeCharacter::SubmitCombatInput(ECombatInput Action, double PressedAt)
{
    // M3-023: one discrete input row per submitted combat intent (the bound
    // key and the intent it carries). This is the single choke point shared by
    // the X/Z/C input bindings and the automation entries, so a press can
    // never be logged twice.
    if (UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(this))
    {
        const TCHAR* IntentText = (Action == ECombatInput::Light)
            ? TEXT("Pressed X (Light)")
            : (Action == ECombatInput::Launcher)
                ? TEXT("Pressed Z (Launcher)")
                : TEXT("Pressed C/Space (Jump)");
        OpLog->LogInput(IntentText);
    }
    if (Combat == nullptr)
    {
        return;
    }
    FBufferedCombatInput Intent;
    Intent.Sequence = NextCombatInputSequence++;
    Intent.Action = Action;
    Intent.PressedAt = PressedAt;
    // Rejections (duplicate/regressing sequence, non-finite time) are only
    // ignored: the character counter stays monotonic either way.
    Combat->QueueInput(Intent);
}
void APrototypeCharacter::SetTrainingResetService(UTrainingResetService* InService)
{
    TrainingResetService = InService;
}

void APrototypeCharacter::ResetPosition()
{
    // M1-027: one reset press (M1-040 key: F2; or one fall-out-of-world
    // recovery, the other
    // ResetPosition caller in Tick) is exactly one unified session reset.
    // The F2 action keeps its single Started binding to this method, and the
    // two branches below are mutually exclusive, so the reset can never run
    // twice per entry. With a registered service the whole training session
    // resets through UTrainingResetService::ResetTrainingSession (which owns
    // the player physics half via ApplyTrainingRoomReset); without one, the
    // M0 local reset stays for bare scaffolding and test worlds.
    if (UTrainingResetService* Service = TrainingResetService.Get())
    {
        Service->ResetTrainingSession();
        return;
    }
    ApplyTrainingRoomReset();
}

void APrototypeCharacter::ApplyTrainingRoomReset()
{
    // M1-027: the physics half of the unified reset, mirroring the enemy-side
    // ResetEnemy physics: zero every velocity source (including any launch or
    // impulse still pending on the movement component) and re-ground the mode,
    // then teleport back to the captured spawn location and facing.
    if (UCharacterMovementComponent* Movement = GetCharacterMovement())
    {
        Movement->StopMovementImmediately();
        Movement->Velocity = FVector::ZeroVector;
        Movement->ClearAccumulatedForces();
        Movement->SetMovementMode(MOVE_Walking);
    }
    SetActorLocation(SpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
    SetActorRotation(SpawnRotation);
    // M2-004: the value half for the pawn itself (the retry entry works while
    // dead, interface contract section 6: the unified reset covers HP): full
    // health pool (opens a fresh death lifecycle), combat teardown plus the
    // dead flag dropped, and the locomotion animation re-attached. Every step
    // is an idempotent no-op on an alive reset press, and the M1-027 service
    // path shares this exact entry (ResetTrainingSession phase 2), so both
    // reset routes revive identically.
    if (Health != nullptr)
    {
        Health->ResetHealth();
    }
    bPlayerDiedBroadcast = false;
    if (Combat != nullptr)
    {
        Combat->ResetCombat();
        Combat->SetDead(false);
    }
    // M5-020: the revive re-entry. The death teardown closed the reload window
    // and voided the pending callbacks; the re-applied equipment load restores
    // the binding identity with its parked magazine (switching/rebinding never
    // initializes new ammo) and re-opens firing for a bound ranged weapon.
    if (WeaponMount != nullptr)
    {
        WeaponMount->NotifyOwnerRevived();
    }
    RefreshWeaponMountFromEquipment();
    if (USkeletalMeshComponent* MeshComponent = GetMesh())
    {
        if (SavedAnimInstanceClass != nullptr)
        {
            MeshComponent->SetAnimInstanceClass(SavedAnimInstanceClass);
            SavedAnimInstanceClass = nullptr;
        }
    }
}
void APrototypeCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // M2-016: the room session clock injection at the M1-041 injection point.
    // The owner's second per-frame duty - hand the World game clock to this
    // world's URoomSessionSubsystem every game frame, so the M2-008 wave pump
    // advances with the real game frames (the session never reads a wall
    // clock itself). The subsystem is resolved through GetSubsystem and cached
    // as a weak reference (re-resolved when it expired or the world changed);
    // a world-less pawn keeps the pre-M2-016 semantics: no injection.
    //
    // The injected value is the frame-start game time (GetTimeSeconds() minus
    // this frame's delta = exactly the previous GetTimeSeconds accumulation),
    // NOT the mid-tick value: UWorld::Tick advances TimeSeconds BEFORE the
    // actor tick phase, so the raw mid-tick read sits a float-rounding step
    // (~1e-8, world ahead) above any externally scripted session-clock driver
    // of the same world. The session pump runs on EVERY injection, so a
    // mid-tick read ahead of the scripted value flips due births from the
    // between-ticks window into the actor-tick phase, which changes physics
    // depenetration outcomes for actors born inside an overlapping capsule
    // (the locked M2-010 retry suite regressed on exactly that). Injecting the
    // frame-start value makes this production driver defer to an earlier
    // injection of the same frame by construction (previous frame time <
    // any same-frame scripted time) while staying strictly monotonic and
    // advancing once per game frame when - as in the real game - it is the
    // only driver.
    if (UWorld* World = GetWorld())
    {
        URoomSessionSubsystem* Session = RoomSessionPtr.Get();
        if (Session == nullptr || Session->GetWorld() != World)
        {
            RoomSessionPtr = Session = World->GetSubsystem<URoomSessionSubsystem>();
        }
        if (Session != nullptr)
        {
            Session->SetSessionClockSeconds(World->GetTimeSeconds() - static_cast<double>(DeltaSeconds));
        }
    }
    // M1-012: the combat component advances its own 60 Hz action clock from
    // here. M1-041 (M1-H01 fix): the owner's per-frame duty - inject the
    // input game clock once per game frame, BEFORE TickCombat, from the World
    // GetTimeSeconds clock (the same clock SubmitCombatInput records
    // PressedAt on; it advances with normal game time only, so pause and hit
    // stop do not move it). The first injection activates the M1-021
    // input-driven Free start and the M1-014/M1-021 cancel-window chaining;
    // the component itself pins the value while a local hit stop freezes
    // (M1-033), so a plain every-frame injection is exactly the contract. A
    // world-less pawn (early tests) keeps the pre-M1-041 semantics: no
    // injection, the buffered-input consumption stays gated off.
    if (Combat != nullptr)
    {
        if (UWorld* World = GetWorld())
        {
            Combat->SetInputClockSeconds(World->GetTimeSeconds());
        }
        Combat->TickCombat(DeltaSeconds);
    }
    ApplyPlanarMovement(*this, PlanarAxes);
    if (GetActorLocation().Z < -1000.f) ResetPosition();
}
