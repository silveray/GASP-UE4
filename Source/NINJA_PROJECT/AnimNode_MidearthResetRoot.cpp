// Port of UE 5.8 AnimGraphRuntime AnimNode_ResetRoot.cpp.
#include "AnimNode_MidearthResetRoot.h"

#include "Animation/AnimInstanceProxy.h"

void FAnimNode_MidearthResetRoot::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	check(OutBoneTransforms.Num() == 0);
	FCSPose<FCompactPose>& CSPose = Output.Pose;
	const FBoneContainer& BoneContainer = CSPose.GetPose().GetBoneContainer();
	const FCompactPoseBoneIndex RootBoneIndex(0);
	OutBoneTransforms.Reserve(RootChildren.Num() + 1);
	OutBoneTransforms.Add(FBoneTransform(RootBoneIndex, BoneContainer.GetRefPoseTransform(RootBoneIndex)));
	for (const FCompactPoseBoneIndex& Child : RootChildren)
	{
		OutBoneTransforms.Add(FBoneTransform(Child, CSPose.GetComponentSpaceTransform(Child)));
	}
	OutBoneTransforms.Sort(FCompareBoneTransformIndex());
}

void FAnimNode_MidearthResetRoot::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	RootChildren.Reset();
	const int32 NumCompactPoseBones = RequiredBones.GetCompactPoseNumBones();
	for (int32 Index = 0; Index < NumCompactPoseBones; Index++)
	{
		const FCompactPoseBoneIndex BoneIndex(Index);
		if (RequiredBones.GetParentBoneIndex(BoneIndex) == 0)
		{
			RootChildren.Add(BoneIndex);
		}
	}
}
