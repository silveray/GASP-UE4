#include "AnimNode_MidearthSpineTwist.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "AvoidanceComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

FAnimNode_MidearthSpineTwist::FAnimNode_MidearthSpineTwist()
	: Spine(TEXT("spine_03"))
{
}

void FAnimNode_MidearthSpineTwist::PreUpdate(const UAnimInstance* InAnimInstance)
{
	Twist = FQuat::Identity;
	const USkeletalMeshComponent* Mesh = InAnimInstance ? InAnimInstance->GetSkelMeshComponent() : nullptr;
	const AActor* Owner = Mesh ? Mesh->GetOwner() : nullptr;
	if (const UAvoidanceComponent* Avoid = Owner ? Owner->FindComponentByClass<UAvoidanceComponent>() : nullptr)
	{
		Twist = Avoid->GetSpineTwistRotation().Quaternion();
	}
}

void FAnimNode_MidearthSpineTwist::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	if (Twist.Equals(FQuat::Identity, 1.e-4f))
	{
		return;
	}
	const FCompactPoseBoneIndex Index = Spine.GetCompactPoseIndex(Output.Pose.GetPose().GetBoneContainer());
	FTransform Bone = Output.Pose.GetComponentSpaceTransform(Index);
	Bone.SetRotation(Twist * Bone.GetRotation()); // add in component space, keep the position (= Modify Bone 'Add to Existing')
	OutBoneTransforms.Add(FBoneTransform(Index, Bone));
}

bool FAnimNode_MidearthSpineTwist::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	return Spine.IsValidToEvaluate(RequiredBones);
}

void FAnimNode_MidearthSpineTwist::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	Spine.Initialize(RequiredBones);
}
