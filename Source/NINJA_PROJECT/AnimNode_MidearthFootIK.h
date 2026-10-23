// Foot IK for slopes and steps (UE 4.27 has no GASP Foot Placement node).
// On the game thread (PreUpdate) it traces the ground under each foot; on the anim thread it moves each foot onto that ground
// (two-bone IK thigh-calf-foot), tilts the foot to the ground and lowers the pelvis only as much as the lower foot needs.
// Safe limits (GASP's 250 cm pelvis offset sank the body into stairs on 09-16): MaxPelvisDrop 25 cm, MaxFootOffset 40 cm.
// Flat ground or in the air = no change. Setup: AnimGraph Slot -> Local To Component -> Midearth Foot IK -> Component To Local -> Output
// (bridge command animbp_insert_skel_control does it).

#pragma once

#include "CoreMinimal.h"
#include "BoneControllers/AnimNode_SkeletalControlBase.h"
#include "AnimNode_MidearthFootIK.generated.h"

USTRUCT(BlueprintInternalUseOnly)
struct NINJA_PROJECT_API FAnimNode_MidearthFootIK : public FAnimNode_SkeletalControlBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference Pelvis;
	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference ThighL;
	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference CalfL;
	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference FootL;
	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference ThighR;
	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference CalfR;
	UPROPERTY(EditAnywhere, Category = "Bones") FBoneReference FootR;

	/** The pelvis never goes lower than this (cm). */
	UPROPERTY(EditAnywhere, Category = "Foot IK", meta = (ClampMin = "0"))
	float MaxPelvisDrop = 25.f;

	/** A foot moves at most this far up or down (cm). */
	UPROPERTY(EditAnywhere, Category = "Foot IK", meta = (ClampMin = "0"))
	float MaxFootOffset = 40.f;

	/** Ground trace from this far above the character's base ... */
	UPROPERTY(EditAnywhere, Category = "Foot IK", meta = (ClampMin = "0"))
	float TraceUp = 50.f;

	/** ... to this far below it (cm). */
	UPROPERTY(EditAnywhere, Category = "Foot IK", meta = (ClampMin = "0"))
	float TraceDown = 60.f;

	/** How fast offsets follow the ground (higher = snappier). */
	UPROPERTY(EditAnywhere, Category = "Foot IK", meta = (ClampMin = "0"))
	float InterpSpeed = 15.f;

	/** Feet tilt with the ground up to this angle (degrees, 0 = no tilt). */
	UPROPERTY(EditAnywhere, Category = "Foot IK", meta = (ClampMin = "0", ClampMax = "60"))
	float MaxFootTilt = 30.f;

	FAnimNode_MidearthFootIK();

	// FAnimNode_Base
	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;

	// FAnimNode_SkeletalControlBase
	virtual void EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms) override;
	virtual bool IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones) override;

private:
	virtual void InitializeBoneReferences(const FBoneContainer& RequiredBones) override;

	// Game thread -> anim thread (component space, cm).
	float OffsetL = 0.f, OffsetR = 0.f, PelvisOffset = 0.f;
	FVector NormalL = FVector::UpVector, NormalR = FVector::UpVector;
};
