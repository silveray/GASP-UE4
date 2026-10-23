// Port of UE 5.8 AnimationWarping AnimNode_OrientationWarping.cpp, Graph mode (see the header).
#include "AnimNode_MidearthOrientationWarping.h"

#include "AnimNode_MidearthOffsetRoot.h"
#include "Animation/AnimInstanceProxy.h"

namespace MidearthOrientationWarping
{
	static float SignedAngleRadBetweenNormals(const FVector& From, const FVector& To, const FVector& Axis)
	{
		const float FromDotTo = FMath::Clamp(FVector::DotProduct(From, To), -1.f, 1.f);
		const float Angle = FMath::Acos(FromDotTo);
		const FVector Cross = FVector::CrossProduct(From, To);
		const float Dot = FVector::DotProduct(Cross, Axis);
		return Dot >= 0 ? Angle : -Angle;
	}
}

FAnimNode_MidearthOrientationWarping::FAnimNode_MidearthOrientationWarping()
{
	// GASP motion matching blend stack values.
	for (const TCHAR* Bone : { TEXT("spine_01"), TEXT("spine_02"), TEXT("spine_03"), TEXT("spine_04"), TEXT("spine_05") })
	{
		SpineBones.Add(FBoneReference(Bone));
	}
	IKFootRootBone = FBoneReference(TEXT("ik_foot_root"));
	IKFootBones.Add(FBoneReference(TEXT("ik_foot_l")));
	IKFootBones.Add(FBoneReference(TEXT("ik_foot_r")));
}

void FAnimNode_MidearthOrientationWarping::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_SkeletalControlBase::Initialize_AnyThread(Context);
	OffsetRoot = MidearthWarping::FindOffsetRootNode(Context);
	RootMotionDeltaDirection = FVector::ZeroVector;
	RootMotionDeltaRotation = FQuat::Identity;
	CounterCompensateTargetAngleRad = 0.f;
	ActualOrientationAngleRad = 0.f;
	bIsFirstUpdate = true;
}

void FAnimNode_MidearthOrientationWarping::PreUpdate(const UAnimInstance* InAnimInstance)
{
	MidearthWarping::GatherGameState(InAnimInstance, GameState);
	if (GameState.bGaspModel)
	{
		if (GameState.Velocity.Size2D() > 5.f) // GASP: HasVelocity (Speed2D > 5) -> LastNonZeroVelocity
		{
			LastNonZeroVelocity = GameState.Velocity;
		}
	}
	else if (!GameState.Acceleration.IsNearlyZero())
	{
		// Root-motion model (10-09, user: orientation warping first): the clip moves the capsule, so its velocity IS the clip's root
		// motion and would never warp. The CMC velocity GASP uses = where the input pushes -> the input direction.
		LastNonZeroVelocity = GameState.Acceleration.GetSafeNormal2D() * FMath::Max(GameState.Velocity.Size2D(), 100.f);
	}
	else if (GameState.Velocity.Size2D() > 5.f)
	{
		LastNonZeroVelocity = GameState.Velocity; // no input: follow the clip (stops) -> no warp
	}
	// GASP: Alpha = GetCurveValueFromAnimation(current blend stack anim, 'Enable_Warping', its time); no curve = 0.
	float CurveValue = 0.f;
	MidearthWarping::GetClipCurveValue(GameState.CurrentClip, WarpingCurveName, GameState.CurrentClipTime, CurveValue);
	Alpha = (MidearthWarping::GetWarpingLevel() >= 2 && GameState.bLocomotionOwned) ? CurveValue : 0.f;
	DebugAlpha = Alpha;
	if (Alpha <= 0.f)
	{
		DebugAngleDeg = 0.f; // node is skipped at alpha 0
	}
}

void FAnimNode_MidearthOrientationWarping::UpdateInternal(const FAnimationUpdateContext& Context)
{
	FAnimNode_SkeletalControlBase::UpdateInternal(Context);
	// If we just became relevant and haven't been initialized yet, then reset.
	if (!bIsFirstUpdate && UpdateCounter.HasEverBeenUpdated() && !UpdateCounter.WasSynchronizedCounter(Context.AnimInstanceProxy->GetUpdateCounter()))
	{
		RootMotionDeltaDirection = FVector::ZeroVector;
		RootMotionDeltaRotation = FQuat::Identity;
		CounterCompensateTargetAngleRad = 0.f;
		ActualOrientationAngleRad = 0.f;
		bIsFirstUpdate = true;
	}
	UpdateCounter.SynchronizeWith(Context.AnimInstanceProxy->GetUpdateCounter());
	// WarpingSpace RootBoneTransform: the offset root (UE5 FRootOffsetProvider), else the component.
	WarpingSpaceTransform = OffsetRoot ? OffsetRoot->Channel.RootBoneTransform : Context.AnimInstanceProxy->GetComponentTransform();
}

void FAnimNode_MidearthOrientationWarping::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	check(OutBoneTransforms.Num() == 0);
	if (!OffsetRoot || !OffsetRoot->Channel.bRootMotionValid)
	{
		return; // UE5: early exit on a missing root motion delta attribute
	}
	const float DeltaSeconds = Output.AnimInstanceProxy->GetDeltaSeconds();
	if (DeltaSeconds <= 0.f)
	{
		return;
	}
	const float MaxAngleCorrectionRad = FMath::DegreesToRadians(MaxCorrectionDegrees);
	const FVector RotationAxisVector = FVector::UpVector;
	FTransform RootMotionTransformDelta = OffsetRoot->Channel.RootMotionDelta;
	float TargetOrientationAngleRad = 0.f;

	// Locomotion direction (LastNonZeroVelocity) into root bone space, flattened along the rotation axis.
	FVector LocomotionForward = FVector::ZeroVector;
	if (LastNonZeroVelocity.SizeSquared() > SMALL_NUMBER)
	{
		LocomotionForward = WarpingSpaceTransform.InverseTransformVector(LastNonZeroVelocity);
		LocomotionForward.Normalize();
	}
	LocomotionForward = (LocomotionForward - FVector::DotProduct(RotationAxisVector, LocomotionForward) * RotationAxisVector).GetSafeNormal();

	FVector RootMotionDeltaTranslation = RootMotionTransformDelta.GetTranslation();
	const FQuat PreviousRootMotionDeltaRotation = RootMotionDeltaRotation;
	RootMotionDeltaRotation = RootMotionTransformDelta.GetRotation();
	RootMotionDeltaTranslation = RootMotionDeltaTranslation - FVector::DotProduct(RotationAxisVector, RootMotionDeltaTranslation) * RotationAxisVector;

	const float RootMotionDeltaSpeed = RootMotionDeltaTranslation.Size() / DeltaSeconds;
	if (RootMotionDeltaSpeed < MinRootMotionSpeedThreshold || LocomotionForward.IsNearlyZero())
	{
		// Under the threshold: snap the target angle to 0, the interpolation handles the delta.
		TargetOrientationAngleRad = 0.0f;
	}
	else
	{
		const FVector PreviousRootMotionDeltaDirection = RootMotionDeltaDirection;
		RootMotionDeltaDirection = RootMotionDeltaTranslation.GetSafeNormal(SMALL_NUMBER);
		if (RootMotionDeltaDirection.IsNearlyZero())
		{
			RootMotionDeltaDirection = PreviousRootMotionDeltaDirection;
		}
		TargetOrientationAngleRad = MidearthOrientationWarping::SignedAngleRadBetweenNormals(RootMotionDeltaDirection, LocomotionForward, RotationAxisVector);

		// Motion matching can play a clip going the other way for a few frames -> use the opposite direction (no over-rotation).
		if (LocomotionAngleDeltaThreshold > 0.f && FMath::Abs(FMath::RadiansToDegrees(TargetOrientationAngleRad)) > LocomotionAngleDeltaThreshold)
		{
			TargetOrientationAngleRad = FMath::UnwindRadians(TargetOrientationAngleRad + PI);
			RootMotionDeltaDirection = -RootMotionDeltaDirection;
		}

		// (GASP TargetTime 0: no look-ahead into the clip.)

		if (bCounterCompenstateInterpolationByRootMotion && !PreviousRootMotionDeltaDirection.IsNearlyZero(SMALL_NUMBER))
		{
			// Counter the interpolated angle by the root motion direction change, so the interpolation doesn't fight the clip.
			const float RootMotionDeltaAngleRad = MidearthOrientationWarping::SignedAngleRadBetweenNormals(RootMotionDeltaDirection,
				PreviousRootMotionDeltaRotation.UnrotateVector(PreviousRootMotionDeltaDirection), RotationAxisVector);
			const float MaxRootMotionDeltaToCompensateRad = FMath::DegreesToRadians(MaxRootMotionDeltaToCompensateDegrees);
			if (FMath::Abs(RootMotionDeltaAngleRad) < MaxRootMotionDeltaToCompensateRad)
			{
				CounterCompensateTargetAngleRad += RootMotionDeltaAngleRad;
				const float CounterCompensateAngle = FMath::FInterpTo(0.f, CounterCompensateTargetAngleRad, DeltaSeconds, CounterCompensateInterpSpeed);
				ActualOrientationAngleRad = FMath::UnwindRadians(ActualOrientationAngleRad + CounterCompensateAngle);
				CounterCompensateTargetAngleRad -= CounterCompensateAngle;
			}
		}

		// Rotate the root motion delta fully by the warped angle (UE5 OverrideRootMotion).
		const FVector WarpedRootMotionTranslationDelta = FQuat(RotationAxisVector, TargetOrientationAngleRad).RotateVector(RootMotionDeltaTranslation);
		RootMotionTransformDelta.SetTranslation(WarpedRootMotionTranslationDelta);
	}
	OffsetRoot->Channel.RootMotionDelta = RootMotionTransformDelta;

	// Interpolate the effective orientation (snap on the first update).
	if (RotationInterpSpeed > 0.f && !bIsFirstUpdate)
	{
		const float SmoothOrientationAngleRad = FMath::FInterpTo(ActualOrientationAngleRad, TargetOrientationAngleRad, DeltaSeconds, RotationInterpSpeed);
		ActualOrientationAngleRad = FMath::Clamp(SmoothOrientationAngleRad, ActualOrientationAngleRad - MaxAngleCorrectionRad, ActualOrientationAngleRad + MaxAngleCorrectionRad);
	}
	else
	{
		ActualOrientationAngleRad = TargetOrientationAngleRad;
	}
	ActualOrientationAngleRad = FMath::Clamp(ActualOrientationAngleRad, -MaxAngleCorrectionRad, MaxAngleCorrectionRad);
	float AppliedAngleRad = ActualOrientationAngleRad * ActualAlpha; // UE5 scales the stored angle by alpha
	DebugAngleDeg = FMath::RadiansToDegrees(AppliedAngleRad);
	ActualOrientationAngleRad = AppliedAngleRad;
	if (!FMath::IsFinite(AppliedAngleRad))
	{
		AppliedAngleRad = ActualOrientationAngleRad = 0.f;
	}

	const float DistributedAlpha = bFeetFollowWithoutLegIK ? 1.f : DistributedBoneOrientationAlpha;
	const float RootOffset = FMath::UnwindRadians(AppliedAngleRad * DistributedAlpha);
	// Rotate the root first (cheaply rotates the whole pose).
	if (!FMath::IsNearlyZero(RootOffset, KINDA_SMALL_NUMBER))
	{
		const FCompactPoseBoneIndex RootBoneIndex(0);
		FTransform RootBoneTransform(Output.Pose.GetComponentSpaceTransform(RootBoneIndex));
		RootBoneTransform.SetRotation(FQuat(RotationAxisVector, RootOffset) * RootBoneTransform.GetRotation());
		RootBoneTransform.NormalizeRotation();
		Output.Pose.SetComponentSpaceTransform(RootBoneIndex, RootBoneTransform);
	}
	// Spine counter-rotates the body orientation, spread over the spine bones.
	if (SpineBoneDataArray.Num() > 0 && !FMath::IsNearlyZero(DistributedAlpha, KINDA_SMALL_NUMBER))
	{
		for (const FSpineBone& BoneData : SpineBoneDataArray)
		{
			const FQuat SpineBoneCounterRotation = FQuat(RotationAxisVector, -AppliedAngleRad * DistributedAlpha * BoneData.Weight);
			FTransform SpineBoneTransform(Output.Pose.GetComponentSpaceTransform(BoneData.BoneIndex));
			SpineBoneTransform.SetRotation(SpineBoneCounterRotation * SpineBoneTransform.GetRotation());
			SpineBoneTransform.NormalizeRotation();
			Output.Pose.SetComponentSpaceTransform(BoneData.BoneIndex, SpineBoneTransform);
		}
	}
	// IK foot root takes the rest; IK feet keep their rotation (only their position follows the rotated parent).
	const float IKFootRootOrientationAlpha = 1.f - DistributedAlpha;
	if (IKFootRootBoneIndex != INDEX_NONE && !FMath::IsNearlyZero(IKFootRootOrientationAlpha, KINDA_SMALL_NUMBER))
	{
		FTransform IKFootRootTransform(Output.Pose.GetComponentSpaceTransform(IKFootRootBoneIndex));
		IKFootRootTransform.SetRotation(FQuat(RotationAxisVector, AppliedAngleRad * IKFootRootOrientationAlpha) * IKFootRootTransform.GetRotation());
		IKFootRootTransform.NormalizeRotation();
		Output.Pose.SetComponentSpaceTransform(IKFootRootBoneIndex, IKFootRootTransform);
		const FQuat IKFootRotation = FQuat(RotationAxisVector, -AppliedAngleRad * IKFootRootOrientationAlpha);
		for (const FCompactPoseBoneIndex& IKFootBoneIndex : IKFootBoneIndexArray)
		{
			FTransform IKFootBoneTransform(Output.Pose.GetComponentSpaceTransform(IKFootBoneIndex));
			IKFootBoneTransform.SetRotation(IKFootRotation * IKFootBoneTransform.GetRotation());
			IKFootBoneTransform.NormalizeRotation();
			Output.Pose.SetComponentSpaceTransform(IKFootBoneIndex, IKFootBoneTransform);
		}
	}
	bIsFirstUpdate = false;
}

bool FAnimNode_MidearthOrientationWarping::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	return OffsetRoot != nullptr && MidearthWarping::GetWarpingLevel() >= 2 && SpineBoneDataArray.Num() > 0
		&& IKFootRootBoneIndex != INDEX_NONE && IKFootBoneIndexArray.Num() > 0;
}

void FAnimNode_MidearthOrientationWarping::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	IKFootRootBone.Initialize(RequiredBones);
	IKFootRootBoneIndex = IKFootRootBone.GetCompactPoseIndex(RequiredBones);
	IKFootBoneIndexArray.Reset();
	for (FBoneReference& Bone : IKFootBones)
	{
		Bone.Initialize(RequiredBones);
		const FCompactPoseBoneIndex Index = Bone.GetCompactPoseIndex(RequiredBones);
		if (Index != INDEX_NONE)
		{
			IKFootBoneIndexArray.Add(Index);
		}
	}
	SpineBoneDataArray.Reset();
	for (FBoneReference& Bone : SpineBones)
	{
		Bone.Initialize(RequiredBones);
		const FCompactPoseBoneIndex Index = Bone.GetCompactPoseIndex(RequiredBones);
		if (Index != INDEX_NONE) // 4.27: skip bones this skeleton lacks (UE5 would disable the node)
		{
			FSpineBone Data;
			Data.BoneIndex = Index;
			SpineBoneDataArray.Add(Data);
		}
	}
	// Sort parent before child, then share the weight along each chain (UE5 InitializeBoneReferences).
	SpineBoneDataArray.Sort([](const FSpineBone& A, const FSpineBone& B) { return A.BoneIndex < B.BoneIndex; });
	TArray<int32, TInlineAllocator<20>> IndicesToUpdate;
	for (int32 Index = SpineBoneDataArray.Num() - 1; Index >= 0; Index--)
	{
		if (SpineBoneDataArray[Index].Weight == 0.f)
		{
			IndicesToUpdate.Reset(SpineBoneDataArray.Num());
			float ExistingWeight = 0.f;
			IndicesToUpdate.Add(Index);
			const FCompactPoseBoneIndex CompactBoneIndex = SpineBoneDataArray[Index].BoneIndex;
			for (int32 ParentIndex = Index - 1; ParentIndex >= 0; ParentIndex--)
			{
				if (RequiredBones.BoneIsChildOf(CompactBoneIndex, SpineBoneDataArray[ParentIndex].BoneIndex))
				{
					if (SpineBoneDataArray[ParentIndex].Weight > 0.f)
					{
						ExistingWeight += SpineBoneDataArray[ParentIndex].Weight;
					}
					else
					{
						IndicesToUpdate.Add(ParentIndex);
					}
				}
			}
			const float IndividualWeight = (1.f - ExistingWeight) / float(IndicesToUpdate.Num());
			for (int32 UpdateIndex : IndicesToUpdate)
			{
				SpineBoneDataArray[UpdateIndex].Weight = IndividualWeight;
			}
		}
	}
}

void FAnimNode_MidearthOrientationWarping::GatherDebugData(FNodeDebugData& DebugData)
{
	FString DebugLine = DebugData.GetNodeName(this);
	DebugLine += FString::Printf(TEXT("(Orientation Angle: %.1f deg, alpha %.2f)"), FMath::RadiansToDegrees(ActualOrientationAngleRad), ActualAlpha);
	DebugData.AddDebugItem(DebugLine);
	ComponentPose.GatherDebugData(DebugData);
}
