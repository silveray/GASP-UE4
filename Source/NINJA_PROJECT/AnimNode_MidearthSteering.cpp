// Port of UE 5.8 AnimationWarping AnimNode_Steering.cpp (see the header).
#include "AnimNode_MidearthSteering.h"

#include "AnimNode_MidearthOffsetRoot.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"

FAnimNode_MidearthSteering::FAnimNode_MidearthSteering()
{
	// GASP: Alpha input = Bool (EnableSteering / TurnInPlace), blend in/out 0.1 s.
	AlphaInputType = EAnimAlphaInputType::Bool;
	AlphaBoolBlend.BlendInTime = 0.1f;
	AlphaBoolBlend.BlendOutTime = 0.1f;
}

void FAnimNode_MidearthSteering::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_SkeletalControlBase::Initialize_AnyThread(Context);
	AngularVelocity = FVector::ZeroVector;
	OffsetRoot = MidearthWarping::FindOffsetRootNode(Context);
}

void FAnimNode_MidearthSteering::PreUpdate(const UAnimInstance* InAnimInstance)
{
	MidearthWarping::GatherGameState(InAnimInstance, GameState);
	// GASP A: EnableSteering = (MovementState Moving OR in the air) AND the clip is active. GASP B: CurrentDatabaseTags contains TurnInPlace.
	bAlphaBoolEnabled = GameState.bValid && (Use == EMidearthSteeringUse::Moving ? (GameState.bMoving || !GameState.bOnGround) : GameState.bTurnInPlace)
		&& !GameState.bSlotActive && MidearthWarping::GetWarpingLevel() >= 2;
}

void FAnimNode_MidearthSteering::UpdateInternal(const FAnimationUpdateContext& Context)
{
	FAnimNode_SkeletalControlBase::UpdateInternal(Context);
	// UE5: FRootOffsetProvider message, else the component transform.
	RootBoneTransform = OffsetRoot ? OffsetRoot->Channel.RootBoneTransform : Context.AnimInstanceProxy->GetComponentTransform();
}

bool FAnimNode_MidearthSteering::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	return OffsetRoot != nullptr && MidearthWarping::GetWarpingLevel() >= 2;
}

void FAnimNode_MidearthSteering::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	const float DeltaSeconds = Output.AnimInstanceProxy->GetDeltaSeconds();
	if (DeltaSeconds <= 0.f || Alpha <= 0.f || !OffsetRoot || !OffsetRoot->Channel.bRootMotionValid)
	{
		return;
	}
	FTransform ThisFrameRootMotionTransform = OffsetRoot->Channel.RootMotionDelta;
	const float CurrentSpeed = ThisFrameRootMotionTransform.GetTranslation().Size() / DeltaSeconds;
	if (CurrentSpeed <= DisableSteeringBelowSpeed)
	{
		return;
	}
	const FQuat TargetOrientation = GameState.DesiredFacing;
	const FQuat RootBoneRotation = RootBoneTransform.GetRotation();
	FQuat DeltaToTargetOrientation = RootBoneRotation.Inverse() * TargetOrientation;

	if (AnimatedTargetTime > 0.f)
	{
		if (const UAnimSequence* AnimSequence = GameState.CurrentClip)
		{
			const FTransform PredictedRootMotionTransform = AnimSequence->ExtractRootMotion(GameState.CurrentClipTime, AnimatedTargetTime, GameState.bCurrentClipLoops);
			FQuat PredictedRootMotionQuat = PredictedRootMotionTransform.GetRotation();
			FRotator PredictedRootMotionRot = FRotator(PredictedRootMotionQuat);
			const float PredictedRootMotionYaw = PredictedRootMotionRot.Yaw;
			if (FMath::Abs(PredictedRootMotionYaw) > RootMotionThreshold)
			{
				float YawToTargetOrientation = FRotator(DeltaToTargetOrientation).Yaw;
				// pick the rotation direction that is the shortest path from the endpoint of the current animated rotation
				if (PredictedRootMotionYaw - YawToTargetOrientation > 180.f)
				{
					YawToTargetOrientation += 360.f;
				}
				else if (YawToTargetOrientation - PredictedRootMotionYaw > 180.f)
				{
					YawToTargetOrientation -= 360.f;
				}
				float Ratio = YawToTargetOrientation / PredictedRootMotionYaw;
				Ratio = FMath::Clamp(Ratio, MinScaleRatio, MaxScaleRatio);
				Ratio = FMath::Lerp(1.0f, Ratio, Alpha);
				FRotator ThisFrameRootMotionRotator(ThisFrameRootMotionTransform.GetRotation());
				ThisFrameRootMotionRotator.Yaw *= Ratio;
				ThisFrameRootMotionTransform.SetRotation(FQuat(ThisFrameRootMotionRotator));
				// Account for future scaling in linear error correction
				PredictedRootMotionRot.Yaw *= Ratio;
				PredictedRootMotionQuat = PredictedRootMotionRot.Quaternion();
				DeltaToTargetOrientation = PredictedRootMotionQuat.Inverse() * RootBoneRotation.Inverse() * TargetOrientation;
			}
		}
	}

	if (CurrentSpeed > DisableAdditiveBelowSpeed)
	{
		// Apply linear correction
		FQuat LinearCorrection = FQuat::Identity;
		MidearthWarping::CriticalSpringDamperQuat(LinearCorrection, AngularVelocity, DeltaToTargetOrientation,
			MidearthWarping::HalfLifeToSmoothingTime(ProceduralTargetTime), DeltaSeconds);
		const FQuat ThisFrameRotation = ThisFrameRootMotionTransform.GetRotation() * LinearCorrection;
		ThisFrameRootMotionTransform.SetRotation(FQuat::Slerp(ThisFrameRootMotionTransform.GetRotation(), ThisFrameRotation, Alpha));
	}
	OffsetRoot->Channel.RootMotionDelta = ThisFrameRootMotionTransform; // UE5 OverrideRootMotion
}

void FAnimNode_MidearthSteering::GatherDebugData(FNodeDebugData& DebugData)
{
	FString DebugLine = DebugData.GetNodeName(this);
	DebugLine += FString::Printf(TEXT("(%s Alpha:%.3f)"), Use == EMidearthSteeringUse::Moving ? TEXT("moving") : TEXT("turn in place"), ActualAlpha);
	DebugData.AddDebugItem(DebugLine);
	ComponentPose.GatherDebugData(DebugData);
}
