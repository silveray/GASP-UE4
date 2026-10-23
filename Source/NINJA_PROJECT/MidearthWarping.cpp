// See MidearthWarping.h.
#include "MidearthWarping.h"

#include "AnimNode_MidearthOffsetRoot.h"
#include "MotionMatchingComponent.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarGaspWarping(TEXT("gasp.Warping"), 2, // 10-09 step 2: on (GASP movement model)
	TEXT("GASP warping nodes: 0 = off (pose passes through), 1 = Offset Root only, 2 = Offset Root + Steering + Orientation Warping."));

int32 MidearthWarping::GetWarpingLevel()
{
	return CVarGaspWarping.GetValueOnAnyThread();
}

void MidearthWarping::GatherGameState(const UAnimInstance* AnimInstance, FGameState& State)
{
	State = FGameState();
	const USkeletalMeshComponent* Mesh = AnimInstance ? AnimInstance->GetSkelMeshComponent() : nullptr;
	const ACharacter* Character = Mesh ? Cast<ACharacter>(Mesh->GetOwner()) : nullptr;
	const UCharacterMovementComponent* Move = Character ? Character->GetCharacterMovement() : nullptr;
	if (!Move)
	{
		return;
	}
	State.bValid = true;
	State.Velocity = Move->Velocity;
	State.Acceleration = Move->GetCurrentAcceleration();
	State.bMoving = !Move->Velocity.Equals(FVector::ZeroVector, 0.1f) && !Move->GetCurrentAcceleration().Equals(FVector::ZeroVector, KINDA_SMALL_NUMBER);
	State.bOnGround = Move->IsMovingOnGround();
	State.RootMotionTranslationScale = Character->GetAnimRootMotionTranslationScale();
	FVector Facing = Character->GetActorForwardVector().GetSafeNormal2D();
	if (const UMotionMatchingComponent* MM = Character->FindComponentByClass<UMotionMatchingComponent>())
	{
		// GASP "DefaultSlot active" = a montage owns the body. Also the battle stance: it keeps the root-motion model, so the body follows
		// the capsule (Offset Root Release, no Steering / Orientation Warping) exactly as before step 2.
		State.bGaspModel = MM->IsGaspMovementModelEnabled();
		State.bLocomotionOwned = !MM->IsSuspended() && (!MM->IsCombatStance() || Character->bIsCrouched);
		State.bSlotActive = MM->IsSuspended() || !MM->UsesGaspMovementModel();
		State.bTurnInPlace = MM->IsPlayingTurnInPlace();
		State.CurrentClip = MM->GetCurrentClip();
		State.CurrentClipTime = MM->GetCurrentClipTime();
		State.bCurrentClipLoops = MM->IsCurrentClipLooping();
		Facing = MM->GetDesiredFacing();
	}
	// Component orientation = actor facing x the mesh's relative rotation (UEFN mesh: yaw -90).
	State.DesiredFacing = Facing.Rotation().Quaternion() * Mesh->GetRelativeRotation().Quaternion();
}

FAnimNode_MidearthOffsetRoot* MidearthWarping::FindOffsetRootNode(const FAnimationBaseContext& Context)
{
	const IAnimClassInterface* AnimClass = Context.AnimInstanceProxy ? Context.AnimInstanceProxy->GetAnimClassInterface() : nullptr;
	UObject* Instance = Context.AnimInstanceProxy ? Context.AnimInstanceProxy->GetAnimInstanceObject() : nullptr;
	if (!AnimClass || !Instance)
	{
		return nullptr;
	}
	for (FStructProperty* Prop : AnimClass->GetAnimNodeProperties())
	{
		if (Prop && Prop->Struct && Prop->Struct->IsChildOf(FAnimNode_MidearthOffsetRoot::StaticStruct()))
		{
			return Prop->ContainerPtrToValuePtr<FAnimNode_MidearthOffsetRoot>(Instance);
		}
	}
	return nullptr;
}

FAnimNode_MidearthOffsetRoot* MidearthWarping::FindOffsetRootNode(UAnimInstance* AnimInstance)
{
	const IAnimClassInterface* AnimClass = AnimInstance ? IAnimClassInterface::GetFromClass(AnimInstance->GetClass()) : nullptr;
	if (!AnimClass)
	{
		return nullptr;
	}
	for (FStructProperty* Prop : AnimClass->GetAnimNodeProperties())
	{
		if (Prop && Prop->Struct && Prop->Struct->IsChildOf(FAnimNode_MidearthOffsetRoot::StaticStruct()))
		{
			return Prop->ContainerPtrToValuePtr<FAnimNode_MidearthOffsetRoot>(AnimInstance);
		}
	}
	return nullptr;
}

void* MidearthWarping::FindAnimNode(UAnimInstance* AnimInstance, const UScriptStruct* Struct)
{
	const IAnimClassInterface* AnimClass = AnimInstance ? IAnimClassInterface::GetFromClass(AnimInstance->GetClass()) : nullptr;
	for (FStructProperty* Prop : AnimClass ? AnimClass->GetAnimNodeProperties() : TArray<FStructProperty*>())
	{
		if (Prop && Prop->Struct && Prop->Struct->IsChildOf(Struct))
		{
			return Prop->ContainerPtrToValuePtr<void>(AnimInstance);
		}
	}
	return nullptr;
}

bool MidearthWarping::GetClipCurveValue(const UAnimSequence* Clip, FName CurveName, float Time, float& OutValue)
{
	if (!Clip)
	{
		return false;
	}
	for (const FFloatCurve& Curve : Clip->GetCurveData().FloatCurves)
	{
		if (Curve.Name.DisplayName == CurveName)
		{
			OutValue = Curve.Evaluate(Time);
			return true;
		}
	}
	return false;
}

FVector MidearthWarping::QuatToRotationVector(const FQuat& Q)
{
	// FQuat::ToRotationVector (UE5): axis * angle, of the shortest arc.
	FQuat S = Q.W < 0.f ? FQuat(-Q.X, -Q.Y, -Q.Z, -Q.W) : Q;
	S.Normalize();
	FVector Axis;
	float Angle;
	S.ToAxisAndAngle(Axis, Angle);
	return Axis * Angle;
}

FQuat MidearthWarping::QuatFromRotationVector(const FVector& V)
{
	const float Angle = V.Size();
	return Angle < SMALL_NUMBER ? FQuat::Identity : FQuat(V / Angle, Angle);
}

void MidearthWarping::CriticalSpringDamperQuat(FQuat& InOutRotation, FVector& InOutAngularVelocityRadians, const FQuat& TargetRotation, float SmoothingTime, float DeltaTime)
{
	// UE5 SpringMath::CriticalSpringDamperQuat, line for line.
	if (SmoothingTime < SMALL_NUMBER)
	{
		InOutRotation = TargetRotation;
		InOutAngularVelocityRadians = FVector::ZeroVector;
		return;
	}
	const float Y = (4.0f / FMath::Max(SmoothingTime, SMALL_NUMBER)) / 2.0f; // SmoothingTimeToDamping / 2
	const FVector J0 = QuatToRotationVector(InOutRotation * TargetRotation.Inverse());
	const FVector J1 = InOutAngularVelocityRadians + J0 * Y;
	const float EyDt = InvExpApprox(Y * DeltaTime);
	InOutRotation = QuatFromRotationVector(EyDt * (J0 + J1 * DeltaTime)) * TargetRotation;
	InOutAngularVelocityRadians = EyDt * (InOutAngularVelocityRadians - J1 * Y * DeltaTime);
}
