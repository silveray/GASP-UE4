// Offset Root Bone for UE 4.27 (10-09) = port of UE5 AnimationWarping FAnimNode_OffsetRootBone (5.8 source, same math),
// set up like GASP SandboxCharacter_CMC_ABP. The visible body (root bone) follows the animation's own root motion while
// the capsule moves on its own, and the gap between them is damped (Interpolate) or kept (Accumulate) - that is what lets
// GASP turn the capsule instantly while the body turns with the clip.
// 4.27 differences: no root-motion attribute -> the per-frame root motion is measured from the playing montages
// (FMontageEvaluationState positions, ExtractRootMotionFromTrackRange, weight-blended, x the character's root motion
// translation scale) = UE5 'Graph' mode input; no graph messages -> Steering / Orientation Warping below it use its channel.
// GASP drivers (bUseGaspDrivers, read from the GASP ABP functions): RotationMode = slot active ? Release : Accumulate;
// TranslationMode = slot active or in the air ? Release : (moving ? Interpolate : Release); TranslationHalflife = moving 0.3 /
// idle 0.1; bClampToTranslationVelocity = moving. Fixed GASP node values: MaxTranslationError 30, RotationHalfLife 0.1.
// Off (pose passes through) unless console gasp.Warping >= 1.
// Setup: AnimGraph, local space, after the Slot (bridge animbp_insert_after_slot).

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"
#include "MidearthWarping.h"
#include "AnimNode_MidearthOffsetRoot.generated.h"

class UAnimMontage;

UENUM(BlueprintType)
enum class EMidearthOffsetRootMode : uint8
{
	/** Accumulate the animation's root motion, ignore the capsule (the gap stays). */
	Accumulate,
	/** Accumulate, and damp the gap to the capsule over the half-life. */
	Interpolate,
	/** Damp the gap to the capsule, ignore the animation's root motion. */
	Release
};

USTRUCT(BlueprintInternalUseOnly)
struct NINJA_PROJECT_API FAnimNode_MidearthOffsetRoot : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Links)
	FPoseLink Source;

	/** Modes / half-life from the GASP ABP functions (see header comment). Off = the values below. */
	UPROPERTY(EditAnywhere, Category = Settings)
	bool bUseGaspDrivers = true;

	UPROPERTY(EditAnywhere, Category = Settings)
	EMidearthOffsetRootMode TranslationMode = EMidearthOffsetRootMode::Interpolate;

	UPROPERTY(EditAnywhere, Category = Settings)
	EMidearthOffsetRootMode RotationMode = EMidearthOffsetRootMode::Accumulate;

	UPROPERTY(EditAnywhere, Category = Settings, meta = (ClampMin = "0"))
	float TranslationHalflife = 0.2f;

	UPROPERTY(EditAnywhere, Category = Settings, meta = (ClampMin = "0"))
	float RotationHalfLife = 0.1f;

	/** Largest gap capsule <-> body (cm, -1 = no limit). GASP 30. */
	UPROPERTY(EditAnywhere, Category = Settings)
	float MaxTranslationError = 30.f;

	/** Largest yaw gap (deg, -1 = no limit). GASP -1. */
	UPROPERTY(EditAnywhere, Category = Settings)
	float MaxRotationError = -1.f;

	UPROPERTY(EditAnywhere, Category = Settings)
	bool bClampToTranslationVelocity = false;

	UPROPERTY(EditAnywhere, Category = Settings)
	float TranslationSpeedRatio = 0.5f;

	/** Keep the body on the capsule's ground plane. */
	UPROPERTY(EditAnywhere, Category = Settings)
	bool bOnGround = true;

	UPROPERTY(EditAnywhere, Category = Settings)
	FVector GroundNormal = FVector(0.f, 0.f, 1.f);

	// Debug / autotest readout (anim thread writes, game thread reads; floats only).
	float DebugTranslationError = 0.f;
	float DebugYawError = 0.f;
	float DebugRootMotionSpeed = 0.f;

	/** Shared with the Steering / Orientation Warping nodes it evaluates. */
	MidearthWarping::FChannel Channel;

	/** World transform of the visible body's root after the last evaluation (component orientation). Valid while bHasEvaluated. */
	FTransform GetBodyTransform() const { return FTransform(SimulatedRotation, SimulatedTranslation); }
	bool HasEvaluated() const { return !bIsFirstUpdate && MidearthWarping::GetWarpingLevel() >= 1; }

	// FAnimNode_Base
	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	virtual bool HasPreUpdate() const override { return true; }
	virtual void PreUpdate(const UAnimInstance* InAnimInstance) override;

private:
	void Reset(const FAnimationBaseContext& Context);
	void MeasureRootMotion(const FAnimationUpdateContext& Context);

	FAnimInstanceProxy* AnimInstanceProxy = nullptr;
	MidearthWarping::FGameState GameState;
	float CachedDeltaTime = 0.f;
	bool bIsFirstUpdate = true;
	FTransform ComponentTransform = FTransform::Identity;
	FVector SimulatedTranslation = FVector::ZeroVector;
	FQuat SimulatedRotation = FQuat::Identity;
	FGraphTraversalCounter UpdateCounter;
	/** Last seen position of every playing montage (root motion = position change). */
	TMap<TWeakObjectPtr<UAnimMontage>, float> LastMontagePositions;
};
