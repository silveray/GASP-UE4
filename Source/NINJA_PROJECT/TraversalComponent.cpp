// See TraversalComponent.h.

#include "TraversalComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MidearthLedgeFinder.h"
#include "GaspCameraComponent.h"
#include "HAL/IConsoleManager.h"
#include "MotionMatchingComponent.h"

static TAutoConsoleVariable<int32> CVarTravHandWarp(TEXT("trav.HandWarp"), 1,
	TEXT("1 = align the clip's hand plant with the real ledge (mantle/climb/vault). 0 = old: align the feet at the top."));

static TAutoConsoleVariable<int32> CVarTravHandIK(TEXT("trav.HandIK"), 1,
	TEXT("1 = hand IK onto the real ledge while the clip's hands rest on it. 0 = off."));

static TAutoConsoleVariable<int32> CVarTravCamFix(TEXT("trav.CamFix"), 1,
	TEXT("1 = camera stays out during traversal (no spring-arm collision while climbing, speed from real movement). 0 = old behaviour."));

namespace TraversalPrivate
{
	const TCHAR* ActionName(ETraversalAction A)
	{
		switch (A)
		{
		case ETraversalAction::Hurdle: return TEXT("Hurdle");
		case ETraversalAction::Vault:  return TEXT("Vault");
		case ETraversalAction::Mantle: return TEXT("Mantle");
		case ETraversalAction::Climb:  return TEXT("Climb");
		default:                       return TEXT("None");
		}
	}
}

UTraversalComponent::UTraversalComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	const FString Base = TEXT("/Game/Animations/GASP/Traversal/");
	auto Add = [this, &Base](const TCHAR* Folder, const TCHAR* Name)
	{
		const FString Path = FString::Printf(TEXT("%s%s/M_Neutral_Traversal_%s.M_Neutral_Traversal_%s"), *Base, Folder, Name, Name);
		Clips.Add(TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(Path)));
	};
	for (const TCHAR* Gait : { TEXT("stand"), TEXT("walk"), TEXT("run") })
	{
		for (const TCHAR* Foot : { TEXT("Lfoot"), TEXT("Rfoot") })
		{
			Add(TEXT("Mantle"), *FString::Printf(TEXT("Mantle_1_0_%s_F_%s"), Gait, Foot));
			Add(TEXT("Climb"), *FString::Printf(TEXT("Climb_Start_2_5_%s_F_%s"), Gait, Foot));
		}
	}
	Add(TEXT("Vault"), TEXT("Vault_1_0_stand_F_Lfoot"));
	Add(TEXT("Vault"), TEXT("Vault_1_0_walk_F_Rfoot"));
	Add(TEXT("Vault"), TEXT("Vault_1_0_run_F_Lfoot"));
	for (const TCHAR* Name : { TEXT("Hurdle_1_0_stand_F_V2_Lfoot"), TEXT("Hurdle_1_0_stand_F_V2_Rfoot"),
		TEXT("Hurdle_1_0_walk_F_Lfoot"), TEXT("Hurdle_1_0_walk_F_Rfoot"), TEXT("Hurdle_1_0_walk_F_V2_Lfoot"), TEXT("Hurdle_1_0_walk_F_V2_Rfoot"),
		TEXT("Hurdle_1_0_run_F_Lfoot"), TEXT("Hurdle_1_0_run_F_Rfoot"), TEXT("Hurdle_1_0_run_F_V2_Lfoot"), TEXT("Hurdle_1_0_run_F_V2_Rfoot") })
	{
		Add(TEXT("Hurdle"), Name);
	}
}

void UTraversalComponent::BeginPlay()
{
	Super::BeginPlay();
	Character = Cast<ACharacter>(GetOwner());
	AnalyzeClips();
	// Place the capsule BEFORE the mesh animates, so the hand IK targets (world -> component space) use this frame's position.
	if (Character && Character->GetMesh())
	{
		Character->GetMesh()->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
	}
}

FVector UTraversalComponent::ClipRoot(const FClipInfo& Clip, float Time) const
{
	// GASP clips: forward = +Y in animation space, +X = the character's left.
	const FVector V = Clip.Seq->ExtractRootTrackTransform(FMath::Clamp(Time, 0.f, Clip.Seq->SequenceLength), nullptr).GetTranslation();
	return FVector(V.Y, -V.X, V.Z);
}

FVector UTraversalComponent::ClipBone(const FClipInfo& Clip, FName Bone, float Time) const
{
	const USkeleton* Skeleton = Clip.Seq->GetSkeleton();
	if (!Skeleton)
	{
		return FVector::ZeroVector;
	}
	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	const int32 Index = Ref.FindBoneIndex(Bone);
	if (Index == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}
	// Local transforms from the clip's tracks (ref pose for bones without a track), chained root -> bone.
	const TArray<FTrackToSkeletonMap>& Map = Clip.Seq->GetCompressedTrackToSkeletonMapTable();
	TArray<int32, TInlineAllocator<16>> Chain;
	for (int32 B = Index; B != INDEX_NONE; B = Ref.GetParentIndex(B))
	{
		Chain.Add(B);
	}
	const float T = FMath::Clamp(Time, 0.f, Clip.Seq->SequenceLength);
	FTransform Component = FTransform::Identity;
	for (int32 c = Chain.Num() - 1; c >= 0; --c)
	{
		FTransform Local = Ref.GetRefBonePose()[Chain[c]];
		for (int32 Track = 0; Track < Map.Num(); ++Track)
		{
			if (Map[Track].BoneTreeIndex == Chain[c])
			{
				Clip.Seq->GetBoneTransform(Local, Track, T, false);
				break;
			}
		}
		Component = Local * Component;
	}
	const FVector V = Component.GetTranslation();
	return FVector(V.Y, -V.X, V.Z);
}

void UTraversalComponent::AnalyzeClips()
{
	ClipInfos.Reset();
	LoadedClips.Reset();
	for (const TSoftObjectPtr<UAnimSequence>& Soft : Clips)
	{
		UAnimSequence* Seq = Soft.LoadSynchronous();
		if (!Seq)
		{
			UE_LOG(LogTemp, Warning, TEXT("Traversal: clip missing %s"), *Soft.ToString());
			continue;
		}
		LoadedClips.Add(Seq);
		FClipInfo Info;
		Info.Seq = Seq;
		const FString Name = Seq->GetName();
		Info.Action = Name.Contains(TEXT("Mantle")) ? ETraversalAction::Mantle : Name.Contains(TEXT("Climb")) ? ETraversalAction::Climb
			: Name.Contains(TEXT("Vault")) ? ETraversalAction::Vault : Name.Contains(TEXT("Hurdle")) ? ETraversalAction::Hurdle : ETraversalAction::None;
		Info.Gait = Name.Contains(TEXT("_stand_")) ? 0 : Name.Contains(TEXT("_walk_")) ? 1 : 2;

		// Root track at 60 Hz: top / apex and the landing after it.
		const float Step = 1.f / 60.f;
		float ApexTime = 0.f;
		for (float T = 0.f; T <= Seq->SequenceLength; T += Step)
		{
			const float Z = ClipRoot(Info, T).Z;
			if (Z > Info.Top)
			{
				Info.Top = Z;
				ApexTime = T;
			}
		}
		const bool bOver = Info.Action == ETraversalAction::Hurdle || Info.Action == ETraversalAction::Vault;
		Info.ContactTime = ApexTime;
		if (!bOver)
		{
			for (float T = 0.f; T <= Seq->SequenceLength; T += Step)
			{
				if (ClipRoot(Info, T).Z >= Info.Top - 2.f)
				{
					Info.ContactTime = T;
					break;
				}
			}
		}
		else
		{
			for (float T = ApexTime; T <= Seq->SequenceLength; T += Step)
			{
				if (ClipRoot(Info, T).Z < 5.f)
				{
					Info.LandTime = T;
					break;
				}
			}
		}
		Info.Ledge = bOver ? 100.f : Info.Top;

		// Hand plant: before contact/apex, the steadiest frame of a hand at the clip's ledge height (Ledge-8 .. Ledge+35).
		// 10-01: 'first frame above 0.6 x ledge' caught a hand hanging at the hip (Mantle_stand_F_Rfoot, t 0.00 z 89).
		if (Info.Action != ETraversalAction::Hurdle)
		{
			float BestSpeed = 100.f; // cm/s; slower than this counts as resting on the ledge
			for (float T = 0.f; T < Info.ContactTime; T += Step)
			{
				for (const FName Hand : { FName(TEXT("hand_l")), FName(TEXT("hand_r")) })
				{
					const FVector P = ClipBone(Info, Hand, T);
					const float HandSpeed = (ClipBone(Info, Hand, T + Step) - P).Size() / Step;
					if (P.Z >= Info.Ledge - 8.f && P.Z <= Info.Ledge + 35.f && HandSpeed < BestSpeed)
					{
						BestSpeed = HandSpeed;
						Info.bHasPlant = true;
						Info.PlantTime = T;
						Info.PlantHand = P;
						Info.PlantBone = Hand;
					}
				}
			}
		}
		// Hand IK windows: per hand the longest run of frames resting (< 100 cm/s) at the ledge height, up to 1 s after contact.
		if (Info.Action != ETraversalAction::Hurdle)
		{
			const float Limit = FMath::Min(Seq->SequenceLength, Info.ContactTime + 1.f);
			for (int32 h = 0; h < 2; ++h)
			{
				const FName Hand = h == 0 ? FName(TEXT("hand_l")) : FName(TEXT("hand_r"));
				float RunStart = -1.f, BestLen = 0.f, RunMinSpeed = 1.e6f;
				FVector RunPos = FVector::ZeroVector;
				for (float T = 0.f; T <= Limit + Step; T += Step)
				{
					const FVector P = ClipBone(Info, Hand, T);
					const float HandSpeed = (ClipBone(Info, Hand, T + Step) - P).Size() / Step;
					const bool bRest = T <= Limit && P.Z >= Info.Ledge - 8.f && P.Z <= Info.Ledge + 35.f && HandSpeed < 100.f;
					if (bRest)
					{
						if (RunStart < 0.f)
						{
							RunStart = T;
							RunMinSpeed = 1.e6f;
						}
						if (HandSpeed < RunMinSpeed)
						{
							RunMinSpeed = HandSpeed;
							RunPos = P;
						}
					}
					else if (RunStart >= 0.f)
					{
						if (T - RunStart > BestLen)
						{
							BestLen = T - RunStart;
							Info.bHand[h] = true;
							Info.HandStart[h] = RunStart;
							Info.HandEnd[h] = T - Step;
							Info.HandPos[h] = RunPos;
						}
						RunStart = -1.f;
					}
				}
			}
		}
		UE_LOG(LogTemp, Log, TEXT("Traversal clip %-48s hand L %d %.2f-%.2f  R %d %.2f-%.2f"), *Name, Info.bHand[0], Info.HandStart[0], Info.HandEnd[0],
			Info.bHand[1], Info.HandStart[1], Info.HandEnd[1]);
		UE_LOG(LogTemp, Log, TEXT("Traversal clip %-48s top %3.0f contact %.2f plant %s %.2f fwd %4.0f (root %4.0f) z %3.0f (root %3.0f)"),
			*Name, Info.Top, Info.ContactTime, *Info.PlantBone.ToString(), Info.PlantTime, Info.PlantHand.X, ClipRoot(Info, Info.PlantTime).X,
			Info.PlantHand.Z, ClipRoot(Info, Info.PlantTime).Z);
		ClipInfos.Add(Info);
	}
	UE_LOG(LogTemp, Log, TEXT("Traversal: %d clips analysed"), ClipInfos.Num());
}

const UTraversalComponent::FClipInfo* UTraversalComponent::PickClip(ETraversalAction Action, int32 Gait) const
{
	TArray<const FClipInfo*> Best;
	int32 BestGaitDiff = MAX_int32;
	for (const FClipInfo& C : ClipInfos)
	{
		if (C.Action != Action)
		{
			continue;
		}
		const int32 Diff = FMath::Abs(C.Gait - Gait);
		if (Diff < BestGaitDiff)
		{
			BestGaitDiff = Diff;
			Best.Reset();
		}
		if (Diff == BestGaitDiff)
		{
			Best.Add(&C);
		}
	}
	return Best.Num() ? Best[FMath::RandRange(0, Best.Num() - 1)] : nullptr;
}

bool UTraversalComponent::TryTraverse()
{
	using namespace TraversalPrivate;
	if (bActive || !Character || ClipInfos.Num() == 0)
	{
		return false;
	}
	UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	UAnimInstance* Anim = Character->GetMesh() ? Character->GetMesh()->GetAnimInstance() : nullptr;
	if (!Move || !Capsule || !Anim || !Move->IsMovingOnGround())
	{
		return false;
	}
	UWorld* World = GetWorld();
	const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const FVector Location = Character->GetActorLocation();
	const FVector Feet = Location - FVector(0.f, 0.f, HalfHeight);
	FVector Dir = Move->GetCurrentAcceleration().GetSafeNormal2D();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector().GetSafeNormal2D();
	}
	const float Speed = Move->Velocity.Size2D();

	// 1) Something ahead (GASP: 75..350 cm by speed). The sweep's bottom sits just under MinHeight so steps don't count.
	FCollisionQueryParams Params(SCENE_QUERY_STAT(TraversalSweep), false, Character);
	const float SweepHalf = 60.f, SweepRadius = 30.f;
	const FVector SweepStart = Feet + FVector(0.f, 0.f, MinHeight - 5.f + SweepHalf);
	const float Reach = FMath::GetMappedRangeValueClamped(FVector2D(0.f, 500.f), FVector2D(75.f, 350.f), Speed);
	FHitResult Hit;
	if (!World->SweepSingleByChannel(Hit, SweepStart, SweepStart + Dir * Reach, FQuat::Identity, ECC_Visibility,
			FCollisionShape::MakeCapsule(SweepRadius, SweepHalf), Params)
		|| Hit.bStartPenetrating || !Hit.GetComponent() || Cast<APawn>(Hit.GetActor()))
	{
		return false;
	}

	// 2) Ledges on that component (ported 5.8 finder; MaxLedgeHeight measured from the feet).
	bool bFront = false, bBack = false;
	FVector Front, FrontNormal, Back, BackNormal;
	UMidearthLedgeFinder::FindLedges(Hit.GetComponent(), Hit.ImpactPoint, Hit.ImpactNormal, Feet, bFront, Front, FrontNormal,
		bBack, Back, BackNormal, 60.f, MaxHeight + 10.f, 250.f, bDebugDraw, 3.f);
	const float Height = Front.Z - Feet.Z;
	if (!bFront || Height < MinHeight || Height > MaxHeight)
	{
		return false;
	}
	const FVector Fwd = (-FrontNormal).GetSafeNormal2D();
	const float Depth = bBack ? FVector::Dist2D(Front, Back) : 1.e6f;

	// 3) What's behind a thin obstacle.
	bool bBackFloor = false;
	float BackFloorZ = 0.f;
	if (bBack)
	{
		FHitResult Floor;
		const FVector P = Back + Fwd * 40.f;
		if (World->LineTraceSingleByChannel(Floor, P + FVector(0.f, 0.f, 5.f), P - FVector(0.f, 0.f, 500.f), ECC_Visibility, Params))
		{
			bBackFloor = true;
			BackFloorZ = Floor.ImpactPoint.Z;
		}
	}

	ETraversalAction Action;
	if (bBack && Depth < ThinDepth && Height <= HurdleMaxHeight)
	{
		Action = (!bBackFloor || BackFloorZ < Feet.Z - 30.f) ? ETraversalAction::Vault
			: FMath::Abs(BackFloorZ - Feet.Z) <= 30.f ? ETraversalAction::Hurdle : ETraversalAction::Mantle;
	}
	else
	{
		Action = Height <= MantleMaxHeight ? ETraversalAction::Mantle : ETraversalAction::Climb;
	}
	const bool bOver = Action == ETraversalAction::Hurdle || Action == ETraversalAction::Vault;
	// 10-11 hang style: a climb stops when the hands land and UHangComponent hangs there (needs no room on top).
	bToHang = bHangOnClimb && Action == ETraversalAction::Climb && HangHandoff;

	// 4) Room for the capsule: on top (mantle/climb) or over the top (hurdle/vault).
	const FVector RoomCenter = bOver ? Front + Fwd * (Depth * 0.5f) + FVector(0.f, 0.f, HalfHeight + 5.f)
		: Front + Fwd * (Radius + ContactInset + 5.f) + FVector(0.f, 0.f, HalfHeight + 3.f);
	if (!bToHang && World->OverlapBlockingTestByChannel(RoomCenter, FQuat::Identity, Capsule->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(Radius, HalfHeight), Params, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
	{
		if (bDebugDraw)
		{
			DrawDebugCapsule(World, RoomCenter, HalfHeight, Radius, FQuat::Identity, FColor::Red, false, 3.f);
		}
		UE_LOG(LogTemp, Log, TEXT("Traversal: no room for %s (H %.0f)"), ActionName(Action), Height);
		return false;
	}

	// 5) Clip and warp numbers.
	const int32 Gait = Speed < 50.f ? 0 : Speed < 300.f ? 1 : 2;
	const FClipInfo* Clip = PickClip(Action, Gait);
	if (!Clip)
	{
		return false;
	}
	DistToContact = FVector::DotProduct(Front - Feet, Fwd) + (bOver ? Depth * 0.5f : ContactInset);
	ContactZ = bOver ? Height + (Clip->Top - 100.f) : Height; // GASP hurdle/vault clips clear a 1 m bar by Top - 100
	const float EdgeDist = FVector::DotProduct(Front - Feet, Fwd);
	EdgePoint = Front;
	bHandWarp = Clip->bHasPlant && CVarTravHandWarp.GetValueOnGameThread() != 0;
	bHandMeasured = false;
	LastHandDX = LastHandDZ = -999.f;
	LastHandDZTarget = Clip->bHasPlant ? Clip->PlantHand.Z - Clip->Ledge : 0.f;
	const float AlignTime = bHandWarp ? Clip->PlantTime : Clip->ContactTime;
	// Hand IK targets: on the real top, HandInset past the edge, the clip's sideways offset and wrist height above its ledge.
	bHandIK = CVarTravHandIK.GetValueOnGameThread() != 0;
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Fwd);
	for (int32 h = 0; h < 2; ++h)
	{
		HandTarget[h] = Feet + Fwd * (EdgeDist + HandInset) + Right * Clip->HandPos[h].Y
			+ FVector(0.f, 0.f, Height + FMath::Max(2.f, Clip->HandPos[h].Z - Clip->Ledge));
	}
	const FVector Cp = ClipRoot(*Clip, Clip->PlantTime);
	if (bHandWarp)
	{
		// Root forward at plant so the hand lands HandInset past the real edge.
		PlantRootFwd = EdgeDist + HandInset - (Clip->PlantHand.X - Cp.X);
		DistToContact = PlantRootFwd;
	}
	const float ClipContactFwd = ClipRoot(*Clip, AlignTime).X;
	// Start at the frame whose remaining run-up matches the real distance (keep >= 0.3 s to get there).
	StartTime = 0.f;
	for (float T = 0.f; T < AlignTime; T += 1.f / 60.f)
	{
		if (ClipContactFwd - ClipRoot(*Clip, T).X < DistToContact)
		{
			break;
		}
		StartTime = T;
	}
	StartTime = FMath::Clamp(FMath::Min(StartTime, AlignTime - (bHandWarp ? 0.25f : 0.3f)), 0.f, AlignTime);
	// Hand warp height: root = clip root + (real top - clip ledge) at plant, never below the floor.
	PlantRootZ = FMath::Max(0.f, (Cp.Z - ClipRoot(*Clip, StartTime).Z) + (Height - Clip->Ledge));
	const float ClipRise = ClipRoot(*Clip, Clip->ContactTime).Z - ClipRoot(*Clip, StartTime).Z;
	ZScale = ContactZ / FMath::Max(ClipRise, 1.f);
	const float Length = Clip->Seq->SequenceLength;
	if (bOver)
	{
		EndTime = Clip->LandTime > 0.f ? Clip->LandTime + 0.05f : Clip->ContactTime + 0.5f;
	}
	else
	{
		EndTime = Clip->Gait == 0 ? Length - BlendOut : FMath::Min(Clip->ContactTime + HandBackDelay, Length - BlendOut);
	}
	if (bToHang)
	{
		// Hand over when BOTH hands rest on the ledge (their IK windows), else at the plant; always before the pull-up (contact).
		float Grab = Clip->bHasPlant ? Clip->PlantTime : Clip->ContactTime * 0.6f;
		if (Clip->bHand[0] && Clip->bHand[1])
		{
			Grab = FMath::Max(Clip->HandStart[0], Clip->HandStart[1]);
		}
		// Run 2 (10-11): 'both hands rest' came right before the pull-up in some clips (stand_Lfoot: 1.0 s, contact 1.45) and the hang
		// pulled the body back down ~1.5 m. Now: from the first hand on the ledge to contact, the frame whose root is closest to
		// the hang depth (HangGrabDepth under the top) = the clip's own 'hanging' moment.
		if (HangGrabDepth > 0.f)
		{
			const float From = (Clip->bHand[0] && Clip->bHand[1]) ? FMath::Min(Clip->HandStart[0], Clip->HandStart[1])
				: (Clip->bHasPlant ? Clip->PlantTime - 0.2f : StartTime);
			const float WantZ = Clip->Top - HangGrabDepth;
			float BestErr = 1.e9f;
			for (float T = FMath::Max(From, StartTime); T < Clip->ContactTime - 0.05f; T += 1.f / 60.f)
			{
				const float Err = FMath::Abs(ClipRoot(*Clip, T).Z - WantZ);
				if (Err < BestErr)
				{
					BestErr = Err;
					Grab = T;
				}
			}
		}
		HangTime = FMath::Clamp(Grab + HangGrabDelay, StartTime + 0.1f, FMath::Max(Clip->ContactTime - 0.05f, StartTime + 0.1f));
		UE_LOG(LogTemp, Log, TEXT("Traversal (hang style): hand over at %.2f, clip root %.0f under its top (hang wants %.0f)"), HangTime,
			Clip->Top - ClipRoot(*Clip, HangTime).Z, HangGrabDepth);
	}

	// 6) Go: face the obstacle, capsule driven by us, motion matching paused.
	Montage = Anim->PlaySlotAnimationAsDynamicMontage(Clip->Seq, TEXT("DefaultSlot"), BlendIn, BlendOut, 1.f, 1, -1.f, StartTime);
	if (!Montage)
	{
		return false;
	}
	if (UMotionMatchingComponent* MM = Character->FindComponentByClass<UMotionMatchingComponent>())
	{
		MM->SetSuspended(true);
	}
	Anim->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	Move->StopMovementImmediately();
	Move->SetMovementMode(MOVE_None);
	Character->SetActorRotation(FRotator(0.f, Fwd.Rotation().Yaw, 0.f));
	Obstacle = Hit.GetComponent();
	Capsule->IgnoreComponentWhenMoving(Hit.GetComponent(), true);
	if (UGaspCameraComponent* Cam = Character->FindComponentByClass<UGaspCameraComponent>())
	{
		Cam->SetCollisionPaused(CVarTravCamFix.GetValueOnGameThread() != 0);
	}

	Active = Clip;
	StartLocation = Location;
	Forward = Fwd;
	Elapsed = 0.f;
	bActive = true;
	LastAction = Action;
	LastClipName = Clip->Seq->GetName();
	LastHeight = Height;
	UE_LOG(LogTemp, Log, TEXT("Traversal %s  H %.0f  depth %.0f  backFloor %s  speed %.0f  clip %s  start %.2f contact %.2f end %.2f  zScale %.2f  dist %.0f"),
		ActionName(Action), Height, bBack ? Depth : -1.f, bBackFloor ? *FString::Printf(TEXT("%.0f"), BackFloorZ - Feet.Z) : TEXT("none"),
		Speed, *LastClipName, StartTime, Clip->ContactTime, EndTime, ZScale, DistToContact);
	return true;
}

bool UTraversalComponent::GetHandIK(int32 Hand, FVector& OutWorldTarget, float& OutAlpha) const
{
	OutAlpha = 0.f;
	if (!bActive || !Active || !bHandIK || Hand < 0 || Hand > 1 || !Active->bHand[Hand])
	{
		return false;
	}
	const float T = ClipTimeNow;
	const float In = HandIKFadeIn > 0.f ? FMath::Clamp((T - (Active->HandStart[Hand] - HandIKFadeIn)) / HandIKFadeIn, 0.f, 1.f)
		: (T >= Active->HandStart[Hand] ? 1.f : 0.f);
	const float Out = HandIKFadeOut > 0.f ? FMath::Clamp(1.f - (T - Active->HandEnd[Hand]) / HandIKFadeOut, 0.f, 1.f)
		: (T <= Active->HandEnd[Hand] ? 1.f : 0.f);
	OutAlpha = FMath::SmoothStep(0.f, 1.f, FMath::Min(In, Out));
	OutWorldTarget = HandTarget[Hand];
	return true;
}

FVector UTraversalComponent::WarpedOffset(float Time) const
{
	const FClipInfo& Clip = *Active;
	const FVector C = ClipRoot(Clip, Time);
	const FVector C0 = ClipRoot(Clip, StartTime);
	const FVector Cc = ClipRoot(Clip, Clip.ContactTime);
	const bool bOver = Clip.Action == ETraversalAction::Hurdle || Clip.Action == ETraversalAction::Vault;
	float Fwd, Z;
	if (bHandWarp)
	{
		const FVector Cp = ClipRoot(Clip, Clip.PlantTime);
		if (Time <= Clip.PlantTime)
		{
			const float Alpha = FMath::Clamp((Time - StartTime) / FMath::Max(Clip.PlantTime - StartTime, 0.01f), 0.f, 1.f);
			Fwd = (C.X - C0.X) + (PlantRootFwd - (Cp.X - C0.X)) * Alpha;
			Z = FMath::Max(0.f, (C.Z - C0.Z) + (PlantRootZ - (Cp.Z - C0.Z)) * Alpha);
		}
		else
		{
			Fwd = PlantRootFwd + (C.X - Cp.X);
			if (Time <= Clip.ContactTime)
			{
				const float ClipRise = Cc.Z - Cp.Z;
				Z = PlantRootZ + (C.Z - Cp.Z) * (ClipRise > 5.f ? (ContactZ - PlantRootZ) / ClipRise : 1.f);
			}
			else
			{
				Z = bOver ? FMath::Max(0.f, C.Z * ContactZ / FMath::Max(Cc.Z, 1.f)) : ContactZ + (C.Z - Cc.Z);
			}
		}
		return Forward * Fwd + FVector(0.f, 0.f, Z);
	}
	if (Time <= Clip.ContactTime)
	{
		// Skew: the clip's own forward motion plus the missing (or extra) distance spread over the run-up.
		const float Alpha = FMath::Clamp((Time - StartTime) / FMath::Max(Clip.ContactTime - StartTime, 0.01f), 0.f, 1.f);
		Fwd = (C.X - C0.X) + (DistToContact - (Cc.X - C0.X)) * Alpha;
		Z = (C.Z - C0.Z) * ZScale;
	}
	else
	{
		Fwd = DistToContact + (C.X - Cc.X);
		Z = bOver ? FMath::Max(0.f, (C.Z - C0.Z) * ZScale) : ContactZ + (C.Z - Cc.Z);
	}
	return Forward * Fwd + FVector(0.f, 0.f, Z);
}

void UTraversalComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bActive || !Character)
	{
		return;
	}
	Elapsed += DeltaTime;
	UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
	const bool bPlaying = Anim && Montage && Anim->Montage_IsPlaying(Montage);
	if (!bPlaying && Elapsed > 0.05f)
	{
		// The montage auto blends out near its end (stand clips end at Length - BlendOut) = a normal end;
		// stopping clearly earlier = something else took the slot (hit reaction, ...).
		Finish(StartTime + Elapsed < EndTime - 0.1f);
		return;
	}
	const float Time = bPlaying ? Anim->Montage_GetPosition(Montage) : StartTime + Elapsed;
	// Measure where the planted hand really is (the pose on screen) at the clip's plant time.
	if (!bHandMeasured && Active->bHasPlant && Time >= Active->PlantTime)
	{
		bHandMeasured = true;
		const FVector Hand = Character->GetMesh()->GetSocketLocation(Active->PlantBone);
		LastHandDX = FVector::DotProduct(Hand - EdgePoint, Forward);
		LastHandDZ = Hand.Z - EdgePoint.Z;
		// Hand IK diagnostics: per hand alpha + distance of the real hand from its IK target (cm).
		for (int32 h = 0; h < 2; ++h)
		{
			FVector Target;
			float A = 0.f;
			const bool bOk = GetHandIK(h, Target, A);
			const FVector Real = Character->GetMesh()->GetSocketLocation(h == 0 ? FName(TEXT("hand_l")) : FName(TEXT("hand_r")));
			UE_LOG(LogTemp, Log, TEXT("Traversal handIK %s: window %d %.2f-%.2f  t %.2f  alpha %.2f  target-real dz %.0f dfwd %.0f dside %.0f  shoulder->target %.0f"),
				h == 0 ? TEXT("L") : TEXT("R"), Active->bHand[h], Active->HandStart[h], Active->HandEnd[h], Time, bOk ? A : -1.f,
				Target.Z - Real.Z, FVector::DotProduct(Target - Real, Forward), FVector::DotProduct(Target - Real, FVector::CrossProduct(FVector::UpVector, Forward)),
				FVector::Dist(Target, Character->GetMesh()->GetSocketLocation(h == 0 ? FName(TEXT("upperarm_l")) : FName(TEXT("upperarm_r")))));
		}
		UE_LOG(LogTemp, Log, TEXT("Traversal hand %s at %.2f: past edge %.0f cm (aim %.0f), above top %.0f cm (aim %.0f)  [%s%s]"),
			*Active->PlantBone.ToString(), Time, LastHandDX, HandInset, LastHandDZ, LastHandDZTarget, bHandWarp ? TEXT("hand warp") : TEXT("old warp"),
			bHandIK ? TEXT(" + IK") : TEXT(""));
	}
	// The mesh advances its montage AFTER this prerequisite tick. Match that upcoming pose,
	// rather than placing the body and computing plant alpha one animation frame behind.
	// Diagnostics above deliberately sample the completed pose before moving its component.
	ClipTimeNow = bPlaying ? FMath::Min(Time + DeltaTime * Anim->Montage_GetPlayRate(Montage) * Montage->RateScale, EndTime) : Time;
	if (bToHang)
	{
		// 10-11: never carry the body past the hang moment (a long frame did - 3 fps: the body was already pulling up, the hang
		// then sank it ~1.3 m back down).
		ClipTimeNow = FMath::Min(ClipTimeNow, HangTime);
	}
	Character->SetActorLocation(StartLocation + WarpedOffset(ClipTimeNow), false, nullptr, ETeleportType::None);
	if (bDebugDraw)
	{
		DrawDebugSphere(GetWorld(), StartLocation + WarpedOffset(Active->ContactTime), 8.f, 8, FColor::Yellow, false, -1.f);
	}
	if (bToHang && Time >= HangTime)
	{
		UE_LOG(LogTemp, Log, TEXT("Traversal -> hang: clip time %.2f (planned %.2f)"), Time, HangTime);
		HandToHang();
		return;
	}
	if (Time >= EndTime)
	{
		Finish(false);
	}
}

void UTraversalComponent::HandToHang()
{
	// 10-11: the climb clip got the hands onto the ledge - UHangComponent takes the body from here (it keeps the movement mode,
	// the paused motion matching and the 'ignore the ledge' on the capsule; its idle clip replaces ours on the slot).
	bActive = false;
	bToHang = false;
	UPrimitiveComponent* Ledge = Obstacle.Get();
	Obstacle.Reset();
	UE_LOG(LogTemp, Log, TEXT("Traversal -> HANG at %.2f s (clip %s, H %.0f)"), Elapsed, *LastClipName, LastHeight);
	Active = nullptr;
	Montage = nullptr;
	if (HangHandoff)
	{
		HangHandoff(EdgePoint, Forward, Ledge);
	}
}

void UTraversalComponent::Finish(bool bAborted)
{
	bActive = false;
	UCharacterMovementComponent* Move = Character->GetCharacterMovement();
	UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
	const float Time = Anim && Montage && Anim->Montage_IsPlaying(Montage) ? Anim->Montage_GetPosition(Montage) : EndTime;
	const FVector Velocity = (WarpedOffset(Time) - WarpedOffset(Time - 1.f / 30.f)) * 30.f;
	if (Obstacle.IsValid())
	{
		Character->GetCapsuleComponent()->IgnoreComponentWhenMoving(Obstacle.Get(), false);
	}
	if (UGaspCameraComponent* Cam = Character->FindComponentByClass<UGaspCameraComponent>())
	{
		Cam->SetCollisionPaused(false);
	}
	const bool bVault = Active && Active->Action == ETraversalAction::Vault;
	Move->SetMovementMode(bVault || bAborted ? MOVE_Falling : MOVE_Walking);
	Move->Velocity = bVault ? Velocity : FVector(Velocity.X, Velocity.Y, 0.f);
	if (UMotionMatchingComponent* MM = Character->FindComponentByClass<UMotionMatchingComponent>())
	{
		MM->SetSuspended(false); // its next search replaces our montage on the slot (blend)
	}
	UE_LOG(LogTemp, Log, TEXT("Traversal end%s  at %.2f s  speed %.0f"), bAborted ? TEXT(" (ABORTED)") : TEXT(""), Elapsed, Velocity.Size2D());
	Active = nullptr;
	Montage = nullptr;
}
