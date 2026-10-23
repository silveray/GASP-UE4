// Climb / vault on ANY geometry (10-01; UE4 has no GASP traversal blocks or Motion Warping).
//
// Space (outside combat) -> TryTraverse(): capsule sweep ahead (GASP distance 75..350 cm by speed) -> UMidearthLedgeFinder (ported
// from 5.8) finds the front/back ledge on whatever was hit -> action:
//   thin (depth < ThinDepth) and H <= HurdleMaxHeight: back floor ~ start floor -> Hurdle, lower/none -> Vault (falls after), higher -> Mantle
//   else H <= MantleMaxHeight -> Mantle (GASP 1 m clips), H <= MaxHeight -> Climb (GASP 2.5 m clips). Otherwise false -> normal jump.
// Clip by gait (stand < 50 cm/s < walk < 300 < run) and a random foot.
// Own warping (replaces GASP Motion Warping): every GASP traversal clip reaches its top (mantle/climb) or apex (hurdle/vault) at a
// 'contact' time measured from the root track at BeginPlay. The clip starts at the frame whose remaining run-up matches the real
// distance (like GASP's start-frame matching), forward motion is skewed so the root hits Edge + ContactInset at contact, height is
// scaled so the clip's top = the real ledge. The capsule is moved by this component (Flying, root motion ignored, motion matching
// suspended) and handed back at: stand clips' end, moving clips' contact + HandBackDelay, hurdle/vault landing.
// HAND WARP (10-01, user: the hands did not match the block; CVar trav.HandWarp 1): for mantle/climb/vault the clip's hand
// plant (steadiest frame of hand_l/hand_r at the clip ledge height before contact, from the bone tracks) is the alignment point instead of the feet:
// the root is skewed so the planted hand lands HandInset past the real edge, at the clip's palm height above the real top
// (root Z = clip root + (H - clip ledge), floor-clamped), then rises to the top by contact. The real hand position at plant time
// is measured every traversal (GetLastHandDX/DZ, 'Traversal hand' log line).
// HAND IK (10-02, CVar trav.HandIK 1): per hand the clip's longest 'resting on the ledge' window (HandStart..HandEnd); during it
// GetHandIK() gives FAnimNode_MidearthHandIK a world target on the real edge (HandInset past it, the clip's wrist height above the
// top, the clip's sideways hand offset) with alpha faded in/out (HandIKFadeIn/Out). Fixes what the root warp can't: low ledges.
// HANG style (10-11, trav.Style 1 / dev panel Ledges: Hang): a Climb hands over to UHangComponent when the hands land (bHangOnClimb).
// Setup: add to the character, call TryTraverse() from the jump key. Clips: Content/Animations/GASP/Traversal.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TraversalComponent.generated.h"

class ACharacter;
class UAnimMontage;
class UAnimSequence;
class UPrimitiveComponent;

UENUM(BlueprintType)
enum class ETraversalAction : uint8
{
	None,
	Hurdle,
	Vault,
	Mantle,
	Climb,
};

UCLASS(ClassGroup = (Midearth), meta = (BlueprintSpawnableComponent))
class NINJA_PROJECT_API UTraversalComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UTraversalComponent();

	/** GASP traversal clips (soft = no load / no dialog at editor start). Action and gait come from the clip name. */
	UPROPERTY(EditAnywhere, Category = "Traversal")
	TArray<TSoftObjectPtr<UAnimSequence>> Clips;

	/** Lowest ledge (cm above the feet) worth climbing; lower = just walk/step/jump. */
	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float MinHeight = 45.f;

	/** Highest ledge for a mantle (1 m GASP clips scaled); above this the 2.5 m climb clips are used. */
	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float MantleMaxHeight = 175.f;

	/** Highest ledge that can be climbed at all. */
	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float MaxHeight = 300.f;

	/** An obstacle thinner than this (cm) is hurdled/vaulted over instead of climbed onto (GASP CHT depth 0-59). */
	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float ThinDepth = 60.f;

	/** Hurdle/vault only up to this height (1 m GASP clips). */
	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float HurdleMaxHeight = 160.f;

	/** Where the root is at the contact time: this far past the front edge (cm). */
	UPROPERTY(EditAnywhere, Category = "Traversal")
	float ContactInset = 20.f;

	/** Hand IK fade in before / fade out after a hand's resting window (s). */
	UPROPERTY(EditAnywhere, Category = "Traversal|Hand IK", meta = (ClampMin = "0"))
	float HandIKFadeIn = 0.1f;

	UPROPERTY(EditAnywhere, Category = "Traversal|Hand IK", meta = (ClampMin = "0"))
	float HandIKFadeOut = 0.15f;

	/** For FAnimNode_MidearthHandIK. Hand 0 = left, 1 = right. False / alpha 0 = leave that arm alone. */
	bool GetHandIK(int32 Hand, FVector& OutWorldTarget, float& OutAlpha) const;

	/** Hand warp: the planted hand lands this far past the front edge (cm). */
	UPROPERTY(EditAnywhere, Category = "Traversal")
	float HandInset = 8.f;

	/** Moving (walk/run) mantle/climb clips: give control back this long after reaching the top (s). */
	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float HandBackDelay = 0.45f;

	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float BlendIn = 0.15f;

	UPROPERTY(EditAnywhere, Category = "Traversal", meta = (ClampMin = "0"))
	float BlendOut = 0.25f;

	UPROPERTY(EditAnywhere, Category = "Traversal|Debug")
	bool bDebugDraw = false;

	/** Starts a traversal if there is something climbable ahead. False = nothing to do (the caller jumps instead). */
	UFUNCTION(BlueprintCallable, Category = "Traversal")
	bool TryTraverse();

	UFUNCTION(BlueprintPure, Category = "Traversal")
	bool IsTraversing() const { return bActive; }

	// 10-11 HANG style (UHangComponent sets these every tick; trav.Style 0 = all false = the climb behaviour above, unchanged):
	// a Climb (MantleMaxHeight..MaxHeight) runs only until both hands rest on the ledge (+ HangGrabDelay), then HangHandoff takes over.
	// No room check on top in that case (hanging needs none).
	bool bHangOnClimb = false;
	float HangGrabDelay = 0.05f;
	float HangGrabDepth = 0.f;   // hang: root this far under the top (UHangComponent's idle pose); 0 = use the hands' rest time
	TFunction<void(const FVector& Edge, const FVector& Fwd, UPrimitiveComponent* Ledge)> HangHandoff;

	ETraversalAction GetLastAction() const { return LastAction; }
	FString GetLastClipName() const { return LastClipName; }
	float GetLastHeight() const { return LastHeight; }
	/** Measured at the clip's hand-plant time: planted hand past the front edge / above the top (cm). -999 = not measured. */
	float GetLastHandDX() const { return LastHandDX; }
	float GetLastHandDZ() const { return LastHandDZ; }
	/** What the clip itself has (palm above its ledge) - the DZ the warp aims for. */
	float GetLastHandDZTarget() const { return LastHandDZTarget; }

protected:

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:

	struct FClipInfo
	{
		UAnimSequence* Seq = nullptr;
		ETraversalAction Action = ETraversalAction::None;
		int32 Gait = 0;           // 0 stand, 1 walk, 2 run
		float Top = 0.f;          // highest root Z in the clip (cm)
		float ContactTime = 0.f;  // mantle/climb: root reaches the top; hurdle/vault: apex
		float LandTime = -1.f;    // hurdle/vault: root back on the ground after the apex
		bool bHasPlant = false;   // a hand rests near the top before contact (mantle/climb/vault)
		float PlantTime = 0.f;
		FVector PlantHand = FVector::ZeroVector; // character space, like ClipRoot
		FName PlantBone;
		float Ledge = 0.f;        // the clip's own ledge height: Top (mantle/climb), 100 (1 m hurdle/vault bar)
		bool bHand[2] = { false, false };   // per hand (0 left, 1 right): rests on the ledge at some point
		float HandStart[2] = { 0.f, 0.f };  // that resting window (clip time)
		float HandEnd[2] = { 0.f, 0.f };
		FVector HandPos[2] = { FVector::ZeroVector, FVector::ZeroVector }; // resting position (clip character space)
	};

	void AnalyzeClips();
	const FClipInfo* PickClip(ETraversalAction Action, int32 Gait) const;
	FVector ClipRoot(const FClipInfo& Clip, float Time) const; // X = forward, Y = right, Z = up (character space, cm)
	FVector ClipBone(const FClipInfo& Clip, FName Bone, float Time) const; // same space, any bone (component space of the clip)
	FVector WarpedOffset(float Time) const;                    // from the start location, world axes
	void Finish(bool bAborted);
	void HandToHang();

	bool bToHang = false;      // this climb ends in a hang (HangTime)
	float HangTime = 0.f;

	UPROPERTY(Transient)
	TArray<UAnimSequence*> LoadedClips;

	TArray<FClipInfo> ClipInfos;

	ACharacter* Character = nullptr;

	// Active traversal.
	bool bActive = false;
	const FClipInfo* Active = nullptr;
	UAnimMontage* Montage = nullptr;
	TWeakObjectPtr<UPrimitiveComponent> Obstacle;
	FVector StartLocation = FVector::ZeroVector;
	FVector Forward = FVector::ForwardVector;
	float StartTime = 0.f;     // clip time the montage starts at
	float EndTime = 0.f;       // clip time control is handed back
	float Elapsed = 0.f;
	float DistToContact = 0.f; // real forward distance start -> contact point
	float ZScale = 1.f;
	float ContactZ = 0.f;      // real height of the root at contact above the start feet
	bool bHandWarp = false;    // this traversal aligns the hand plant
	float PlantRootFwd = 0.f;  // hand warp: root forward distance from the start at PlantTime
	float PlantRootZ = 0.f;    // hand warp: root height at PlantTime
	FVector EdgePoint = FVector::ZeroVector;
	bool bHandMeasured = false;
	float LastHandDX = -999.f;
	float LastHandDZ = -999.f;
	float LastHandDZTarget = 0.f;
	float ClipTimeNow = 0.f;   // clip time of the running traversal (for GetHandIK)
	bool bHandIK = false;
	FVector HandTarget[2] = { FVector::ZeroVector, FVector::ZeroVector }; // world

	ETraversalAction LastAction = ETraversalAction::None;
	FString LastClipName;
	float LastHeight = 0.f;
};
