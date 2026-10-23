// Feature layouts (all in the animation's root space at the frame, cm and cm/s). Each block = one normalization
// group (like UE "NormalizeWithCommonSchema") with GASP's weight (sample weight x channel weight).
//
// LOCOMOTION = GASP PSS_Default (30 floats)          JUMP = GASP PSS_Jump (39 floats, trajectory channel weight 10)
//  idx    block                       weight         idx    block                                  weight
//  0-1    pos xy @-0.05               0.3            0-2    pos @-0.2                              0.25 x10
//  2-3    vel xy @0                   1              3-5    vel direction @0 (clamped 1)           1 x10
//  4-5    facing xy @0                1              6-8    facing @0                              1 x10
//  6-7    pos xy @0.35                1              9-11   vel @0.2                               1 x10
//  8-9    facing xy @0.35             1              12-14  pos @0.2                               1 x10
//  10-11  pos xy @0.7                 1              15-17  vel direction @0.2                     1 x10
//  12-13  facing xy @0.7              1              18-20  vel @0.5                               1 x10
//  14-15  vel xy @0.7                 1              21-23  pos @0.5                               1 x10
//  16-18  vel direction @1.0 (clamp1) 1.5            24-26  vel direction @0.5                     1 x10
//  19-21  foot_l pos rel. foot_r      1              27-29  foot_l pos rel. foot_r                 1
//  22-24  foot_l vel (char space)     0.3            30-32  foot_l vel rel. foot_r                 2
//  25-27  foot_r vel (char space)     0.3            33-35  foot_l vel                             0.3
//  28-29  pelvis heading (Y axis) XY  0.1            36     foot_r vel Z only                      0.3
//                                                    37-38  pelvis heading (Y axis) XY             0.3

#include "MotionMatchingDatabase.h"

#include "Animation/AnimationPoseData.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimSequence.h"
#include "Animation/CustomAttributesRuntime.h"
#include "Animation/Skeleton.h"
#include "BonePose.h"

DEFINE_LOG_CATEGORY_STATIC(LogMotionMatching, Log, All);

namespace MotionMatchingDb
{
	struct FBlock
	{
		int32 Start, Size;
		float Weight;
		const TCHAR* Name;
	};

	const FBlock LocomotionBlocks[] = {
		{ 0, 2, 0.3f, TEXT("pos-0.05") }, { 2, 2, 1.f, TEXT("vel0") }, { 4, 2, 1.f, TEXT("face0") }, { 6, 2, 1.f, TEXT("pos.35") },
		{ 8, 2, 1.f, TEXT("face.35") }, { 10, 2, 1.f, TEXT("pos.7") }, { 12, 2, 1.f, TEXT("face.7") }, { 14, 2, 1.f, TEXT("vel.7") },
		{ 16, 3, 1.5f, TEXT("velDir1") }, { 19, 3, 1.f, TEXT("footLR") }, { 22, 3, 0.3f, TEXT("velL") }, { 25, 3, 0.3f, TEXT("velR") },
		{ 28, 2, 0.1f, TEXT("pelvis") } };

	const FBlock JumpBlocks[] = {
		{ 0, 3, 2.5f, TEXT("pos-0.2") }, { 3, 3, 10.f, TEXT("velDir0") }, { 6, 3, 10.f, TEXT("face0") }, { 9, 3, 10.f, TEXT("vel.2") },
		{ 12, 3, 10.f, TEXT("pos.2") }, { 15, 3, 10.f, TEXT("velDir.2") }, { 18, 3, 10.f, TEXT("vel.5") }, { 21, 3, 10.f, TEXT("pos.5") },
		{ 24, 3, 10.f, TEXT("velDir.5") }, { 27, 3, 1.f, TEXT("footLR") }, { 30, 3, 2.f, TEXT("velLR") }, { 33, 3, 0.3f, TEXT("velL") },
		{ 36, 1, 0.3f, TEXT("velRz") }, { 37, 2, 0.3f, TEXT("pelvis") } };

	TArrayView<const FBlock> BlocksFor(EMotionMatchingSchema Schema)
	{
		return Schema == EMotionMatchingSchema::Jump ? TArrayView<const FBlock>(JumpBlocks) : TArrayView<const FBlock>(LocomotionBlocks);
	}

	int32 NumFeaturesFor(EMotionMatchingSchema Schema)
	{
		return Schema == EMotionMatchingSchema::Jump ? 39 : 30;
	}

	// Root-relative transforms of the feet and pelvis at one time.
	struct FBoneSample
	{
		FVector LeftFoot, RightFoot;
		FTransform Pelvis;
	};

	bool SampleBones(const UAnimSequence* Seq, float Time, const FName Bones[3], FBoneSample& Out)
	{
		USkeleton* Skeleton = Seq ? Seq->GetSkeleton() : nullptr;
		if (!Skeleton)
		{
			return false;
		}
		const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();

		TArray<FBoneIndexType> Required;
		Required.Reserve(RefSkel.GetNum());
		for (int32 i = 0; i < RefSkel.GetNum(); ++i)
		{
			Required.Add(static_cast<FBoneIndexType>(i));
		}
		FBoneContainer Container(Required, FCurveEvaluationOption(false), *Skeleton);

		FCompactPose Pose;
		Pose.SetBoneContainer(&Container);
		FBlendedCurve Curve;
		Curve.InitFrom(Container);
		FStackCustomAttributes Attributes;
		FAnimationPoseData PoseData(Pose, Curve, Attributes);
		Seq->GetBonePose(PoseData, FAnimExtractContext(Time, false));

		FCSPose<FCompactPose> CSPose;
		CSPose.InitPose(Pose);
		const FTransform RootCS = CSPose.GetComponentSpaceTransform(FCompactPoseBoneIndex(0));

		FTransform Rel[3];
		for (int32 b = 0; b < 3; ++b)
		{
			const int32 SkelIndex = RefSkel.FindBoneIndex(Bones[b]);
			if (SkelIndex == INDEX_NONE)
			{
				return false;
			}
			Rel[b] = CSPose.GetComponentSpaceTransform(Container.GetCompactPoseIndexFromSkeletonIndex(SkelIndex)).GetRelativeTransform(RootCS);
		}
		Out.LeftFoot = Rel[0].GetLocation();
		Out.RightFoot = Rel[1].GetLocation();
		Out.Pelvis = Rel[2];
		return true;
	}

	// Root position/rotation at (Time + Offset) relative to the root at Time. Offset may be negative.
	// Past the end of a one-shot clip the clip's last root velocity is continued (like UE5 PoseSearch):
	// a stop stays standing, a start keeps going. That lets every frame up to the end be stored.
	FTransform RootDelta(const UAnimSequence* Seq, float Time, float Offset, bool bLoop)
	{
		if (Offset >= 0.f)
		{
			const float Length = Seq->SequenceLength;
			if (bLoop || Time + Offset <= Length)
			{
				return Seq->ExtractRootMotion(Time, Offset, bLoop);
			}
			const float Inside = FMath::Max(Length - Time, 0.f);
			FTransform ToEnd = Seq->ExtractRootMotion(Time, Inside, false);
			const float Tail = FMath::Min(1.f / 30.f, Length);
			const FVector EndVelocity = Seq->ExtractRootMotion(Length - Tail, Tail, false).GetLocation() / Tail; // root space at the end
			const FVector Extra = ToEnd.GetRotation().RotateVector(EndVelocity * (Time + Offset - Length));
			ToEnd.AddToTranslation(Extra);
			return ToEnd;
		}
		// Going backwards: the motion from (Time+Offset) to Time, inverted.
		const float Start = Time + Offset;
		if (Start >= 0.f || bLoop)
		{
			const float From = Start >= 0.f ? Start : Start + Seq->SequenceLength;
			return Seq->ExtractRootMotion(From, -Offset, bLoop).Inverse();
		}
		// Clip start: extrapolate backwards with the motion just after Time.
		const FTransform Ahead = Seq->ExtractRootMotion(Time, -Offset, false);
		return FTransform(Ahead.GetRotation().Inverse(), -Ahead.GetLocation());
	}
}

bool UMotionMatchingDatabase::ComputeFeatures(int32 SequenceIndex, float Time, float* Out) const
{
	using namespace MotionMatchingDb;
	if (!Sequences.IsValidIndex(SequenceIndex) || !Sequences[SequenceIndex].Sequence)
	{
		return false;
	}
	const UAnimSequence* Seq = Sequences[SequenceIndex].Sequence;
	const bool bLoop = Sequences[SequenceIndex].bLoop;
	const float Dt = 1.f / 30.f;

	auto Set2 = [Out](int32 i, const FVector& V) { Out[i] = V.X; Out[i + 1] = V.Y; };
	auto Set3 = [Out](int32 i, const FVector& V) { Out[i] = V.X; Out[i + 1] = V.Y; Out[i + 2] = V.Z; };
	auto Facing2D = [this](const FTransform& T) { return T.GetRotation().RotateVector(AnimForward).GetSafeNormal2D(); };
	// Root velocity (root space at Time) around Time+Offset - backward difference, like a trajectory velocity.
	auto Velocity = [&](float Offset)
	{
		const FTransform A = RootDelta(Seq, Time, Offset - Dt, bLoop);
		const FTransform B = RootDelta(Seq, Time, Offset, bLoop);
		return (B.GetLocation() - A.GetLocation()) / Dt;
	};

	// Pose: now and one sample earlier (for velocities).
	const FName Bones[3] = { LeftFootBone, RightFootBone, PelvisBone };
	const float Prev = Time - Dt >= 0.f ? Time - Dt : (bLoop ? Time - Dt + Seq->SequenceLength : 0.f);
	FBoneSample Now, Before;
	if (!SampleBones(Seq, Time, Bones, Now) || !SampleBones(Seq, Prev, Bones, Before))
	{
		return false;
	}
	const float InvDt = (Time - Prev) > KINDA_SMALL_NUMBER || bLoop ? 1.f / Dt : 0.f;
	const FVector VelL = (Now.LeftFoot - Before.LeftFoot) * InvDt;
	const FVector VelR = (Now.RightFoot - Before.RightFoot) * InvDt;
	const FVector PelvisHeading = Now.Pelvis.GetUnitAxis(EAxis::Y).GetSafeNormal2D();

	if (Schema == EMotionMatchingSchema::Jump)
	{
		Set3(0, RootDelta(Seq, Time, JumpPastTime, bLoop).GetLocation());
		Set3(3, Velocity(0.f).GetClampedToMaxSize(1.f));
		Set3(6, AnimForward.GetSafeNormal());
		for (int32 k = 0; k < 2; ++k)
		{
			const float T = JumpFutureTimes[k];
			const FVector V = Velocity(T);
			Set3(9 + k * 9, V);
			Set3(12 + k * 9, RootDelta(Seq, Time, T, bLoop).GetLocation());
			Set3(15 + k * 9, V.GetClampedToMaxSize(1.f));
		}
		Set3(27, Now.LeftFoot - Now.RightFoot);
		Set3(30, VelL - VelR);
		Set3(33, VelL);
		Out[36] = VelR.Z;
		Set2(37, PelvisHeading);
		return true;
	}

	Set2(0, RootDelta(Seq, Time, PastTime, bLoop).GetLocation());
	Set2(2, Velocity(0.f));
	Set2(4, AnimForward.GetSafeNormal2D());
	const FTransform F035 = RootDelta(Seq, Time, FutureTimes[0], bLoop);
	Set2(6, F035.GetLocation());
	Set2(8, Facing2D(F035));
	const FTransform F070 = RootDelta(Seq, Time, FutureTimes[1], bLoop);
	Set2(10, F070.GetLocation());
	Set2(12, Facing2D(F070));
	Set2(14, Velocity(FutureTimes[1]));
	// UE5 VelocityDirection = velocity clamped to length 1 (cm/s): moving = unit direction, standing = zero.
	Set3(16, Velocity(FutureTimes[2]).GetClampedToMaxSize(1.f));
	Set3(19, Now.LeftFoot - Now.RightFoot);
	Set3(22, VelL);
	Set3(25, VelR);
	Set2(28, PelvisHeading);
	return true;
}

void UMotionMatchingDatabase::Build()
{
	using namespace MotionMatchingDb;
	const int32 NF = NumFeaturesFor(Schema);
	Frames.Reset();
	Features.Reset();
	SequenceFirstFrame.Init(INDEX_NONE, Sequences.Num());
	SequenceLastTime.Init(-1.f, Sequences.Num());

	TArray<float> Raw;
	float Buffer[MaxFeatures];
	for (int32 s = 0; s < Sequences.Num(); ++s)
	{
		const UAnimSequence* Seq = Sequences[s].Sequence;
		if (!Seq)
		{
			continue;
		}
		// Loops: every frame. Non-looping clips: the last ExcludeEndSeconds are not stored (GASP ExcludeFromDatabase).
		// Futures past a clip's end continue its last motion (RootDelta), so stops stay standing and starts keep going.
		const bool bLoop = Sequences[s].bLoop;
		const float Length = Seq->SequenceLength;
		float LastTime = bLoop ? Length : FMath::Max(Length - ExcludeEndSeconds, 0.f);
		if (Sequences[s].SampleEnd > 0.f)
		{
			LastTime = FMath::Min(LastTime, Sequences[s].SampleEnd); // GASP Sampling Range
		}
		for (float t = FMath::Min(Sequences[s].SampleStart, LastTime); bLoop ? t < LastTime : t <= LastTime + KINDA_SMALL_NUMBER; t += 1.f / SampleRate)
		{
			if (!ComputeFeatures(s, t, Buffer))
			{
				UE_LOG(LogMotionMatching, Warning, TEXT("%s: bones %s/%s/%s not found - skipped."),
					*Seq->GetName(), *LeftFootBone.ToString(), *RightFootBone.ToString(), *PelvisBone.ToString());
				break;
			}
			if (SequenceFirstFrame[s] == INDEX_NONE)
			{
				SequenceFirstFrame[s] = Frames.Num();
			}
			SequenceLastTime[s] = t;
			FMotionMatchingFrame& F = Frames.AddDefaulted_GetRef();
			F.SequenceIndex = s;
			F.Time = t;
			Raw.Append(Buffer, NF);
		}
	}

	// Normalize: mean per feature, ONE deviation per block, weight per block.
	const int32 N = Frames.Num();
	Mean.Init(0.f, NF);
	Deviation.Init(1.f, NF);
	Weights.Init(0.f, NF);
	if (N > 0)
	{
		for (int32 i = 0; i < N; ++i)
			for (int32 f = 0; f < NF; ++f)
				Mean[f] += Raw[i * NF + f] / N;
		for (const FBlock& B : BlocksFor(Schema))
		{
			double Var = 0.0;
			for (int32 i = 0; i < N; ++i)
				for (int32 f = B.Start; f < B.Start + B.Size; ++f)
					Var += FMath::Square(Raw[i * NF + f] - Mean[f]);
			Var /= double(N) * B.Size;
			const float Dev = FMath::Max(static_cast<float>(FMath::Sqrt(Var)), 0.001f);
			for (int32 f = B.Start; f < B.Start + B.Size; ++f)
			{
				Deviation[f] = Dev;
				Weights[f] = B.Weight;
			}
		}
	}
	NumFeaturesBuilt = NF;
	Features = Raw;
	for (int32 i = 0; i < N; ++i)
	{
		Normalize(&Features[i * NF]);
	}

	MarkPackageDirty();
	UE_LOG(LogMotionMatching, Log, TEXT("%s built (%s schema): %d frames from %d sequences."), *GetName(),
		Schema == EMotionMatchingSchema::Jump ? TEXT("Jump") : TEXT("Locomotion"), N, Sequences.Num());
}

void UMotionMatchingDatabase::Normalize(float* InOut) const
{
	if (NumFeaturesBuilt == 0 || Mean.Num() != NumFeaturesBuilt)
	{
		return;
	}
	for (int32 f = 0; f < NumFeaturesBuilt; ++f)
	{
		InOut[f] = (InOut[f] - Mean[f]) / Deviation[f];
	}
}

int32 UMotionMatchingDatabase::FindFrame(int32 SequenceIndex, float Time) const
{
	if (!SequenceFirstFrame.IsValidIndex(SequenceIndex) || SequenceFirstFrame[SequenceIndex] == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	const int32 First = SequenceFirstFrame[SequenceIndex];
	int32 Index = First + FMath::RoundToInt((Time - Frames[First].Time) * SampleRate);
	Index = FMath::Clamp(Index, First, Frames.Num() - 1);
	// Clamp back if we ran into the next sequence (past the stored part of a non-looping clip).
	while (Index > First && Frames[Index].SequenceIndex != SequenceIndex)
	{
		--Index;
	}
	return Index;
}

bool UMotionMatchingDatabase::IsTimeInDatabase(int32 SequenceIndex, float Time) const
{
	if (!Sequences.IsValidIndex(SequenceIndex) || !SequenceLastTime.IsValidIndex(SequenceIndex) || SequenceLastTime[SequenceIndex] < 0.f)
	{
		return false;
	}
	return Sequences[SequenceIndex].bLoop || (Time <= SequenceLastTime[SequenceIndex] + 0.5f / SampleRate
		&& Time >= Frames[SequenceFirstFrame[SequenceIndex]].Time - 0.5f / SampleRate);
}

float UMotionMatchingDatabase::FrameBias(int32 FrameIndex) const
{
	const int32 Seq = Frames[FrameIndex].SequenceIndex;
	if (!Sequences.IsValidIndex(Seq))
	{
		return 0.f;
	}
	const FMotionMatchingSequence& E = Sequences[Seq];
	return E.BaseCostBias + (E.bLoop ? E.LoopingCostBias : 0.f);
}

float UMotionMatchingDatabase::Cost(int32 FrameIndex, const float* Query) const
{
	const int32 NF = NumFeaturesBuilt;
	if (!Frames.IsValidIndex(FrameIndex) || NF == 0 || Weights.Num() != NF)
	{
		return MAX_flt;
	}
	const float* F = &Features[FrameIndex * NF];
	float Sum = FrameBias(FrameIndex);
	for (int32 f = 0; f < NF; ++f)
	{
		Sum += Weights[f] * FMath::Square(Query[f] - F[f]);
	}
	return Sum;
}

void UMotionMatchingDatabase::LogCandidates(const float* Query, const FString& MustContain, const FString& Label) const
{
	using namespace MotionMatchingDb;
	const int32 NF = NumFeaturesBuilt;
	if (NF == 0)
	{
		return;
	}

	auto Describe = [&](int32 i)
	{
		const float* F = &Features[i * NF];
		FString Parts;
		for (const FBlock& B : BlocksFor(Schema))
		{
			float C = 0.f;
			for (int32 f = B.Start; f < B.Start + B.Size; ++f)
			{
				C += Weights[f] * FMath::Square(Query[f] - F[f]);
			}
			Parts += FString::Printf(TEXT(" %s=%.2f"), B.Name, C);
		}
		const FMotionMatchingFrame& Fr = Frames[i];
		return FString::Printf(TEXT("%s(%.2f) cost=%.2f bias=%.3f |%s"), *GetNameSafe(Sequences[Fr.SequenceIndex].Sequence), Fr.Time,
			Cost(i, Query), FrameBias(i), *Parts);
	};

	TArray<TPair<float, int32>> All;
	int32 BestMatch = INDEX_NONE;
	float BestMatchCost = MAX_flt;
	for (int32 i = 0; i < Frames.Num(); ++i)
	{
		const float C = Cost(i, Query);
		All.Emplace(C, i);
		if (C < BestMatchCost && GetNameSafe(Sequences[Frames[i].SequenceIndex].Sequence).Contains(MustContain))
		{
			BestMatchCost = C;
			BestMatch = i;
		}
	}
	All.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
	UE_LOG(LogMotionMatching, Log, TEXT("MMDebug %s ---- %s db %s"), *Label,
		Schema == EMotionMatchingSchema::Jump ? TEXT("Jump") : TEXT("Locomotion"), *GetName());
	for (int32 k = 0; k < FMath::Min(5, All.Num()); ++k)
	{
		UE_LOG(LogMotionMatching, Log, TEXT("MMDebug  top%d %s"), k + 1, *Describe(All[k].Value));
	}
	if (BestMatch != INDEX_NONE)
	{
		UE_LOG(LogMotionMatching, Log, TEXT("MMDebug  best '%s' %s"), *MustContain, *Describe(BestMatch));
	}
}

FString UMotionMatchingDatabase::DescribeFrame(int32 FrameIndex, const float* Query) const
{
	using namespace MotionMatchingDb;
	const int32 NF = NumFeaturesBuilt;
	if (NF == 0 || !Frames.IsValidIndex(FrameIndex) || Mean.Num() != NF)
	{
		return TEXT("none");
	}
	const float* F = &Features[FrameIndex * NF];
	auto Raw = [this](const float* V, const FBlock& B)
	{
		FString S;
		for (int32 f = B.Start; f < B.Start + B.Size; ++f)
		{
			const float X = V[f] * Deviation[f] + Mean[f];
			S += f == B.Start ? TEXT("") : TEXT(",");
			S += FMath::Abs(X) < 5.f ? FString::Printf(TEXT("%.2f"), X) : FString::Printf(TEXT("%.0f"), X);
		}
		return S;
	};
	FString Parts;
	for (const FBlock& B : BlocksFor(Schema))
	{
		float C = 0.f;
		for (int32 f = B.Start; f < B.Start + B.Size; ++f)
		{
			C += Weights[f] * FMath::Square(Query[f] - F[f]);
		}
		Parts += FString::Printf(TEXT(" %s=%.1f[q %s f %s]"), B.Name, C, *Raw(Query, B), *Raw(F, B));
	}
	const FMotionMatchingFrame& Fr = Frames[FrameIndex];
	return FString::Printf(TEXT("%s(%.2f) cost=%.2f |%s"), *GetNameSafe(Sequences[Fr.SequenceIndex].Sequence), Fr.Time, Cost(FrameIndex, Query), *Parts);
}

int32 UMotionMatchingDatabase::Search(const float* Query, float& OutCost, TFunctionRef<bool(int32, float)> IsExcluded) const
{
	const int32 NF = NumFeaturesBuilt;
	int32 Best = INDEX_NONE;
	float BestCost = MAX_flt;
	const int32 N = Frames.Num();
	if (NF == 0 || Weights.Num() != NF)
	{
		OutCost = BestCost;
		return INDEX_NONE;
	}
	for (int32 i = 0; i < N; ++i)
	{
		const float Bias = FrameBias(i);
		if (Bias >= BestCost || IsExcluded(Frames[i].SequenceIndex, Frames[i].Time))
		{
			continue;
		}
		const float* F = &Features[i * NF];
		float Cost = Bias;
		for (int32 f = 0; f < NF && Cost < BestCost; ++f)
		{
			Cost += Weights[f] * FMath::Square(Query[f] - F[f]);
		}
		if (Cost < BestCost)
		{
			BestCost = Cost;
			Best = i;
		}
	}
	OutCost = BestCost;
	return Best;
}
