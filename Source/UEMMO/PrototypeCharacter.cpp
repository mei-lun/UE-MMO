#include "PrototypeCharacter.h"
#include "Animation/AnimInstance.h"
#include "Character/AttackMovementGate.h"
#include "Character/SideViewCameraComponent.h"
#include "Combat/AttackCatalog.h"
#include "Combat/CombatComponent.h"
#include "Combat/CombatPresentationComponent.h"
#include "Combat/HealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
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
#include "PrototypeHUD.h"
#include "Room/RoomSessionSubsystem.h"
#include "Room/TrainingResetService.h"
#include "UObject/ConstructorHelpers.h"

using namespace UE::UEMMO::Tasks::M1_029;

namespace
{
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
    // skill-slot move removes the old R collision for free).
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
    Input->BindAction(ResetAction, ETriggerEvent::Started, this, &APrototypeCharacter::ResetPosition);
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
}
void APrototypeCharacter::MoveDepth(const FInputActionValue& Value) { PlanarAxes.SetAxisY(Value.Get<float>()); }
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
void APrototypeCharacter::EndJump() { StopJumping(); }
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
    if (APlayerController* PC = Cast<APlayerController>(Controller))
    {
        if (APrototypeHUD* HUD = Cast<APrototypeHUD>(PC->GetHUD()))
        {
            HUD->ToggleCombatDebugOverlay();
        }
    }
}
void APrototypeCharacter::SubmitCombatInput(ECombatInput Action)
{
    const UWorld* World = GetWorld();
    // Input game clock (interface contract section 2): World GetTimeSeconds
    // advances with normal game time only, so pause and hit stop do not
    // advance it. World-less callers (early tests) read 0.0.
    SubmitCombatInput(Action, World ? World->GetTimeSeconds() : 0.0);
}
void APrototypeCharacter::SubmitCombatInput(ECombatInput Action, double PressedAt)
{
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
