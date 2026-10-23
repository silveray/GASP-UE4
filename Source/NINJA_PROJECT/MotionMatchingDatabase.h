// Motion matching database: every frame of the listed animations, described by one of GASP's schemas
// (read from the UE 5.8 GASP project 10-01):
//  - Locomotion = PSS_Default: trajectory at -0.05/0/0.35/0.7/1.0 s (XY), left foot rel. right foot, feet velocities, pelvis heading.
//  - Jump       = PSS_Jump: 3D trajectory at -0.2/0/0.2/0.5 s with x10 channel weight, foot / pelvis channels.
// Fill Sequences, press "Build" in the asset's Details panel, save the asset.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MotionMatchingDatabase.generated.h"

class UAnimSequence;

UENUM(BlueprintType)
enum class EMotionMatchingSchema : uint8
{
	/** GASP PSS_Default - walking, running, sprinting, starts, stops, idles. */
	Locomotion,
	/** GASP PSS_Jump - jump starts, falling, landings (3D trajectory). */
	Jump
};

USTRUCT(BlueprintType)
struct FMotionMatchingSequence
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	UAnimSequence* Sequence = nullptr;

	/** Loops (idle, walk/run cycles). Non-looping clips (starts, stops, turns) can't be picked near their end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	bool bLoop = false;

	/**
	 * GASP Sampling Range: only clip time SampleStart..SampleEnd is stored (s). SampleEnd 0 = to the end.
	 * Land clips: air part = 0..touchdown (air database), landing part = touchdown..end (land databases).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching", meta = (ClampMin = "0"))
	float SampleStart = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching", meta = (ClampMin = "0"))
	float SampleEnd = 0.f;

	// GASP per-database biases (PoseSearchDatabase). Negative = preferred.
	/** Added to every frame of this clip. GASP: idles +0.1, turn-in-place -0.2, others 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Bias")
	float BaseCostBias = 0.f;

	/** Added when this clip is the one already playing. GASP: loops/walk starts -0.01, run starts/pivots -0.05, stops -0.3, jumps -0.5. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Bias")
	float ContinuingPoseCostBias = -0.01f;

	/** Added to frames of looping clips. GASP: idles -0.1, others -0.005. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Bias")
	float LoopingCostBias = -0.005f;

	/** UE5 FPoseSearchDatabaseAnimationAsset::bDisableReselection (10-09): while this clip plays, no other frame of it can be picked
	 *  (UPoseSearchDatabase::PopulateNonSelectableIdx) - the playing frame still competes as the continuing pose. GASP sets it on almost every clip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	bool bDisableReselection = false;

	/** GASP sub-database this clip came from (10-09), e.g. "Starts", "Loops", "Pivots", "Stops", "Idles", "TurnInPlace".
	 *  Databases whose chooser works by group (crouch) only allow the groups GASP's chooser table allows in the current state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	FName Group;
};

/** Where one database frame comes from. */
USTRUCT()
struct FMotionMatchingFrame
{
	GENERATED_BODY()

	UPROPERTY()
	int32 SequenceIndex = 0;

	UPROPERTY()
	float Time = 0.f;
};

UCLASS(BlueprintType)
class NINJA_PROJECT_API UMotionMatchingDatabase : public UDataAsset
{
	GENERATED_BODY()

public:

	/** Most floats any schema uses for one frame (Jump = 39, Locomotion = 30). Query buffers use this size. */
	static constexpr int32 MaxFeatures = 39;

	/** Locomotion schema trajectory sample times (s), GASP PSS_Default. */
	static constexpr float PastTime = -0.05f;
	static constexpr float FutureTimes[3] = { 0.35f, 0.7f, 1.0f };

	/** Jump schema trajectory sample times (s), GASP PSS_Jump. */
	static constexpr float JumpPastTime = -0.2f;
	static constexpr float JumpFutureTimes[2] = { 0.2f, 0.5f };

	/** Which GASP schema describes the frames. Changing it needs a Build. */
	UPROPERTY(EditAnywhere, Category = "Motion Matching")
	EMotionMatchingSchema Schema = EMotionMatchingSchema::Locomotion;

	/** Seconds at the end of every non-looping clip that are not stored (GASP ExcludeFromDatabaseParameters max -0.3). */
	UPROPERTY(EditAnywhere, Category = "Motion Matching", meta = (ClampMin = "0"))
	float ExcludeEndSeconds = 0.3f;

	/** 10-13: battle set made of walk / run loops only (no starts between them, e.g. the katana pack): walk gait plays only clips with
	 *  "Walk" in the name, run gait only "Run" (GASP picks its walk / run databases by gait the same way), and only clips that travel
	 *  near the input direction (UMotionMatchingComponent::CombatDirectionSlack). Off = the cost decides. */
	UPROPERTY(EditAnywhere, Category = "Motion Matching")
	bool bGaitByClipName = false;

	UPROPERTY(EditAnywhere, Category = "Motion Matching")
	TArray<FMotionMatchingSequence> Sequences;

	/** Frames stored per second of animation (GASP schema SampleRate 30). */
	UPROPERTY(EditAnywhere, Category = "Motion Matching", meta = (ClampMin = "5", ClampMax = "60"))
	float SampleRate = 30.f;

	UPROPERTY(EditAnywhere, Category = "Motion Matching|Bones")
	FName LeftFootBone = TEXT("foot_l");

	UPROPERTY(EditAnywhere, Category = "Motion Matching|Bones")
	FName RightFootBone = TEXT("foot_r");

	UPROPERTY(EditAnywhere, Category = "Motion Matching|Bones")
	FName PelvisBone = TEXT("pelvis");

	/** The character's forward direction inside the animations (GASP/UEFN = +Y). */
	UPROPERTY(EditAnywhere, Category = "Motion Matching|Bones")
	FVector AnimForward = FVector(0.f, 1.f, 0.f);

	/** Reads every frame of every sequence. Press after changing Sequences or bones, then save. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Motion Matching")
	void Build();

	int32 NumFrames() const { return Frames.Num(); }

	/** Floats per frame for this database's schema (as built). */
	int32 GetNumFeatures() const { return NumFeaturesBuilt; }

	/** Index of the first pose feature; everything before it is trajectory, filled from the prediction at runtime. */
	int32 GetFirstPoseFeature() const { return Schema == EMotionMatchingSchema::Jump ? 27 : 19; }

	/** True if (sequence, time) lies inside the stored part of the clip (loops: always). */
	bool IsTimeInDatabase(int32 SequenceIndex, float Time) const;
	const FMotionMatchingFrame& GetFrame(int32 Index) const { return Frames[Index]; }

	/** Frame index nearest to (sequence, time), or INDEX_NONE if that spot isn't in the database. */
	int32 FindFrame(int32 SequenceIndex, float Time) const;

	/** Turns raw feature values into the normalized form stored in the database. */
	void Normalize(float* InOutFeatures) const;

	/** Weighted cost of one frame against a normalized query, incl. its Base/Looping biases (no continuing bias). */
	float Cost(int32 FrameIndex, const float* NormalizedQuery) const;

	/**
	 * Best frame for a normalized query (biases included). Frames for which IsExcluded(seq, time)
	 * returns true are skipped. Returns INDEX_NONE if nothing is left.
	 */
	int32 Search(const float* NormalizedQuery, float& OutCost, TFunctionRef<bool(int32, float)> IsExcluded) const;

	/** Debug: one frame's cost per block with the raw (cm, cm/s) query and frame values: "block=cost[q .. f ..]". */
	FString DescribeFrame(int32 FrameIndex, const float* NormalizedQuery) const;

	/** Debug: logs the 5 best frames and the best frame of a clip whose name contains MustContain, with per-block costs. */
	void LogCandidates(const float* NormalizedQuery, const FString& MustContain, const FString& Label) const;

	/** Raw (not normalized) features of one frame - also used for the pose part of queries. */
	bool ComputeFeatures(int32 SequenceIndex, float Time, float* OutFeatures) const;

private:

	float FrameBias(int32 FrameIndex) const;

	UPROPERTY()
	TArray<FMotionMatchingFrame> Frames;

	/** NumFrames * NumFeatures, normalized. */
	UPROPERTY()
	TArray<float> Features;

	UPROPERTY()
	TArray<float> Mean;

	UPROPERTY()
	TArray<float> Deviation;

	UPROPERTY()
	TArray<float> Weights;

	/** First database frame of each sequence (INDEX_NONE if it has none). */
	UPROPERTY()
	TArray<int32> SequenceFirstFrame;

	/** Clip time of the last stored frame of each sequence. */
	UPROPERTY()
	TArray<float> SequenceLastTime;

	/** Floats per frame when Build ran (30 Locomotion / 39 Jump). */
	UPROPERTY()
	int32 NumFeaturesBuilt = 0;
};
