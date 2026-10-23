// See HangComponent.h.

#include "HangComponent.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GaspCameraComponent.h"
#include "HAL/IConsoleManager.h"
#include "MotionMatchingComponent.h"
#include "TraversalComponent.h"

static TAutoConsoleVariable<int32> CVarTravStyle(TEXT("trav.Style"), 0,
	TEXT("Ledges (dev panel F2 > GAME MODE). 0 = Climb: Space climbs straight up (old traversal). 1 = Hang: grab and hang (Splinter Cell),\n")
	TEXT("A/D shimmy, Space climb up / hop up, W hop up, C let go; Space at a roof edge lowers into a hang."));

static TAutoConsoleVariable<int32> CVarHangHandIK(TEXT("hang.HandIK"), 1,
	TEXT("1 = hanging body held WallGap off the wall + hand IK onto the ledge (10-11). 0 = the clip as is (head/chest go into the wall)."));

static TAutoConsoleVariable<int32> CVarHangLegFix(TEXT("hang.LegFix"), 2, // 10-11 v6 user: keep the clip's tucked knee (option 3)
	TEXT("Hanging legs vs the wall: 2 = TUCKED (knee turns out to its side, keeps the clip's tuck; toes tilt down onto the wall), 1 = STRAIGHT (swing each leg back at the hip, 10-11 v1), 0 = off (knees / feet go into the wall)."));

static TAutoConsoleVariable<int32> CVarHangPose(TEXT("hang.Pose"), 0, // 10-11 user: back to the ORIGINAL clip pose by default (high = option)
	TEXT("1 = HIGH hang (user video 10-11): body raised so the head is at the ledge, pressed to the wall, elbows bent out. 0 = low hang (arms straight, WallGap off the wall)."));

static TAutoConsoleVariable<int32> CVarHangLegBrace(TEXT("hang.LegBrace"), 1,
	TEXT("High hang: 1 = legs braced on the wall (knees out, toes on the wall, user video 10-11). 0 = the clip's legs + the leg fix."));

static TAutoConsoleVariable<int32> CVarHangHandGrip(TEXT("hang.HandGrip"), 0, // 10-11 user: original clip hands (flat on the top) by default
	TEXT("1 = hanging hands HOOK the ledge: palms on the wall face, fingers curled over the top (10-11 v2, user photo). 0 = flat on top (v1)."));

namespace HangPrivate
{
	const TCHAR* MoveName(EHangMove M)
	{
		switch (M)
		{
		case EHangMove::Idle:        return TEXT("Idle");
		case EHangMove::ShimmyLeft:  return TEXT("ShimmyLeft");
		case EHangMove::ShimmyRight: return TEXT("ShimmyRight");
		case EHangMove::HopUp:       return TEXT("HopUp");
		case EHangMove::ClimbUp:     return TEXT("ClimbUp");
		case EHangMove::LowerIn:     return TEXT("LowerIn");
		default:                     return TEXT("None");
		}
	}
	const FName Slot(TEXT("DefaultSlot"));
}

UHangComponent::UHangComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	auto Clip = [](const TCHAR* Name)
	{
		return TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(FString::Printf(TEXT("/Game/Animations/LedgeHang/%s.%s"), Name, Name)));
	};
	IdleClip = Clip(TEXT("anim_LedgeClimb_Idle_UEFN"));
	ShimmyLeftClip = Clip(TEXT("anim_LedgeClimbing_Left_UEFN"));
	ShimmyRightClip = Clip(TEXT("anim_LedgeClimbing_Right_UEFN"));
	HopUpClip = Clip(TEXT("anim_LedgeClimbing_Up_UEFN"));
	ClimbUpClip = Clip(TEXT("anim_LedgeClimb_ClimbUp_UEFN"));
}

bool UHangComponent::IsHangStyle()
{
	return CVarTravStyle.GetValueOnGameThread() == 1;
}

// ============================================================ Setup

void UHangComponent::BeginPlay()
{
	Super::BeginPlay();
	Character = Cast<ACharacter>(GetOwner());
	if (!Character)
	{
		return;
	}
	auto Load = [this](const TSoftObjectPtr<UAnimSequence>& Soft, FHangClip& Out)
	{
		Out.Seq = Soft.LoadSynchronous();
		Out.Length = Out.Seq ? Out.Seq->SequenceLength : 0.f;
		if (Out.Seq)
		{
			Loaded.Add(Out.Seq);
			const FVector D = ClipRoot(Out, Out.Length) - ClipRoot(Out, 0.f);
			UE_LOG(LogTemp, Log, TEXT("Hang clip %-34s %.2f s  root delta fwd %.0f right %.0f up %.0f  root yaw@0 %.1f"), *Out.Seq->GetName(), Out.Length,
				D.X, D.Y, D.Z, Out.Seq->ExtractRootTrackTransform(0.f, nullptr).Rotator().Yaw);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Hang: clip missing %s"), *Soft.ToString());
		}
	};
	Load(IdleClip, Idle);
	Load(ShimmyLeftClip, Left);
	Load(ShimmyRightClip, Right);
	Load(HopUpClip, HopUp);
	Load(ClimbUpClip, ClimbUp);
	if (Idle.Seq)
	{
		// On THIS character's mesh (run 1 10-11: the skeleton-only number put the hands 18 cm over the top - the skeleton's
		// translation retargeting (pelvis AnimationScaled, limbs Skeleton) uses the player mesh's own bone lengths).
		const FVector Raw = (ClipBone(Idle, TEXT("hand_l"), 0.f) + ClipBone(Idle, TEXT("hand_r"), 0.f)) * 0.5f - ClipRoot(Idle, 0.f);
		HandRel = (ClipBoneOnMesh(Idle, TEXT("hand_l"), 0.f) + ClipBoneOnMesh(Idle, TEXT("hand_r"), 0.f)) * 0.5f;
		UE_LOG(LogTemp, Log, TEXT("Hang: idle hands from the root on this mesh fwd %.1f right %.1f up %.1f  (skeleton only: %.1f %.1f %.1f)"),
			HandRel.X, HandRel.Y, HandRel.Z, Raw.X, Raw.Y, Raw.Z);
		HeadRel = ClipBoneOnMesh(Idle, TEXT("head"), 0.f);
		if (ClimbUp.Seq)
		{
			FString Track;
			for (float T = 0.f; T <= ClimbUp.Length + 0.01f; T += 0.1f)
			{
				const FVector R = ClipRoot(ClimbUp, T) - ClipRoot(ClimbUp, 0.f);
				const FVector HL = ClipRoot(ClimbUp, T) + ClipBoneOnMesh(ClimbUp, TEXT("hand_l"), T) - ClipRoot(ClimbUp, 0.f);
				Track += FString::Printf(TEXT(" %.1f:z%.0f/f%.0f/hz%.0f/hf%.0f"), T, R.Z, R.X, HL.Z, HL.X);
			}
			UE_LOG(LogTemp, Log, TEXT("Hang climb track (t: root z / fwd / hand_l z / fwd from start):%s"), *Track);
		}

		UE_LOG(LogTemp, Log, TEXT("Hang: idle head from the root fwd %.1f up %.1f -> low hang head %.1f cm vs the top, high-hang raise %.1f"),
			HeadRel.X, HeadRel.Z, HandAboveTop - HandRel.Z + HeadRel.Z, GetRaise());
	}
	// Capsule placed before the pose (same as UTraversalComponent).
	if (Character->GetMesh())
	{
		Character->GetMesh()->PrimaryComponentTick.AddPrerequisite(this, PrimaryComponentTick);
	}
	if (UTraversalComponent* Trav = Character->FindComponentByClass<UTraversalComponent>())
	{
		Traversal = Trav;
		TWeakObjectPtr<UHangComponent> WeakThis(this);
		Trav->HangHandoff = [WeakThis](const FVector& InEdge, const FVector& InFwd, UPrimitiveComponent* InLedge)
		{
			if (UHangComponent* Self = WeakThis.Get())
			{
				Self->BeginHang(InEdge, InFwd, InLedge);
			}
		};
	}
}

void UHangComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UTraversalComponent* Trav = Traversal.Get())
	{
		Trav->HangHandoff = nullptr;
		Trav->bHangOnClimb = false;
	}
	Super::EndPlay(EndPlayReason);
}

// ============================================================ Clip helpers (same conventions as UTraversalComponent)

FVector UHangComponent::ClipRoot(const FHangClip& Clip, float Time) const
{
	// Forward = +Y in animation space, +X = the character's left.
	const FVector V = Clip.Seq->ExtractRootTrackTransform(FMath::Clamp(Time, 0.f, Clip.Length), nullptr).GetTranslation();
	return FVector(V.Y, -V.X, V.Z);
}

FVector UHangComponent::ClipBone(const FHangClip& Clip, FName Bone, float Time) const
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
	const TArray<FTrackToSkeletonMap>& Map = Clip.Seq->GetCompressedTrackToSkeletonMapTable();
	TArray<int32, TInlineAllocator<16>> Chain;
	for (int32 B = Index; B != INDEX_NONE; B = Ref.GetParentIndex(B))
	{
		Chain.Add(B);
	}
	const float T = FMath::Clamp(Time, 0.f, Clip.Length);
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

FVector UHangComponent::ClipBoneOnMesh(const FHangClip& Clip, FName Bone, float Time) const
{
	// Like ClipBone, but the way the engine poses THIS character's mesh: per bone translation retargeting
	// (FAnimationRuntime::RetargetBoneTransform) against the mesh's own ref pose, root locked at the ref pose (root motion clips).
	const USkeleton* Skeleton = Clip.Seq->GetSkeleton();
	const USkeletalMesh* MeshAsset = Character->GetMesh() ? Character->GetMesh()->SkeletalMesh : nullptr;
	if (!Skeleton || !MeshAsset)
	{
		return ClipBone(Clip, Bone, Time) - ClipRoot(Clip, Time);
	}
	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	const FReferenceSkeleton& MeshRef = MeshAsset->GetRefSkeleton();
	const int32 Index = Ref.FindBoneIndex(Bone);
	if (Index == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}
	const TArray<FTrackToSkeletonMap>& Map = Clip.Seq->GetCompressedTrackToSkeletonMapTable();
	TArray<int32, TInlineAllocator<16>> Chain;
	for (int32 B = Index; B != INDEX_NONE; B = Ref.GetParentIndex(B))
	{
		Chain.Add(B);
	}
	const float T = FMath::Clamp(Time, 0.f, Clip.Length);
	FTransform Component = FTransform::Identity;
	for (int32 c = Chain.Num() - 1; c >= 0; --c)
	{
		const int32 SkelBone = Chain[c];
		const int32 MeshBone = MeshRef.FindBoneIndex(Ref.GetBoneName(SkelBone));
		const FTransform SkelRefLocal = Ref.GetRefBonePose()[SkelBone];
		const FTransform MeshRefLocal = MeshBone != INDEX_NONE ? MeshRef.GetRefBonePose()[MeshBone] : SkelRefLocal;
		FTransform Local = MeshRefLocal;
		bool bTrack = false;
		for (int32 Track = 0; Track < Map.Num(); ++Track)
		{
			if (Map[Track].BoneTreeIndex == SkelBone)
			{
				Clip.Seq->GetBoneTransform(Local, Track, T, false);
				bTrack = true;
				break;
			}
		}
		if (c == Chain.Num() - 1)
		{
			Local = MeshRefLocal; // root locked at the ref pose
		}
		else if (bTrack)
		{
			switch (Skeleton->GetBoneTranslationRetargetingMode(SkelBone))
			{
			case EBoneTranslationRetargetingMode::Skeleton:
				Local.SetTranslation(MeshRefLocal.GetTranslation());
				break;
			case EBoneTranslationRetargetingMode::AnimationScaled:
			{
				const float SkelLen = SkelRefLocal.GetTranslation().Size();
				Local.SetTranslation(Local.GetTranslation() * (SkelLen > KINDA_SMALL_NUMBER ? MeshRefLocal.GetTranslation().Size() / SkelLen : 1.f));
				break;
			}
			case EBoneTranslationRetargetingMode::AnimationRelative:
				Local.SetTranslation(Local.GetTranslation() + MeshRefLocal.GetTranslation() - SkelRefLocal.GetTranslation());
				break;
			default: // Animation (and OrientAndScale, not used on this skeleton): clip translation as is
				break;
			}
		}
		Component = Local * Component;
	}
	const FVector V = Component.GetTranslation();
	return FVector(V.Y, -V.X, V.Z);
}

FVector UHangComponent::ToWorld(const FVector& C) const
{
	const FVector RightDir = FVector::CrossProduct(FVector::UpVector, Fwd);
	return Fwd * C.X + RightDir * C.Y + FVector::UpVector * C.Z;
}

FVector UHangComponent::HangRootFor(const FVector& InEdge, const FVector& InFwd) const
{
	const FVector RightDir = FVector::CrossProduct(FVector::UpVector, InFwd);
	// The clip's own hang spot (low hang, touching the clip's wall). Body raise / gap from the wall are added on top (BodyOffset).
	return InEdge + InFwd * (HandInset - HandRel.X) - RightDir * HandRel.Y + FVector::UpVector * (HandAboveTop - HandRel.Z);
}

bool UHangComponent::IsHighPose() const
{
	return CVarHangPose.GetValueOnGameThread() != 0 && CVarHangHandIK.GetValueOnGameThread() != 0;
}

float UHangComponent::GetRaise() const
{
	// High hang (user video 10-11): the head comes up to HeadToTop under the top; hands stay on the ledge (IK bends the elbows).
	if (!IsHighPose())
	{
		return 0.f;
	}
	const float LowHeadZ = HandAboveTop - HandRel.Z + HeadRel.Z; // head above the top in the clip's low hang
	return FMath::Clamp(HeadToTop - LowHeadZ, 0.f, MaxRaise);
}

FVector UHangComponent::BodyOffsetFor(const FVector& InFwd) const
{
	if (CVarHangHandIK.GetValueOnGameThread() == 0)
	{
		return FVector::ZeroVector; // v0: the clip as is
	}
	// Low: fixed WallGap off the wall (10-11 v1). High: raised + AutoGap = pressed against the wall (measured every tick).
	// Climb up fades it out (ends standing on the real top), lower in fades it in.
	const float Fade = Move == EHangMove::ClimbUp ? 1.f - CurProgress : Move == EHangMove::LowerIn ? CurProgress : 1.f;
	const float Gap = IsHighPose() ? AutoGap : WallGap;
	return (FVector::UpVector * GetRaise() - InFwd * Gap) * Fade;
}

void UHangComponent::UpdateIK(float DeltaTime, float Progress)
{
	CurProgress = Progress;
	IKAlpha = FMath::Min(IKAlpha + DeltaTime / FMath::Max(BlendTime, 0.01f), 1.f);
	BodyOffsetNow = BodyOffsetFor(Fwd);
	if (IsHighPose() && IKAlpha > 0.5f && (Move == EHangMove::Idle || Move == EHangMove::ShimmyLeft || Move == EHangMove::ShimmyRight || Move == EHangMove::HopUp))
	{
		// Press the body to the wall: the torso point nearest the wall (minus a flesh radius) is steered to TouchGap in front of it.
		const TCHAR* Torso[5] = { TEXT("head"), TEXT("neck_01"), TEXT("spine_05"), TEXT("spine_03"), TEXT("pelvis") };
		const float Radius[5] = { 11.f, 7.f, 14.f, 13.f, 12.f };
		float MinClear = 1.e6f;
		for (int32 k = 0; k < 5; ++k)
		{
			const FVector P = Character->GetMesh()->GetSocketLocation(Torso[k]);
			if (P.Z < Edge.Z + Radius[k])
			{
				MinClear = FMath::Min(MinClear, FVector::DotProduct(P - Edge, -Fwd) - Radius[k]);
			}
		}
		if (MinClear < 1.e5f)
		{
			LastTorsoClear = MinClear;
			AutoGap = FMath::Clamp(FMath::FInterpTo(AutoGap, AutoGap + (TouchGap - MinClear), DeltaTime, AutoGapSpeed), -20.f, 40.f);
		}
	}
	const FName HandBones[2] = { TEXT("hand_l"), TEXT("hand_r") };
	const float BaseZ = (Move == EHangMove::HopUp && Progress > 0.5f) ? NextEdge.Z : Edge.Z;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HangHandTop), false, Character);
	for (int32 h = 0; h < 2; ++h)
	{
		// Planted = the clip's hand rests at the ledge height it started on (or, hopping, the one it ends on) and barely moves.
		float Want = 1.f;
		if (Move != EHangMove::Idle && Cur && Cur->Seq)
		{
			auto P = [&](float T) { return ClipRoot(*Cur, T) + ClipBoneOnMesh(*Cur, HandBones[h], T); };
			const FVector Now = P(MoveTime);
			const float Speed = (P(FMath::Min(MoveTime + 1.f / 30.f, Cur->Length)) - Now).Size() * 30.f;
			const bool bAtStart = FMath::Abs(Now.Z - P(0.f).Z) < 5.f;
			const bool bAtEnd = Move == EHangMove::HopUp && FMath::Abs(Now.Z - P(Cur->Length).Z) < 5.f;
			Want = (Speed < 50.f && (bAtStart || bAtEnd)) ? 1.f : 0.f;
		}
		PlantW[h] = FMath::FInterpConstantTo(PlantW[h], Want, DeltaTime, 1.f / 0.1f);
		// Top surface under this hand (sloped / stepped ledges).
		HandTopZ[h] = BaseZ;
		const FVector Hand = Character->GetMesh()->GetSocketLocation(HandBones[h]);
		const FVector OnTop = Hand - Fwd * FVector::DotProduct(Hand - Edge, Fwd) + Fwd * 8.f;
		FHitResult Top;
		if (GetWorld()->LineTraceSingleByChannel(Top, FVector(OnTop.X, OnTop.Y, BaseZ + 40.f), FVector(OnTop.X, OnTop.Y, BaseZ - 40.f),
				ECC_Visibility, Params) && !Top.bStartPenetrating && Top.ImpactNormal.Z > 0.7f)
		{
			HandTopZ[h] = Top.ImpactPoint.Z;
		}
		// 10-11 v3 (user: hands exactly on the edge also when moving and climbing):
		auto Pc = [&](const FHangClip& C, float T) { return ClipRoot(C, T) + ClipBoneOnMesh(C, HandBones[h], T); };
		float WantEdge = 1.f, WantHook = 1.f, WantLift = 0.f, WantPush = 0.f;
		FVector Delta = FVector::ZeroVector;
		if ((Move == EHangMove::ShimmyLeft || Move == EHangMove::ShimmyRight) && Cur && Cur->Seq)
		{
			// the moving hand slides along the edge in its grip, lifting only a little (the clip lifts it ~25 cm into the air)
			WantLift = FMath::Clamp(Pc(*Cur, MoveTime).Z - Pc(*Cur, 0.f).Z, 0.f, ShimmyHandLift);
		}
		else if ((Move == EHangMove::ClimbUp || Move == EHangMove::LowerIn) && ClimbUp.Seq)
		{
			// Run 1 (copying the clip's hands relative to its ledge): they left the edge at 0.6 s and waved in the air while the body
			// climbed. Now a mantle: hook grip -> palms pushing flat on the top as the body rises past it -> let go near the top.
			const float Rise0 = ClipRoot(ClimbUp, 0.f).Z;
			const float Rise = FMath::Clamp((ClipRoot(ClimbUp, MoveTime).Z - Rise0) / FMath::Max(ClipRoot(ClimbUp, ClimbUp.Length).Z - Rise0, 1.f), 0.f, 1.f);
			WantPush = FMath::SmoothStep(ClimbPushStart, FMath::Max(ClimbPushEnd, ClimbPushStart + 0.01f), Rise);
			// release in clip time: from the moment the rise passes ClimbReleaseRise, over HandReleaseTime
			float ReleaseT = ClimbUp.Length;
			for (float T = 0.f; T <= ClimbUp.Length; T += 1.f / 30.f)
			{
				if ((ClipRoot(ClimbUp, T).Z - Rise0) >= ClimbReleaseRise * (ClipRoot(ClimbUp, ClimbUp.Length).Z - Rise0))
				{
					ReleaseT = T;
					break;
				}
			}
			WantEdge = 1.f - FMath::SmoothStep(ReleaseT, ReleaseT + HandReleaseTime, MoveTime);
			WantHook = WantEdge;
		}
		else if (Move == EHangMove::HopUp)
		{
			WantEdge = WantHook = PlantW[h]; // hop: the clip's hands travel to the ledge above, snapping on when they land
		}
		EdgeW[h] = WantEdge;
		HookWNow[h] = WantHook;
		LiftNow[h] = FMath::FInterpTo(LiftNow[h], WantLift, DeltaTime, 20.f);
		HandDeltaNow[h] = Delta;
		PushNow[h] = WantPush;
	}
}

bool UHangComponent::GetIKFrame(FHangIKFrame& Out) const
{
	if (!IsHanging() || !Character)
	{
		return false;
	}
	const bool bHands = CVarHangHandIK.GetValueOnGameThread() != 0;
	Out.Edge = Edge;
	Out.Fwd = Fwd;
	Out.HandShift = bHands ? -BodyOffsetNow : FVector::ZeroVector; // a lifted hand undoes the body's raise / gap
	Out.bHigh = IsHighPose();
	Out.bBrace = Out.bHigh && CVarHangLegBrace.GetValueOnGameThread() != 0;
	Out.PushInset = PushInset;
	Out.PushAbove = PushAbove;
	for (int32 h = 0; h < 2; ++h)
	{
		Out.HandDelta[h] = HandDeltaNow[h];
		Out.HandEdgeW[h] = EdgeW[h];
		Out.HookW[h] = HookWNow[h];
		Out.Lift[h] = LiftNow[h];
		Out.Push[h] = PushNow[h];
	}
	Out.BraceDrop = BraceDrop;
	Out.BraceSpread = BraceSpread;
	Out.BraceFootOut = BraceFootOut;
	Out.BraceToeAngle = BraceToeAngle;
	Out.HandAlpha = bHands ? IKAlpha : 0.f;
	Out.LegAlpha = CVarHangLegFix.GetValueOnGameThread() != 0 ? IKAlpha : 0.f;
	Out.HandInset = HandInset;
	Out.HandAboveTop = HandAboveTop;
	Out.HandSpread = HandSpread;
	Out.LegMargin = LegMargin;
	Out.MaxLegAngle = MaxLegAngle;
	Out.LegMode = CVarHangLegFix.GetValueOnGameThread() >= 2 ? 2 : 1;
	Out.MaxKneeOut = MaxKneeOut;
	Out.MaxToeTilt = MaxToeTilt;
	Out.bHook = CVarHangHandGrip.GetValueOnGameThread() != 0;
	Out.KnuckleAbove = KnuckleAbove;
	Out.KnuckleOut = KnuckleOut;
	Out.FingerLean = FingerLean;
	Out.FingerBend[0] = FingerBend1;
	Out.FingerBend[1] = FingerBend2;
	Out.FingerBend[2] = FingerBend3;
	for (int32 h = 0; h < 2; ++h)
	{
		Out.Plant[h] = PlantW[h];
		Out.TopZ[h] = HandTopZ[h];
	}
	return true;
}

FVector UHangComponent::ActorFromRoot(const FVector& Root) const
{
	// The mesh component origin is the root bone (root locked); actor = that minus the mesh offset (incl. crouch offset).
	const FVector MeshRel = Character->GetMesh()->GetRelativeLocation();
	return Root - Character->GetActorQuat().RotateVector(MeshRel);
}

bool UHangComponent::ProbeLedge(const FVector& Around, float MinZ, float MaxZ, FVector& OutEdge, FVector& OutFwd, UPrimitiveComponent*& OutComp) const
{
	UWorld* World = GetWorld();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HangProbe), false, Character);
	// 1) a steep face just under the top (front of the ledge)
	const float FaceZ = MinZ - 10.f;
	const FVector A(Around.X, Around.Y, FaceZ);
	FHitResult Face;
	if (!World->LineTraceSingleByChannel(Face, A - Fwd * 70.f, A + Fwd * 70.f, ECC_Visibility, Params)
		|| Cast<APawn>(Face.GetActor()) || FMath::Abs(Face.ImpactNormal.Z) > 0.5f)
	{
		return false;
	}
	const FVector NewFwd = (-Face.ImpactNormal).GetSafeNormal2D();
	if (FVector::DotProduct(NewFwd, Fwd) < 0.85f) // > ~30 deg turn: a corner (not handled yet)
	{
		return false;
	}
	// 2) a flat top over it, between MinZ and MaxZ. Starting inside a wall that goes on up = no hit = no ledge here.
	FHitResult Top;
	const FVector P = Face.ImpactPoint + NewFwd * 8.f;
	if (!World->LineTraceSingleByChannel(Top, FVector(P.X, P.Y, MaxZ + 30.f), FVector(P.X, P.Y, MinZ - 5.f), ECC_Visibility, Params)
		|| Top.bStartPenetrating || Top.ImpactNormal.Z < 0.7f || Top.ImpactPoint.Z < MinZ || Top.ImpactPoint.Z > MaxZ)
	{
		return false;
	}
	OutEdge = FVector(Face.ImpactPoint.X, Face.ImpactPoint.Y, Top.ImpactPoint.Z);
	OutFwd = NewFwd;
	OutComp = Top.GetComponent();
	if (bDebugDraw)
	{
		DrawDebugSphere(World, OutEdge, 6.f, 8, FColor::Green, false, 2.f);
	}
	return true;
}

FVector2D UHangComponent::GetMoveInput() const
{
	if (!TestInput.IsNearlyZero())
	{
		return TestInput;
	}
	const FVector In = Character->GetLastMovementInputVector();
	const FVector RightDir = FVector::CrossProduct(FVector::UpVector, Fwd);
	return FVector2D(FVector::DotProduct(In, RightDir), FVector::DotProduct(In, Fwd));
}

// ============================================================ Moves

bool UHangComponent::PlayClip(const FHangClip& Clip, float StartAt, float Blend, int32 Loops)
{
	UAnimInstance* Anim = Character->GetMesh() ? Character->GetMesh()->GetAnimInstance() : nullptr;
	if (!Anim || !Clip.Seq)
	{
		return false;
	}
	Montage = Anim->PlaySlotAnimationAsDynamicMontage(Clip.Seq, HangPrivate::Slot, Blend, Blend, 1.f, Loops, -1.f, StartAt);
	if (Montage)
	{
		// We end every move ourselves (next clip / motion matching takes the slot). An auto blend-out near the clip end - or near
		// its START while the lower-in plays it backwards at rate 0 - would read as 'replaced' and drop the player.
		Montage->bEnableAutoBlendOut = false;
	}
	Cur = &Clip;
	MoveTime = StartAt;
	MoveElapsed = 0.f;
	return Montage != nullptr;
}

void UHangComponent::SetLedgeComp(UPrimitiveComponent* Comp)
{
	UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	if (LedgeComp.IsValid() && LedgeComp.Get() != Comp)
	{
		Capsule->IgnoreComponentWhenMoving(LedgeComp.Get(), false);
	}
	LedgeComp = Comp;
	if (Comp)
	{
		Capsule->IgnoreComponentWhenMoving(Comp, true); // the hanging body is a few cm inside the wall's capsule space
	}
}

void UHangComponent::BeginHang(const FVector& InEdge, const FVector& InFwd, UPrimitiveComponent* Ledge)
{
	if (!Character || !Idle.Seq)
	{
		return;
	}
	UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
	if (UMotionMatchingComponent* MM = Character->FindComponentByClass<UMotionMatchingComponent>())
	{
		MM->SetSuspended(true);
	}
	if (UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance())
	{
		Anim->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	}
	MoveComp->StopMovementImmediately();
	MoveComp->SetMovementMode(MOVE_None);
	if (UGaspCameraComponent* Cam = Character->FindComponentByClass<UGaspCameraComponent>())
	{
		Cam->SetCollisionPaused(false);
	}
	Edge = InEdge;
	Fwd = InFwd.GetSafeNormal2D();
	SetLedgeComp(Ledge);
	BaseRoot = HangRootFor(Edge, Fwd);
	// The capsule eases from where the grab left it onto the hang spot during the blend (a few cm).
	BlendFrom = Character->GetMesh()->GetComponentLocation();
	BlendInLeft = BlendTime;
	TurnFromYaw = Character->GetActorRotation().Yaw;
	IKAlpha = 0.f;
	PlantW[0] = PlantW[1] = 1.f;
	EnterIdle(BlendTime);
	const FVector Ease = BaseRoot + BodyOffsetFor(Fwd) - BlendFrom; // where the body really settles (raise / gap included)
	UE_LOG(LogTemp, Log, TEXT("Hang BEGIN edge %s  fwd %s  ledge %s  capsule eases %.0f cm (fwd %.0f up %.0f)  from %s  to %s  root bone %s  actor %s"),
		*Edge.ToCompactString(), *Fwd.ToCompactString(), *GetNameSafe(Ledge), Ease.Size(), FVector::DotProduct(Ease, Fwd), Ease.Z,
		*BlendFrom.ToCompactString(), *BaseRoot.ToCompactString(), *Character->GetMesh()->GetSocketLocation(TEXT("root")).ToCompactString(),
		*Character->GetActorLocation().ToCompactString());
}

void UHangComponent::EnterIdle(float Blend)
{
	Move = EHangMove::Idle;
	MoveScale = FVector::OneVector;
	MoveSkew = FVector::ZeroVector;
	bMeasured = false;
	PlayClip(Idle, 0.f, Blend, 1000);
}

void UHangComponent::StartMove(EHangMove NewMove, const FHangClip& Clip, const FVector& EndRoot, const FVector& NewEdge, const FVector& NewFwd,
	UPrimitiveComponent* NewComp)
{
	// Clip root delta over the whole move vs where it must really end: scale the big axes, skew the rest.
	const FVector D = ClipRoot(Clip, Clip.Length) - ClipRoot(Clip, 0.f);
	const FVector W = EndRoot - BaseRoot;
	const FVector RightDir = FVector::CrossProduct(FVector::UpVector, Fwd);
	const FVector Want(FVector::DotProduct(W, Fwd), FVector::DotProduct(W, RightDir), W.Z);
	MoveScale = FVector::OneVector;
	if (FMath::Abs(D.Z) > 10.f && Want.Z / D.Z > 0.3f && Want.Z / D.Z < 3.f)
	{
		MoveScale.Z = Want.Z / D.Z;
	}
	if (FMath::Abs(D.X) > 10.f && Want.X / D.X > 0.5f && Want.X / D.X < 2.f)
	{
		MoveScale.X = Want.X / D.X;
	}
	MoveSkew = ToWorld(Want - D * MoveScale);
	NextEdge = NewEdge;
	NextFwd = NewFwd;
	NextComp = NewComp;
	TurnFromYaw = Character->GetActorRotation().Yaw;
	Move = NewMove;
	PlayClip(Clip, 0.f, BlendTime * 0.5f, 1);
	if (NewComp)
	{
		Character->GetCapsuleComponent()->IgnoreComponentWhenMoving(NewComp, true);
	}
	UE_LOG(LogTemp, Log, TEXT("Hang %s  clip delta fwd %.0f right %.0f up %.0f  real fwd %.0f right %.0f up %.0f  scale %.2f/%.2f  skew %.0f cm"),
		HangPrivate::MoveName(NewMove), D.X, D.Y, D.Z, Want.X, Want.Y, Want.Z, MoveScale.X, MoveScale.Z, MoveSkew.Size());
}

bool UHangComponent::TryShimmy(float Side)
{
	const FHangClip& Clip = Side < 0.f ? Left : Right;
	if (!Clip.Seq)
	{
		return false;
	}
	const float Step = FMath::Abs(ClipRoot(Clip, Clip.Length).Y - ClipRoot(Clip, 0.f).Y);
	const FVector RightDir = FVector::CrossProduct(FVector::UpVector, Fwd) * FMath::Sign(Side);
	FVector NewEdge, NewFwd, Far, FarFwd;
	UPrimitiveComponent* NewComp = nullptr;
	UPrimitiveComponent* FarComp = nullptr;
	if (!ProbeLedge(Edge + RightDir * Step, Edge.Z - ShimmyMaxStep, Edge.Z + ShimmyMaxStep, NewEdge, NewFwd, NewComp)
		|| !ProbeLedge(Edge + RightDir * (Step + ShimmyAhead), Edge.Z - ShimmyMaxStep, Edge.Z + ShimmyMaxStep, Far, FarFwd, FarComp))
	{
		return false;
	}
	// Body room along the way (radius shrunk: the hanging body already touches the wall).
	UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HangShimmy), false, Character);
	Params.AddIgnoredComponent(LedgeComp.Get());
	Params.AddIgnoredComponent(NewComp);
	const FVector EndRoot = HangRootFor(NewEdge, NewFwd);
	const FVector From = Character->GetActorLocation();
	const FVector To = ActorFromRoot(EndRoot + BodyOffsetFor(NewFwd));
	FHitResult Block;
	if (GetWorld()->SweepSingleByChannel(Block, From, To, FQuat::Identity, Capsule->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(FMath::Max(Capsule->GetScaledCapsuleRadius() - 14.f, 10.f), Capsule->GetScaledCapsuleHalfHeight() - 5.f),
			Params, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
	{
		if (bDebugDraw)
		{
			DrawDebugSphere(GetWorld(), Block.ImpactPoint, 8.f, 8, FColor::Red, false, 1.f);
		}
		return false;
	}
	StartMove(Side < 0.f ? EHangMove::ShimmyLeft : EHangMove::ShimmyRight, Clip, EndRoot, NewEdge, NewFwd, NewComp);
	return true;
}

bool UHangComponent::TryHopUp()
{
	if (!HopUp.Seq)
	{
		return false;
	}
	FVector NewEdge, NewFwd;
	UPrimitiveComponent* NewComp = nullptr;
	if (!ProbeLedge(Edge, Edge.Z + HopMinRise, Edge.Z + HopMaxRise, NewEdge, NewFwd, NewComp))
	{
		return false;
	}
	const FVector EndRoot = HangRootFor(NewEdge, NewFwd);
	UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HangHop), false, Character);
	Params.AddIgnoredComponent(LedgeComp.Get());
	Params.AddIgnoredComponent(NewComp);
	if (GetWorld()->OverlapBlockingTestByChannel(ActorFromRoot(EndRoot + BodyOffsetFor(NewFwd)), FQuat::Identity, Capsule->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(FMath::Max(Capsule->GetScaledCapsuleRadius() - 14.f, 10.f), Capsule->GetScaledCapsuleHalfHeight() - 5.f),
			Params, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
	{
		return false;
	}
	StartMove(EHangMove::HopUp, HopUp, EndRoot, NewEdge, NewFwd, NewComp);
	return true;
}

bool UHangComponent::TryClimbUp()
{
	if (!ClimbUp.Seq)
	{
		return false;
	}
	UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	const float Radius = Capsule->GetScaledCapsuleRadius();
	const float Half = Capsule->GetScaledCapsuleHalfHeight();
	const FVector StandFeet = Edge + Fwd * (Radius + StandInset);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HangClimbRoom), false, Character);
	if (GetWorld()->OverlapBlockingTestByChannel(StandFeet + FVector(0.f, 0.f, Half + 3.f), FQuat::Identity, Capsule->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(Radius, Half), Params, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
	{
		if (bDebugDraw)
		{
			DrawDebugCapsule(GetWorld(), StandFeet + FVector(0.f, 0.f, Half + 3.f), Half, Radius, FQuat::Identity, FColor::Red, false, 2.f);
		}
		UE_LOG(LogTemp, Log, TEXT("Hang: no room on top to climb up"));
		return false;
	}
	StartMove(EHangMove::ClimbUp, ClimbUp, StandFeet, Edge, Fwd, LedgeComp.Get());
	return true;
}

bool UHangComponent::HandleJump()
{
	if (!IsHanging())
	{
		return false;
	}
	if (Move == EHangMove::Idle)
	{
		if (!TryClimbUp())
		{
			TryHopUp();
		}
	}
	else if (Move == EHangMove::ShimmyLeft || Move == EHangMove::ShimmyRight || Move == EHangMove::HopUp)
	{
		bJumpQueued = true; // run 4 (10-11): Space mid-shimmy was lost - now it climbs as soon as the step ends
	}
	return true; // never jump off a ledge with Space
}

bool UHangComponent::TryLowerIn()
{
	if (!Character || !IsHangStyle() || IsHanging() || !ClimbUp.Seq || !Idle.Seq)
	{
		return false;
	}
	UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
	UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	if (!MoveComp->IsMovingOnGround() || Character->bIsCrouched)
	{
		return false;
	}
	UWorld* World = GetWorld();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(HangLower), false, Character);
	const FVector Feet = Character->GetActorLocation() - FVector(0.f, 0.f, Capsule->GetScaledCapsuleHalfHeight());
	FVector Dir = MoveComp->GetCurrentAcceleration().GetSafeNormal2D();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector().GetSafeNormal2D();
	}
	// 1) where the floor ends ahead
	float EdgeAt = -1.f;
	for (float D = 20.f; D <= LowerEdgeReach; D += 10.f)
	{
		const FVector P = Feet + Dir * D;
		FHitResult Floor;
		if (!World->LineTraceSingleByChannel(Floor, P + FVector(0.f, 0.f, 20.f), P - FVector(0.f, 0.f, 40.f), ECC_Visibility, Params))
		{
			EdgeAt = D;
			break;
		}
	}
	if (EdgeAt < 0.f)
	{
		return false;
	}
	// 2) the outer face of that edge (trace back toward us just under the top)
	FHitResult Face;
	const FVector FaceA = Feet + Dir * (EdgeAt + 40.f) - FVector(0.f, 0.f, 15.f);
	if (!World->LineTraceSingleByChannel(Face, FaceA, FaceA - Dir * 80.f, ECC_Visibility, Params) || Cast<APawn>(Face.GetActor())
		|| FMath::Abs(Face.ImpactNormal.Z) > 0.5f || FVector::DotProduct(Face.ImpactNormal.GetSafeNormal2D(), Dir) < 0.7f)
	{
		return false;
	}
	const FVector WallFwd = (-Face.ImpactNormal).GetSafeNormal2D(); // hanging faces the wall = back toward where we stand
	FHitResult Top;
	const FVector TopP = Face.ImpactPoint + WallFwd * 10.f;
	if (!World->LineTraceSingleByChannel(Top, TopP + FVector(0.f, 0.f, 40.f), TopP - FVector(0.f, 0.f, 40.f), ECC_Visibility, Params)
		|| Top.ImpactNormal.Z < 0.7f)
	{
		return false;
	}
	const FVector NewEdge(Face.ImpactPoint.X, Face.ImpactPoint.Y, Top.ImpactPoint.Z);
	// 3) a real drop below the hang (not a step)
	FHitResult Below;
	const FVector Out = NewEdge - WallFwd * 40.f;
	if (World->LineTraceSingleByChannel(Below, Out, Out - FVector(0.f, 0.f, 3000.f), ECC_Visibility, Params)
		&& NewEdge.Z - Below.ImpactPoint.Z < LowerMinDrop)
	{
		return false;
	}
	// 4) room for the hanging body
	const FVector OldFwd = Fwd;
	Fwd = WallFwd;
	const FVector HangRoot = HangRootFor(NewEdge, WallFwd);
	FCollisionQueryParams RoomParams(SCENE_QUERY_STAT(HangLowerRoom), false, Character);
	RoomParams.AddIgnoredComponent(Face.GetComponent());
	const FVector HangActor = HangRoot + BodyOffsetFor(WallFwd) - FRotator(0.f, WallFwd.Rotation().Yaw, 0.f).Quaternion().RotateVector(Character->GetMesh()->GetRelativeLocation());
	if (World->OverlapBlockingTestByChannel(HangActor, FQuat::Identity, Capsule->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(FMath::Max(Capsule->GetScaledCapsuleRadius() - 14.f, 10.f), Capsule->GetScaledCapsuleHalfHeight() - 5.f),
			RoomParams, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels())))
	{
		Fwd = OldFwd;
		return false;
	}

	// Go: the climb-up backwards. Clip t = 0 is the hang (= BaseRoot); scales as for a climb up onto this edge.
	Edge = NewEdge;
	BaseRoot = HangRoot;
	const FVector D = ClipRoot(ClimbUp, ClimbUp.Length) - ClipRoot(ClimbUp, 0.f);
	const float StandFwd = (HandRel.X - HandInset) + Capsule->GetScaledCapsuleRadius() + StandInset;
	MoveScale = FVector(FMath::Abs(D.X) > 10.f ? FMath::Clamp(StandFwd / D.X, 0.5f, 2.f) : 1.f, 1.f,
		FMath::Abs(D.Z) > 10.f ? FMath::Clamp((Edge.Z - BaseRoot.Z) / D.Z, 0.3f, 3.f) : 1.f);
	// Where the clip would start vs where we stand: that difference fades out over the move (MoveSkew x (1 - progress)).
	MoveSkew = Character->GetMesh()->GetComponentLocation() - (BaseRoot + ToWorld(D * MoveScale));
	NextEdge = Edge;
	NextFwd = Fwd;
	NextComp = Face.GetComponent();
	SetLedgeComp(Face.GetComponent());
	if (UMotionMatchingComponent* MM = Character->FindComponentByClass<UMotionMatchingComponent>())
	{
		MM->SetSuspended(true);
	}
	UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
	Anim->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	MoveComp->StopMovementImmediately();
	MoveComp->SetMovementMode(MOVE_None);
	TurnFromYaw = Character->GetActorRotation().Yaw;
	Move = EHangMove::LowerIn;
	bMeasured = false;
	IKAlpha = 0.f;
	PlantW[0] = PlantW[1] = 0.f;
	BlendInLeft = 0.f;
	PlayClip(ClimbUp, ClimbUp.Length - 0.01f, BlendTime * 1.5f, 1);
	if (Montage)
	{
		Anim->Montage_SetPlayRate(Montage, 0.f); // we set the position every tick (backwards)
	}
	UE_LOG(LogTemp, Log, TEXT("Hang LowerIn  edge %s  drop %s  start skew %.0f cm  scale %.2f/%.2f"), *Edge.ToCompactString(),
		Below.bBlockingHit ? *FString::Printf(TEXT("%.0f"), NewEdge.Z - Below.ImpactPoint.Z) : TEXT("none"), MoveSkew.Size(), MoveScale.X, MoveScale.Z);
	return true;
}

void UHangComponent::Drop()
{
	if (!IsHanging())
	{
		return;
	}
	UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
	if (Anim && Montage)
	{
		Anim->Montage_Stop(BlendTime, Montage);
	}
	// Out of the wall's capsule space before falling (without WallGap the hanging capsule sits a few cm inside it).
	const float FaceToCapsule = FVector::DotProduct(Edge - Character->GetActorLocation(), Fwd);
	const float Inside = Character->GetCapsuleComponent()->GetScaledCapsuleRadius() + 2.f - FaceToCapsule;
	if (Inside > 0.f)
	{
		Character->SetActorLocation(Character->GetActorLocation() - Fwd * Inside, false, nullptr, ETeleportType::TeleportPhysics);
	}
	EndHang(false);
}

void UHangComponent::EndHang(bool bStandOnTop)
{
	bJumpQueued = false;
	IKAlpha = 0.f;
	PlantW[0] = PlantW[1] = 0.f;
	UE_LOG(LogTemp, Log, TEXT("Hang END (%s) from %s"), bStandOnTop ? TEXT("standing on top") : TEXT("let go"), HangPrivate::MoveName(Move));
	Move = EHangMove::None;
	Cur = nullptr;
	Montage = nullptr;
	SetLedgeComp(nullptr);
	if (NextComp.IsValid())
	{
		Character->GetCapsuleComponent()->IgnoreComponentWhenMoving(NextComp.Get(), false);
	}
	NextComp.Reset();
	UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement();
	MoveComp->SetMovementMode(bStandOnTop ? MOVE_Walking : MOVE_Falling);
	MoveComp->Velocity = FVector::ZeroVector;
	if (UMotionMatchingComponent* MM = Character->FindComponentByClass<UMotionMatchingComponent>())
	{
		MM->SetSuspended(false); // its next search replaces our clip on the slot (blend)
	}
}

// ============================================================ Tick

void UHangComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (UTraversalComponent* Trav = Traversal.Get())
	{
		Trav->bHangOnClimb = IsHangStyle();
		Trav->HangGrabDelay = GrabDelay;
		Trav->HangGrabDepth = HandRel.Z - HandAboveTop - GetRaise(); // the clip moment whose body is as high as our (raised) hang
	}
	if (!IsHanging() || !Character)
	{
		return;
	}
	UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance();
	MoveElapsed += DeltaTime;
	// Something else took the slot (hit reaction, death...): let go. Run 3 (10-11): our own one-shot clip still auto-faded out
	// 0.1 s before its end at full frame rate (bEnableAutoBlendOut set after PlaySlot... came too late) and Montage_IsPlaying read
	// that as 'replaced' -> dropped mid-shimmy. Now: another clip active = replaced; ours fading near the move's end = move done.
	const UAnimMontage* ActiveMontage = Anim ? Anim->GetCurrentActiveMontage() : nullptr;
	bool bForceMoveEnd = false;
	if (!Anim || !Montage || (ActiveMontage != Montage && MoveElapsed > 0.1f))
	{
		const bool bOursFadingAtEnd = Anim && Montage && !ActiveMontage && Cur && Move != EHangMove::Idle && Move != EHangMove::LowerIn
			&& MoveTime >= Cur->Length - 0.3f;
		if (!bOursFadingAtEnd)
		{
			UE_LOG(LogTemp, Log, TEXT("Hang: our clip was replaced (%s) - letting go"), *GetNameSafe(ActiveMontage));
			Drop();
			return;
		}
		bForceMoveEnd = true;
	}

	// Clip time of the pose about to be drawn (like UTraversalComponent: one frame ahead of the finished pose).
	float Progress = 0.f;
	if (Move == EHangMove::LowerIn)
	{
		MoveTime = FMath::Max(MoveTime - DeltaTime * LowerPlayRate, 0.f);
		Anim->Montage_SetPosition(Montage, MoveTime);
		Progress = 1.f - MoveTime / FMath::Max(Cur->Length, 0.01f);
	}
	else if (Move != EHangMove::Idle)
	{
		MoveTime = bForceMoveEnd ? Cur->Length
			: FMath::Min(FMath::Max(MoveTime, Anim->Montage_GetPosition(Montage) + DeltaTime * Anim->Montage_GetPlayRate(Montage)), Cur->Length);
		Progress = MoveTime / FMath::Max(Cur->Length, 0.01f);
	}

	// Facing: turn smoothly to the wall (lower-in turns round in its first 0.35 s, shimmy follows a bending wall).
	const float TargetYaw = Fwd.Rotation().Yaw;
	float Yaw = TargetYaw;
	if (Move == EHangMove::LowerIn)
	{
		Yaw = FMath::Lerp(TurnFromYaw, TurnFromYaw + FMath::FindDeltaAngleDegrees(TurnFromYaw, TargetYaw),
			FMath::SmoothStep(0.f, 1.f, FMath::Clamp(MoveElapsed / 0.35f, 0.f, 1.f)));
	}
	else if (Move == EHangMove::ShimmyLeft || Move == EHangMove::ShimmyRight || Move == EHangMove::HopUp)
	{
		Yaw = FMath::Lerp(TurnFromYaw, TurnFromYaw + FMath::FindDeltaAngleDegrees(TurnFromYaw, NextFwd.Rotation().Yaw), Progress);
	}
	else if (BlendInLeft > 0.f)
	{
		Yaw = FMath::Lerp(TurnFromYaw, TurnFromYaw + FMath::FindDeltaAngleDegrees(TurnFromYaw, TargetYaw), 1.f - BlendInLeft / FMath::Max(BlendTime, 0.01f));
	}
	Character->SetActorRotation(FRotator(0.f, Yaw, 0.f));

	// Root path of the current move. Idle: re-read the hang spot every tick so F2 HandInset / HandAboveTop move the body live (10-11).
	if (Move == EHangMove::Idle)
	{
		BaseRoot = HangRootFor(Edge, Fwd);
	}
	FVector Root = BaseRoot;
	if (Move != EHangMove::Idle)
	{
		const FVector Delta = (ClipRoot(*Cur, MoveTime) - ClipRoot(*Cur, 0.f)) * MoveScale;
		Root = BaseRoot + ToWorld(Delta) + MoveSkew * (Move == EHangMove::LowerIn ? 1.f - Progress : Progress);
	}
	CurProgress = Progress;
	Root += BodyOffsetFor(Fwd); // 10-11: low = WallGap off the wall, high = raised + pressed to it (fades over climb up / lower in)
	if (BlendInLeft > 0.f)
	{
		BlendInLeft = FMath::Max(BlendInLeft - DeltaTime, 0.f);
		Root = FMath::Lerp(BlendFrom, Root, FMath::SmoothStep(0.f, 1.f, 1.f - BlendInLeft / FMath::Max(BlendTime, 0.01f)));
	}
	Character->SetActorLocation(ActorFromRoot(Root), false, nullptr, ETeleportType::None);
	UpdateIK(DeltaTime, Progress);
	if (bDebugDraw)
	{
		DrawDebugSphere(GetWorld(), Edge, 5.f, 8, FColor::Yellow, false, -1.f);
	}

	switch (Move)
	{
	case EHangMove::Idle:
	{
		if (!bMeasured && MoveElapsed > BlendTime + 0.4f && BlendInLeft <= 0.f)
		{
			bMeasured = true;
			const FVector RightDir = FVector::CrossProduct(FVector::UpVector, Fwd);
			for (const TCHAR* Bone : { TEXT("hand_l"), TEXT("hand_r") })
			{
				const FVector H = Character->GetMesh()->GetSocketLocation(Bone);
				UE_LOG(LogTemp, Log, TEXT("Hang hands %s: past edge %.1f  above top %.1f  side %.1f cm  (aim %.0f / %.0f)"), Bone,
					FVector::DotProduct(H - Edge, Fwd), H.Z - Edge.Z, FVector::DotProduct(H - Edge, RightDir), HandInset, HandAboveTop);
			}
			UE_LOG(LogTemp, Log, TEXT("Hang body (%s): raise %.1f  gap %.1f  head above top %.1f  torso nearest the wall %.1f cm (aim %.1f)"),
				IsHighPose() ? TEXT("HIGH") : TEXT("low"), GetRaise(), IsHighPose() ? AutoGap : WallGap,
				Character->GetMesh()->GetSocketLocation(TEXT("head")).Z - Edge.Z, LastTorsoClear, TouchGap);
			// 10-11 v2 grip: knuckles (middle_01) at the corner, fingertip (middle_03) on the top
			for (const TCHAR* Side : { TEXT("l"), TEXT("r") })
			{
				const FVector K = Character->GetMesh()->GetSocketLocation(*FString::Printf(TEXT("middle_01_%s"), Side));
				const FVector T3 = Character->GetMesh()->GetSocketLocation(*FString::Printf(TEXT("middle_03_%s"), Side));
				const FVector W = Character->GetMesh()->GetSocketLocation(*FString::Printf(TEXT("hand_%s"), Side));
				UE_LOG(LogTemp, Log, TEXT("Hang grip %s: knuckle past face %.1f above top %.1f (aim %.0f / %.0f) | finger tip past %.1f above top %.1f | wrist past %.1f above %.1f"),
					Side, FVector::DotProduct(K - Edge, Fwd), K.Z - Edge.Z, -KnuckleOut, KnuckleAbove,
					FVector::DotProduct(T3 - Edge, Fwd), T3.Z - Edge.Z, FVector::DotProduct(W - Edge, Fwd), W.Z - Edge.Z);
			}
		}
		if (BlendInLeft > 0.f)
		{
			break;
		}
		const FVector2D In = GetMoveInput();
		if (FMath::Abs(In.X) > InputThreshold && FMath::Abs(In.X) >= FMath::Abs(In.Y))
		{
			TryShimmy(In.X);
		}
		else if (In.Y > InputThreshold)
		{
			TryHopUp();
		}
		break;
	}
	case EHangMove::LowerIn:
		if (MoveTime <= 0.02f)
		{
			BaseRoot = HangRootFor(Edge, Fwd);
			EnterIdle(BlendTime);
		}
		break;
	case EHangMove::ClimbUp:
		if (MoveTime >= Cur->Length - BlendTime)
		{
			EndHang(true);
		}
		break;
	default: // shimmy / hop: arrive at the next hang spot, then idle or the next step
		if (MoveTime >= Cur->Length - 0.02f)
		{
			Edge = NextEdge;
			Fwd = NextFwd;
			SetLedgeComp(NextComp.Get());
			BaseRoot = HangRootFor(Edge, Fwd);
			const EHangMove Was = Move;
			const FVector2D In = GetMoveInput();
			const bool bChain = (Was == EHangMove::ShimmyLeft && In.X < -InputThreshold) || (Was == EHangMove::ShimmyRight && In.X > InputThreshold);
			if (bJumpQueued)
			{
				bJumpQueued = false;
				Move = EHangMove::Idle; // HandleJump's idle path: climb up, else hop up, else stay hanging
				if (TryClimbUp() || TryHopUp())
				{
					break;
				}
				EnterIdle(BlendTime);
			}
			else if (!(bChain && TryShimmy(In.X)))
			{
				EnterIdle(BlendTime);
			}
		}
		break;
	}
}
