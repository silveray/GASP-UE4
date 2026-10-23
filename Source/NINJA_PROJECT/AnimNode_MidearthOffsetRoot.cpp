// Port of UE 5.8 AnimationWarping AnimNode_OffsetRootBone.cpp (see the header for the 4.27 differences).
#include "AnimNode_MidearthOffsetRoot.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"

namespace MidearthOffsetRoot
{
	static TAutoConsoleVariable<int32> CVarRootMotionSource(TEXT("gasp.WarpRootMotionSource"), 0,
		TEXT("Offset Root input: 0 = all playing montages blended by weight (GASP blend stack), 1 = newest root-motion montage only (4.27 capsule rule, test)"));

	// 4.27 keeps FAnimInstanceProxy::GetMontageEvaluationData protected (UE5 nodes read the root-motion attribute instead).
	struct FProxyAccess : public FAnimInstanceProxy
	{
		static const TArray<FMontageEvaluationState>& MontageData(const FAnimInstanceProxy* Proxy)
		{
			return static_cast<const FProxyAccess*>(Proxy)->GetMontageEvaluationData();
		}
	};

	// UE5 UE::Anim::OffsetRootBone::DamperImplicit (https://theorangeduck.com/page/spring-roll-call#implicitspringdamper)
	static float DamperImplicit(float Halflife, float DeltaTime, float Epsilon = 1e-8f)
	{
		Halflife = FMath::Max(Halflife, DeltaTime);
		return FMath::Clamp(1.0f - MidearthWarping::InvExpApprox((0.69314718056f * DeltaTime) / (Halflife + Epsilon)), 0.0f, 1.0f);
	}

	static bool ShouldExtractRootMotion(EMidearthOffsetRootMode Mode)
	{
		return Mode == EMidearthOffsetRootMode::Accumulate || Mode == EMidearthOffsetRootMode::Interpolate;
	}

	static bool ShouldCounterComponentDelta(EMidearthOffsetRootMode Mode)
	{
		return Mode == EMidearthOffsetRootMode::Release;
	}
}

void FAnimNode_MidearthOffsetRoot::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	FAnimNode_Base::Initialize_AnyThread(Context);
	AnimInstanceProxy = Context.AnimInstanceProxy;
	Source.Initialize(Context);
	Reset(Context);
}

void FAnimNode_MidearthOffsetRoot::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	Source.CacheBones(Context);
}

void FAnimNode_MidearthOffsetRoot::PreUpdate(const UAnimInstance* InAnimInstance)
{
	MidearthWarping::GatherGameState(InAnimInstance, GameState);
	if (bUseGaspDrivers && GameState.bValid)
	{
		// GASP Get_OffsetRootRotationMode / Get_OffsetRootTranslationMode / Get_OffsetRootTranslationHalfLife; bClampToTranslationVelocity = IsMoving.
		RotationMode = GameState.bSlotActive ? EMidearthOffsetRootMode::Release : EMidearthOffsetRootMode::Accumulate;
		TranslationMode = (GameState.bSlotActive || !GameState.bOnGround) ? EMidearthOffsetRootMode::Release
			: (GameState.bMoving ? EMidearthOffsetRootMode::Interpolate : EMidearthOffsetRootMode::Release);
		TranslationHalflife = GameState.bMoving ? 0.3f : 0.1f;
		bClampToTranslationVelocity = GameState.bMoving;
	}
}

void FAnimNode_MidearthOffsetRoot::MeasureRootMotion(const FAnimationUpdateContext& Context)
{
	// UE5 gets this from the root-motion attribute of the sub-graph. 4.27: position change of every playing montage this frame,
	// blended by montage weight (like FRootMotionMovementParams::AccumulateWithBlend), x the character's translation scale.
	Channel.RootMotionDelta = FTransform::Identity;
	Channel.bRootMotionValid = false;
	FVector Translation = FVector::ZeroVector;
	FQuat Rotation(0.f, 0.f, 0.f, 0.f);
	float TotalWeight = 0.f;
	TMap<TWeakObjectPtr<UAnimMontage>, float> Seen;
	const TArray<FMontageEvaluationState>& Montages = MidearthOffsetRoot::FProxyAccess::MontageData(Context.AnimInstanceProxy);
	// gasp.WarpRootMotionSource 1 (test only): measure like 4.27's capsule does with RootMotionFromMontagesOnly = the newest root-motion
	// montage at full weight, no blend. Proves the measurement while root motion still moves the capsule (gap must stay ~0).
	int32 OnlyIndex = INDEX_NONE;
	if (MidearthOffsetRoot::CVarRootMotionSource.GetValueOnAnyThread() == 1)
	{
		for (int32 i = Montages.Num() - 1; i >= 0 && OnlyIndex == INDEX_NONE; --i)
		{
			if (Montages[i].Montage.IsValid() && Montages[i].Montage->HasRootMotion() && Montages[i].bIsActive)
			{
				OnlyIndex = i;
			}
		}
	}
	for (int32 Index = 0; Index < Montages.Num(); ++Index)
	{
		const FMontageEvaluationState& State = Montages[Index];
		UAnimMontage* Montage = State.Montage.Get();
		if (!Montage || State.MontageWeight <= ZERO_ANIMWEIGHT_THRESH)
		{
			continue;
		}
		if (OnlyIndex != INDEX_NONE && Index != OnlyIndex)
		{
			Seen.Add(Montage, State.MontagePosition);
			continue;
		}
		const float Cur = State.MontagePosition;
		const float* Last = LastMontagePositions.Find(Montage);
		const float Prev = Last ? *Last : FMath::Max(0.f, Cur - CachedDeltaTime);
		Seen.Add(Montage, Cur);
		FTransform Delta = FTransform::Identity;
		if (Cur >= Prev)
		{
			Delta = Montage->ExtractRootMotionFromTrackRange(Prev, Cur);
		}
		else if (Prev - Cur > Montage->SequenceLength * 0.5f)
		{
			// looping clip wrapped: end of the loop, then the start (Total = Second * First, like FRootMotionMovementParams)
			const FTransform First = Montage->ExtractRootMotionFromTrackRange(Prev, Montage->SequenceLength);
			const FTransform Second = Montage->ExtractRootMotionFromTrackRange(0.f, Cur);
			Delta = Second * First;
		}
		Delta.SetTranslation(Delta.GetTranslation() * GameState.RootMotionTranslationScale);
		const float W = OnlyIndex != INDEX_NONE ? 1.f : State.MontageWeight;
		Translation += Delta.GetTranslation() * W;
		FQuat Q = Delta.GetRotation();
		if ((Rotation | Q) < 0.f)
		{
			Q = FQuat(-Q.X, -Q.Y, -Q.Z, -Q.W);
		}
		Rotation = FQuat(Rotation.X + Q.X * W, Rotation.Y + Q.Y * W, Rotation.Z + Q.Z * W, Rotation.W + Q.W * W);
		TotalWeight += W;
	}
	LastMontagePositions = MoveTemp(Seen);
	if (TotalWeight > ZERO_ANIMWEIGHT_THRESH)
	{
		Rotation.Normalize();
		Channel.RootMotionDelta = FTransform(Rotation, Translation / TotalWeight);
		Channel.bRootMotionValid = true;
	}
}

void FAnimNode_MidearthOffsetRoot::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	CachedDeltaTime = Context.GetDeltaTime();
	GetEvaluateGraphExposedInputs().Execute(Context);
	// If we just became relevant and haven't been initialized yet, then reset.
	if (!bIsFirstUpdate && UpdateCounter.HasEverBeenUpdated() && !UpdateCounter.WasSynchronizedCounter(Context.AnimInstanceProxy->GetUpdateCounter()))
	{
		Reset(Context);
	}
	UpdateCounter.SynchronizeWith(Context.AnimInstanceProxy->GetUpdateCounter());
	MeasureRootMotion(Context);
	// UE5: TScopedGraphMessage<FRootOffsetProvider>(SimulatedRotation, SimulatedTranslation) for the nodes below.
	Channel.RootBoneTransform = FTransform(SimulatedRotation, SimulatedTranslation);
	Source.Update(Context);
}

void FAnimNode_MidearthOffsetRoot::Evaluate_AnyThread(FPoseContext& Output)
{
	Source.Evaluate(Output); // Steering / Orientation Warping edit Channel.RootMotionDelta in here
	const bool bOn = MidearthWarping::GetWarpingLevel() >= 1 && GameState.bGaspModel;
	if (!bOn)
	{
		// passthrough, but keep following the capsule so switching on doesn't pop
		ComponentTransform = AnimInstanceProxy->GetComponentTransform();
		SimulatedTranslation = ComponentTransform.GetLocation();
		SimulatedRotation = ComponentTransform.GetRotation();
		DebugTranslationError = DebugYawError = 0.f;
		return;
	}

	const FCompactPoseBoneIndex TargetBoneIndex(0);
	const FTransform InputBoneTransform = Output.Pose[TargetBoneIndex];

	FTransform LastComponentTransform = ComponentTransform;
	ComponentTransform = AnimInstanceProxy->GetComponentTransform();

	// Teleport: carry the offset over to the new place (bResetOnTeleport false in GASP).
	const float TeleportDistanceThreshold = AnimInstanceProxy->GetSkelMeshComponent()->GetTeleportDistanceThreshold();
	if (FVector::Distance(LastComponentTransform.GetLocation(), ComponentTransform.GetLocation()) > TeleportDistanceThreshold)
	{
		const FTransform OffsetTransform = FTransform(SimulatedRotation, SimulatedTranslation).GetRelativeTransformReverse(LastComponentTransform);
		const FTransform TeleportedTransform = OffsetTransform * ComponentTransform;
		SimulatedTranslation = TeleportedTransform.GetLocation();
		SimulatedRotation = TeleportedTransform.GetRotation();
		LastComponentTransform = ComponentTransform;
	}

	const EMidearthOffsetRootMode CurrentTranslationMode = TranslationMode;
	const EMidearthOffsetRootMode CurrentRotationMode = RotationMode;
	const bool bShouldConsumeTranslationOffset = MidearthOffsetRoot::ShouldExtractRootMotion(CurrentTranslationMode);
	const bool bShouldConsumeRotationOffset = MidearthOffsetRoot::ShouldExtractRootMotion(CurrentRotationMode);

	FTransform RootMotionTransformDelta = Channel.bRootMotionValid ? Channel.RootMotionDelta : FTransform::Identity;
	RootMotionTransformDelta.NormalizeRotation();
	DebugRootMotionSpeed = CachedDeltaTime > 0.f ? RootMotionTransformDelta.GetTranslation().Size() / CachedDeltaTime : 0.f;
	const float MaxTranslationOffset = MaxTranslationError;

	FTransform ConsumedRootMotionDelta = FTransform::Identity;
	if (bShouldConsumeTranslationOffset)
	{
		ConsumedRootMotionDelta.SetTranslation(RootMotionTransformDelta.GetTranslation());
	}
	if (bShouldConsumeRotationOffset)
	{
		ConsumedRootMotionDelta.SetRotation(RootMotionTransformDelta.GetRotation());
	}
	if (MidearthOffsetRoot::ShouldCounterComponentDelta(CurrentRotationMode))
	{
		const FQuat ComponentRotationDelta = LastComponentTransform.GetRotation().Inverse() * ComponentTransform.GetRotation();
		SimulatedRotation = ComponentRotationDelta * SimulatedRotation;
	}
	if (MidearthOffsetRoot::ShouldCounterComponentDelta(CurrentTranslationMode))
	{
		SimulatedTranslation += ComponentTransform.GetLocation() - LastComponentTransform.GetLocation();
	}

	FTransform SimulatedTransform(SimulatedRotation, SimulatedTranslation);
	SimulatedTransform = ConsumedRootMotionDelta * SimulatedTransform;
	SimulatedTranslation = SimulatedTransform.GetLocation();
	SimulatedRotation = SimulatedTransform.GetRotation();

	if (bOnGround)
	{
		SimulatedTranslation = FVector::PointPlaneProject(SimulatedTranslation, ComponentTransform.GetLocation(), GroundNormal);
	}

	if (CurrentTranslationMode == EMidearthOffsetRootMode::Release || CurrentTranslationMode == EMidearthOffsetRootMode::Interpolate)
	{
		const FVector TranslationOffset = ComponentTransform.GetLocation() - SimulatedTranslation;
		const float DampenAlpha = MidearthOffsetRoot::DamperImplicit(TranslationHalflife, CachedDeltaTime);
		FVector TranslationOffsetDelta = FMath::Lerp(FVector::ZeroVector, TranslationOffset, DampenAlpha);
		if (bClampToTranslationVelocity)
		{
			const float RootMotionDelta = RootMotionTransformDelta.GetLocation().Size();
			const float MaxDelta = TranslationSpeedRatio * RootMotionDelta;
			if (TranslationOffsetDelta.Size() > MaxDelta)
			{
				TranslationOffsetDelta = MaxDelta * TranslationOffsetDelta.GetSafeNormal2D();
			}
		}
		SimulatedTranslation = SimulatedTranslation + TranslationOffsetDelta;
	}

	if (CurrentRotationMode == EMidearthOffsetRootMode::Release || CurrentRotationMode == EMidearthOffsetRootMode::Interpolate)
	{
		FQuat RotationOffset = ComponentTransform.GetRotation() * SimulatedRotation.Inverse();
		RotationOffset.Normalize();
		RotationOffset = RotationOffset.W < 0.0f ? FQuat(-RotationOffset.X, -RotationOffset.Y, -RotationOffset.Z, -RotationOffset.W) : RotationOffset;
		const float DampenAlpha = MidearthOffsetRoot::DamperImplicit(RotationHalfLife, CachedDeltaTime);
		const FQuat RotationOffsetDelta = FQuat::Slerp(FQuat::Identity, RotationOffset, DampenAlpha);
		SimulatedRotation = RotationOffsetDelta * SimulatedRotation;
	}

	if (MaxTranslationOffset >= 0.0f)
	{
		FVector TranslationOffset = ComponentTransform.GetLocation() - SimulatedTranslation;
		if (TranslationOffset.Size() > MaxTranslationOffset)
		{
			TranslationOffset = TranslationOffset.GetClampedToMaxSize(MaxTranslationOffset);
			SimulatedTranslation = ComponentTransform.GetLocation() - TranslationOffset;
		}
	}

	if (MaxRotationError >= 0.0f)
	{
		const float MaxAngleRadians = FMath::DegreesToRadians(MaxRotationError);
		FQuat RotationOffset = ComponentTransform.GetRotation().Inverse() * SimulatedRotation;
		RotationOffset.Normalize();
		RotationOffset = RotationOffset.W < 0.0f ? FQuat(-RotationOffset.X, -RotationOffset.Y, -RotationOffset.Z, -RotationOffset.W) : RotationOffset;
		FVector OffsetAxis;
		float OffsetAngle;
		RotationOffset.ToAxisAndAngle(OffsetAxis, OffsetAngle);
		if (FMath::Abs(OffsetAngle) > MaxAngleRadians)
		{
			SimulatedRotation = FQuat(OffsetAxis, MaxAngleRadians) * ComponentTransform.GetRotation();
			SimulatedRotation.Normalize();
		}
	}

	SimulatedTransform.SetLocation(SimulatedTranslation);
	SimulatedTransform.SetRotation(SimulatedRotation);

	// Start with the input pose's bone transform, accumulate the simulated transform and counter the component transform.
	FTransform TargetBoneTransform = InputBoneTransform;
	TargetBoneTransform.Accumulate(SimulatedTransform * ComponentTransform.Inverse());
	TargetBoneTransform.SetScale3D(InputBoneTransform.GetScale3D());
	TargetBoneTransform.NormalizeRotation();
	Output.Pose[TargetBoneIndex] = TargetBoneTransform;

	DebugTranslationError = FVector::Dist2D(SimulatedTranslation, ComponentTransform.GetLocation());
	DebugYawError = FMath::Abs(FMath::FindDeltaAngleDegrees(SimulatedRotation.Rotator().Yaw, ComponentTransform.Rotator().Yaw));
	bIsFirstUpdate = false;
}

void FAnimNode_MidearthOffsetRoot::GatherDebugData(FNodeDebugData& DebugData)
{
	FString DebugLine = DebugData.GetNodeName(this);
	DebugLine += FString::Printf(TEXT("(gap %.1f cm / %.1f deg, root motion %.0f cm/s)"), DebugTranslationError, DebugYawError, DebugRootMotionSpeed);
	DebugData.AddDebugItem(DebugLine);
	Source.GatherDebugData(DebugData);
}

void FAnimNode_MidearthOffsetRoot::Reset(const FAnimationBaseContext& Context)
{
	ComponentTransform = Context.AnimInstanceProxy->GetComponentTransform();
	SimulatedTranslation = ComponentTransform.GetLocation();
	SimulatedRotation = ComponentTransform.GetRotation();
	LastMontagePositions.Reset();
	bIsFirstUpdate = true;
}
