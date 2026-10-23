// Upper-body turn for the wall reaction (4.27 replacement of the 5.8 Transform (Modify) Bone spine_03, Add to Existing,
// Component Space, rotation = UAvoidanceComponent::GetSpineTwistRotation()). PreUpdate reads the owner's component on the
// game thread; Evaluate adds the yaw to spine_03 in component space (children follow). Zero turn = animation untouched.
// Setup: ... -> Local To Component -> Midearth Spine Twist -> Component To Local -> Output (bridge animbp_insert_skel_control).

#pragma once

#include "CoreMinimal.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "AnimNode_MidearthSpineTwist.generated.h"

USTRUCT(BlueprintInternalUseOnly)
struct NINJA_PROJECT_API FAnimNode_MidearthSpineTwist : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference Spine;

	FAnimNode_MidearthSpineTwist();

	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;

private:
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;

	FQuat Twist = FQuat::Identity; // game thread -> anim thread
};
