// Reset Root for UE 4.27 (10-09) = port of UE5 AnimGraphRuntime FAnimNode_ResetRoot: puts the root bone back to its
// reference pose while every direct child keeps its component-space transform. GASP runs it right after Orientation
// Warping (which rotates the root to turn the whole pose), so the Offset Root node gets a clean root.
// Setup: AnimGraph component space, after Midearth Orientation Warping (bridge animbp_insert_after_slot).

#pragma once

#include "CoreMinimal.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "AnimNode_MidearthResetRoot.generated.h"

USTRUCT(BlueprintInternalUseOnly)
struct NINJA_PROJECT_API FAnimNode_MidearthResetRoot : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override { return true; }

private:
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;
	TArray<FCompactPoseBoneIndex> RootChildren;
};
