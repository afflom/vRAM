module
public import Init
public import LexLeanRuntime.Portable
set_option autoImplicit false
set_option maxRecDepth 100000
set_option maxHeartbeats 1000000000
set_option linter.constructorNameAsVariable false
namespace Vram.Main

public inductive Representation where
  | fill
  | sparse
  | palette
  | raw
  | selector
  | corrected

@[expose] public def choose (uniform : Bool) (sparse : Nat) (palette : Nat) (model : Nat) (corrected : Nat) : Representation := (if uniform then Representation.fill else (if ((Nat.blt (model) (4096)) && ((Nat.ble (model) (corrected)) && ((Nat.blt (model) (sparse)) && (Nat.blt (model) (palette))))) then Representation.selector else (if ((Nat.blt (corrected) (4096)) && ((Nat.blt (corrected) (sparse)) && (Nat.blt (corrected) (palette)))) then Representation.corrected else (if ((Nat.blt (sparse) (4096)) && (Nat.blt (sparse) (palette))) then Representation.sparse else (if (Nat.blt (palette) (4096)) then Representation.palette else Representation.raw)))))

public inductive Event where
  | fault (_ : Bool) (_ : Bool) (_ : Bool)
  | reclaim (_ : Bool) (_ : Bool)
  | captured (_ : Bool)
  | discarded (_ : Bool)
  | budget (_ : Nat) (_ : Nat)
  | permission (_ : Bool) (_ : Bool) (_ : Bool) (_ : Bool) (_ : Bool) (_ : Bool)

public inductive Action where
  | skip
  | restoreClean
  | restoreDirty
  | markWritable
  | capture
  | discard
  | releaseFrame
  | retain
  | trim
  | allow

@[expose] public def step (event : Event) : Action := (match event with | Event.fault protectedWrite writes tracking => (if protectedWrite then Action.markWritable else (if tracking then (if writes then Action.restoreDirty else Action.restoreClean) else Action.restoreDirty)) | Event.reclaim present dirty => (if present then (if dirty then Action.capture else Action.discard) else Action.skip) | Event.captured success => (if success then Action.discard else Action.retain) | Event.discarded success => (if success then Action.releaseFrame else Action.retain) | Event.budget limit live => (if ((Nat.blt (0) (limit)) && (Nat.blt (limit) (live))) then Action.trim else Action.skip) | Event.permission opted quiescent tcg kvm captureAll migrationIdle => (if ((opted && quiescent) && ((tcg || (kvm && captureAll)) && migrationIdle)) then Action.allow else Action.skip))

@[expose] public def rangeAllowed (offset : Nat) (length : Nat) (extent : Nat) : Bool := ((Nat.blt (0) (length)) && ((Nat.beq ((LexLeanRuntime.remainder (offset) (4096) (0) : Nat)) (0)) && ((Nat.beq ((LexLeanRuntime.remainder (length) (4096) (0) : Nat)) (0)) && ((Nat.ble (offset) (extent)) && (Nat.ble (length) ((LexLeanRuntime.subtract (extent) (offset) : Nat)))))))

@[expose] public def contractWord (left : UInt64) (right : UInt64) (mask : UInt64) : UInt64 := (LexLeanRuntime.bitXor (left) ((LexLeanRuntime.bitAnd ((LexLeanRuntime.bitXor (left) (right) : UInt64)) (mask) : UInt64)) : UInt64)

public theorem uniformAlwaysFits (a : Nat) (b : Nat) (c : Nat) (d : Nat) : (choose (true) (a) (b) (c) (d) = Representation.fill) := by
  rfl

public theorem noDiscardOnFailedCapture : (step (Event.captured (false)) = Action.retain) := by
  rfl

public theorem noReleaseOnFailedDiscard : (step (Event.discarded (false)) = Action.retain) := by
  rfl

public theorem cleanBackedPage : (step (Event.reclaim (true) (false)) = Action.discard) := by
  rfl

public theorem noTrackingIsDirty (writes : Bool) : (step (Event.fault (false) (writes) (false)) = Action.restoreDirty) := by
  rfl

public theorem allCandidatesTooLarge : (choose (false) (4096) (4096) (4096) (4096) = Representation.raw) := by
  rfl

public theorem retainSparseChanges : (choose (false) (4096) (4096) (4096) (164) = Representation.corrected) := by
  rfl

public theorem outOfRange : (rangeAllowed (8192) (4096) (4096) = false) := by
  rfl

public theorem kvmCaptureAll : (step (Event.permission (true) (true) (false) (true) (true) (true)) = Action.allow) := by
  rfl

public theorem denyKvmWithUntrackedWrites : (step (Event.permission (true) (true) (false) (true) (false) (true)) = Action.skip) := by
  rfl

end Vram.Main
