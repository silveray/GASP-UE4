#include "AnimNode_MidearthFootIK.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MotionMatchingComponent.h"
#include "TwoBoneIK.h"

FAnimNode_MidearthFootIK::FAnimNode_MidearthFootIK()
	: Pelvis(TEXT("pelvis"))
	, ThighL(TEXT("thigh_l")), CalfL(TEXT("calf_l")), FootL(TEXT("foot_l"))
	, ThighR(TEXT("thigh_r")), CalfR(TEXT("calf_r")), FootR(TEXT("foot_r"))
{
}

void FAnimNode_MidearthFootIK::PreUpdate(const UAnimInstance* InAnimInstance)
{
	USkeletalMeshComponent* Mesh = InAnimInstance ? InAnimInstance->GetSkelMeshComponent() : nullptr;
	const ACharacter* Character = Mesh ? Cast<ACharacter>(Mesh->GetOwner()) : nullptr;
	UWorld* World = Mesh ? Mesh->GetWorld() : nullptr;
	if (!Character || !World || !Character->GetCharacterMovement())
	{
		return;
	}
	const float Dt = World->GetDeltaSeconds();
	const FTransform MeshXf = Mesh->GetComponentTransform();
	const bool bGround = Character->GetCharacterMovement()->IsMovingOnGround();

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MidearthFootIK), false, Character);
	// Reference = ground under the character's centre (the movement keeps the capsule ~2 cm above the floor), so flat ground = 0.
	float GroundZ = MeshXf.GetLocation().Z;
	{
		FHitResult Hit;
		const FVector C = MeshXf.GetLocation();
		if (bGround && World->LineTraceSingleByChannel(Hit, C + FVector(0.f, 0.f, TraceUp), C - FVector(0.f, 0.f, TraceDown), ECC_Visibility, Params))
		{
			GroundZ = Hit.ImpactPoint.Z;
		}
	}
	auto TraceFoot = [&](const FBoneReference& Foot, float& InOutOffset, FVector& InOutNormal)
	{
		float Target = 0.f;
		FVector Normal = FVector::UpVector;
		if (bGround)
		{
			const FVector FootWS = Mesh->GetSocketLocation(Foot.BoneName);
			const FVector Base(FootWS.X, FootWS.Y, MeshXf.GetLocation().Z);
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Base + FVector(0.f, 0.f, TraceUp), Base - FVector(0.f, 0.f, TraceDown), ECC_Visibility, Params)
				&& Hit.ImpactNormal.Z > 0.6f)
			{
				Target = FMath::Clamp(Hit.ImpactPoint.Z - GroundZ, -MaxFootOffset, MaxFootOffset);
				Normal = MeshXf.InverseTransformVectorNoScale(Hit.ImpactNormal);
			}
		}
		InOutOffset = FMath::FInterpTo(InOutOffset, Target, Dt, InterpSpeed);
		InOutNormal = FMath::VInterpNormalRotationTo(InOutNormal, Normal, Dt, 360.f).GetSafeNormal();
	};
	TraceFoot(FootL, OffsetL, NormalL);
	TraceFoot(FootR, OffsetR, NormalR);
	PelvisOffset = FMath::Clamp(FMath::Min3(OffsetL, OffsetR, 0.f), -MaxPelvisDrop, 0.f);

	if (UMotionMatchingComponent* MM = Character->FindComponentByClass<UMotionMatchingComponent>())
	{
		MM->FootIKPelvis = PelvisOffset;
		MM->FootIKLeft = OffsetL;
		MM->FootIKRight = OffsetR;
	}
}

void FAnimNode_MidearthFootIK::EvaluateSkeletalControl_AnyThread(FComponentSpacePoseContext& Output, TArray<FBoneTransform>& OutBoneTransforms)
{
	if (FMath::Abs(OffsetL) < 0.1f && FMath::Abs(OffsetR) < 0.1f && FMath::Abs(PelvisOffset) < 0.1f)
	{
		return; // flat ground: leave the animation alone
	}
	const FBoneContainer& Bones = Output.Pose.GetPose().GetBoneContainer();
	const FVector PelvisShift(0.f, 0.f, PelvisOffset);

	const FCompactPoseBoneIndex PelvisIndex = Pelvis.GetCompactPoseIndex(Bones);
	FTransform PelvisCS = Output.Pose.GetComponentSpaceTransform(PelvisIndex);
	PelvisCS.AddToTranslation(PelvisShift);
	OutBoneTransforms.Add(FBoneTransform(PelvisIndex, PelvisCS));

	auto SolveLeg = [&](const FBoneReference& Thigh, const FBoneReference& Calf, const FBoneReference& Foot, float Offset, const FVector& Normal)
	{
		const FCompactPoseBoneIndex TI = Thigh.GetCompactPoseIndex(Bones), CI = Calf.GetCompactPoseIndex(Bones), FI = Foot.GetCompactPoseIndex(Bones);
		FTransform T = Output.Pose.GetComponentSpaceTransform(TI);
		FTransform C = Output.Pose.GetComponentSpaceTransform(CI);
		FTransform F = Output.Pose.GetComponentSpaceTransform(FI);
		const FVector Effector = F.GetLocation() + FVector(0.f, 0.f, Offset); // foot onto the ground under it
		T.AddToTranslation(PelvisShift);
		C.AddToTranslation(PelvisShift);
		F.AddToTranslation(PelvisShift);
		// Knee keeps bending the way it already does.
		const FVector Bend = (C.GetLocation() - 0.5f * (T.GetLocation() + F.GetLocation())).GetSafeNormal();
		const FVector JointTarget = C.GetLocation() + Bend * 50.f;
		const FQuat FootRot = F.GetRotation();
		AnimationCore::SolveTwoBoneIK(T, C, F, JointTarget, Effector, false, 1.f, 1.f);
		// Tilt the foot with the ground (limited).
		const FQuat Tilt = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
		const float Angle = FMath::RadiansToDegrees(Tilt.GetAngle());
		const float Alpha = Angle > KINDA_SMALL_NUMBER ? FMath::Min(1.f, MaxFootTilt / Angle) : 0.f;
		F.SetRotation(FQuat::Slerp(FQuat::Identity, Tilt, Alpha) * FootRot);
		OutBoneTransforms.Add(FBoneTransform(TI, T));
		OutBoneTransforms.Add(FBoneTransform(CI, C));
		OutBoneTransforms.Add(FBoneTransform(FI, F));
	};
	SolveLeg(ThighL, CalfL, FootL, OffsetL, NormalL);
	SolveLeg(ThighR, CalfR, FootR, OffsetR, NormalR);
	OutBoneTransforms.Sort(FCompareBoneTransformIndex());
}

bool FAnimNode_MidearthFootIK::IsValidToEvaluate(const USkeleton* Skeleton, const FBoneContainer& RequiredBones)
{
	return Pelvis.IsValidToEvaluate(RequiredBones) && ThighL.IsValidToEvaluate(RequiredBones) && CalfL.IsValidToEvaluate(RequiredBones)
		&& FootL.IsValidToEvaluate(RequiredBones) && ThighR.IsValidToEvaluate(RequiredBones) && CalfR.IsValidToEvaluate(RequiredBones)
		&& FootR.IsValidToEvaluate(RequiredBones);
}

void FAnimNode_MidearthFootIK::InitializeBoneReferences(const FBoneContainer& RequiredBones)
{
	for (FBoneReference* B : { &Pelvis, &ThighL, &CalfL, &FootL, &ThighR, &CalfR, &FootR })
	{
		B->Initialize(RequiredBones);
	}
}
