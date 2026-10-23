#include "GaspCameraComponent.h"

#include "Misc/App.h"

#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/IConsoleManager.h"

UGaspCameraComponent::UGaspCameraComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

void UGaspCameraComponent::BeginPlay()
{
	Super::BeginPlay();

	Arm = GetOwner()->FindComponentByClass<USpringArmComponent>();
	Camera = GetOwner()->FindComponentByClass<UCameraComponent>();
	if (!Arm || !Camera)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaspCamera on %s needs a SpringArm and a Camera."), *GetNameSafe(GetOwner()));
		SetComponentTickEnabled(false);
		return;
	}

	Arm->bUsePawnControlRotation = true;
	Arm->bEnableCameraLag = PositionLagSpeed > 0.f;
	Arm->CameraLagSpeed = PositionLagSpeed;
	Arm->bDoCollisionTest = true;

	CurrentDistance = Walk.Distance;
	CurrentOffset = FVector(0.f, Walk.Shoulder, Walk.Height);
	CurrentFOV = Walk.FOV;
	LastOwnerLocation = GetOwner()->GetActorLocation();
}

void UGaspCameraComponent::SetCollisionPaused(bool bPaused)
{
	if (Arm)
	{
		Arm->bDoCollisionTest = !bPaused;
	}
}

void UGaspCameraComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn)
	{
		return;
	}

	// Walk -> Run blend by ground speed.
	// Speed from the real movement (trav.CamFix): a traversal moves the capsule itself, so GetVelocity() is 0 there.
	static IConsoleVariable* CamFix = IConsoleManager::Get().FindConsoleVariable(TEXT("trav.CamFix"));
	const FVector Loc = Pawn->GetActorLocation();
	const float Moved = DeltaTime > 0.f ? FVector::Dist2D(Loc, LastOwnerLocation) / DeltaTime : 0.f;
	LastOwnerLocation = Loc;
	if (Moved < 2000.f) // > 2000 cm/s = a teleport, ignore
	{
		MovedSpeed = FMath::FInterpTo(MovedSpeed, Moved, DeltaTime, 10.f);
	}
	const float Speed = (CamFix && CamFix->GetInt() != 0) ? MovedSpeed : Pawn->GetVelocity().Size2D();
	const float Alpha = FMath::Clamp((Speed - WalkSpeed) / FMath::Max(RunSpeed - WalkSpeed, 1.f), 0.f, 1.f);
	float Distance = FMath::Lerp(Walk.Distance, Run.Distance, Alpha);
	float Height = FMath::Lerp(Walk.Height, Run.Height, Alpha);
	const float Shoulder = FMath::Lerp(Walk.Shoulder, Run.Shoulder, Alpha);
	const float FOV = FMath::Lerp(Walk.FOV, Run.FOV, Alpha);

	// Pitch: -1 looking straight down ... +1 looking straight up.
	const float Pitch = FRotator::NormalizeAxis(Pawn->GetControlRotation().Pitch) / 90.f;
	if (Pitch < 0.f)
	{
		Distance *= FMath::Lerp(1.f, LookDownDistanceScale, -Pitch);
		Height += PitchHeightOffset * -Pitch;
	}
	else
	{
		Distance *= FMath::Lerp(1.f, LookUpDistanceScale, Pitch);
		Height -= PitchHeightOffset * Pitch;
	}

	FVector OffsetGoal(0.f, Shoulder * (bRightShoulder ? 1.f : -1.f), Height);
	float Blend = BlendSpeed;
	if (bOverride)
	{
		Distance = OverridePreset.Distance;
		OffsetGoal = FVector(0.f, OverridePreset.Shoulder, OverridePreset.Height);
		DeltaTime = FApp::GetDeltaTime(); // real time: the finisher runs in slow motion
		Blend = BlendSpeed * 2.f;
	}

	// Smooth everything, including the shoulder swap.
	CurrentShoulderSign = FMath::FInterpTo(CurrentShoulderSign, bRightShoulder ? 1.f : -1.f, DeltaTime, BlendSpeed * 2.f);
	CurrentDistance = FMath::FInterpTo(CurrentDistance, Distance, DeltaTime, Blend);
	CurrentOffset = FMath::VInterpTo(CurrentOffset, bOverride ? OffsetGoal : FVector(0.f, Shoulder * CurrentShoulderSign, Height), DeltaTime, Blend);
	CurrentFOV = FMath::FInterpTo(CurrentFOV, bOverride ? OverridePreset.FOV : FOV, DeltaTime, Blend);

	Arm->TargetArmLength = FMath::Max(50.f, CurrentDistance - PushOffset()); // 10-04 parry push-in
	Arm->SocketOffset = CurrentOffset;
	Camera->SetFieldOfView(CurrentFOV);
}

void UGaspCameraComponent::Push(float Amount, float InTime, float HoldTime, float OutTime)
{
	if (Amount <= 0.f)
	{
		return;
	}
	PushStart = FPlatformTime::Seconds();
	PushAmount = Amount;
	PushIn = FMath::Max(0.01f, InTime);
	PushHold = FMath::Max(0.f, HoldTime);
	PushOut = FMath::Max(0.01f, OutTime);
	++PushCount;
}

float UGaspCameraComponent::PushOffset()
{
	if (PushStart < 0.0)
	{
		return 0.f;
	}
	const float T = float(FPlatformTime::Seconds() - PushStart);
	if (T >= PushIn + PushHold + PushOut)
	{
		PushStart = -1.0;
		return 0.f;
	}
	const float A = T < PushIn ? FMath::InterpEaseOut(0.f, 1.f, T / PushIn, 2.f)
		: T < PushIn + PushHold ? 1.f : 1.f - FMath::InterpEaseInOut(0.f, 1.f, (T - PushIn - PushHold) / PushOut, 2.f);
	return PushAmount * A;
}
