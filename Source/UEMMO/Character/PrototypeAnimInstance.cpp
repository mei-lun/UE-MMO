#include "PrototypeAnimInstance.h"

#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_EDITOR
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimNode_SequencePlayer.h"
#include "Animation/BlendSpace.h"
#include "AnimGraphNode_BlendListByInt.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimNodes/AnimNode_BlendListBase.h"
#include "AnimNodes/AnimNode_BlendListByInt.h"
#include "AnimNodes/AnimNode_BlendSpacePlayer.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_VariableGet.h"
#include "UObject/UnrealType.h"
#endif

// ---------------------------------------------------------------------------
// Pure mapping helpers (unit tested as UEMMO.Tasks.M1_031.*)
// ---------------------------------------------------------------------------

float UPrototypeAnimInstance::FacingYawFromHorizontalAxis(float AxisX)
{
    // Mirrors the M1-029 facing rule in PrototypeCharacter: positive horizontal
    // input faces +X (yaw 0), negative faces -X (yaw 180).
    return AxisX < 0.0f ? 180.0f : 0.0f;
}

FVector2D UPrototypeAnimInstance::ComputeLocalVelocity(const FVector& WorldVelocity, float FacingYaw)
{
    const float YawRadians = FMath::DegreesToRadians(FacingYaw);
    const float CosYaw = FMath::Cos(YawRadians);
    const float SinYaw = FMath::Sin(YawRadians);
    // UE yaw convention: yaw 0 forward is +X, yaw 90 forward is +Y, and the
    // right axis is the forward axis rotated -90 degrees around Z. A side move
    // (world depth axis) therefore never gains a forward component.
    const FVector Forward(CosYaw, SinYaw, 0.0f);
    const FVector Right(-SinYaw, CosYaw, 0.0f);
    return FVector2D(FVector::DotProduct(WorldVelocity, Forward), FVector::DotProduct(WorldVelocity, Right));
}

float UPrototypeAnimInstance::ComputeSpeed(const FVector& WorldVelocity)
{
    return WorldVelocity.Size2D();
}

float UPrototypeAnimInstance::ComputeLocomotionDirection(const FVector2D& LocalVelocity)
{
    if (LocalVelocity.SizeSquared() <= UE_SMALL_NUMBER)
    {
        return 0.0f;
    }
    return FMath::RadiansToDegrees(FMath::Atan2(LocalVelocity.Y, LocalVelocity.X));
}

int32 UPrototypeAnimInstance::ComputeAirPoseIndex(bool bIsFalling, float VerticalVelocity)
{
    if (!bIsFalling)
    {
        return Pose_Locomotion;
    }
    return VerticalVelocity > RiseFallSplitSpeed ? Pose_Jump : Pose_Fall;
}

// ---------------------------------------------------------------------------
// Per-frame export for the generated AnimBP
// ---------------------------------------------------------------------------

void UPrototypeAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);

    const ACharacter* Character = Cast<ACharacter>(TryGetPawnOwner());
    if (!Character)
    {
        return;
    }

    const FVector WorldVelocity = Character->GetVelocity();
    LocalVelocity = ComputeLocalVelocity(WorldVelocity, Character->GetActorRotation().Yaw);
    Speed = ComputeSpeed(WorldVelocity);
    Direction = ComputeLocomotionDirection(LocalVelocity);
    IsMoving = Speed > MovingThreshold;

    const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
    IsFalling = Movement && Movement->IsFalling();

    // Pose selection with a short hold on the land pose after touchdown. The
    // BlendSpace player underneath keeps evaluating every frame, so switching
    // between idle and move never restarts the animation from time zero.
    if (IsFalling)
    {
        bWasFalling = true;
        LandTimer = 0.0f;
        PoseIndex = ComputeAirPoseIndex(IsFalling, WorldVelocity.Z);
    }
    else if (bWasFalling)
    {
        bWasFalling = false;
        LandTimer = LandPoseDuration;
        PoseIndex = Pose_Land;
    }
    else if (LandTimer > 0.0f)
    {
        LandTimer = FMath::Max(0.0f, LandTimer - DeltaSeconds);
        PoseIndex = Pose_Land;
    }
    else
    {
        PoseIndex = Pose_Locomotion;
    }
}

// ---------------------------------------------------------------------------
// Editor-side graph builder (see header comment for why this exists)
// ---------------------------------------------------------------------------

#if WITH_EDITOR

namespace
{
    /** Creates and places a graph node the way the AnimGraph editor does. */
    template <typename NodeType>
    NodeType* CreatePlacedNode(UEdGraph& Graph, int32 PosX, int32 PosY)
    {
        FGraphNodeCreator<NodeType> Creator(Graph);
        NodeType* Node = Creator.CreateNode(false);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();
        return Node;
    }

    UEdGraphPin* FindOutputPin(UEdGraphNode& Node, const TCHAR* PinName)
    {
        return Node.FindPin(PinName, EGPD_Output);
    }

    UEdGraphPin* FindInputPin(UEdGraphNode& Node, const TCHAR* PinName)
    {
        return Node.FindPin(PinName, EGPD_Input);
    }

    bool LinkPins(UEdGraphPin* Output, UEdGraphPin* Input, const TCHAR* Description)
    {
        if (!Output || !Input)
        {
            UE_LOG(LogTemp, Error, TEXT("UEMMO locomotion graph: missing pin for %s (out=%p in=%p)."),
                Description, static_cast<void*>(Output), static_cast<void*>(Input));
            return false;
        }
        Output->MakeLinkTo(Input);
        return true;
    }

    // The FAnimNode structs keep their data private and expose it only to
    // their editor graph node friends; non-engine code writes them through
    // the reflection system (the same surface set_editor_property uses).
    bool SetObjectNodeField(void* StructPtr, UScriptStruct* StructType, const TCHAR* FieldName, UObject* Value)
    {
        FObjectPropertyBase* Field = CastField<FObjectPropertyBase>(StructType->FindPropertyByName(FieldName));
        if (!Field)
        {
            UE_LOG(LogTemp, Error, TEXT("UEMMO locomotion graph: object field %s not found."), FieldName);
            return false;
        }
        Field->SetObjectPropertyValue(Field->ContainerPtrToValuePtr<void>(StructPtr), Value);
        return true;
    }

    bool SetBoolNodeField(void* StructPtr, UScriptStruct* StructType, const TCHAR* FieldName, bool Value)
    {
        FBoolProperty* Field = CastField<FBoolProperty>(StructType->FindPropertyByName(FieldName));
        if (!Field)
        {
            UE_LOG(LogTemp, Error, TEXT("UEMMO locomotion graph: bool field %s not found."), FieldName);
            return false;
        }
        Field->SetPropertyValue(Field->ContainerPtrToValuePtr<void>(StructPtr), Value);
        return true;
    }

    bool SetFloatArrayNodeField(void* StructPtr, UScriptStruct* StructType, const TCHAR* FieldName, TArrayView<const float> Values)
    {
        FArrayProperty* Field = CastField<FArrayProperty>(StructType->FindPropertyByName(FieldName));
        if (!Field)
        {
            UE_LOG(LogTemp, Error, TEXT("UEMMO locomotion graph: array field %s not found."), FieldName);
            return false;
        }
        void* ArrayPtr = Field->ContainerPtrToValuePtr<void>(StructPtr);
        FScriptArrayHelper Helper(Field, ArrayPtr);
        Helper.Resize(Values.Num());
        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            *reinterpret_cast<float*>(Helper.GetRawPtr(Index)) = Values[Index];
        }
        return true;
    }
}

bool UPrototypeAnimInstance::EditorBuildLocomotionGraph(UBlueprint* Blueprint, UBlendSpace* LocomotionBlendSpace, UAnimSequenceBase* JumpSequence, UAnimSequenceBase* FallSequence, UAnimSequenceBase* LandSequence)
{
    UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Blueprint);
    if (!AnimBP || !LocomotionBlendSpace || !JumpSequence || !FallSequence || !LandSequence)
    {
        UE_LOG(LogTemp, Error, TEXT("UEMMO locomotion graph builder: blueprint or animation asset is null."));
        return false;
    }

    // BlendSpace axis ranges: X = Speed (cm/s, matches M1-029 planar speeds),
    // Y = Direction (degrees). BlendParameters is a fixed C-array UPROPERTY
    // that UE Python cannot address, hence this editor-side configuration.
    if (FProperty* ParamProperty = LocomotionBlendSpace->GetClass()->FindPropertyByName(TEXT("BlendParameters")))
    {
        FBlendParameter* Params = static_cast<FBlendParameter*>(ParamProperty->ContainerPtrToValuePtr<void>(LocomotionBlendSpace));
        Params[0].DisplayName = TEXT("Speed");
        Params[0].Min = 0.0f;
        Params[0].Max = 420.0f;
        Params[0].GridNum = 8;
        Params[1].DisplayName = TEXT("Direction");
        Params[1].Min = -180.0f;
        Params[1].Max = 180.0f;
        Params[1].GridNum = 8;
        LocomotionBlendSpace->ValidateSampleData();
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("UEMMO locomotion graph builder: BlendParameters property not found."));
        return false;
    }

    // The AnimGraph is a domain-specific graph named "AnimGraph" (class
    // UAnimationGraph, schema UAnimationGraphSchema) stored in FunctionGraphs;
    // the ubergraph pages hold the event graph instead.
    UEdGraph* AnimGraph = nullptr;
    for (UEdGraph* Graph : AnimBP->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == TEXT("AnimGraph"))
        {
            AnimGraph = Graph;
            break;
        }
    }
    if (!AnimGraph)
    {
        UAnimationGraph* NewGraph = NewObject<UAnimationGraph>(AnimBP, TEXT("AnimGraph"), RF_Transactional);
        NewGraph->Schema = UAnimationGraphSchema::StaticClass();
        AnimBP->FunctionGraphs.Add(NewGraph);
        NewGraph->GetSchema()->CreateDefaultNodesForGraph(*NewGraph);
        AnimGraph = NewGraph;
    }

    // Idempotency: an existing BlendSpacePlayer means the graph is already built.
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        if (Node && Node->IsA<UAnimGraphNode_BlendSpacePlayer>())
        {
            TMap<FName, int32> Census;
            for (UEdGraphNode* Existing : AnimGraph->Nodes)
            {
                if (Existing)
                {
                    int32& Count = Census.FindOrAdd(Existing->GetClass()->GetFName());
                    ++Count;
                }
            }
            for (const TPair<FName, int32>& Entry : Census)
            {
                UE_LOG(LogTemp, Display, TEXT("UEMMO locomotion graph census: %s = %d"), *Entry.Key.ToString(), Entry.Value);
            }
            UE_LOG(LogTemp, Display, TEXT("UEMMO locomotion graph: AnimGraph already contains a BlendSpacePlayer; nothing to do."));
            return true;
        }
    }

    UAnimGraphNode_Root* RootNode = nullptr;
    for (UEdGraphNode* Node : AnimGraph->Nodes)
    {
        RootNode = Cast<UAnimGraphNode_Root>(Node);
        if (RootNode)
        {
            break;
        }
    }
    if (!RootNode)
    {
        RootNode = CreatePlacedNode<UAnimGraphNode_Root>(*AnimGraph, 900, 0);
    }
    UEdGraphPin* RootInput = RootNode->FindPin(TEXT("Result"), EGPD_Input);
    if (!RootInput)
    {
        for (UEdGraphPin* Pin : RootNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input)
            {
                RootInput = Pin;
                break;
            }
        }
    }
    if (!RootInput)
    {
        UE_LOG(LogTemp, Error, TEXT("UEMMO locomotion graph builder: AnimGraph root has no input pin."));
        return false;
    }

    // Locomotion BlendSpace player with Speed/Direction wired to the native
    // exports. Setting the asset first, then reconstructing, refreshes pins.
    UAnimGraphNode_BlendSpacePlayer* LocoNode = CreatePlacedNode<UAnimGraphNode_BlendSpacePlayer>(*AnimGraph, 60, -140);
    {
        const bool bAssetSet = SetObjectNodeField(&LocoNode->Node, FAnimNode_BlendSpacePlayer::StaticStruct(), TEXT("BlendSpace"), LocomotionBlendSpace);
        if (!bAssetSet)
        {
            return false;
        }
        LocoNode->ReconstructNode();
    }

    // Jump / Fall / Land sequence players; Fall loops, the others play once.
    auto MakeSequencePlayer = [AnimGraph](UAnimSequenceBase* Sequence, bool bLoop, int32 PosX, int32 PosY)
    {
        UAnimGraphNode_SequencePlayer* Node = CreatePlacedNode<UAnimGraphNode_SequencePlayer>(*AnimGraph, PosX, PosY);
        SetObjectNodeField(&Node->Node, FAnimNode_SequencePlayer::StaticStruct(), TEXT("Sequence"), Sequence);
        SetBoolNodeField(&Node->Node, FAnimNode_SequencePlayer::StaticStruct(), TEXT("bLoopAnimation"), bLoop);
        return Node;
    };
    UAnimGraphNode_SequencePlayer* JumpNode = MakeSequencePlayer(JumpSequence, false, 60, 140);
    UAnimGraphNode_SequencePlayer* FallNode = MakeSequencePlayer(FallSequence, true, 60, 280);
    UAnimGraphNode_SequencePlayer* LandNode = MakeSequencePlayer(LandSequence, false, 60, 420);

    // Pose blend list: index 0 locomotion, 1 jump, 2 fall, 3 land (matches the
    // PoseIndex constants). The constructor and PostPlacedNewNode each seed one
    // pose; add two more so the list ends up with exactly four entries.
    UAnimGraphNode_BlendListByInt* BlendNode = nullptr;
    {
        static constexpr float BlendListTimes[] = {0.15f, 0.15f, 0.15f, 0.15f};
        FGraphNodeCreator<UAnimGraphNode_BlendListByInt> Creator(*AnimGraph);
        BlendNode = Creator.CreateNode(false);
        BlendNode->Node.AddPose();
        BlendNode->Node.AddPose();
        BlendNode->NodePosX = 520;
        BlendNode->NodePosY = 60;
        Creator.Finalize();
        SetFloatArrayNodeField(&BlendNode->Node, FAnimNode_BlendListBase::StaticStruct(), TEXT("BlendTime"), TArrayView<const float>(BlendListTimes, UE_ARRAY_COUNT(BlendListTimes)));
    }

    auto MakeVariableGet = [AnimGraph](const TCHAR* PropertyName, int32 PosX, int32 PosY) -> UK2Node_VariableGet*
    {
        FGraphNodeCreator<UK2Node_VariableGet> Creator(*AnimGraph);
        UK2Node_VariableGet* Node = Creator.CreateNode(false);
        Node->VariableReference.SetSelfMember(PropertyName);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();
        return Node;
    };
    UK2Node_VariableGet* SpeedNode = MakeVariableGet(TEXT("Speed"), -260, -260);
    UK2Node_VariableGet* DirectionNode = MakeVariableGet(TEXT("Direction"), -260, -120);
    UK2Node_VariableGet* PoseIndexNode = MakeVariableGet(TEXT("PoseIndex"), 260, 60);

    bool bOk = true;
    bOk &= LinkPins(FindOutputPin(*SpeedNode, TEXT("Speed")), FindInputPin(*LocoNode, TEXT("X")), TEXT("Speed -> BlendSpace X"));
    bOk &= LinkPins(FindOutputPin(*DirectionNode, TEXT("Direction")), FindInputPin(*LocoNode, TEXT("Y")), TEXT("Direction -> BlendSpace Y"));
    bOk &= LinkPins(FindOutputPin(*LocoNode, TEXT("Pose")), FindInputPin(*BlendNode, TEXT("BlendPose_0")), TEXT("BlendSpace -> BlendPose 0"));
    bOk &= LinkPins(FindOutputPin(*JumpNode, TEXT("Pose")), FindInputPin(*BlendNode, TEXT("BlendPose_1")), TEXT("Jump -> BlendPose 1"));
    bOk &= LinkPins(FindOutputPin(*FallNode, TEXT("Pose")), FindInputPin(*BlendNode, TEXT("BlendPose_2")), TEXT("Fall -> BlendPose 2"));
    bOk &= LinkPins(FindOutputPin(*LandNode, TEXT("Pose")), FindInputPin(*BlendNode, TEXT("BlendPose_3")), TEXT("Land -> BlendPose 3"));
    UEdGraphPin* IndexPin = FindInputPin(*BlendNode, TEXT("ActiveChildIndex"));
    if (!IndexPin)
    {
        IndexPin = FindInputPin(*BlendNode, TEXT("BlendIndex"));
    }
    bOk &= LinkPins(FindOutputPin(*PoseIndexNode, TEXT("PoseIndex")), IndexPin, TEXT("PoseIndex -> BlendList index"));
    bOk &= LinkPins(FindOutputPin(*BlendNode, TEXT("Pose")), RootInput, TEXT("BlendList -> Result"));

    UE_LOG(LogTemp, Display, TEXT("UEMMO locomotion graph builder: %s (%d nodes)."), bOk ? TEXT("graph built") : TEXT("graph built with pin errors"), AnimGraph->Nodes.Num());
    return bOk;
}

#else

bool UPrototypeAnimInstance::EditorBuildLocomotionGraph(UBlueprint*, UBlendSpace*, UAnimSequenceBase*, UAnimSequenceBase*, UAnimSequenceBase*)
{
    return false;
}

#endif
