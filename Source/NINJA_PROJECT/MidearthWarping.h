// Shared pieces of the GASP warping port (10-09): Offset Root, Steering, Orientation Warping (AnimNode_Midearth*.h).
// UE5 passes the animation's per-frame root motion between those nodes as a root-motion attribute and the offset-root
// transform as a graph message; UE 4.27 has neither. Here the Offset Root node owns an FMidearthWarpChannel and the
// Steering / Orientation Warping nodes below it in the graph find that node at Initialize and read/write its channel
// while it evaluates them (same order as UE5: Source evaluates first, then Offset Root consumes the delta).
// CVar `gasp.Warping`: 0 = every node passes the pose through, 1 = Offset Root only, 2 = all (DEFAULT since step 2, 10-09: the CMC
// moves + turns the capsule like GASP, UMotionMatchingComponent::bGaspMovementModel).
// Setup: nothing here - see the node headers.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNodeBase.h"

class UAnimInstance;
class UAnimSequence;
struct FAnimNode_MidearthOffsetRoot;

namespace MidearthWarping
{
	/** gasp.Warping value (any thread). */
	int32 GetWarpingLevel();

	/** Anim-thread data the Offset Root node shares with the warping nodes it evaluates (UE5: root motion attribute + FRootOffsetProvider). */
	struct FChannel
	{
		/** This frame's root motion of the playing animation, root/component space (cm, rotation). Steering / Orientation Warping edit it. */
		FTransform RootMotionDelta = FTransform::Identity;
		bool bRootMotionValid = false;
		/** World transform of the offset root (the visible body's root) - GASP 'RootBoneTransform' warping space. */
		FTransform RootBoneTransform = FTransform::Identity;
	};

	/** Game-thread state the GASP ABP functions read (Update_EssentialValues / IsMoving / Get_DesiredFacing ...), taken in PreUpdate. */
	struct FGameState
	{
		bool bValid = false;
		/** GASP IsMoving: velocity != 0 (tolerance 0.1) AND acceleration != 0. */
		bool bMoving = false;
		bool bOnGround = true;
		/** GASP 'DefaultSlot is active' = a montage owns the body. Here: the motion matching component is suspended (traversal, attacks...). */
		bool bSlotActive = false;
		/** UMotionMatchingComponent::UsesGaspMovementModel() (gasp.MovementModel 1). Off = every warping node passes the pose through. */
		bool bGaspModel = false;
		/** GASP database tag TurnInPlace. */
		bool bTurnInPlace = false;
		/** Character velocity (world). */
		FVector Velocity = FVector::ZeroVector;
		/** CMC acceleration = the move input (world). */
		FVector Acceleration = FVector::ZeroVector;
		/** Motion matching owns the body (not suspended by a montage, not the battle stance unless crouched) - Orientation Warping may run. */
		bool bLocomotionOwned = false;
		/** GASP Get_DesiredFacing in component orientation (actor facing x mesh relative rotation), world. */
		FQuat DesiredFacing = FQuat::Identity;
		UAnimSequence* CurrentClip = nullptr;
		float CurrentClipTime = 0.f;
		/** The clip loops (motion matching database entry bLoop; 4.27 sequences have no bLoop). */
		bool bCurrentClipLoops = false;
		/** Root motion translation scale of the character (StrideScale) - the capsule moves this much more than the clip. */
		float RootMotionTranslationScale = 1.f;
	};

	/** Fills State from the anim instance's owner (Character + UMotionMatchingComponent). Game thread. */
	void GatherGameState(const UAnimInstance* AnimInstance, FGameState& State);

	/** The Offset Root node of the same anim instance (null if the graph has none). Any thread, read-only class data. */
	FAnimNode_MidearthOffsetRoot* FindOffsetRootNode(const FAnimationBaseContext& Context);
	/** Same from the anim instance (game thread; autotest readout). */
	FAnimNode_MidearthOffsetRoot* FindOffsetRootNode(UAnimInstance* AnimInstance);
	/** First anim node of struct Struct in the anim instance (game thread, debug / autotest). */
	void* FindAnimNode(UAnimInstance* AnimInstance, const UScriptStruct* Struct);

	/** Value of float curve CurveName in Clip at Time; false if the clip has no such curve (GASP GetCurveValueFromAnimation). */
	bool GetClipCurveValue(const UAnimSequence* Clip, FName CurveName, float Time, float& OutValue);

	// UE5 Core Math/SpringMath.h + FQuat helpers that 4.27 lacks.
	FVector QuatToRotationVector(const FQuat& Q);
	FQuat QuatFromRotationVector(const FVector& V);
	inline float HalfLifeToSmoothingTime(float HalfLife) { return HalfLife / 0.69314718056f; }
	inline float InvExpApprox(float X) { return 1.f / (1.f + 1.00746054f * X + 0.45053901f * X * X + 0.25724632f * X * X * X); }
	void CriticalSpringDamperQuat(FQuat& InOutRotation, FVector& InOutAngularVelocityRadians, const FQuat& TargetRotation, float SmoothingTime, float DeltaTime);
}
