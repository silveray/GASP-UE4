// See AvoidanceComponent.h. Logic = the 5.8 file; changes marked "4.27".

#include "AvoidanceComponent.h"

#include "Components/CapsuleComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarAvoidTwist(TEXT("avoid.Twist"), 1,
	TEXT("1 = upper body turns away from walls (UAvoidanceComponent). 0 = off (for comparing)."));

namespace AvoidancePrivate
{
	// A hit facing mostly up/down is floor or ceiling, not a wall (steep slope beside the path).
	constexpr float MaxWallNormalZ = 0.7f;
}

UAvoidanceComponent::UAvoidanceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	ObstacleObjectTypes.Add(UEngineTypes::ConvertToObjectType(ECC_WorldStatic));
	ObstacleObjectTypes.Add(UEngineTypes::ConvertToObjectType(ECC_Pawn)); // 5.8 final setting (09-23): NPCs too
}

FRotator UAvoidanceComponent::GetSpineTwistRotation() const
{
	return CVarAvoidTwist.GetValueOnAnyThread() != 0 ? FRotator(0.f, -AvoidanceAlphaSigned * MaxSpineTwistDegrees, 0.f) : FRotator::ZeroRotator;
}

float UAvoidanceComponent::GetCapsuleRadius() const
{
	return Capsule ? Capsule->GetUnscaledCapsuleRadius() : 0.f;
}

void UAvoidanceComponent::BeginPlay()
{
	Super::BeginPlay();
	const AActor* Owner = GetOwner();
	Capsule = Owner ? Owner->FindComponentByClass<UCapsuleComponent>() : nullptr;
	if (!Capsule)
	{
		UE_LOG(LogTemp, Warning, TEXT("AvoidanceComponent on %s: no capsule found."), *GetNameSafe(Owner));
		return;
	}
	DefaultCapsuleRadius = Capsule->GetUnscaledCapsuleRadius();
	Movement = Owner->FindComponentByClass<UCharacterMovementComponent>();
	if (Movement)
	{
		Movement->AddTickPrerequisiteComponent(this); // this -> movement (5.8 order; no PreCMCTick component here)
	}
}

void UAvoidanceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Capsule && DefaultCapsuleRadius > 0.f)
	{
		Capsule->SetCapsuleRadius(DefaultCapsuleRadius); // never leave the pawn thin
	}
	Super::EndPlay(EndPlayReason);
}

void UAvoidanceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	const AActor* Owner = GetOwner();
	if (!Owner || DeltaTime <= 0.f)
	{
		return;
	}

	// 4.27: movement mode None = a traversal is moving the capsule -> no turn, no squeeze.
	const bool bMovementOff = Movement && Movement->MovementMode == MOVE_None;
	const FVector Velocity2D(Owner->GetVelocity().X, Owner->GetVelocity().Y, 0.f);
	const float Speed = Velocity2D.Size();
	const bool bMoving = !bMovementOff && Speed >= MinMoveSpeed;
	if (bMoving)
	{
		LastMoveDir = Velocity2D / Speed;
	}
	else if (LastMoveDir.IsNearlyZero())
	{
		LastMoveDir = FVector(Owner->GetActorForwardVector().X, Owner->GetActorForwardVector().Y, 0.f).GetSafeNormal();
	}

	const FVector TargetLookAhead = bMoving ? LastMoveDir * LookAheadDistance : FVector::ZeroVector;
	SmoothedLookAhead = FMath::VInterpTo(SmoothedLookAhead, TargetLookAhead, DeltaTime, DirectionInterpSpeed);

	float LeftAlpha = 0.f, RightAlpha = 0.f;
	if (bMoving || (bSidestepping && !bMovementOff))
	{
		const FVector Center = Owner->GetActorLocation();
		const FVector PathRight = GetPathRight();
		const FVector Start = Center + FVector(0.f, 0.f, TraceHeightOffset) + SmoothedLookAhead;
		LeftAlpha = SweepSide(Start, -PathRight, Center);
		RightAlpha = SweepSide(Start, PathRight, Center);
	}

	const float TargetSigned = (bMoving && !bSidestepping) ? RightAlpha - LeftAlpha : 0.f;
	const float TargetSqueeze = FMath::Max(LeftAlpha, RightAlpha);
	// Grow fast, relax slow.
	const float SignedSpeed = FMath::Abs(TargetSigned) > FMath::Abs(AvoidanceAlphaSigned) ? AlphaInterpSpeed : ReleaseInterpSpeed;
	const float SqueezeSpeed = TargetSqueeze > SqueezeAlpha ? AlphaInterpSpeed : ReleaseInterpSpeed;
	AvoidanceAlphaSigned = FMath::FInterpTo(AvoidanceAlphaSigned, TargetSigned, DeltaTime, SignedSpeed);
	SqueezeAlpha = FMath::FInterpTo(SqueezeAlpha, TargetSqueeze, DeltaTime, SqueezeSpeed);

	UpdateSidestep(DeltaTime, LeftAlpha, RightAlpha, bMoving);
	if (bResizeCapsule)
	{
		UpdateCapsule(DeltaTime);
	}

#if ENABLE_DRAW_DEBUG
	if (bDrawDebug)
	{
		DrawDebugString(GetWorld(), Owner->GetActorLocation() + FVector(0.f, 0.f, 110.f),
			FString::Printf(TEXT("Lean %.2f  Squeeze %.2f  R %.1f%s"), AvoidanceAlphaSigned, SqueezeAlpha, GetCapsuleRadius(),
				bSidestepping ? TEXT("  SIDESTEP") : TEXT("")), nullptr, bSidestepping ? FColor::Yellow : FColor::White, 0.f, true);
	}
#endif
}

FVector UAvoidanceComponent::GetPathRight() const
{
	// Sides of the PATH, not the body (a body turned sideways would lose the walls and flip every few frames).
	return FVector::CrossProduct(FVector::UpVector, LastMoveDir).GetSafeNormal();
}

void UAvoidanceComponent::UpdateSidestep(float DeltaTime, float LeftAlpha, float RightAlpha, bool bMoving)
{
	AActor* Owner = GetOwner();
	// Only when the owner's rotation logic leaves orient-to-movement on (our motion matching character turns by root motion
	// with it OFF, so this never runs there until MM supports a strafe facing).
	if (!bAllowSidestep || !Movement || !Owner || !Movement->bOrientRotationToMovement)
	{
		bSidestepping = false;
		return;
	}
	if (!bSidestepping)
	{
		if (!bMoving || SqueezeAlpha < SidestepEnterSqueeze)
		{
			return;
		}
		bSidestepping = true;
		const FVector PathRight = GetPathRight();
		if (LeftAlpha - RightAlpha > SidestepWallBias)
		{
			SidestepFacing = PathRight;
		}
		else if (RightAlpha - LeftAlpha > SidestepWallBias)
		{
			SidestepFacing = -PathRight;
		}
		else
		{
			SidestepFacing = FVector::DotProduct(Owner->GetActorForwardVector(), PathRight) >= 0.f ? PathRight : -PathRight;
		}
	}
	else if (SqueezeAlpha <= SidestepExitSqueeze)
	{
		bSidestepping = false;
		return;
	}
	const FVector PathRight = GetPathRight();
	SidestepFacing = FVector::DotProduct(SidestepFacing, PathRight) >= 0.f ? PathRight : -PathRight;
	Movement->bOrientRotationToMovement = false;
	Movement->bUseControllerDesiredRotation = false;
	Owner->SetActorRotation(FMath::RInterpTo(Owner->GetActorRotation(), SidestepFacing.Rotation(), DeltaTime, SidestepTurnSpeed));
}

float UAvoidanceComponent::SweepSide(const FVector& Start, const FVector& SideDir, const FVector& Center) const
{
	UWorld* World = GetWorld();
	if (!World || ObstacleObjectTypes.Num() == 0)
	{
		return 0.f;
	}
	FCollisionObjectQueryParams ObjectParams;
	for (const TEnumAsByte<EObjectTypeQuery>& ObjectType : ObstacleObjectTypes)
	{
		ObjectParams.AddObjectTypesToQuery(UEngineTypes::ConvertToCollisionChannel(ObjectType));
	}
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(AvoidanceSweep), false, GetOwner());
	// 4.27: lengths from the capsule surface.
	const float Radius = DefaultCapsuleRadius;
	const FVector End = Start + SideDir * (Radius + TraceGap);

	// An ignored actor (enemy) in front of a wall must not hide the wall: skip it and sweep again (max 4).
	FHitResult Hit;
	bool bHit = false;
	for (int32 Attempt = 0; Attempt < 4; ++Attempt)
	{
		bHit = World->SweepSingleByObjectType(Hit, Start, End, FQuat::Identity, ObjectParams, FCollisionShape::MakeSphere(TraceRadius), QueryParams);
		if (!bHit || !IsIgnoredActor(Hit.GetActor()))
		{
			break;
		}
		QueryParams.AddIgnoredActor(Hit.GetActor());
		bHit = false;
	}
	// Starting inside geometry = walking INTO a wall ahead, not along one: no turn.
	const bool bIsWall = bHit && !Hit.bStartPenetrating && FMath::Abs(Hit.ImpactNormal.Z) < AvoidancePrivate::MaxWallNormalZ;
	float Alpha = 0.f;
	if (bIsWall)
	{
		const float Gap = FMath::Abs(FVector::DotProduct(Hit.ImpactPoint - Center, SideDir)) - Radius;
		Alpha = FMath::GetMappedRangeValueClamped(FVector2D(FullAlphaGap, TraceGap), FVector2D(1.f, 0.f), Gap);
	}
#if ENABLE_DRAW_DEBUG
	if (bDrawDebug)
	{
		const FColor Color = bIsWall ? FColor::Red : FColor::Green;
		DrawDebugLine(World, Start, bIsWall ? FVector(Hit.Location) : End, Color, false, 0.f, 0, 1.f);
		if (bIsWall)
		{
			DrawDebugSphere(World, Hit.ImpactPoint, 6.f, 8, Color, false, 0.f);
		}
	}
#endif
	return Alpha;
}

bool UAvoidanceComponent::IsIgnoredActor(const AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}
	for (const FName& Tag : IgnoreActorsWithTags)
	{
		if (Actor->ActorHasTag(Tag))
		{
			return true;
		}
	}
	return false;
}

void UAvoidanceComponent::UpdateCapsule(float DeltaTime)
{
	if (!Capsule || DefaultCapsuleRadius <= 0.f)
	{
		return;
	}
	const float MinRadius = FMath::Max(1.f, DefaultCapsuleRadius - CapsuleShrinkAmount);
	const float TargetRadius = FMath::Lerp(DefaultCapsuleRadius, MinRadius, SqueezeAlpha);
	const float CurrentRadius = Capsule->GetUnscaledCapsuleRadius();
	const float NewRadius = FMath::FInterpTo(CurrentRadius, TargetRadius, DeltaTime, CapsuleInterpSpeed);
	if (FMath::IsNearlyEqual(NewRadius, CurrentRadius, 0.01f))
	{
		return; // SetCapsuleRadius re-runs overlap checks every call
	}
	if (NewRadius > CurrentRadius && !CanGrowCapsuleTo(NewRadius))
	{
		return; // growing is the only direction that can push into a wall
	}
	Capsule->SetCapsuleRadius(NewRadius);
}

bool UAvoidanceComponent::CanGrowCapsuleTo(float NewRadius) const
{
	UWorld* World = GetWorld();
	if (!World || !Capsule)
	{
		return false;
	}
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(AvoidanceCapsuleGrow), false, GetOwner());
	FCollisionResponseParams ResponseParams;
	Capsule->InitSweepCollisionParams(QueryParams, ResponseParams);
	const float Scale = Capsule->GetShapeScale();
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(NewRadius * Scale, Capsule->GetScaledCapsuleHalfHeight());
	return !World->OverlapBlockingTestByChannel(Capsule->GetComponentLocation(), Capsule->GetComponentQuat(),
		Capsule->GetCollisionObjectType(), Shape, QueryParams, ResponseParams);
}
