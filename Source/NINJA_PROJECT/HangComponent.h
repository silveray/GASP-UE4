// Ledge HANG (10-11). Dev panel F2 > GAME MODE > Ledges: Climb / Hang
// (console trav.Style 0 / 1). Climb (0, default) = the old UTraversalComponent behaviour, untouched. Hang (1):
//   - Space at a wall whose ledge the old code would CLIMB (MantleMaxHeight..MaxHeight): the same GASP climb clip + warp runs up to the
//     hands landing on the ledge, then UTraversalComponent hands over (HangHandoff) -> hanging. Lower ledges still mantle / hurdle.
//   - Space on a roof / platform edge (a drop >= LowerMinDrop within LowerEdgeReach ahead, no climbable wall): turn round and lower into
//     a hang = the climb-up clip played BACKWARDS (montage position set every tick).
//   - Hanging: A / D shimmy (clip step ~78 cm, only if the ledge continues ShimmyAhead past the hands and the side is free), Space = climb
//     up (room on top) else hop up, W = hop up to a ledge HopMinRise..HopMaxRise above, C (crouch key) = let go.
// Clips: /Game/Animations/LedgeHang (Free Animation Library, retargeted to UEFN in 5.8; root motion ON,
// root locked). Like UTraversalComponent the capsule is moved by this component from the clip's root track (MOVE_None, root motion
// ignored, motion matching suspended, capsule ignores the ledge it hangs on); per move the root track is scaled / skewed so the move ends
// exactly at the next real hang spot / on the real top. Hang spot = the idle clip's hands HandInset past the edge, HandAboveTop above the top.
// Setup: AMMTestCharacter creates it; BeginPlay links itself to the character's UTraversalComponent.
// Not built yet (status board): catch a ledge while falling, hop down, shimmy round corners, hand IK while hanging, wall-climb set.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HangComponent.generated.h"

class ACharacter;
class UAnimMontage;
class UAnimSequence;
class UPrimitiveComponent;
class UTraversalComponent;

/** 10-11 hand IK + body clearance while hanging, for FAnimNode_MidearthHandIK (game thread -> anim thread). World space. */
struct FHangIKFrame
{
	FVector Edge = FVector::ZeroVector;    // front edge point (top height)
	FVector Fwd = FVector::ForwardVector;  // into the wall
	FVector HandShift = FVector::ZeroVector; // a LIFTED hand moves by this (undoes the body's raise / gap); planted hands go onto the ledge
	bool bHigh = false;                    // high hang: elbows bend out to the sides
	bool bBrace = false;                   // high hang: legs braced on the wall (frog)
	// 10-11 v3 hands ON THE EDGE during moves:
	FVector HandDelta[2] = { FVector::ZeroVector, FVector::ZeroVector }; // climb / lower in: the clip's hand motion relative to its ledge
	float HandEdgeW[2] = { 1.f, 1.f };     // 1 = wrist on the edge grip (+ HandDelta + Lift); 0 = the clip's own hand (lifted / let go)
	float HookW[2] = { 1.f, 1.f };         // hook rotation + finger curl weight
	float Lift[2] = { 0.f, 0.f };          // shimmy: the moving hand lifts this much (cm) while sliding along the edge
	float Push[2] = { 0.f, 0.f };          // climb: 0 = hook grip, 1 = palm flat on the top pressing (mantle push)
	float PushInset = 8.f;                 // push: knuckles this far in from the edge on the top
	float PushAbove = 2.5f;                // ... and this high over it
	float BraceDrop = 55.f;                // ankle this far under the hip
	float BraceSpread = 14.f;              // ... and this far out to its side
	float BraceFootOut = 14.f;             // ankle this far in front of the wall
	float BraceToeAngle = 40.f;            // foot (ankle -> ball) points down, tilted this much into the wall
	float Plant[2] = { 0.f, 0.f };         // 0..1 per hand (0 left, 1 right): the clip's hand rests on the ledge -> snap onto it
	float TopZ[2] = { 0.f, 0.f };          // top surface under each hand
	float HandInset = 6.f;
	float HandAboveTop = 4.f;
	float HandSpread = 0.f;                // 10-11 F2 'HAND POSITION': hands this much further apart (cm, - = closer)
	float HandAlpha = 0.f;                 // whole hand IK (fades in with the hang)
	float LegAlpha = 0.f;                  // leg swing-out off the wall
	float LegMargin = 6.f;                 // knee / ankle / toes kept this far in front of the wall (on top of the bone radius below)
	float MaxLegAngle = 75.f;
	int32 LegMode = 2;                     // hang.LegFix: 1 straight (swing the leg down/back), 2 tucked (turn the knee out, keep the tuck)
	float MaxKneeOut = 90.f;
	float MaxToeTilt = 70.f;
	// 10-11 v2 GRIP (user photo: palms on the wall face, only the fingers hooked over the top). False = the old flat hands.
	bool bHook = true;
	float KnuckleAbove = 3.f;              // knuckle row (finger _01 joints) above the top
	float KnuckleOut = 2.f;                // ... and this far out of the wall face
	float FingerLean = 10.f;               // hand: fingers point up, leaning this much toward the wall (deg)
	float FingerBend[3] = { 8.f, 40.f, 65.f };    // segment _01 / _02 / _03 direction below horizontal-into-the-wall (deg)
};

UENUM(BlueprintType)
enum class EHangMove : uint8
{
	None,        // not hanging
	Idle,
	ShimmyLeft,
	ShimmyRight,
	HopUp,
	ClimbUp,     // ends standing on the top (hang over)
	LowerIn,     // reversed climb-up from the top into the hang
};

UCLASS(ClassGroup = (Midearth), meta = (BlueprintSpawnableComponent, DisplayName = "Hang"))
class NINJA_PROJECT_API UHangComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UHangComponent();

	// ---------------- Clips ----------------

	UPROPERTY(EditAnywhere, Category = "Hang|Clips")
	TSoftObjectPtr<UAnimSequence> IdleClip;

	UPROPERTY(EditAnywhere, Category = "Hang|Clips")
	TSoftObjectPtr<UAnimSequence> ShimmyLeftClip;

	UPROPERTY(EditAnywhere, Category = "Hang|Clips")
	TSoftObjectPtr<UAnimSequence> ShimmyRightClip;

	UPROPERTY(EditAnywhere, Category = "Hang|Clips")
	TSoftObjectPtr<UAnimSequence> HopUpClip;

	/** Hang -> standing on top. Played backwards for LowerIn. */
	UPROPERTY(EditAnywhere, Category = "Hang|Clips")
	TSoftObjectPtr<UAnimSequence> ClimbUpClip;

	// ---------------- Hang spot ----------------

	/** Hand bone this far past the front edge, over the top (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang", meta = (ClampMin = "0"))
	float HandInset = 6.f;

	/** Hand bone this far above the top surface (the palm under it rests on the top) (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang")
	float HandAboveTop = 4.f;

	/** Hands this much further apart than the clip (cm, negative = closer). Both grips. F2 > LEDGE HANGING > HAND POSITION (10-11). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang", meta = (UIMin = "-20", UIMax = "40"))
	float HandSpread = 0.f;

	/** Blend into the hang idle / between moves (s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang", meta = (ClampMin = "0"))
	float BlendTime = 0.2f;

	/** Grab from the ground: hand over from the climb clip this long after both hands rest on the ledge (s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang", meta = (ClampMin = "0"))
	float GrabDelay = 0.05f;

	// ---------------- Moving while hanging ----------------

	/** A / D / W count when the stick is past this (0..1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Move", meta = (ClampMin = "0", ClampMax = "1"))
	float InputThreshold = 0.5f;

	/** Shimmy only if the ledge goes on this far past where the step ends (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Move", meta = (ClampMin = "0"))
	float ShimmyAhead = 30.f;

	/** The next ledge may be this much higher / lower than the current one along a shimmy (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Move", meta = (ClampMin = "0"))
	float ShimmyMaxStep = 15.f;

	/** Hop up: the ledge above must be between these heights over the current one (cm). Clip = 45. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Move", meta = (ClampMin = "0"))
	float HopMinRise = 25.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Move", meta = (ClampMin = "0"))
	float HopMaxRise = 70.f;

	/** Climb up: the capsule ends this far past the edge (+ its radius) on the top (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Move", meta = (ClampMin = "0"))
	float StandInset = 15.f;

	// ---------------- Lowering in from a roof ----------------

	/** Space on the top: an edge within this distance ahead (cm)... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Lower", meta = (ClampMin = "0"))
	float LowerEdgeReach = 90.f;

	/** ...with at least this drop below it (cm). Less = just step / jump down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Lower", meta = (ClampMin = "0"))
	float LowerMinDrop = 180.f;

	/** Reversed climb-up playback speed (1 = clip speed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Lower", meta = (ClampMin = "0.1"))
	float LowerPlayRate = 1.f;

	// ---------------- Body clearance + hand IK (10-11) ----------------

	/** The hanging body hangs this far further off the wall than the clip (measured: head 12 cm, chest 10 cm inside the block at 0).
	 *  Hand IK puts the hands back onto the edge. Console hang.HandIK 0 = off (also the gap). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Clearance", meta = (ClampMin = "0", UIMax = "40"))
	float WallGap = 14.f;

	// ---------------- HIGH hang (10-11, user video: head at the ledge, body pressed to the wall, elbows bent out, feet on the wall) ----------------
	// Console hang.Pose 1 = high (default) / 0 = low (arms straight, WallGap off the wall).

	/** High hang: the head bone this far above the top (negative = below it; the eyes peek over at about -6). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (UIMin = "-40", UIMax = "10"))
	float HeadToTop = -6.f;

	/** High hang legs (console hang.LegBrace): ankle this far under the hip (cm)... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (UIMin = "20", UIMax = "90"))
	float BraceDrop = 55.f;

	/** ...this far out to its side (cm)... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (UIMin = "-10", UIMax = "40"))
	float BraceSpread = 14.f;

	/** ...and this far in front of the wall (cm); the toes then press on it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (UIMin = "5", UIMax = "30"))
	float BraceFootOut = 14.f;

	/** Foot (ankle -> toes) points down, tilted this much into the wall (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (UIMin = "0", UIMax = "80"))
	float BraceToeAngle = 40.f;

	/** Shimmy: the moving hand slides along the edge, lifting at most this much (cm). The clip itself lifts it ~25. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (ClampMin = "0", UIMax = "30"))
	float ShimmyHandLift = 6.f;

	/** Climb up: the hands turn from the hook grip into a push (palms on the top) between these parts of the body's rise (0..1)... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (ClampMin = "0", ClampMax = "1"))
	float ClimbPushStart = 0.05f; // climb clip (10-11): body pulled up 0.3-1.0 s, forward onto the top 1.0-1.5 s

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (ClampMin = "0", ClampMax = "1"))
	float ClimbPushEnd = 0.35f;

	/** ...and let go of the edge from this part of the rise on (fading out in HandReleaseTime). Run 1: the clip's own hands left at 0.3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (ClampMin = "0", ClampMax = "1"))
	float ClimbReleaseRise = 0.8f;

	/** Push: knuckles this far in from the edge on the top / this high over it (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "0", UIMax = "20"))
	float PushInset = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "0", UIMax = "8"))
	float PushAbove = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (ClampMin = "0.01", UIMax = "1"))
	float HandReleaseTime = 0.3f;

	/** High hang: most the body is raised above the clip's low hang (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (ClampMin = "0", UIMax = "70"))
	float MaxRaise = 50.f;

	/** High hang: the torso point nearest the wall (head / chest / belly, flesh included) is kept this far in front of it (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (UIMin = "-3", UIMax = "15"))
	float TouchGap = 1.f;

	/** How fast the body settles in / out to TouchGap (1/s). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|High", meta = (ClampMin = "0.1", UIMax = "20"))
	float AutoGapSpeed = 6.f;

	/** Legs: knee, ankle and toes are kept this far in front of the wall (cm) by swinging the leg back at the hip (console hang.LegFix). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Clearance", meta = (ClampMin = "0", UIMax = "20"))
	float LegMargin = 6.f;

	/** Most the leg may swing back (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Clearance", meta = (ClampMin = "0", ClampMax = "130"))
	float MaxLegAngle = 110.f; // the clip's left knee needs ~100 deg (measured 10-11); the shin turns back so the feet stay at the wall

	/** Legs 'Tucked' (hang.LegFix 2, 10-11 v6): most a knee turns OUT to its side to clear the wall, keeping the clip's tuck (deg).
	 *  Only what this can't clear is swung down (Straight's swing, counted double). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Clearance", meta = (ClampMin = "0", ClampMax = "90"))
	float MaxKneeOut = 90.f; // 10-11: 60 wasn't enough when the shimmy swings the knee inward -> fell back to swinging (knee 15 cm in / pops); 90 = clean, peak 82

	/** Legs 'Tucked': most a foot tilts toes-down so the toes rest on the wall instead of in it (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Clearance", meta = (ClampMin = "0", ClampMax = "90"))
	float MaxToeTilt = 70.f;

	// GRIP (10-11 v2, user's photo): palms against the wall face, knuckles at the edge, fingers curled over onto the top.
	// Console hang.HandGrip 1 = hook (default) / 0 = the first version (hands flat on top, HandInset / HandAboveTop).

	/** Knuckle row (the fingers' first joints) this far above the top (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "-5", UIMax = "8"))
	float KnuckleAbove = 3.f; // 10-11: at 1 the fingertips sat 1.2 cm inside the top, at 2.5 on it (bone centre) -> 3 = skin on the surface

	/** ...and this far out of the wall face (cm). Bigger = palms further off the wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "-3", UIMax = "8"))
	float KnuckleOut = 2.f;

	/** Hand: fingers point up the wall, leaning this much toward it (deg). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "-30", UIMax = "45"))
	float FingerLean = 10.f;

	/** Finger segments over the top: first / middle / tip, degrees below 'flat into the wall' (more = curled down harder). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "-30", UIMax = "90"))
	float FingerBend1 = 8.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "-30", UIMax = "120"))
	float FingerBend2 = 40.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Grip", meta = (UIMin = "-30", UIMax = "150"))
	float FingerBend3 = 65.f;

	/** For the anim node. False = not hanging (node leaves the pose alone). */
	bool GetIKFrame(FHangIKFrame& Out) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hang|Debug")
	bool bDebugDraw = false;

	// ---------------- API ----------------

	/** trav.Style == 1 (dev panel 'Ledges: Hang'). */
	static bool IsHangStyle();

	/** Space while hanging: climb up (room on top) or hop up. Always true while hanging (Space never jumps off a ledge). */
	UFUNCTION(BlueprintCallable, Category = "Hang")
	bool HandleJump();

	/** Space on the ground in hang style: lower into a hang over an edge ahead. False = nothing there. */
	UFUNCTION(BlueprintCallable, Category = "Hang")
	bool TryLowerIn();

	/** Let go (falls). */
	UFUNCTION(BlueprintCallable, Category = "Hang")
	void Drop();

	/** Start hanging on Edge (a point on the front edge line, at the top's height), facing WallFwd (into the wall). */
	void BeginHang(const FVector& Edge, const FVector& WallFwd, UPrimitiveComponent* Ledge);

	UFUNCTION(BlueprintPure, Category = "Hang")
	bool IsHanging() const { return Move != EHangMove::None; }

	UFUNCTION(BlueprintPure, Category = "Hang")
	EHangMove GetMove() const { return Move; }

	UFUNCTION(BlueprintPure, Category = "Hang")
	FVector GetEdge() const { return Edge; }

	/** Tests: overrides the move input (Right, Forward in -1..1) while not both zero. */
	UFUNCTION(BlueprintCallable, Category = "Hang|Test")
	void SetTestInput(float InRight, float InForward) { TestInput = FVector2D(InRight, InForward); }

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:

	struct FHangClip
	{
		UAnimSequence* Seq = nullptr;
		float Length = 0.f;
	};

	FVector ClipRoot(const FHangClip& Clip, float Time) const;            // character space: X fwd, Y right, Z up (cm)
	FVector ClipBone(const FHangClip& Clip, FName Bone, float Time) const;
	FVector ClipBoneOnMesh(const FHangClip& Clip, FName Bone, float Time) const; // from the locked root, on THIS character's mesh
	FVector ToWorld(const FVector& CharSpace) const;                       // with the current wall axes
	FVector HangRootFor(const FVector& InEdge, const FVector& InFwd) const; // root (= feet point of the mesh) world location
	FVector ActorFromRoot(const FVector& Root) const;
	bool ProbeLedge(const FVector& Around, float MinZ, float MaxZ, FVector& OutEdge, FVector& OutFwd, UPrimitiveComponent*& OutComp) const;
	FVector2D GetMoveInput() const;                                        // X = right along the wall, Y = up (into the wall)
	bool PlayClip(const FHangClip& Clip, float StartAt, float Blend, int32 Loops);
	void StartMove(EHangMove NewMove, const FHangClip& Clip, const FVector& EndRoot, const FVector& NewEdge, const FVector& NewFwd, UPrimitiveComponent* NewComp);
	void SetLedgeComp(UPrimitiveComponent* Comp);
	void EnterIdle(float Blend);
	void EndHang(bool bStandOnTop);
	bool TryShimmy(float Side);
	bool TryHopUp();
	bool TryClimbUp();

	ACharacter* Character = nullptr;
	TWeakObjectPtr<UTraversalComponent> Traversal;

	FHangClip Idle, Left, Right, HopUp, ClimbUp;
	UPROPERTY(Transient)
	TArray<UAnimSequence*> Loaded;
	FVector HandRel = FVector::ZeroVector; // idle clip: middle of the hands from the root (character space)

	EHangMove Move = EHangMove::None;
	UPROPERTY(Transient)
	UAnimMontage* Montage = nullptr;
	const FHangClip* Cur = nullptr;
	FVector Edge = FVector::ZeroVector;    // current (move start) ledge
	FVector Fwd = FVector::ForwardVector;  // into the wall
	TWeakObjectPtr<UPrimitiveComponent> LedgeComp;
	FVector BaseRoot = FVector::ZeroVector; // root world at the move's clip start
	FVector MoveScale = FVector::OneVector; // per axis (char space) on the clip's root delta
	FVector MoveSkew = FVector::ZeroVector; // world offset reached linearly by the move's end
	float MoveTime = 0.f;                   // clip time
	float MoveElapsed = 0.f;
	float BlendInLeft = 0.f;                // capsule eases from BlendFrom to the clip path while blending in
	FVector BlendFrom = FVector::ZeroVector;
	float TurnFromYaw = 0.f;
	// pending move end
	FVector NextEdge = FVector::ZeroVector;
	FVector NextFwd = FVector::ForwardVector;
	TWeakObjectPtr<UPrimitiveComponent> NextComp;
	FVector2D TestInput = FVector2D::ZeroVector;
	bool bMeasured = false;
	bool bJumpQueued = false;               // Space during a shimmy / hop: climb (or hop) when it ends
	// IK frame (UpdateIK every tick)
	void UpdateIK(float DeltaTime, float Progress);
	float PlantW[2] = { 0.f, 0.f };
	float EdgeW[2] = { 1.f, 1.f };
	float HookWNow[2] = { 1.f, 1.f };
	float LiftNow[2] = { 0.f, 0.f };
	FVector HandDeltaNow[2] = { FVector::ZeroVector, FVector::ZeroVector };
	float PushNow[2] = { 0.f, 0.f };
	float HandTopZ[2] = { 0.f, 0.f };
	FVector BodyOffsetNow = FVector::ZeroVector; // raise + gap applied this tick
	FVector HeadRel = FVector::ZeroVector;       // idle head from the locked root on this mesh
	float AutoGap = 14.f;                        // high hang: steered so the torso touches the wall (kept between hangs)
	float LastTorsoClear = 0.f;
	float IKAlpha = 0.f;
	float CurProgress = 0.f;
	bool IsHighPose() const;
	float GetRaise() const;
	FVector BodyOffsetFor(const FVector& InFwd) const; // raise + gap for the current move (fades over climb up / lower in)
};
