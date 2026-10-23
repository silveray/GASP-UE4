// Motion matching for a Character: predicts where the input is taking the character, finds the database frame
// whose motion matches best, and plays it on the mesh with a short crossfade.
//
// Databases, picked like GASP's chooser table:
//  - Database      (Locomotion schema = GASP PSS_Default) on the ground; no input -> only stop/idle clips.
//  - CrouchDatabase (10-09) on the ground while crouched; GASP 'Crouch Idle' / 'Crouch Moving' groups (see UpdateCrouchState).
//  - JumpDatabase  (Jump schema = GASP PSS_Jump) in the air (jump starts, ledge-offs, fall loop) and for
//                  JustLandedTime after landing (land clips, heavy when falling faster than HeavyLandSpeedThreshold).
// On the ground the playing clip moves the character (root motion); in the air the CharacterMovement physics do.
//
// Setup: add to the Character, set Database (+ JumpDatabase). The mesh's Animation Blueprint only needs
// AnimGraph: Slot 'DefaultSlot' -> Output Pose.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MotionMatchingDatabase.h"
#include "MotionMatchingComponent.generated.h"

class ACharacter;
class UAnimMontage;

UCLASS(ClassGroup = (Animation), meta = (BlueprintSpawnableComponent))
class NINJA_PROJECT_API UMotionMatchingComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UMotionMatchingComponent();

	/** Ground locomotion (Locomotion schema). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	UMotionMatchingDatabase* Database = nullptr;

	/** Jumps, falls and landings (Jump schema). Optional - without it the air uses the locomotion database. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	UMotionMatchingDatabase* JumpDatabase = nullptr;

	/** BATTLE stance locomotion (10-02, no blending): replaces Database on the
	 *  ground while SetCombatStance(true). Facing = StrafeFacing (strafe clips), turns on the spot by clip (> StrafeClipTurnAngle). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Battle")
	UMotionMatchingDatabase* CombatDatabase = nullptr;

	/** 10-04: a battle Turn clip started while standing plays to its end (no timed reselection) unless there is move input. The 2H
	 *  TurnL_180 was dropped after 0.13 s for a stop clip (cost 2.4 vs 23) = left 79 deg off the target; GASP commits to turns too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Battle")
	bool bCommitTurnClips = true;

	/** Battle database clips allowed (only) without input: stops, idle and the turns on the spot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Battle")
	TArray<FString> CombatStoppedClips = { TEXT("Stop"), TEXT("Idle"), TEXT("Turn") };

	/** Battle: code turn toward the facing while moving or for small angles (deg/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Battle", meta = (ClampMin = "0"))
	float StrafeTurnRate = 360.f;

	/** Battle, standing: off by more than this (deg) = a turn-on-the-spot clip does it; less = the code turn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Battle", meta = (ClampMin = "0"))
	float StrafeClipTurnAngle = 35.f;

	/** 10-13, battle sets with bGaitByClipName: a moving clip may play when its travel direction (from the character's facing) is at most
	 *  this many degrees further from the input direction than the best-fitting clip's (GASP's strafe chooser picks by direction too). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Battle", meta = (ClampMin = "0"))
	float CombatDirectionSlack = 20.f;

	/** Owner: battle stance on/off + the direction to face (target, else the camera). */
	void SetCombatStance(bool bOn, const FVector& FacingDir);
	bool IsCombatStance() const { return bCombatStance && CombatDatabase != nullptr; }

	/** 10-04 (2H sword step 3): swap the battle database at runtime (weapon set). Not built / null = keeps the current one.
	 *  Rebuilds the stopped-clip flags; in the stance a DB change forces a search on the next tick (ChooseDatabase). */
	void SetCombatDatabase(UMotionMatchingDatabase* Db);

	/** 10-13 (Ninja set): swap the crouch database at runtime (weapon set). Not built / null = keeps the current one.
	 *  Rebuilds the crouch groups; while crouched the DB change forces a search on the next tick (ChooseDatabase). */
	void SetCrouchDatabase(UMotionMatchingDatabase* Db);

	/** 10-04 battle sprint gait (GASP chooses by gait too): the owner sets it every frame. With move input, a combat database that has
	 *  'Sprint' clips allows ONLY those while true and none of them while false (the pose cost alone kept RunFwd over SprintLoop). */
	void SetWantSprint(bool bSprint) { bWantSprint = bSprint; }
	/** Walk gait held (10-13, battle sets with bGaitByClipName). */
	void SetWantWalk(bool bWalk) { bWantWalk = bWalk; }

	/** CROUCH locomotion (10-09, GASP PSD_Dense_Crouch_*): replaces Database on the ground while the owner is crouched
	 *  (ACharacter::bIsCrouched). Every clip carries its GASP group (Idles, TurnInPlace, Loops,
	 *  Pivots, Starts, Stops) and the chooser below allows the groups GASP's CHT_PoseSearchDatabases_Dense allows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Crouch")
	UMotionMatchingDatabase* CrouchDatabase = nullptr;

	/** GASP 'Crouch Idle' table: Idles row Speed2D <= this, Stops row Speed2D >= this (cm/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Crouch", meta = (ClampMin = "0"))
	float CrouchIdleSpeedSplit = 20.f;

	/** GASP ABP IsStarting: future velocity (trajectory 0.4-0.5 s) at least this much faster than now (cm/s) -> Starts allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Crouch", meta = (ClampMin = "0"))
	float StartingSpeedGain = 100.f;

	/** GASP ABP 'MM Pivot conditions': |angle between acceleration and velocity| >= this (deg) -> Pivots allowed.
	 *  GASP: 45 with orient-to-movement (the mode crouch runs in here), 30 while strafing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Crouch", meta = (ClampMin = "0", ClampMax = "180"))
	float PivotTurnAngle = 45.f;

	/** True while the crouch database is the one chosen (owner crouched, on the ground, not just landed). */
	bool IsCrouchLocomotion() const { return CrouchDatabase && CurrentDb == CrouchDatabase; }

	/** Seconds between searches. 0 = every frame (UE5 SearchThrottleTime default). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching", meta = (ClampMin = "0"))
	float SearchInterval = 0.f;

	/** Crossfade when jumping to a new frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching", meta = (ClampMin = "0"))
	float BlendTime = 0.2f;

	/** Never re-select a pose that was playing within this many seconds (UE5 PoseReselectHistory default). Stops backward jumps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching", meta = (ClampMin = "0"))
	float PoseReselectHistory = 0.3f;

	/**
	 * GASP-style chooser: on the ground with no input only clips whose name contains a StoppedClips word may play
	 * (stops, idles); with input those clips are excluded.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	bool bUseStateChooser = true;

	/** 10-09: + Transition_Crouch_to_Stand (GASP PSD_Dense_Stand_Idles = Stand_Idle_Loop + that clip: standing up plays it). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	TArray<FString> StoppedClips = { TEXT("Stop"), TEXT("Idle"), TEXT("Transition_Crouch_to_Stand") };

	/**
	 * 10-07 idle flicker fix: once a stop clip has played to its end with no input, only clips
	 * whose name contains an IdleAfterStopClips word may play until input returns. Without it the stops' -0.3 cost bias beat the
	 * idle loop (+0.1) every time and the ground set ping-ponged between stop-clip endings (~1 pop/s, idle loop picked 0 times).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	bool bIdleAfterStop = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	TArray<FString> IdleAfterStopClips = { TEXT("Stand_Idle") };

	/** Jump database clips whose name contains this are landings (only allowed for JustLandedTime after touching down). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	FString LandClipWord = TEXT("Land");

	/** Landing clips that count as heavy (name contains one of these). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	TArray<FString> HeavyLandWords = { TEXT("Heavy"), TEXT("Roll"), TEXT("Stumble") };

	/** GASP SandboxCharacter: JustLanded stays true this long after landing (RetriggerableDelay 0.3). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser", meta = (ClampMin = "0"))
	float JustLandedTime = 0.3f;

	/**
	 * In the air, landing clips may play once the predicted touchdown is this close (s). GASP land clips start in the air
	 * (e.g. Land_Run_Light touches down 0.5 s into the clip); GASP's ABP keeps the predicted TimeToLand for this.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser", meta = (ClampMin = "0"))
	float LandAnticipationTime = 0.f; // 10-01: 0.5 showed post-touchdown (running) land frames mid-air -> off

	/**
	 * GASP land databases are split by gait (PSD_Dense_Stand_{Idle,Walk,Run,Sprint}_Lands_*). After touchdown only land clips
	 * whose name contains the word for the gait at touchdown are allowed. Order: idle (no input), walk, run, sprint.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	TArray<FString> LandGaitWords = { TEXT("Land_Stand"), TEXT("Land_Walk"), TEXT("Land_Run"), TEXT("Land_Sprint") };

	/** Gait at touchdown from CharacterMovement MaxWalkSpeed (GASP walk 200 / run 500 / sprint 700): up to this = walk... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser", meta = (ClampMin = "0"))
	float WalkGaitMaxSpeed = 300.f;

	/** ...up to this = run, faster = sprint. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser", meta = (ClampMin = "0"))
	float RunGaitMaxSpeed = 600.f;

	/**
	 * In the air, never pick frames whose clip still has its feet on the ground (root at height 0, not rising or falling):
	 * the run-up of jump starts and ledge run-offs. Without this, holding W while falling played running steps in mid-air
	 * (10-01 'air walk'; GASP shows none). Off = old behaviour.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	bool bNoGroundFramesInAir = true;

	/**
	 * Uphill slowdown: walking up a slope plays the animation slower (root motion -> slower walk, feet don't slide).
	 * Play rate at UphillMaxAngle or steeper (1 = off). Like GASP's dynamic play rate range (0.75-1.25). Downhill: no change.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Slope", meta = (ClampMin = "0.5", ClampMax = "1"))
	float UphillSpeedScale = 0.85f;

	/** Slope angle (degrees, along the walking direction) where UphillSpeedScale is fully reached. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Slope", meta = (ClampMin = "1", ClampMax = "60"))
	float UphillMaxAngle = 30.f;

	/** Falling (after the apex): no new search, the jump clip plays on (GASP look). Off = searching hops back to apex frames (frozen legs). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	bool bKeepClipWhileFalling = true;

	/**
	 * Long falls: once the jump clip runs out while falling, only looping air clips (Jump_Loop_Fall) may play until landing.
	 * Off = old behaviour (ping-pong between the ends of Start_Run L/R, 10-01).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	bool bFallLoopWhenClipEnds = true;

	/** GASP ABP HeavyLandSpeedThreshold: landing faster than this (cm/s, downward) uses heavy landings. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser", meta = (ClampMin = "0"))
	float HeavyLandSpeedThreshold = 700.f;

	/** User wish 10-01: after a fall of at least RollMinFallHeight the landing is a roll (GASP Land_Roll clips, in DB_GASP_Jump). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	bool bAutoRollOnHighFall = true;

	/** Fall height (cm, highest point in the air -> touchdown) from which the landing is a roll. A normal jump falls ~130 cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser", meta = (ClampMin = "0"))
	float RollMinFallHeight = 300.f;

	/** Jump database clips whose name contains this are the roll landings (only used after a high fall). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Chooser")
	FString RollLandWord = TEXT("Land_Roll");

	/** Fall height of the last landing (cm) - for the auto test. */
	float GetLastFallHeight() const { return LastFallHeight; }

	/** A roll landing (high fall) is playing. */
	bool IsPlayingRoll() const
	{
		return bRollLanding && CurrentDb && CurrentDb == JumpDatabase && IsRollLandClip.IsValidIndex(CurrentSequence) && IsRollLandClip[CurrentSequence];
	}

	/** Write every clip switch to the log (LogTemp 'MMSwitch') and candidate tables on input edges ('MMDebug'). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Debug")
	bool bLogSwitches = true;

	/** Don't jump if the best frame is in the same clip and closer than this to where we already are (s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	float SameClipTolerance = 0.2f;

	/**
	 * Step of the trajectory prediction (s). Ground: port of UE5's PoseSearch CharacterMovement prediction (uses the
	 * CharacterMovementComponent's MaxAcceleration, GroundFriction, braking values and max speed). Air: constant velocity
	 * plus gravity with a floor trace (port of UE5 HandleTransformTrajectoryWorldCollisionsWithGravity).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Trajectory", meta = (ClampMin = "0.005", ClampMax = "0.1"))
	float PredictionStep = 1.f / 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching")
	FName SlotName = TEXT("DefaultSlot");

	/**
	 * On: on the ground the playing clip moves and turns the character (root motion) - no foot sliding; input only chooses clips.
	 * Off: the capsule moves by input and the clip is only visual (feet slide when they disagree).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Movement")
	bool bRootMotionMovement = true;

	/**
	 * 10-09 GASP movement model: on the ground outside the battle stance the CharacterMovement moves the capsule
	 * and turns it instantly to the movement (GASP UpdateRotation_PreCMC: orient to movement, RotationRate yaw -1 ground / 200 air); the
	 * clip's root motion only moves the visible body, through the GASP warping nodes (Offset Root / Steering / Orientation Warping,
	 * console gasp.Warping 2). Off = bRootMotionMovement as before. Battle stance and montages owned by other systems keep root motion.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Movement")
	bool bGaspMovementModel = true;

	/** True while the GASP movement model drives the capsule (see bGaspMovementModel). */
	bool UsesGaspMovementModel() const;

	/** bGaspMovementModel AND console gasp.MovementModel 1 (also during battle stance / montages, when UsesGaspMovementModel is false). */
	bool IsGaspMovementModelEnabled() const;

	/** With root motion on: extra turn toward the input direction (deg/s). 0 = clips only (default: user test 10-01, 180 caused switch storms). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Movement", meta = (ClampMin = "0"))
	float FacingCorrectionRate = 0.f;

	/** Draw the predicted path (green) and print the chosen clip. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Motion Matching|Debug")
	bool bDebugDraw = true;

	/** Clip switches since play started (read by the automatic test). */
	int32 SwitchCount = 0;

	/** Name of the clip playing now (debug / automatic test). */
	FString GetCurrentClipName() const;

	/** 10-09 GASP warping nodes (AnimNode_MidearthSteering / OrientationWarping): the clip playing now and its time
	 *  (GASP GetCurrentBlendStackAnimAsset / AnimAssetTime). Null when nothing plays. */
	UAnimSequence* GetCurrentClip() const;
	float GetCurrentClipTime() const { return CurrentTime; }
	bool IsCurrentClipLooping() const { return CurrentDb && CurrentDb->Sequences.IsValidIndex(CurrentSequence) && CurrentDb->Sequences[CurrentSequence].bLoop; }

	/** GASP Get_DesiredFacing = trajectory facing at +0.5 s: the acceleration direction (battle: the strafe facing), else the actor's. World, 2D. */
	FVector GetDesiredFacing() const;

	/** GASP database tag 'TurnInPlace' (Stand_TurnInPlace / Crouch_TurnInPlace clips): Steering B runs only then. */
	bool IsPlayingTurnInPlace() const;

	/** Written by the Midearth Foot IK anim node each frame (cm): pelvis drop, left/right foot offsets. For the auto test / debugging. */
	float FootIKPelvis = 0.f, FootIKLeft = 0.f, FootIKRight = 0.f;

	/** Uphill factor now (1 = flat or downhill). The character scales MaxWalkSpeed with it so the predicted path matches the slower walk. */
	float GetSlopeSpeedFactor() const { return SlopeSpeedFactor; }

	/** 10-07 Rossale: 1 / the character's StrideScale. The predicted path is in the WORLD's distances, the database in the CLIPS';
	 *  a long-legged body moves StrideScale x further per step, so the trajectory query is shrunk back by this. 1 = unchanged. */
	float TrajectoryScale = 1.f;

	/** Automatic test: true if the playing frame has the clip's feet on the ground (root at height 0, not rising or falling). */
	bool IsPlayingGroundFrame() const;

	/** Forget the position history (call after teleporting the character, so the past trajectory sample doesn't jump). */
	void ClearPositionHistory() { PositionHistory.Reset(); }
    /** Fresh save-load teleport: discard spawn trajectory and seed a stationary idle pose. */
    void ResetAfterTeleport();

	/**
	 * Another system owns the DefaultSlot (traversal now; attack / dodge montages later): while suspended there are no searches,
	 * no root-motion mode changes and no steering. Resume = fresh forced search (pose part empty) + root-motion mode re-applied.
	 */
	void SetSuspended(bool bInSuspended);
	bool IsSuspended() const { return bSuspended; }

	/** For action clips (combo, flinch, dodge, block) when their clip blends out: resume NEXT tick, and only if no other montage
	 *  owns the slot then. (4.27 fires the old clip's blending-out event inside the play call of the new one, before the new one
	 *  exists - resuming right there let motion matching cut the new clip.) */
	void ResumeWhenSlotFree();

protected:

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:

	/** Ground prediction: world positions, facings (2D) and velocities at Times[]. */
	void PredictGround(const float* Times, int32 Num, FVector* OutPositions, FVector* OutFacings, FVector* OutVelocities) const;

	/**
	 * Air prediction: constant velocity + gravity, clamped to the floor found by traces. Optionally returns the time until
	 * touchdown (-1 if none before the last sample) and the downward speed at touchdown (UE5 TimeToLand / LandSpeed).
	 */
	void PredictAir(const float* Times, int32 Num, FVector* OutPositions, FVector* OutVelocities,
		float* OutTimeToLand = nullptr, float* OutLandSpeedZ = nullptr) const;

	/** Where the character was Seconds ago (Seconds < 0), from PositionHistory. */
	FVector PastPosition(float Seconds) const;

	/** Fills Query (normalized) for Db's schema; pose part from the playing frame when it belongs to Db. Returns false if Db is empty. */
	bool BuildQuery(const UMotionMatchingDatabase* Db, float* Query, FVector* OutDrawPositions, int32& OutNumDraw) const;

	void SearchAndPlay(bool bForce);
	void PlayFrame(UMotionMatchingDatabase* Db, int32 FrameIndex);

	/** Database the chooser wants now (air / just landed -> JumpDatabase). */
	UMotionMatchingDatabase* ChooseDatabase() const;

	/** Chooser filter: true if this clip may not play in the current state. */
	bool IsClipExcludedByState(const UMotionMatchingDatabase* Db, int32 Sequence, bool bHasInput) const;
	/** Battle clip travels too far off the input direction (CombatDirectionSlack, 10-13). */
	bool IsCombatClipOffDirection(int32 Sequence) const;

	/** GASP crouch chooser state, refreshed every tick (UpdateCrouchState): ABP IsMoving / IsStarting / IsPivoting / Speed2D. */
	void UpdateCrouchState();
	enum class ECrouchGroup : uint8 { None, Idles, TurnInPlace, Loops, Pivots, Starts, Stops };
	TArray<ECrouchGroup> CrouchGroup;  // CrouchDatabase: group per sequence (from FMotionMatchingSequence::Group)
	bool bCrouchMoving = false;        // GASP MovementState Moving: velocity != 0 (tol 0.1) AND acceleration != 0
	bool bCrouchMovingLastFrame = false;
	bool bCrouchStarting = false;      // GASP IsStarting
	bool bCrouchPivoting = false;      // GASP IsPivoting (MM Pivot conditions)
	float CrouchSpeed2D = 0.f;
	bool bWasCrouchDb = false;         // crouch database chosen last tick (resets the stand idle-after-stop latch on a change)
	// 10-09 step 3: GASP stand chooser (CHT_PoseSearchDatabases_Dense 'Stand Idles / Walks / Runs / Sprint'), used when the standing
	// database carries GASP groups and the GASP movement model is on.
	enum class EStandGroup : uint8 { None, Idles, TurnInPlace, Starts, Loops, Pivots, Stops, Spin };
	TArray<EStandGroup> StandGroup;    // Database: group type per sequence
	TArray<uint8> StandGroupGait;      // Database: 0 none, 1 walk, 2 run, 3 sprint
	bool bStandHasGroups = false;
	bool UseStandGroups() const { return bStandHasGroups && UsesGaspMovementModel(); }
	uint8 GaspGait = 2;                // GASP Gait now (1 walk 2 run 3 sprint, from MaxWalkSpeed like LandGait)
	uint8 GaspGaitLastFrame = 2;
	bool bGaspSpinning = false;        // GASP ShouldSpinTransition (>= 130 deg turn and >= 150 cm/s)

	ACharacter* Character = nullptr;

	/** The database and clip playing now. */
	UPROPERTY(Transient)
	UMotionMatchingDatabase* CurrentDb = nullptr;
	int32 CurrentSequence = INDEX_NONE;
	TWeakObjectPtr<UAnimMontage> CurrentMontage;
	float CurrentTime = 0.f;
	float SearchTimer = 0.f;
	float LastCost = 0.f;

	/** Chooser categories, filled in BeginPlay from clip names. */
	TArray<bool> IsStoppedClip;   // Database: stop/idle clips
	TArray<bool> IsStopClip;      // Database: names containing 'Stop' (ends one stop -> idle-only, bIdleAfterStop)
	TArray<bool> IsIdleAfterStopClip; // Database: names containing an IdleAfterStopClips word
	bool bStopFinished = false;   // a stop clip ran out with no input; cleared by input
	TArray<bool> IsCombatStoppedClip; // CombatDatabase: stop/idle/turn clips
	TArray<bool> IsCombatSprintClip;  // CombatDatabase: names containing 'Sprint' (10-04)
	bool bCombatDbHasSprint = false;
	bool bCombatDbHasTurn = false;   // CombatDatabase has turn-on-the-spot clips (10-13: the katana set has none -> code turn)
	bool bWantSprint = false;
	bool bWantWalk = false;
	TArray<bool> IsCombatWalkClip, IsCombatRunClip; // CombatDatabase: 'Walk' / 'Run' in the name (bGaitByClipName, 10-13)
	TArray<float> CombatClipYaw;   // CombatDatabase: travel direction, deg from forward (+ = right); NoClipYaw = stands still
	static constexpr float NoClipYaw = 1000.f;
	bool bCombatStance = false;
	FVector StrafeFacing = FVector::ForwardVector;
	TArray<bool> IsLandClip;      // JumpDatabase: landings (landing part; a land clip with SampleEnd set is its air part = air clip)
	TArray<bool> IsHeavyLandClip; // JumpDatabase: heavy landings
	TArray<bool> IsRollLandClip;  // JumpDatabase: roll landings (RollLandWord)
	float FallTopZ = 0.f;         // highest capsule Z during the current air time
	float LastFallHeight = 0.f;   // FallTopZ - Z at the last touchdown
	bool bRollLanding = false;    // last touchdown was a high fall -> roll clips only
	TArray<int32> LandClipGait;   // JumpDatabase: index into LandGaitWords (INDEX_NONE = no gait)
	float SlopeSpeedFactor = 1.f;
	TArray<bool> JumpGroundFrame; // JumpDatabase frame index -> clip's feet on the ground (excluded in the air, bNoGroundFramesInAir)
	int32 LandGait = 0;           // gait at touchdown: 0 Stand(idle) 1 Walk 2 Run 3 Sprint
	bool bJumpDbHasLoop = false;  // JumpDatabase has a looping clip (fall loop)
	bool bFallLoopOnly = false;   // falling and the air clip ran out -> only looping air clips until landing (bFallLoopWhenClipEnds)

	// Air / landing state (GASP JustLanded / LandVelocity).
	bool bWasInAir = false;
	FVector LastAirVelocity = FVector::ZeroVector;
	float LandWorldTime = -100.f;
	float LandSpeedZ = 0.f;
	bool bSuspended = false;
	float PredictedTimeToLand = -1.f;   // in the air: seconds until touchdown (-1 = not within LandAnticipationTime)
	float PredictedLandSpeedZ = 0.f;    // in the air: downward speed at the predicted touchdown

	// Debug: log candidates when input is pressed / released (bLogSwitches).
	bool bHadInputForDebug = false;
	float SinceRelease = -1.f;
	int32 ReleaseLogsDone = 0;
	bool bEdgeIsPress = false;

	/** Poses that were playing recently. Used by PoseReselectHistory. */
	struct FPlayedPose
	{
		const UMotionMatchingDatabase* Db;
		int32 Sequence;
		float Time;
		float WorldTime;
	};
	TArray<FPlayedPose> History;

	/** Recent actor positions (world time, position) for the trajectory's past samples. */
	TArray<TPair<float, FVector>> PositionHistory;
};

