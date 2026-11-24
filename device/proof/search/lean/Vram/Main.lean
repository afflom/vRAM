module
public import Init
public import LexLeanRuntime.Portable
set_option autoImplicit false
set_option maxRecDepth 100000
set_option maxHeartbeats 1000000000
set_option linter.constructorNameAsVariable false
namespace Vram.Main

public inductive Request where
  | explore (_ : Nat) (_ : Nat)
  | split (_ : Nat) (_ : Nat)
  | replace (_ : Nat) (_ : Nat) (_ : Bool)
  | factor (_ : Nat) (_ : Nat)

@[expose] public def search (request : Request) : Bool := (match request with | Request.explore incumbent nodeBudget => ((Nat.blt (256) (incumbent)) && (Nat.blt (0) (nodeBudget))) | Request.split extent remaining => ((Nat.blt (16) (extent)) && (Nat.blt (0) (remaining))) | Request.replace incumbent challenger exact => (exact && (Nat.blt (challenger) (incumbent))) | Request.factor classes axis => ((Nat.blt (0) (classes)) && ((Nat.blt (classes) (axis)) && (Nat.ble (classes) (16)))))

public theorem rejectUnchecked (a : Nat) (b : Nat) : (search (Request.replace (a) (b) (false)) = false) := by
  rfl

public theorem noSplitWithoutBudget : (search (Request.split (64) (0)) = false) := by
  rfl

public theorem noWideCodebook : (search (Request.factor (17) (64)) = false) := by
  rfl

public inductive Job where
  | probe
  | refit
  | skip

@[expose] public def schedule (adaptive : Bool) (credits : Nat) (template : Bool) : Job := (if adaptive then (if (Nat.beq (credits) (0)) then Job.probe else (if template then Job.refit else Job.skip)) else Job.probe)

@[expose] public def advance (probed : Bool) (credits : Nat) : Nat := (if probed then 15 else (LexLeanRuntime.subtract (credits) (1) : Nat))

public theorem revisitAfterCreditsExpire (template : Bool) : (schedule (true) (0) (template) = Job.probe) := by
  rfl

public theorem fixedSearchAlwaysProbes (credits : Nat) (template : Bool) : (schedule (false) (credits) (template) = Job.probe) := by
  rfl

end Vram.Main
