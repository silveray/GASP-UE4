// GASP-style third-person camera. Drives the owner's SpringArm + Camera every frame:
//  - over-the-shoulder offset (swap shoulders with SwapShoulder()),
//  - distance / height / FOV blend from the Walk to the Run preset by ground speed,
//  - closer and higher when looking down, closer and lower when looking up,
//  - smooth position lag (spring arm lag), collision handled by the spring arm.
//  - 10-01 climb fix (CVar trav.CamFix 1): speed from the real movement (traversal moves the capsule with velocity 0 -> camera
//    pulled in to the Walk preset) and SetCollisionPaused() while a traversal moves the capsule through the obstacle's corner
//    (the arm's probe started inside the box -> camera snapped onto the character).
// Add to a Character that has a USpringArmComponent and a UCameraComponent. All numbers are tunable in Details.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GaspCameraComponent.generated.h"

class UCameraComponent;
class USpringArmComponent;

USTRUCT(BlueprintType)
struct FGaspCameraPreset
{
	GENERATED_BODY()

	/** Spring arm length (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float Distance = 280.f;

	/** Sideways shoulder offset (cm, + = right shoulder). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float Shoulder = 45.f;

	/** Height above the pivot (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float Height = 55.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float FOV = 75.f;
};

UCLASS(ClassGroup = (Camera), meta = (BlueprintSpawnableComponent))
class NINJA_PROJECT_API UGaspCameraComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UGaspCameraComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera")
	FGaspCameraPreset Walk = { 260.f, 45.f, 55.f, 75.f };

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera")
	FGaspCameraPreset Run = { 320.f, 50.f, 60.f, 82.f };

	/** Ground speed (cm/s) that counts as full Walk / full Run; blends in between. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera")
	float WalkSpeed = 180.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera")
	float RunSpeed = 400.f;

	/** How fast distance / offset / FOV follow the target (higher = snappier). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera", meta = (ClampMin = "0.1"))
	float BlendSpeed = 3.f;

	/** Distance multiplier when looking straight down / straight up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera|Pitch")
	float LookDownDistanceScale = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera|Pitch")
	float LookUpDistanceScale = 0.55f;

	/** Extra height when looking straight down (cm); looking up lowers by the same amount. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera|Pitch")
	float PitchHeightOffset = 40.f;

	/** Spring arm position lag (higher = tighter follow). 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GASP Camera")
	float PositionLagSpeed = 10.f;

	/** Traversal: no spring-arm collision test while the capsule passes through the obstacle (restored on false). */
	void SetCollisionPaused(bool bPaused);

	UFUNCTION(BlueprintCallable, Category = "GASP Camera")
	void SwapShoulder() { bRightShoulder = !bRightShoulder; }

	/** Cinematic override (finisher close-up, 10-02): this preset instead of Walk/Run, Shoulder used as given (+ = right),
	 *  blended in REAL time (works in slow motion). Off = back to normal. */
	void SetOverride(bool bOn, const FGaspCameraPreset& Preset = FGaspCameraPreset()) { bOverride = bOn; OverridePreset = Preset; }

	/** 10-04: the arm shortens by Amount cm over InTime, holds, eases back
	 *  over OutTime (real seconds: works in the freeze / slow motion). A new push restarts it. */
	void Push(float Amount, float InTime, float HoldTime, float OutTime);
	bool IsPushing() const { return PushStart >= 0.0; }
	int32 GetPushCount() const { return PushCount; }
	bool IsOverridden() const { return bOverride; }

protected:

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:

	UPROPERTY(Transient)
	USpringArmComponent* Arm = nullptr;

	UPROPERTY(Transient)
	UCameraComponent* Camera = nullptr;

	bool bRightShoulder = true;
	bool bOverride = false;
	FGaspCameraPreset OverridePreset;
	float CurrentShoulderSign = 1.f;
	FVector LastOwnerLocation = FVector::ZeroVector;
	float MovedSpeed = 0.f; // smoothed ground speed from the actor's real movement
	float CurrentDistance = 0.f;
	FVector CurrentOffset = FVector::ZeroVector;
	float CurrentFOV = 0.f;
	double PushStart = -1.0;
	float PushAmount = 0.f, PushIn = 0.f, PushHold = 0.f, PushOut = 0.f;
	int32 PushCount = 0;
	float PushOffset(); // cm the arm is shorter right now (0 when no push runs)
};
