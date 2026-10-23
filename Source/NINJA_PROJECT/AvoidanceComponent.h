// Wall reaction (port of the UE5.8 UAvoidanceComponent, 10-02): two sideways sweeps along the PATH publish how close a wall
// (or a neutral NPC) is on the left/right; the upper body turns away from it (FAnimNode_MidearthSpineTwist reads
// GetSpineTwistRotation) and the capsule narrows in tight gaps so the character fits through.
// 4.27 changes vs 5.8: distances measured from the capsule SURFACE (FullAlphaGap / TraceGap; this capsule is 35 cm, the 5.8 one
// 23.5); paused while the movement mode is None (traversal moves the capsule); sidestep code kept but OFF by default - it needs
// motion matching to support a fixed (strafe) facing, our MM always faces the movement direction.
// Sweeps use object types (WorldStatic + Pawn), not Visibility. Actors tagged 'Enemy' are never avoided (faced, not squeezed past).
// Setup: add to the character; ABP: ... -> Local To Component -> Midearth Spine Twist -> Component To Local -> Output.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "AvoidanceComponent.generated.h"

class UCapsuleComponent;
class UCharacterMovementComponent;

UCLASS(ClassGroup = (Midearth), meta = (BlueprintSpawnableComponent))
class NINJA_PROJECT_API UAvoidanceComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UAvoidanceComponent();

	/** -1 = wall on the LEFT of the path, +1 = on the RIGHT, 0 = clear / standing still. Both sides cancel (a corridor needs no turn). */
	UFUNCTION(BlueprintPure, Category = "Midearth|Avoidance")
	float GetAvoidanceAlphaSigned() const { return AvoidanceAlphaSigned; }

	/** 0..1, how tightly squeezed on EITHER side (stays high in a corridor) - drives the capsule. */
	UFUNCTION(BlueprintPure, Category = "Midearth|Avoidance")
	float GetSqueezeAlpha() const { return SqueezeAlpha; }

	/** Upper-body turn, component space, yaw only (the UEFN mesh sits rotated -90 in the capsule). 0 when CVar avoid.Twist is 0. */
	UFUNCTION(BlueprintPure, Category = "Midearth|Avoidance")
	FRotator GetSpineTwistRotation() const;

	UFUNCTION(BlueprintPure, Category = "Midearth|Avoidance")
	bool IsSidestepping() const { return bSidestepping; }

	float GetCapsuleRadius() const;

	// --- sidestep (OFF: needs strafe facing in motion matching) ---

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Sidestep")
	bool bAllowSidestep = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Sidestep", meta = (ClampMin = "0", ClampMax = "1"))
	float SidestepEnterSqueeze = 0.85f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Sidestep", meta = (ClampMin = "0", ClampMax = "1"))
	float SidestepExitSqueeze = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Sidestep", meta = (ClampMin = "0"))
	float SidestepTurnSpeed = 16.0f; // 5.8 play test 09-24: 8 was too slow

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Sidestep", meta = (ClampMin = "0", ClampMax = "1"))
	float SidestepWallBias = 0.25f;

	// --- pose ---

	/** Upper-body turn at full squeeze (degrees). 4.27 MEASURED 10-02 (avoid.AutoTest, 15 cm from a wall): +35 = 40 cm of air at the
	 *  near shoulder, off = 30, -35 = 21 (the 5.8 value turns INTO the wall here). Keep +35 unless the test says otherwise. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Pose", meta = (ClampMin = "-90", ClampMax = "90"))
	float MaxSpineTwistDegrees = 35.0f;

	// --- trace (gaps measured from the capsule surface) ---

	/** A wall is noticed from this much air between shoulder and wall (cm). 5.8: 80 from the centre with radius 23.5 = ~57. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Trace", meta = (ClampMin = "1"))
	float TraceGap = 57.0f;

	/** This much air or less gives the full +-1 (cm). 5.8: 35 from the centre with radius 23.5 = ~12. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Trace", meta = (ClampMin = "0"))
	float FullAlphaGap = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Trace", meta = (ClampMin = "1"))
	float TraceRadius = 10.0f;

	/** Sweep height above the capsule centre (~shoulders). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Trace")
	float TraceHeightOffset = 45.0f;

	/** Sweeps start this far ahead along the path so the turn begins before the wall arrives. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Trace", meta = (ClampMin = "0"))
	float LookAheadDistance = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Trace")
	TArray<TEnumAsByte<EObjectTypeQuery>> ObstacleObjectTypes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Trace")
	TArray<FName> IgnoreActorsWithTags = { TEXT("Enemy") };

	// --- response ---

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Response", meta = (ClampMin = "0"))
	float MinMoveSpeed = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Response", meta = (ClampMin = "0"))
	float DirectionInterpSpeed = 8.0f;

	/** How quickly the turn grows when a wall comes close. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Response", meta = (ClampMin = "0"))
	float AlphaInterpSpeed = 6.0f;

	/** How quickly it relaxes when the wall is gone. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Response", meta = (ClampMin = "0"))
	float ReleaseInterpSpeed = 4.0f;

	// --- capsule ---

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Capsule")
	bool bResizeCapsule = true;

	/** cm taken off the starting radius at full squeeze (35 -> 29). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Capsule", meta = (ClampMin = "0"))
	float CapsuleShrinkAmount = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Midearth|Avoidance|Capsule", meta = (ClampMin = "0"))
	float CapsuleInterpSpeed = 6.0f;

	/** Sweeps (green clear / red wall) + values over the head. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Midearth|Avoidance|Debug")
	bool bDrawDebug = false;

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:

	float SweepSide(const FVector& Start, const FVector& SideDir, const FVector& Center) const;
	bool IsIgnoredActor(const AActor* Actor) const;
	FVector GetPathRight() const;
	void UpdateSidestep(float DeltaTime, float LeftAlpha, float RightAlpha, bool bMoving);
	void UpdateCapsule(float DeltaTime);
	bool CanGrowCapsuleTo(float NewRadius) const;

	UPROPERTY(Transient)
	UCharacterMovementComponent* Movement = nullptr;

	UPROPERTY(Transient)
	UCapsuleComponent* Capsule = nullptr;

	FVector LastMoveDir = FVector::ZeroVector;
	bool bSidestepping = false;
	FVector SidestepFacing = FVector::ZeroVector;

	UPROPERTY(VisibleInstanceOnly, Category = "Midearth|Avoidance|Debug")
	float AvoidanceAlphaSigned = 0.0f;

	UPROPERTY(VisibleInstanceOnly, Category = "Midearth|Avoidance|Debug")
	float SqueezeAlpha = 0.0f;

	FVector SmoothedLookAhead = FVector::ZeroVector;
	float DefaultCapsuleRadius = 0.0f;
};
