// Orientation Warping for UE 4.27 (10-09) = port of UE5 AnimationWarping FAnimNode_OrientationWarping, 'Graph' mode
// (5.8 source, same math), set up like GASP's motion matching blend stack: rotates the lower body (root + IK foot root)
// toward the real movement direction and counter-rotates the spine, when the clip's root motion goes a different way than
// the character (e.g. a forward-left loop playing while moving straight). Also turns this frame's root motion by the same
// angle (the Offset Root node above applies it). Needs Leg IK after it (feet follow ik_foot_l/r, GASP Leg IK).
// GASP values: LocomotionDirection = LastNonZeroVelocity, Alpha = clip curve 'Enable_Warping' at the clip time, WarpingSpace
// RootBoneTransform (offset root), TargetTime 0, LocomotionAngleDeltaThreshold 135, spine_01..05, ik_foot_root + ik_foot_l/r,
// RotationInterpSpeed 8, DistributedBoneOrientationAlpha 0.5, MinRootMotionSpeedThreshold 10, CounterCompensateInterpSpeed 45,
// MaxCorrectionDegrees 180, MaxRootMotionDeltaToCompensateDegrees 45. Missing spine bones are skipped (4.27 skeletons vary).
// Off unless console gasp.Warping >= 2. Setup: AnimGraph component space, first below the Offset Root node, then Reset Root.

#pragma once

#include "CoreMinimal.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "MidearthWarping.h"
#include "AnimNode_MidearthOrientationWarping.generated.h"

USTRUCT(BlueprintInternalUseOnly)
struct NINJA_PROJECT_API FAnimNode_MidearthOrientationWarping : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	/** Curve on the clips that turns warping on (GASP 'Enable_Warping'). No curve on the clip = 0 = off, like GASP. */
	UPROPERTY(EditAnywhere, Category = Evaluation)
	FName WarpingCurveName = TEXT("Enable_Warping");

	/** Root motion slower than this (cm/s) = no warping. */
	UPROPERTY(EditAnywhere, Category = Evaluation, meta = (ClampMin = "0"))
	float MinRootMotionSpeedThreshold = 10.0f;

	/** Above this angle (deg) the clip runs 'the other way' and the opposite direction is used. GASP 135. */
	UPROPERTY(EditAnywhere, Category = Evaluation, meta = (ClampMin = "0", ClampMax = "180"))
	float LocomotionAngleDeltaThreshold = 135.f;

	UPROPERTY(EditAnywhere, Category = Settings)
	TArray<FBoneReference> SpineBones;

	UPROPERTY(EditAnywhere, Category = Settings)
	FBoneReference IKFootRootBone;

	UPROPERTY(EditAnywhere, Category = Settings)
	TArray<FBoneReference> IKFootBones;

	/** Share of the angle on the root (rest on the IK foot root); the spine counter-rotates the root part. */
	UPROPERTY(EditAnywhere, Category = Settings, meta = (ClampMin = "0", ClampMax = "1"))
	float DistributedBoneOrientationAlpha = 0.5f;

	/** 10-09: no Leg IK after this node (Alen's IK foot bones don't follow the animation: 17-29 cm off at idle) -> the whole angle goes on
	 *  the root and the spine counter-rotates it, so legs + feet still face the movement. GASP (with Leg IK) = off + alpha 0.5. */
	UPROPERTY(EditAnywhere, Category = Settings)
	bool bFeetFollowWithoutLegIK = true;

	UPROPERTY(EditAnywhere, Category = Settings, meta = (ClampMin = "0"))
	float RotationInterpSpeed = 8.f;

	UPROPERTY(EditAnywhere, Category = Settings, meta = (ClampMin = "0"))
	float CounterCompensateInterpSpeed = 45.f;

	UPROPERTY(EditAnywhere, Category = Settings, meta = (ClampMin = "0"))
	float MaxCorrectionDegrees = 180.f;

	UPROPERTY(EditAnywhere, Category = Settings, meta = (ClampMin = "0"))
	float MaxRootMotionDeltaToCompensateDegrees = 45.f;

	UPROPERTY(EditAnywhere, Category = Settings)
	bool bCounterCompenstateInterpolationByRootMotion = true;

	// Debug / autotest readout (game thread reads; floats only).
	float DebugAngleDeg = 0.f;
	float DebugAlpha = 0.f;

	FAnimNode_MidearthOrientationWarping();

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void UpdateInternal(const FAnimationUpdateContext& Context) override;
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;

private:
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;

	struct FSpineBone
	{
		FCompactPoseBoneIndex BoneIndex = FCompactPoseBoneIndex(INDEX_NONE);
		float Weight = 0.f;
	};
	TArray<FSpineBone> SpineBoneDataArray;
	FCompactPoseBoneIndex IKFootRootBoneIndex = FCompactPoseBoneIndex(INDEX_NONE);
	TArray<FCompactPoseBoneIndex> IKFootBoneIndexArray;

	FAnimNode_MidearthOffsetRoot* OffsetRoot = nullptr;
	MidearthWarping::FGameState GameState;
	FVector LastNonZeroVelocity = FVector::ZeroVector; // GASP ABP Update_EssentialValues (velocity > 5 cm/s)
	FTransform WarpingSpaceTransform;
	FVector RootMotionDeltaDirection = FVector::ZeroVector;
	FQuat RootMotionDeltaRotation = FQuat::Identity;
	float CounterCompensateTargetAngleRad = 0.0f;
	float ActualOrientationAngleRad = 0.f;
	FGraphTraversalCounter UpdateCounter;
	bool bIsFirstUpdate = true;
};
