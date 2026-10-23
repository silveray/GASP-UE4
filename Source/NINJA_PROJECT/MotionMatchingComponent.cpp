// Motion matching and fresh save-load initialization; action recovery restores the actual root-motion mode.
#include "MotionMatchingComponent.h"

#include "TimerManager.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "CollisionQueryParams.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "AnimNode_MidearthOffsetRoot.h"

// 10-09: 1 = GASP's crouch-moving gate (Starts only while IsStarting, Pivots only while IsPivoting). Needs the GASP movement model (CMC
// turns the capsule, Steering turns the body); with root motion turning the capsule it left diagonals only strafe loops -> 0 there.
static TAutoConsoleVariable<int32> CVarGaspMovementModel(TEXT("gasp.MovementModel"), 1, // 10-09 step 3: on again with the GASP stand chooser (mouse turns 40-61 -> 4-7 switches)
	TEXT("1 = GASP movement model (CMC moves + turns the capsule, warping nodes move the body). 0 = clip root motion moves the capsule (default: ")
	TEXT("10-09 test of step 2 -> off until the GASP stand chooser exists)."));

static TAutoConsoleVariable<int32> CVarMMCrouchGaspGate(TEXT("mm.CrouchGaspGate"), 0, // forced on while gasp.MovementModel is on (crouch mouse-turn 24 -> 2 switches)
	TEXT("1 = crouch uses GASP's IsStarting / IsPivoting gate while moving (A/B test; 0 = every moving group, like standing)."));

UMotionMatchingComponent::UMotionMatchingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

namespace MotionMatchingAir
{
	// Feet on the ground in the clip at Time: root at height 0 and not rising or falling in the next 0.1 s.
	bool IsClipGroundedAt(const UAnimSequence* Seq, float Time)
	{
		const float Z0 = Seq->ExtractRootTrackTransform(Time, nullptr).GetLocation().Z;
		const float Z1 = Seq->ExtractRootTrackTransform(FMath::Min(Time + 0.1f, Seq->SequenceLength), nullptr).GetLocation().Z;
		return FMath::Abs(Z0) < 2.f && FMath::Abs(Z1 - Z0) < 2.f;
	}
}

void UMotionMatchingComponent::BeginPlay()
{
	Super::BeginPlay();

	Character = Cast<ACharacter>(GetOwner());
	if (Character && Character->GetMesh())
	{
		if (UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance())
		{
			// Root motion on: the playing montage moves the capsule on the ground. Off: input moves it, clips only provide the look.
			// (Clips need Enable Root Motion = true either way)
			Anim->SetRootMotionMode(bRootMotionMovement ? ERootMotionMode::RootMotionFromMontagesOnly : ERootMotionMode::IgnoreRootMotion);
		}
	}
	if (Character && bRootMotionMovement)
	{
		// The clips do the turning; FacingCorrectionRate adds the steering in TickComponent.
		Character->GetCharacterMovement()->bOrientRotationToMovement = false;
	}
	if (!Character || !Database || Database->NumFrames() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("MotionMatching on %s: needs a Character owner and a built Database."), *GetNameSafe(GetOwner()));
		SetComponentTickEnabled(false);
		return;
	}
	if (JumpDatabase && JumpDatabase->NumFrames() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("MotionMatching on %s: JumpDatabase %s is not built - jumps use the locomotion database."),
			*GetNameSafe(GetOwner()), *JumpDatabase->GetName());
		JumpDatabase = nullptr;
	}

	// Chooser categories from clip names (GASP picks databases by state with its chooser table).
	auto NameContainsAny = [](const UMotionMatchingDatabase* Db, int32 i, const TArray<FString>& Words)
	{
		const FString Name = GetNameSafe(Db->Sequences[i].Sequence);
		for (const FString& Word : Words)
		{
			if (Name.Contains(Word))
			{
				return true;
			}
		}
		return false;
	};
	SetCombatDatabase(CombatDatabase);
	if (CombatDatabase && CombatDatabase->NumFrames() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("MotionMatching on %s: CombatDatabase %s is not built - battle uses the normal set."),
			*GetNameSafe(GetOwner()), *CombatDatabase->GetName());
		CombatDatabase = nullptr;
	}
	if (CombatDatabase)
	{
		IsCombatStoppedClip.Init(false, CombatDatabase->Sequences.Num());
		for (int32 i = 0; i < CombatDatabase->Sequences.Num(); ++i)
		{
			IsCombatStoppedClip[i] = NameContainsAny(CombatDatabase, i, CombatStoppedClips);
		}
	}
	IsStoppedClip.Init(false, Database->Sequences.Num());
	IsStopClip.Init(false, Database->Sequences.Num());
	IsIdleAfterStopClip.Init(false, Database->Sequences.Num());
	for (int32 i = 0; i < Database->Sequences.Num(); ++i)
	{
		IsStoppedClip[i] = NameContainsAny(Database, i, StoppedClips);
		// Standing up from crouch ends like a stop: its end without input = idle loop only (bIdleAfterStop).
		IsStopClip[i] = NameContainsAny(Database, i, { TEXT("Stop"), TEXT("Transition_Crouch_to_Stand") });
		IsIdleAfterStopClip[i] = NameContainsAny(Database, i, IdleAfterStopClips);
	}
	// GASP stand groups ("Walk_Starts", "Run_Loops", "Idles", ...) written by the database setup (step 3).
	StandGroup.Init(EStandGroup::None, Database->Sequences.Num());
	StandGroupGait.Init(0, Database->Sequences.Num());
	bStandHasGroups = false;
	for (int32 i = 0; i < Database->Sequences.Num(); ++i)
	{
		const FString G = Database->Sequences[i].Group.ToString();
		if (G.IsEmpty() || G == TEXT("None"))
		{
			continue;
		}
		bStandHasGroups = true;
		StandGroupGait[i] = G.StartsWith(TEXT("Walk_")) ? 1 : G.StartsWith(TEXT("Run_")) ? 2 : G.StartsWith(TEXT("Sprint_")) ? 3 : 0;
		StandGroup[i] = G == TEXT("Idles") ? EStandGroup::Idles : G == TEXT("TurnInPlace") ? EStandGroup::TurnInPlace
			: G.EndsWith(TEXT("_Starts")) ? EStandGroup::Starts : G.EndsWith(TEXT("_Loops")) ? EStandGroup::Loops
			: G.EndsWith(TEXT("_Pivots")) ? EStandGroup::Pivots : G.EndsWith(TEXT("_Stops")) ? EStandGroup::Stops
			: G.EndsWith(TEXT("_SpinTransition")) ? EStandGroup::Spin : EStandGroup::None;
	}
	if (CrouchDatabase && CrouchDatabase->NumFrames() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("MotionMatching on %s: CrouchDatabase %s is not built - crouching uses the normal set."),
			*GetNameSafe(GetOwner()), *CrouchDatabase->GetName());
		CrouchDatabase = nullptr;
	}
	SetCrouchDatabase(CrouchDatabase);
	if (JumpDatabase)
	{
		IsLandClip.Init(false, JumpDatabase->Sequences.Num());
		IsHeavyLandClip.Init(false, JumpDatabase->Sequences.Num());
		LandClipGait.Init(INDEX_NONE, JumpDatabase->Sequences.Num());
		IsRollLandClip.Init(false, JumpDatabase->Sequences.Num());
		for (int32 i = 0; i < JumpDatabase->Sequences.Num(); ++i)
		{
			const FString Name = GetNameSafe(JumpDatabase->Sequences[i].Sequence);
			// A land clip cut at touchdown (SampleEnd set) is its in-air part = GASP PSD_Dense_Jumps -> air clip.
			IsLandClip[i] = Name.Contains(LandClipWord) && JumpDatabase->Sequences[i].SampleEnd <= 0.f;
			IsHeavyLandClip[i] = IsLandClip[i] && NameContainsAny(JumpDatabase, i, HeavyLandWords);
			IsRollLandClip[i] = IsLandClip[i] && !RollLandWord.IsEmpty() && Name.Contains(RollLandWord);
			for (int32 g = 0; g < LandGaitWords.Num(); ++g)
			{
				if (Name.Contains(LandGaitWords[g]))
				{
					LandClipGait[i] = g;
				}
			}
		}
		JumpGroundFrame.Init(false, JumpDatabase->NumFrames());
		for (int32 f = 0; f < JumpDatabase->NumFrames(); ++f)
		{
			const FMotionMatchingFrame& Fr = JumpDatabase->GetFrame(f);
			if (!JumpDatabase->Sequences.IsValidIndex(Fr.SequenceIndex))
			{
				continue;
			}
			const FMotionMatchingSequence& Entry = JumpDatabase->Sequences[Fr.SequenceIndex];
			// Loops (Jump_Loop_Fall) play in place: root Z stays 0, which looks 'grounded' but is the fall pose -> never excluded.
			JumpGroundFrame[f] = !Entry.bLoop && Entry.Sequence && MotionMatchingAir::IsClipGroundedAt(Entry.Sequence, Fr.Time);
		}
		bJumpDbHasLoop = JumpDatabase->Sequences.ContainsByPredicate([](const FMotionMatchingSequence& E) { return E.bLoop; });
	}
}

namespace MotionMatchingPrediction
{
	// Port of UE5.8 FPoseSearchTrajectoryData::StepCharacterMovementGroundPrediction (PoseSearchTrajectoryLibrary.cpp),
	// itself a copy of UCharacterMovementComponent::ApplyVelocityBraking / CalcVelocity.
	FVector StepGround(float DeltaTime, const FVector& InVelocity, const FVector& InAcceleration,
		float MaxSpeed, float BrakingDeceleration, float BrakingSubStepTime, float Friction)
	{
		FVector OutVelocity = InVelocity;
		if (InAcceleration.IsZero())
		{
			if (InVelocity.IsZero())
			{
				return FVector::ZeroVector;
			}
			const bool bZeroFriction = (Friction == 0.f);
			const bool bZeroBraking = (BrakingDeceleration == 0.f);
			if (bZeroFriction && bZeroBraking)
			{
				return InVelocity;
			}

			float RemainingTime = DeltaTime;
			const float MaxTimeStep = FMath::Clamp(BrakingSubStepTime, 1.0f / 75.0f, 1.0f / 20.0f);
			const FVector PrevLinearVelocity = OutVelocity;
			const FVector RevAccel = (bZeroBraking ? FVector::ZeroVector : (-BrakingDeceleration * OutVelocity.GetSafeNormal()));

			// Decelerate to brake to a stop
			while (RemainingTime >= UCharacterMovementComponent::MIN_TICK_TIME)
			{
				const float Dt = ((RemainingTime > MaxTimeStep && !bZeroFriction) ? FMath::Min(MaxTimeStep, RemainingTime * 0.5f) : RemainingTime);
				RemainingTime -= Dt;
				OutVelocity = OutVelocity + ((-Friction) * OutVelocity + RevAccel) * Dt;
				// Don't reverse direction
				if ((OutVelocity | PrevLinearVelocity) <= 0.f)
				{
					return FVector::ZeroVector;
				}
			}

			// Clamp to zero if nearly zero, or if below min threshold and braking
			const float VSizeSq = OutVelocity.SizeSquared();
			if (VSizeSq <= KINDA_SMALL_NUMBER || (!bZeroBraking && VSizeSq <= FMath::Square(UCharacterMovementComponent::BRAKE_TO_STOP_VELOCITY)))
			{
				OutVelocity = FVector::ZeroVector;
			}
		}
		else
		{
			const FVector AccelDir = InAcceleration.GetSafeNormal();
			const float VelSize = OutVelocity.Size();
			OutVelocity = OutVelocity - (OutVelocity - AccelDir * VelSize) * FMath::Min(DeltaTime * Friction, 1.f);
			OutVelocity += InAcceleration * DeltaTime;
			OutVelocity = OutVelocity.GetClampedToMaxSize(MaxSpeed);
		}
		return OutVelocity;
	}
}

void UMotionMatchingComponent::PredictGround(const float* Times, int32 Num, FVector* OutPositions, FVector* OutFacings, FVector* OutVelocities) const
{
	// Same inputs UE5 takes from the CharacterMovementComponent (FPoseSearchTrajectoryData::UpdateData).
	const UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	const float MaxSpeed = FMath::Max(Move->GetMaxSpeed() * Move->GetAnalogInputModifier(), Move->GetMinAnalogSpeed());
	const float Braking = FMath::Max(0.f, Move->GetMaxBrakingDeceleration());
	const FVector Accel = Move->GetCurrentAcceleration() * FVector(1.f, 1.f, 0.f);
	float Friction = Move->GroundFriction;
	if (Accel.IsZero())
	{
		Friction = Move->bUseSeparateBrakingFriction ? Move->BrakingFriction : Move->GroundFriction;
		Friction = FMath::Max(0.f, Friction * FMath::Max(0.f, Move->BrakingFrictionFactor));
	}

	// Facing: GASP TrajectoryGenerationData RotateTowardsMovementSpeed = 0 -> snaps to the acceleration direction.
	FVector Facing = Character->GetActorForwardVector().GetSafeNormal2D();
	if (IsCombatStance() && !(CrouchDatabase && Character->bIsCrouched))
	{
		Facing = StrafeFacing; // battle: keep facing the target, the legs strafe / step back (crouched = crouch set, faces movement)
	}
	else if (!Accel.IsNearlyZero())
	{
		Facing = Accel.GetSafeNormal2D();
	}

	// Simulate in PredictionStep steps (position first, then velocity - same order as UE5), sample by interpolation.
	FVector Pos = Character->GetActorLocation();
	FVector Vel = Move->Velocity * FVector(1.f, 1.f, 0.f);
	float T = 0.f;
	int32 k = 0;
	while (k < Num)
	{
		const FVector NextPos = Pos + Vel * PredictionStep;
		const FVector NextVel = MotionMatchingPrediction::StepGround(PredictionStep, Vel, Accel, MaxSpeed, Braking, Move->BrakingSubStepTime, Friction);
		const float NextT = T + PredictionStep;
		while (k < Num && Times[k] <= NextT + KINDA_SMALL_NUMBER)
		{
			const float Alpha = FMath::Clamp((Times[k] - T) / PredictionStep, 0.f, 1.f);
			OutPositions[k] = FMath::Lerp(Pos, NextPos, Alpha);
			OutVelocities[k] = FMath::Lerp(Vel, NextVel, Alpha);
			if (OutFacings)
			{
				OutFacings[k] = Facing;
			}
			++k;
		}
		Pos = NextPos;
		Vel = NextVel;
		T = NextT;
	}
}

void UMotionMatchingComponent::PredictAir(const float* Times, int32 Num, FVector* OutPositions, FVector* OutVelocities,
	float* OutTimeToLand, float* OutLandSpeedZ) const
{
	if (OutTimeToLand)
	{
		*OutTimeToLand = -1.f;
	}
	if (OutLandSpeedZ)
	{
		*OutLandSpeedZ = 0.f;
	}
	// UE5 in the air: the prediction keeps the current velocity (no ground step), then
	// HandleTransformTrajectoryWorldCollisionsWithGravity adds 0.5*g*t^2 and clamps samples to the floor it traces.
	const UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	UWorld* World = GetWorld();
	const float GravityZ = World->GetGravityZ() * Move->GravityScale; // negative
	const float HalfHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float MaxObstacleHeight = 100.f;
	const FVector P0 = Character->GetActorLocation();
	const FVector V0 = Move->Velocity;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(MotionMatchingAirTrajectory), false, Character);
	bool bLanded = false;
	float LandZ = 0.f;

	FVector Pos = P0;
	FVector Vel = V0;
	float T = 0.f;
	int32 k = 0;
	while (k < Num)
	{
		const float NextT = T + PredictionStep;
		FVector NextPos(P0.X + V0.X * NextT, P0.Y + V0.Y * NextT, 0.f);
		FVector NextVel(V0.X, V0.Y, 0.f);
		if (bLanded)
		{
			NextPos.Z = LandZ;
		}
		else
		{
			NextPos.Z = P0.Z + V0.Z * NextT + 0.5f * GravityZ * NextT * NextT;
			NextVel.Z = V0.Z + GravityZ * NextT;
			// Feet below the floor? (trace from above the feet down to the feet, like UE5's floor trace)
			const FVector Feet = NextPos - FVector(0.f, 0.f, HalfHeight);
			FHitResult Hit;
			if (NextVel.Z < 0.f && World->LineTraceSingleByChannel(Hit, Feet + FVector(0.f, 0.f, MaxObstacleHeight), Feet, ECC_Visibility, Params))
			{
				if (OutTimeToLand)
				{
					*OutTimeToLand = NextT;
				}
				if (OutLandSpeedZ)
				{
					*OutLandSpeedZ = -NextVel.Z;
				}
				bLanded = true;
				LandZ = Hit.ImpactPoint.Z + HalfHeight;
				NextPos.Z = LandZ;
				NextVel.Z = 0.f;
			}
		}
		while (k < Num && Times[k] <= NextT + KINDA_SMALL_NUMBER)
		{
			const float Alpha = FMath::Clamp((Times[k] - T) / PredictionStep, 0.f, 1.f);
			OutPositions[k] = FMath::Lerp(Pos, NextPos, Alpha);
			OutVelocities[k] = FMath::Lerp(Vel, NextVel, Alpha);
			++k;
		}
		Pos = NextPos;
		Vel = NextVel;
		T = NextT;
	}
}

FVector UMotionMatchingComponent::PastPosition(float Seconds) const
{
	const float Target = GetWorld()->GetTimeSeconds() + Seconds;
	FVector Best = PositionHistory.Num() > 0 ? PositionHistory[0].Value : Character->GetActorLocation();
	for (const TPair<float, FVector>& P : PositionHistory)
	{
		if (P.Key <= Target)
		{
			Best = P.Value; // newest entry that is at least |Seconds| old
		}
	}
	return Best;
}

UMotionMatchingDatabase* UMotionMatchingComponent::ChooseDatabase() const
{
	if (JumpDatabase)
	{
		const bool bInAir = Character->GetCharacterMovement()->IsFalling();
		const bool bJustLanded = GetWorld()->GetTimeSeconds() - LandWorldTime < JustLandedTime;
		if (bInAir || bJustLanded || IsPlayingRoll())
		{
			return JumpDatabase;
		}
	}
	// GASP root chooser: MovementMode OnGround + Stance Crouch -> 'Crouch Idle' / 'Crouch Moving' (crouch wins over the battle set:
	// GASP has no battle stance, and the crouched capsule needs the crouch poses).
	if (CrouchDatabase && Character->bIsCrouched && Character->GetCharacterMovement()->IsMovingOnGround())
	{
		return CrouchDatabase;
	}
	return IsCombatStance() ? CombatDatabase : Database;
}

void UMotionMatchingComponent::UpdateCrouchState()
{
	// Port of GASP SandboxCharacter_CMC_ABP: Update_EssentialValues (Speed2D), IsMoving, IsStarting, IsPivoting / MM Pivot conditions.
	const UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	const FVector Velocity = Move->Velocity;
	const FVector Accel = Move->GetCurrentAcceleration();
	CrouchSpeed2D = Velocity.Size2D();
	bCrouchMovingLastFrame = bCrouchMoving;
	// GASP IsMoving = (Velocity != 0, tolerance 0.1) AND (Acceleration != 0); its FutureVelocity term is unplugged in 5.8 (pin B = true).
	// In GASP the CMC turns input into velocity the next frame, so the velocity term is a 1-frame lag. Here the clip's root motion makes
	// the velocity: requiring it deadlocks (no velocity -> Idle -> idle clip -> no root motion; 10-09 autotest 'crouch-walk-up' 0 cm).
	// Equivalent here = Acceleration != 0 (the velocity term is only kept for the pivot angle below).
	const bool bHasVelocity = !Velocity.Equals(FVector::ZeroVector, 0.1f);
	bCrouchMoving = !Accel.Equals(FVector::ZeroVector, KINDA_SMALL_NUMBER);
	if (UsesGaspMovementModel())
	{
		bCrouchMoving = bCrouchMoving && bHasVelocity; // GASP model: the CMC makes the velocity -> GASP IsMoving exactly
	}
	GaspGaitLastFrame = GaspGait;
	GaspGait = Move->MaxWalkSpeed <= WalkGaitMaxSpeed ? 1 : Move->MaxWalkSpeed <= RunGaitMaxSpeed ? 2 : 3;
	// IsStarting = IsMoving AND |Trj_FutureVelocity| >= Speed2D + 100; Trj_FutureVelocity = trajectory velocity between 0.4 and 0.5 s.
	bCrouchStarting = false;
	if (bCrouchMoving)
	{
		const float Times[2] = { 0.4f, 0.5f };
		FVector P[2], V[2];
		PredictGround(Times, 2, P, nullptr, V);
		const FVector FutureVelocity = (P[1] - P[0]) / (Times[1] - Times[0]);
		bCrouchStarting = FutureVelocity.Size2D() >= CrouchSpeed2D + StartingSpeedGain;
	}
	// IsPivoting = IsMoving AND |Get_TrajectoryTurnAngle| >= 45 (orient to movement); turn angle = Delta(Rot(Acceleration), Rot(Velocity)).
	const float TurnAngle = (bCrouchMoving && bHasVelocity) ? FMath::Abs(FMath::FindDeltaAngleDegrees(Velocity.Rotation().Yaw, Accel.Rotation().Yaw)) : 0.f;
	bCrouchPivoting = bCrouchMoving && bHasVelocity && TurnAngle >= PivotTurnAngle; // GASP IsMoving guarantees a velocity direction
	// GASP ShouldSpinTransition: |turn| >= 130 and Speed2D >= 150 (turn measured like Get_TrajectoryTurnAngle: acceleration vs velocity).
	bGaspSpinning = TurnAngle >= 130.f && CrouchSpeed2D >= 150.f;
}

void UMotionMatchingComponent::SetCombatStance(bool bOn, const FVector& FacingDir)
{
	if (bOn != bCombatStance)
	{
		UE_LOG(LogTemp, Log, TEXT("MotionMatching %s: battle stance %s"), *GetNameSafe(GetOwner()), bOn ? TEXT("ON (sword & shield set)") : TEXT("off"));
	}
	bCombatStance = bOn;
	const FVector Flat = FacingDir.GetSafeNormal2D();
	if (!Flat.IsNearlyZero())
	{
		StrafeFacing = Flat;
	}
}

bool UMotionMatchingComponent::IsClipExcludedByState(const UMotionMatchingDatabase* Db, int32 Sequence, bool bHasInput) const
{
	if (!bUseStateChooser)
	{
		return false;
	}
	if (Db == JumpDatabase && JumpDatabase)
	{
		if (!IsLandClip.IsValidIndex(Sequence))
		{
			return true;
		}
		if (Character->GetCharacterMovement()->IsFalling())
		{
			const bool bLandingSoon = LandAnticipationTime > 0.f && PredictedTimeToLand >= 0.f && PredictedTimeToLand <= LandAnticipationTime;
			if (!bLandingSoon)
			{
				if (bFallLoopOnly && !JumpDatabase->Sequences[Sequence].bLoop)
				{
					return true; // long fall: the jump clip ran out -> fall loop only
				}
				return IsLandClip[Sequence]; // in the air: starts, ledge-offs, fall loop
			}
			// Touchdown is near: landing clips (they start in the air), light or heavy by the predicted landing speed.
			const bool bHeavySoon = PredictedLandSpeedZ > HeavyLandSpeedThreshold;
			return !IsLandClip[Sequence] || IsHeavyLandClip[Sequence] != bHeavySoon;
		}
		// High fall (bAutoRollOnHighFall): roll landings only. Otherwise never a roll.
		if (bRollLanding)
		{
			return !IsRollLandClip[Sequence];
		}
		if (IsRollLandClip[Sequence])
		{
			return true;
		}
		// Just landed: landing clips of the right weight and of the gait at touchdown (GASP land DB per gait).
		const bool bHeavy = LandSpeedZ > HeavyLandSpeedThreshold;
		return !IsLandClip[Sequence] || IsHeavyLandClip[Sequence] != bHeavy || LandClipGait[Sequence] != LandGait;
	}
	if (Db == CrouchDatabase && CrouchDatabase)
	{
		if (!CrouchGroup.IsValidIndex(Sequence))
		{
			return true;
		}
		// GASP CHT_PoseSearchDatabases_Dense 'Crouch Idle': Idles (Speed2D <= 20), Stops (Speed2D >= 20), TurnInPlace (ShouldTurnInPlace).
		// 'Crouch Moving' in GASP gates Starts by IsStarting and Pivots (incl. all Turn/Box/Diamond clips) by IsPivoting. That only works
		// because GASP's CMC turns the capsule toward the movement by itself. Here the clips turn the capsule (root motion), so with the
		// gate a diagonal (A+W / D+W) had only loops left -> strafe loop Crouch_Loop_FR/FL crab-walked, cost 190-250, Turn<->Reface
		// ping-pong (10-09). Moving = every moving group, the same rule as the standing set.
		// GASP gate needs the GASP movement model (CMC turns the capsule); with root motion turning it, diagonals crab-walked.
		const bool bGaspGate = UsesGaspMovementModel() || CVarMMCrouchGaspGate.GetValueOnGameThread() != 0;
		switch (CrouchGroup[Sequence])
		{
		case ECrouchGroup::Loops:  return !bCrouchMoving;
		// Starts only while starting (GASP IsStarting), in both movement models: mid-walk the reface starts (Reface_Start_B = start, then
		// walk BACKWARD) won turns and the character walked backward after W -> W+A / W+D (10-09).
		case ECrouchGroup::Starts: return !bCrouchMoving || !bCrouchStarting;
		case ECrouchGroup::Pivots: return !bCrouchMoving || (bGaspGate && !bCrouchPivoting);
		case ECrouchGroup::Idles:  return bCrouchMoving || CrouchSpeed2D > CrouchIdleSpeedSplit;
		case ECrouchGroup::Stops:  return bCrouchMoving || CrouchSpeed2D < CrouchIdleSpeedSplit;
		// ShouldTurnInPlace = |actor yaw - offset-root yaw| >= 50 and (aiming or just stopped). The offset root lags the capsule in GASP;
		// here root motion turns the capsule, so that difference is always 0 -> never true (fidelity gaps).
		case ECrouchGroup::TurnInPlace: return true;
		default: return true;
		}
	}
	if (Db == CombatDatabase && CombatDatabase)
	{
		// battle: no input -> only stops / idle / turns; input -> none of those
		if (IsCombatStoppedClip.IsValidIndex(Sequence) && IsCombatStoppedClip[Sequence] == bHasInput)
		{
			return true;
		}
		// 10-13 gait by name (sets with no walk->run starts): walk = Walk clips only, run = Run clips only
		if (bHasInput && CombatDatabase->bGaitByClipName && IsCombatWalkClip.IsValidIndex(Sequence)
			&& (bWantWalk ? IsCombatRunClip[Sequence] : IsCombatWalkClip[Sequence]))
		{
			return true;
		}
		// ... and by direction: the clips carry the character, so one that travels elsewhere would keep going there
		if (bHasInput && CombatDatabase->bGaitByClipName && IsCombatClipOffDirection(Sequence))
		{
			return true;
		}
		// 10-04 gait: moving + sprint wanted = sprint clips only; moving without sprint = no sprint clips
		return bHasInput && bCombatDbHasSprint && IsCombatSprintClip.IsValidIndex(Sequence) && IsCombatSprintClip[Sequence] != bWantSprint;
	}
	if (Db == Database && UseStandGroups())
	{
		if (!StandGroup.IsValidIndex(Sequence))
		{
			return true;
		}
		const EStandGroup Group = StandGroup[Sequence];
		const uint8 Gait = StandGroupGait[Sequence];
		if (!bCrouchMoving)
		{
			// GASP 'Stand Idles': Idles (Speed2D <= 20, not turning in place), Walk/Run/Sprint stops from 20 / 100 / 550 cm/s, TurnInPlace.
			switch (Group)
			{
			case EStandGroup::Idles: return CrouchSpeed2D > CrouchIdleSpeedSplit;
			case EStandGroup::Stops: return CrouchSpeed2D < (Gait == 3 ? 550.f : Gait == 2 ? 100.f : CrouchIdleSpeedSplit);
			case EStandGroup::TurnInPlace: return true; // ShouldTurnInPlace needs the offset-root lag (board) - like crouch
			default: return true;
			}
		}
		// GASP 'Stand Walks / Runs / Sprint' by gait: Starts (IsStarting), Loops, Pivots (IsPivoting), SpinTransition (ShouldSpinTransition).
		if (Gait != GaspGait)
		{
			return true;
		}
		switch (Group)
		{
		case EStandGroup::Loops:  return Gait == 3 && bCrouchPivoting;                       // Sprint loops: IsPivoting false
		case EStandGroup::Starts: return !bCrouchStarting || (Gait == 3 && bCrouchPivoting); // Sprint starts: IsPivoting false
		case EStandGroup::Pivots: return !bCrouchPivoting;
		case EStandGroup::Spin:   return !bGaspSpinning || (Gait == 2 && bCrouchPivoting);   // Run spin: IsPivoting false
		default: return true;
		}
	}
	// Ground, a stop already finished (bIdleAfterStop): idle clips only until input returns.
	if (!bHasInput && bStopFinished && IsIdleAfterStopClip.IsValidIndex(Sequence))
	{
		return !IsIdleAfterStopClip[Sequence];
	}
	// Ground: no input -> only stop/idle clips; input -> no stop/idle clips.
	return IsStoppedClip.IsValidIndex(Sequence) && IsStoppedClip[Sequence] == bHasInput;
}

void UMotionMatchingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	UAnimInstance* Anim = Character->GetMesh() ? Character->GetMesh()->GetAnimInstance() : nullptr;
	const float Now = GetWorld()->GetTimeSeconds();

	if (bSuspended)
	{
		bWasInAir = false; // the owner of the slot moves the capsule (flying); no landing is detected from it
		return;
	}

	// Air / landing state (GASP: OnLanded -> LandVelocity, JustLanded for 0.3 s).
	const bool bInAir = Move->IsFalling();
	if (bInAir)
	{
		LastAirVelocity = Move->Velocity;
		const float Z = Character->GetActorLocation().Z;
		FallTopZ = bWasInAir ? FMath::Max(FallTopZ, Z) : Z;
	}
	else if (bWasInAir)
	{
		LastFallHeight = FallTopZ - Character->GetActorLocation().Z;
		bRollLanding = bAutoRollOnHighFall && LastFallHeight >= RollMinFallHeight && IsRollLandClip.Contains(true);
		LandWorldTime = Now;
		LandSpeedZ = -LastAirVelocity.Z;
		const float Max = Move->MaxWalkSpeed;
		LandGait = Move->GetCurrentAcceleration().IsNearlyZero() ? 0 : Max <= WalkGaitMaxSpeed ? 1 : Max <= RunGaitMaxSpeed ? 2 : 3;
	}
	bWasInAir = bInAir;
	if (!bInAir)
	{
		bFallLoopOnly = false;
	}

	// In the air: predicted touchdown (for landing anticipation in the chooser).
	PredictedTimeToLand = -1.f;
	PredictedLandSpeedZ = 0.f;
	if (bInAir && JumpDatabase && LandAnticipationTime > 0.f)
	{
		const float Times[1] = { LandAnticipationTime };
		FVector P[1], V[1];
		PredictAir(Times, 1, P, V, &PredictedTimeToLand, &PredictedLandSpeedZ);
	}

	// Root motion only on the ground; in the air the CharacterMovement physics (jump, gravity, air control) move the capsule.
	// GASP movement model: never - the CMC moves and turns the capsule, the warping nodes move the body with the clip.
	const bool bGaspModel = UsesGaspMovementModel();
	const bool bWantRootMotion = bRootMotionMovement && !bInAir && !bGaspModel;
	// Action components change the actual AnimInstance mode while MM is suspended. Read it directly on recovery:
	// a cached flag can still say "off" after a sword action enabled root motion, deadlocking GASP's velocity gate.
	const ERootMotionMode::Type WantedRootMotionMode = bWantRootMotion
		? ERootMotionMode::RootMotionFromMontagesOnly : ERootMotionMode::IgnoreRootMotion;
	if (Anim && Anim->RootMotionMode != WantedRootMotionMode)
	{
		Anim->SetRootMotionMode(WantedRootMotionMode);
	}
	// GASP UpdateRotation_PreCMC (not strafing): orient rotation to movement, RotationRate yaw -1 (instant) on the ground, 200 in the air.
	Move->bOrientRotationToMovement = bGaspModel || (!bRootMotionMovement);
	Move->bUseControllerDesiredRotation = false;
	if (bGaspModel)
	{
		Move->RotationRate = FRotator(0.f, bInAir ? 200.f : -1.f, 0.f);
	}

	// Steering on top of root motion: turn the capsule toward the input direction.
	if (bRootMotionMovement && FacingCorrectionRate > 0.f && !bInAir)
	{
		const FVector InputDir = Move->GetCurrentAcceleration().GetSafeNormal2D();
		if (!InputDir.IsNearlyZero())
		{
			FRotator Rot = Character->GetActorRotation();
			Rot.Yaw = FMath::FixedTurn(Rot.Yaw, InputDir.Rotation().Yaw, FacingCorrectionRate * DeltaTime);
			Character->SetActorRotation(Rot);
		}
	}

	// Battle stance: face the target. Moving (strafe clips don't turn) or a small angle = code turn; standing and far off =
	// the turn-on-the-spot clips (their root motion turns us; the code stays out of it while one plays).
	if (bRootMotionMovement && IsCombatStance() && !bInAir && CurrentDb == CombatDatabase)
	{
		const bool bMoving = !Move->GetCurrentAcceleration().IsNearlyZero();
		const float Delta = FMath::FindDeltaAngleDegrees(Character->GetActorRotation().Yaw, StrafeFacing.Rotation().Yaw);
		const bool bTurnClip = GetCurrentClipName().Contains(TEXT("Turn"));
		if (!bTurnClip && (bMoving || FMath::Abs(Delta) < StrafeClipTurnAngle || !bCombatDbHasTurn))
		{
			FRotator Rot = Character->GetActorRotation();
			Rot.Yaw = FMath::FixedTurn(Rot.Yaw, StrafeFacing.Rotation().Yaw, StrafeTurnRate * DeltaTime);
			Character->SetActorRotation(Rot);
		}
	}

	// Uphill slowdown: slope along the walking direction from the floor normal; the playing clip runs slower (UphillSpeedScale).
	float SlopeTarget = 1.f;
	if (!bInAir && Move->CurrentFloor.IsWalkableFloor())
	{
		const FVector N = Move->CurrentFloor.HitResult.ImpactNormal;
		const FVector Dir = Move->Velocity.GetSafeNormal2D();
		if (!Dir.IsNearlyZero() && N.Z > KINDA_SMALL_NUMBER)
		{
			const float UphillDeg = FMath::RadiansToDegrees(FMath::Atan(-FVector::DotProduct(N, Dir) / N.Z));
			SlopeTarget = FMath::Lerp(1.f, UphillSpeedScale, FMath::Clamp(UphillDeg / UphillMaxAngle, 0.f, 1.f));
		}
	}
	SlopeSpeedFactor = FMath::FInterpTo(SlopeSpeedFactor, SlopeTarget, DeltaTime, 5.f);
	if (Anim && CurrentMontage.IsValid() && Anim->Montage_IsPlaying(CurrentMontage.Get()))
	{
		Anim->Montage_SetPlayRate(CurrentMontage.Get(), bInAir ? 1.f : SlopeSpeedFactor);
	}

	// Follow the clip we're playing.
	bool bForce = CurrentDb == nullptr || CurrentSequence == INDEX_NONE;
	if (!bForce)
	{
		const FMotionMatchingSequence& Entry = CurrentDb->Sequences[CurrentSequence];
		const float Length = Entry.Sequence ? Entry.Sequence->SequenceLength : 0.f;
		// Clip time = the playing montage's real position (no drift); a looping montage repeats the clip, so wrap it.
		if (Anim && CurrentMontage.IsValid() && Anim->Montage_IsPlaying(CurrentMontage.Get()))
		{
			CurrentTime = Anim->Montage_GetPosition(CurrentMontage.Get());
		}
		else
		{
			CurrentTime += DeltaTime;
		}
		if (Entry.bLoop)
		{
			CurrentTime = Length > 0.f ? FMath::Fmod(CurrentTime, Length) : 0.f;
		}
		else if (!CurrentDb->IsTimeInDatabase(CurrentSequence, CurrentTime) || CurrentTime >= Length - 1.f / CurrentDb->SampleRate)
		{
			bForce = true; // past the stored part of a one-shot clip (GASP excludes the last 0.3 s)
			if (bIdleAfterStop && CurrentDb == Database && IsStopClip.IsValidIndex(CurrentSequence) && IsStopClip[CurrentSequence]
				&& Move->GetCurrentAcceleration().IsNearlyZero())
			{
				bStopFinished = true; // the stop did its job -> idle clips only (IsClipExcludedByState) until input
			}
			if (IsPlayingRoll())
			{
				bRollLanding = false; // roll finished -> normal clips again (else the search could restart the roll)
			}
			if (bFallLoopWhenClipEnds && bJumpDbHasLoop && bInAir && Move->Velocity.Z < 0.f && CurrentDb == JumpDatabase)
			{
				bFallLoopOnly = true; // long fall: the air clip is used up -> Jump_Loop_Fall until landing
			}
		}
	}

	// Remember what is playing so the search can't jump back to it (PoseReselectHistory),
	// and where we have been (trajectory past samples).
	History.RemoveAll([Now, this](const FPlayedPose& P) { return Now - P.WorldTime > PoseReselectHistory; });
	if (CurrentDb && CurrentSequence != INDEX_NONE)
	{
		History.Add({ CurrentDb, CurrentSequence, CurrentTime, Now });
	}
	PositionHistory.RemoveAll([Now](const TPair<float, FVector>& P) { return Now - P.Key > 0.5f; });
	PositionHistory.Emplace(Now, Character->GetActorLocation());

	// Debug timer: WASD pressed or released -> log candidates at 0, 0.15 and 0.4 s (see SearchAndPlay).
	const bool bHasInput = !Move->GetCurrentAcceleration().IsNearlyZero();
	if (bHasInput)
	{
		bStopFinished = false;
	}
	if (bHadInputForDebug != bHasInput)
	{
		SinceRelease = 0.f;
		ReleaseLogsDone = 0;
		bEdgeIsPress = bHasInput;
	}
	else if (SinceRelease >= 0.f)
	{
		SinceRelease += DeltaTime;
	}
	bHadInputForDebug = bHasInput;

	// Crouch chooser inputs (GASP ABP values). Entering / leaving crouch clears the stand idle-after-stop latch.
	UMotionMatchingDatabase* const WantedDb = ChooseDatabase();
	const bool bCrouchDb = CrouchDatabase && WantedDb == CrouchDatabase;
	const bool bStandGroupsNow = !bCrouchDb && WantedDb == Database && UseStandGroups();
	if (bCrouchDb || bStandGroupsNow)
	{
		UpdateCrouchState(); // GASP ABP state (IsMoving / IsStarting / IsPivoting / gait); only when a group chooser runs
	}
	if (bCrouchDb != bWasCrouchDb)
	{
		bStopFinished = false;
		bWasCrouchDb = bCrouchDb;
		bCrouchMovingLastFrame = bCrouchMoving; // the DB change itself forces the search; no second one next frame
	}

	// Chooser: another database wanted, or the playing clip no longer allowed -> pick a new one now.
	if (!bForce && WantedDb != CurrentDb)
	{
		bForce = true;
	}
	else if (!bForce && bStandGroupsNow)
	{
		// GASP Get_MMInterruptMode: interrupt on MovementState change, or on a gait change while moving; else DoNotInterrupt.
		bForce = bCrouchMoving != bCrouchMovingLastFrame || (bCrouchMoving && GaspGait != GaspGaitLastFrame);
	}
	else if (!bForce && bCrouchDb)
	{
		// GASP Get_MMInterruptMode: InterruptOnDatabaseChange only when MovementState (Idle/Moving) changes on the ground, else
		// DoNotInterrupt - the playing clip keeps competing as the continuing pose even after its group stops being chosen.
		bForce = bCrouchMoving != bCrouchMovingLastFrame;
	}
	else if (!bForce && IsClipExcludedByState(CurrentDb, CurrentSequence, bHasInput))
	{
		bForce = true;
	}

	SearchTimer -= DeltaTime;
	// Falling: keep the jump clip playing forward (its falling part). Searching there hopped back to the apex frames again and
	// again = frozen legs (10-01). Forced searches (clip end -> fall loop, landing, database change) still run.
	const bool bHoldWhileFalling = bKeepClipWhileFalling && bInAir && Move->Velocity.Z < 0.f && CurrentDb == JumpDatabase;
	// Roll landing plays to its end (GASP land databases: bDisableReselection), the clip end forces the next search.
	const bool bHoldRoll = IsPlayingRoll();
	// 10-04: a standing battle turn finishes its rotation (the clip end / move input / forced searches still reselect)
	const bool bHoldTurn = bCommitTurnClips && !bHasInput && CurrentDb && CurrentDb == CombatDatabase && GetCurrentClipName().Contains(TEXT("Turn"));
	if (bForce || (SearchTimer <= 0.f && !bHoldWhileFalling && !bHoldRoll && !bHoldTurn))
	{
		SearchTimer = SearchInterval;
		SearchAndPlay(bForce);
	}
}

bool UMotionMatchingComponent::BuildQuery(const UMotionMatchingDatabase* Db, float* Query, FVector* OutDrawPositions, int32& OutNumDraw) const
{
	OutNumDraw = 0;
	if (!Db || Db->GetNumFeatures() == 0)
	{
		return false;
	}

	// Pose part: what the playing clip is doing right now, if it belongs to this database (else zero).
	const int32 CurrentFrame = (CurrentDb == Db && CurrentSequence != INDEX_NONE) ? Db->FindFrame(CurrentSequence, CurrentTime) : INDEX_NONE;
	if (CurrentFrame == INDEX_NONE || !Db->ComputeFeatures(CurrentSequence, Db->GetFrame(CurrentFrame).Time, Query))
	{
		FMemory::Memzero(Query, sizeof(float) * UMotionMatchingDatabase::MaxFeatures);
	}

	// Trajectory part: in the animation's root space (= mesh space), relative to the character now.
	const UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	FTransform MeshXf = Character->GetMesh()->GetComponentTransform();
	FVector Origin = Character->GetActorLocation();
	FVector ForwardNow = Character->GetActorForwardVector();
	// 10-09 GASP movement model: the capsule snaps to the movement direction, the visible body (Offset Root) lags and turns with the clip.
	// UE5 PoseSearch measures from the animated root, so the query sees the turn the BODY still has to make -> turn clips. Same here.
	if (UsesGaspMovementModel())
	{
		if (const FAnimNode_MidearthOffsetRoot* OffsetRoot = MidearthWarping::FindOffsetRootNode(Character->GetMesh()->GetAnimInstance()))
		{
			if (OffsetRoot->HasEvaluated())
			{
				const FTransform Body = OffsetRoot->GetBodyTransform();
				ForwardNow = Body.GetRotation() * (MeshXf.GetRotation().Inverse() * ForwardNow);
				Origin += Body.GetLocation() - MeshXf.GetLocation();
				MeshXf.SetRotation(Body.GetRotation());
			}
		}
	}
	// TrajectoryScale (character StrideScale): world distances back into clip distances. Facing is a unit vector - unaffected.
	auto ToPos = [&](const FVector& WorldPos) { return MeshXf.InverseTransformVectorNoScale(WorldPos - Origin) * TrajectoryScale; };
	auto ToDir = [&](const FVector& WorldDir) { return MeshXf.InverseTransformVectorNoScale(WorldDir) * TrajectoryScale; };
	auto Set2 = [Query](int32 i, const FVector& V) { Query[i] = V.X; Query[i + 1] = V.Y; };
	auto Set3 = [Query](int32 i, const FVector& V) { Query[i] = V.X; Query[i + 1] = V.Y; Query[i + 2] = V.Z; };

	if (Db->Schema == EMotionMatchingSchema::Jump)
	{
		const float* Times = UMotionMatchingDatabase::JumpFutureTimes;
		FVector Positions[2], Velocities[2];
		if (Move->IsFalling())
		{
			PredictAir(Times, 2, Positions, Velocities);
		}
		else
		{
			PredictGround(Times, 2, Positions, nullptr, Velocities);
		}
		Set3(0, ToPos(PastPosition(UMotionMatchingDatabase::JumpPastTime)));
		Set3(3, ToDir(Move->Velocity).GetClampedToMaxSize(1.f));
		Set3(6, ToDir(ForwardNow).GetSafeNormal());
		for (int32 k = 0; k < 2; ++k)
		{
			Set3(9 + k * 9, ToDir(Velocities[k]));
			Set3(12 + k * 9, ToPos(Positions[k]));
			Set3(15 + k * 9, ToDir(Velocities[k]).GetClampedToMaxSize(1.f));
			OutDrawPositions[OutNumDraw++] = Positions[k];
		}
	}
	else
	{
		const float* Times = UMotionMatchingDatabase::FutureTimes;
		FVector Positions[3], Facings[3], Velocities[3];
		PredictGround(Times, 3, Positions, Facings, Velocities);
		const FVector VelNow = Move->Velocity * FVector(1.f, 1.f, 0.f);
		Set2(0, ToPos(PastPosition(UMotionMatchingDatabase::PastTime)));
		Set2(2, ToDir(VelNow));
		Set2(4, ToDir(ForwardNow).GetSafeNormal2D());
		Set2(6, ToPos(Positions[0]));
		Set2(8, ToDir(Facings[0]).GetSafeNormal2D());
		Set2(10, ToPos(Positions[1]));
		Set2(12, ToDir(Facings[1]).GetSafeNormal2D());
		Set2(14, ToDir(Velocities[1]));
		Set3(16, ToDir(Velocities[2]).GetClampedToMaxSize(1.f)); // UE5 VelocityDirection encoding
		for (int32 k = 0; k < 3; ++k)
		{
			OutDrawPositions[OutNumDraw++] = Positions[k];
		}
	}
	Db->Normalize(Query);
	return true;
}

void UMotionMatchingComponent::ResumeWhenSlotFree()
{
	if (!bSuspended || !GetWorld())
	{
		return;
	}
	TWeakObjectPtr<UMotionMatchingComponent> WeakThis(this);
	GetWorld()->GetTimerManager().SetTimerForNextTick([WeakThis]()
	{
		UMotionMatchingComponent* Self = WeakThis.Get();
		if (!Self || !Self->bSuspended || !Self->Character)
		{
			return;
		}
		const UAnimInstance* Anim = Self->Character->GetMesh() ? Self->Character->GetMesh()->GetAnimInstance() : nullptr;
		if (Anim && Anim->GetCurrentActiveMontage())
		{
			return; // another action clip owns DefaultSlot now (it resumes us when it ends)
		}
		Self->SetSuspended(false);
	});
}

void UMotionMatchingComponent::SetSuspended(bool bInSuspended)
{
	if (bSuspended == bInSuspended)
	{
		return;
	}
	bSuspended = bInSuspended;
	if (bSuspended && Character)
	{
		// Another system's montage owns the body (attack, dodge, climb...): its root motion moves + turns the capsule, as before the
		// GASP model - orienting to movement would turn a side dodge sideways. The tick re-applies the model on resume.
		Character->GetCharacterMovement()->bOrientRotationToMovement = false;
	}
	if (!bSuspended)
	{
		// The montage we were following was replaced: start clean, and let the tick re-apply the root motion mode.
		CurrentDb = nullptr;
		CurrentSequence = INDEX_NONE;
		CurrentMontage = nullptr;
		History.Reset();
		PositionHistory.Reset();
		bWasInAir = Character->GetCharacterMovement()->IsFalling();
	}
}

void UMotionMatchingComponent::SearchAndPlay(bool bForce)
{
	UMotionMatchingDatabase* Db = ChooseDatabase();
	float Query[UMotionMatchingDatabase::MaxFeatures];
	FVector DrawPositions[3];
	int32 NumDraw = 0;
	if (!BuildQuery(Db, Query, DrawPositions, NumDraw))
	{
		return;
	}

	// Chooser filter + skip poses that were playing in the last PoseReselectHistory seconds (within one sample of them).
	const float SampleTolerance = 1.f / Db->SampleRate;
	const bool bHasInputNow = !Character->GetCharacterMovement()->GetCurrentAcceleration().IsNearlyZero();
	const bool bAirFilter = bNoGroundFramesInAir && Db == JumpDatabase && Character->GetCharacterMovement()->IsFalling();
	// UE5 bDisableReselection (PopulateNonSelectableIdx): the playing clip's other frames can't be picked; the playing frame still
	// competes as the continuing pose below.
	const int32 NonSelectableSeq = (!bForce && Db == CurrentDb && Db->Sequences.IsValidIndex(CurrentSequence)
		&& Db->Sequences[CurrentSequence].bDisableReselection) ? CurrentSequence : INDEX_NONE;
	auto IsExcluded = [this, Db, SampleTolerance, bHasInputNow, bAirFilter, NonSelectableSeq](int32 Seq, float Time)
	{
		if (Seq == NonSelectableSeq || IsClipExcludedByState(Db, Seq, bHasInputNow))
		{
			return true;
		}
		if (bAirFilter)
		{
			const int32 F = Db->FindFrame(Seq, Time);
			if (JumpGroundFrame.IsValidIndex(F) && JumpGroundFrame[F])
			{
				return true; // in the air: no frames with the clip's feet on the ground ('air walk')
			}
		}
		for (const FPlayedPose& P : History)
		{
			if (P.Db == Db && P.Sequence == Seq && FMath::Abs(P.Time - Time) <= SampleTolerance)
			{
				return true;
			}
		}
		return false;
	};

	if (bLogSwitches && SinceRelease >= 0.f && ReleaseLogsDone < 3)
	{
		const float Marks[3] = { 0.f, 0.15f, 0.4f };
		if (SinceRelease >= Marks[ReleaseLogsDone])
		{
			Db->LogCandidates(Query, bEdgeIsPress ? TEXT("Start") : TEXT("Stop"), FString::Printf(TEXT("%s+%.2fs playing %s(%.2f) speed %.0f"),
				bEdgeIsPress ? TEXT("press") : TEXT("release"), SinceRelease, *GetCurrentClipName(), CurrentTime, Character->GetVelocity().Size2D()));
			++ReleaseLogsDone;
		}
	}

	float Cost = 0.f;
	const int32 Best = Db->Search(Query, Cost, IsExcluded);
	if (Best == INDEX_NONE)
	{
		return;
	}
	LastCost = Cost;

	const FMotionMatchingFrame& Frame = Db->GetFrame(Best);
	const bool bSameDb = CurrentDb == Db && CurrentSequence != INDEX_NONE;
	const bool bSameSpot = bSameDb && Frame.SequenceIndex == CurrentSequence && FMath::Abs(Frame.Time - CurrentTime) < SameClipTolerance;

	// Like UE5: keep playing unless a candidate beats the continuing pose (its cost + the clip's ContinuingPoseCostBias).
	const int32 CurrentFrame = bSameDb ? Db->FindFrame(CurrentSequence, CurrentTime) : INDEX_NONE;
	const float CurrentCost = (!bForce && CurrentFrame != INDEX_NONE)
		? Db->Cost(CurrentFrame, Query) + Db->Sequences[CurrentSequence].ContinuingPoseCostBias
		: MAX_flt;

	if (!bSameSpot && Cost < CurrentCost)
	{
		if (bLogSwitches)
		{
			UE_LOG(LogTemp, Log, TEXT("MMSwitch t=%.2f %s(%.2f, cost %.2f) -> %s(%.2f, cost %.2f)%s%s"),
				GetWorld()->GetTimeSeconds(), *GetCurrentClipName(), CurrentTime, CurrentCost < MAX_flt ? CurrentCost : -1.f,
				*GetNameSafe(Db->Sequences[Frame.SequenceIndex].Sequence), Frame.Time, Cost, bForce ? TEXT(" FORCED") : TEXT(""),
				Db == JumpDatabase ? TEXT(" [jump db]") : Db == CrouchDatabase ? TEXT(" [crouch db]") : Db != Database ? TEXT(" [other db]") : TEXT(""));
			if (Db == JumpDatabase && Character->GetCharacterMovement()->IsFalling())
			{
				// Debug: why this in-air switch (per-block cost + raw query/frame values).
				UE_LOG(LogTemp, Log, TEXT("MMAir cur %s"), CurrentFrame != INDEX_NONE ? *Db->DescribeFrame(CurrentFrame, Query) : TEXT("none"));
				UE_LOG(LogTemp, Log, TEXT("MMAir new %s"), *Db->DescribeFrame(Best, Query));
			}
		}
		PlayFrame(Db, Best);
	}

	if (bDebugDraw)
	{
		UWorld* World = GetWorld();
		const float Life = FMath::Max(SearchInterval, 0.f);
		FVector Prev = Character->GetActorLocation();
		for (int32 k = 0; k < NumDraw; ++k)
		{
			DrawDebugLine(World, Prev, DrawPositions[k], FColor::Green, false, Life, 0, 2.f);
			Prev = DrawPositions[k];
		}
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(reinterpret_cast<uint64>(this), FMath::Max(SearchInterval * 2.f, 0.1f), FColor::Yellow,
				FString::Printf(TEXT("MM: %s  t=%.2f  cost=%.2f"), *GetCurrentClipName(), CurrentTime, LastCost));
		}
	}
}

void UMotionMatchingComponent::PlayFrame(UMotionMatchingDatabase* Db, int32 FrameIndex)
{
	const FMotionMatchingFrame& Frame = Db->GetFrame(FrameIndex);
	const FMotionMatchingSequence& Entry = Db->Sequences[Frame.SequenceIndex];
	UAnimInstance* Anim = Character->GetMesh() ? Character->GetMesh()->GetAnimInstance() : nullptr;
	if (!Anim || !Entry.Sequence)
	{
		return;
	}

	CurrentMontage = Anim->PlaySlotAnimationAsDynamicMontage(Entry.Sequence, SlotName, BlendTime, BlendTime, 1.f,
		Entry.bLoop ? 1000 : 1, -1.f, Frame.Time);
	CurrentDb = Db;
	CurrentSequence = Frame.SequenceIndex;
	CurrentTime = Frame.Time;
	++SwitchCount;
}

bool UMotionMatchingComponent::IsPlayingGroundFrame() const
{
	if (!CurrentDb || !CurrentDb->Sequences.IsValidIndex(CurrentSequence) || !CurrentDb->Sequences[CurrentSequence].Sequence)
	{
		return false;
	}
	if (CurrentDb == JumpDatabase && CurrentDb->Sequences[CurrentSequence].bLoop)
	{
		return false; // fall loop: in place (root Z 0) but it is the fall pose, same rule as JumpGroundFrame
	}
	return MotionMatchingAir::IsClipGroundedAt(CurrentDb->Sequences[CurrentSequence].Sequence, CurrentTime);
}

bool UMotionMatchingComponent::IsGaspMovementModelEnabled() const
{
	return bGaspMovementModel && CVarGaspMovementModel.GetValueOnGameThread() != 0;
}

bool UMotionMatchingComponent::UsesGaspMovementModel() const
{
	// Battle stance keeps the tuned root-motion model (GASP has no battle stance); crouching in it is GASP crouch.
	return IsGaspMovementModelEnabled() && !bSuspended && Character
		&& (!IsCombatStance() || (CrouchDatabase && Character->bIsCrouched));
}

UAnimSequence* UMotionMatchingComponent::GetCurrentClip() const
{
	return (CurrentDb && CurrentDb->Sequences.IsValidIndex(CurrentSequence)) ? CurrentDb->Sequences[CurrentSequence].Sequence : nullptr;
}

FVector UMotionMatchingComponent::GetDesiredFacing() const
{
	// Same facing PredictGround puts on every future sample (GASP TrajectoryGenerationData RotateTowardsMovementSpeed 0).
	if (!Character)
	{
		return FVector::ForwardVector;
	}
	const FVector Accel = Character->GetCharacterMovement()->GetCurrentAcceleration() * FVector(1.f, 1.f, 0.f);
	if (IsCombatStance() && !(CrouchDatabase && Character->bIsCrouched))
	{
		return StrafeFacing;
	}
	return Accel.IsNearlyZero() ? Character->GetActorForwardVector().GetSafeNormal2D() : Accel.GetSafeNormal2D();
}

bool UMotionMatchingComponent::IsPlayingTurnInPlace() const
{
	if (CurrentDb && CurrentDb == CrouchDatabase && CrouchGroup.IsValidIndex(CurrentSequence))
	{
		return CrouchGroup[CurrentSequence] == ECrouchGroup::TurnInPlace;
	}
	const FString Name = GetCurrentClipName();
	return Name.Contains(TEXT("Stand_Turn_")) || Name.Contains(TEXT("Crouch_Idle_Turn_"));
}

FString UMotionMatchingComponent::GetCurrentClipName() const
{
	return (CurrentDb && CurrentDb->Sequences.IsValidIndex(CurrentSequence)) ? GetNameSafe(CurrentDb->Sequences[CurrentSequence].Sequence) : FString(TEXT("none"));
}

void UMotionMatchingComponent::SetCrouchDatabase(UMotionMatchingDatabase* Db)
{
	if (!Db || Db->NumFrames() == 0)
	{
		if (Db)
		{
			UE_LOG(LogTemp, Warning, TEXT("MotionMatching on %s: CrouchDatabase %s is not built - keeping %s."), *GetNameSafe(GetOwner()),
				*Db->GetName(), *GetNameSafe(CrouchDatabase));
		}
		return;
	}
	CrouchDatabase = Db;
	// GASP PSD_Dense_Crouch_* membership, written into each entry's Group.
	CrouchGroup.Init(ECrouchGroup::None, CrouchDatabase->Sequences.Num());
	for (int32 i = 0; i < CrouchDatabase->Sequences.Num(); ++i)
	{
		const FName G = CrouchDatabase->Sequences[i].Group;
		CrouchGroup[i] = G == TEXT("Idles") ? ECrouchGroup::Idles : G == TEXT("TurnInPlace") ? ECrouchGroup::TurnInPlace
			: G == TEXT("Loops") ? ECrouchGroup::Loops : G == TEXT("Pivots") ? ECrouchGroup::Pivots
			: G == TEXT("Starts") ? ECrouchGroup::Starts : G == TEXT("Stops") ? ECrouchGroup::Stops : ECrouchGroup::None;
		if (CrouchGroup[i] == ECrouchGroup::None)
		{
			UE_LOG(LogTemp, Warning, TEXT("MotionMatching: crouch clip %s has no GASP group - never chosen."), *GetNameSafe(CrouchDatabase->Sequences[i].Sequence));
		}
	}
}

void UMotionMatchingComponent::SetCombatDatabase(UMotionMatchingDatabase* Db)
{
	if (!Db || Db->NumFrames() == 0)
	{
		if (Db)
		{
			UE_LOG(LogTemp, Warning, TEXT("MotionMatching on %s: CombatDatabase %s is not built - keeping %s."), *GetNameSafe(GetOwner()),
				*Db->GetName(), *GetNameSafe(CombatDatabase));
		}
		return;
	}
	CombatDatabase = Db;
	IsCombatStoppedClip.Init(false, CombatDatabase->Sequences.Num());
	for (int32 i = 0; i < CombatDatabase->Sequences.Num(); ++i)
	{
		const FString Name = GetNameSafe(CombatDatabase->Sequences[i].Sequence);
		for (const FString& Word : CombatStoppedClips)
		{
			IsCombatStoppedClip[i] |= Name.Contains(Word);
		}
	}
	IsCombatSprintClip.Init(false, CombatDatabase->Sequences.Num());
	bCombatDbHasSprint = false;
	bCombatDbHasTurn = false;
	for (int32 i = 0; i < CombatDatabase->Sequences.Num(); ++i)
	{
		IsCombatSprintClip[i] = GetNameSafe(CombatDatabase->Sequences[i].Sequence).Contains(TEXT("Sprint"));
		bCombatDbHasSprint |= IsCombatSprintClip[i];
		bCombatDbHasTurn |= GetNameSafe(CombatDatabase->Sequences[i].Sequence).Contains(TEXT("Turn"));
	}
	IsCombatWalkClip.Init(false, CombatDatabase->Sequences.Num());
	IsCombatRunClip.Init(false, CombatDatabase->Sequences.Num());
	for (int32 i = 0; i < CombatDatabase->Sequences.Num(); ++i)
	{
		const FString Name = GetNameSafe(CombatDatabase->Sequences[i].Sequence);
		IsCombatWalkClip[i] = Name.Contains(TEXT("Walk"));
		IsCombatRunClip[i] = Name.Contains(TEXT("Run"));
	}
	// travel direction of each clip from its root motion, in the animation's space (forward = AnimForward, right = 90 deg from it)
	CombatClipYaw.Init(NoClipYaw, CombatDatabase->Sequences.Num());
	const FVector AnimFwd = CombatDatabase->AnimForward.GetSafeNormal2D();
	const FVector AnimRight = FRotator(0.f, 90.f, 0.f).RotateVector(AnimFwd);
	for (int32 i = 0; i < CombatDatabase->Sequences.Num(); ++i)
	{
		if (const UAnimSequence* Seq = CombatDatabase->Sequences[i].Sequence)
		{
			const FVector D = Seq->ExtractRootMotion(0.f, Seq->SequenceLength, false).GetLocation();
			if (D.Size2D() > 10.f)
			{
				CombatClipYaw[i] = FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(D, AnimRight), FVector::DotProduct(D, AnimFwd)));
			}
		}
	}
}

bool UMotionMatchingComponent::IsCombatClipOffDirection(int32 Sequence) const
{
	if (!CombatClipYaw.IsValidIndex(Sequence) || CombatClipYaw[Sequence] == NoClipYaw || !Character)
	{
		return false;
	}
	const FVector Input = Character->GetCharacterMovement()->GetCurrentAcceleration().GetSafeNormal2D();
	if (Input.IsNearlyZero())
	{
		return false;
	}
	// input direction from where the character faces (the clips move relative to the body)
	const float InputYaw = FMath::FindDeltaAngleDegrees(Character->GetActorRotation().Yaw, Input.Rotation().Yaw);
	float Nearest = 180.f;
	for (int32 i = 0; i < CombatClipYaw.Num(); ++i)
	{
		if (CombatClipYaw[i] != NoClipYaw && IsCombatWalkClip[i] == IsCombatWalkClip[Sequence] && IsCombatRunClip[i] == IsCombatRunClip[Sequence])
		{
			Nearest = FMath::Min(Nearest, FMath::Abs(FMath::FindDeltaAngleDegrees(CombatClipYaw[i], InputYaw)));
		}
	}
	return FMath::Abs(FMath::FindDeltaAngleDegrees(CombatClipYaw[Sequence], InputYaw)) > Nearest + CombatDirectionSlack;
}

void UMotionMatchingComponent::ResetAfterTeleport()
{
    if(!Character)return;
    if(UAnimInstance* Anim=Character->GetMesh()->GetAnimInstance())Anim->StopAllMontages(0.f);
    Character->GetCharacterMovement()->StopMovementImmediately();
    History.Reset();PositionHistory.Reset();CurrentDb=nullptr;CurrentSequence=INDEX_NONE;CurrentMontage=nullptr;
    bSuspended=false;SearchTimer=0.f;LandWorldTime=-100.f;bRollLanding=false;bFallLoopOnly=false;
    bWasInAir=Character->GetCharacterMovement()->IsFalling();
    if(bWasInAir)return;
    UMotionMatchingDatabase* Db=ChooseDatabase();
    if(!Db || Db->NumFrames()==0)return;
    for(int32 I=0;I<Db->Sequences.Num();++I)
    {
        const FMotionMatchingSequence& E=Db->Sequences[I];
        if(E.Sequence && E.bLoop && E.Sequence->GetName().Contains(TEXT("Idle")))
        {
            const int32 Frame=Db->FindFrame(I,FMath::Max(0.f,E.SampleStart));
            if(Frame>=0){PlayFrame(Db,Frame);break;}
        }
    }
}
