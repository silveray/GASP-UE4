// Steering for UE 4.27 (10-09) = port of UE5 AnimationWarping FAnimNode_Steering (5.8 source, same math), set up like GASP.
// Turns the animation's root motion (this frame) toward the desired facing: first scales the clip's own turn so it ends
// on the target (AnimatedTargetTime look-ahead, Min/MaxScaleRatio), then adds a critically damped correction
// (ProceduralTargetTime half-life). It moves no bones - the Offset Root node above it applies the steered root motion.
// GASP has two in its motion matching blend stack:
//   A: ProceduralTargetTime 0.4, DisableSteeringBelowSpeed 10, on while GASP EnableSteering (moving or in the air), AlphaBoolBlend 0.1.
//   B: ProceduralTargetTime 1000000, DisableSteeringBelowSpeed -1, on while a TurnInPlace clip plays.
// Target = GASP Get_DesiredFacing (trajectory facing at +0.5 s). Clip + time = the motion matching component's current clip.
// Off unless console gasp.Warping >= 2. Setup: AnimGraph component space, below the Offset Root node (bridge animbp_insert_after_slot).

#pragma once

#include "CoreMinimal.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "MidearthWarping.h"
#include "AnimNode_MidearthSteering.generated.h"

UENUM(BlueprintType)
enum class EMidearthSteeringUse : uint8
{
	/** GASP Steering A: on while moving (EnableSteering). */
	Moving,
	/** GASP Steering B: on while a turn-in-place clip plays (database tag TurnInPlace). */
	TurnInPlace
};

USTRUCT(BlueprintInternalUseOnly)
struct NINJA_PROJECT_API FAnimNode_MidearthSteering : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	/** Which GASP Steering node this is (sets when it is on). */
	UPROPERTY(EditAnywhere, Category = Evaluation)
	EMidearthSteeringUse Use = EMidearthSteeringUse::Moving;

	/** Half-life of the procedural correction (s). GASP A 0.4, B 1000000. */
	UPROPERTY(EditAnywhere, Category = Evaluation)
	float ProceduralTargetTime = 0.4f;

	/** How far ahead the clip's own turn is read (s). GASP 2. */
	UPROPERTY(EditAnywhere, Category = Evaluation)
	float AnimatedTargetTime = 2.0f;

	/** Clip turns smaller than this (deg) are not scaled. */
	UPROPERTY(EditAnywhere, Category = Evaluation)
	float RootMotionThreshold = 1.0f;

	/** No steering below this root motion speed (cm/s). GASP A 10, B -1. */
	UPROPERTY(EditAnywhere, Category = Evaluation)
	float DisableSteeringBelowSpeed = 10.0f;

	UPROPERTY(EditAnywhere, Category = Evaluation)
	float DisableAdditiveBelowSpeed = -1.0f;

	UPROPERTY(EditAnywhere, Category = Evaluation)
	float MinScaleRatio = 0.5f;

	UPROPERTY(EditAnywhere, Category = Evaluation)
	float MaxScaleRatio = 1.5f;

	FAnimNode_MidearthSteering();

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void UpdateInternal(const FAnimationUpdateContext& Context) override;
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;

private:
	FAnimNode_MidearthOffsetRoot* OffsetRoot = nullptr;
	MidearthWarping::FGameState GameState;
	FVector AngularVelocity = FVector::ZeroVector;
	FTransform RootBoneTransform;
};
