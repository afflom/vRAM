module
public import Vram.Main
import Std

public section
namespace Vram.Laws
open Vram.Main

theorem contractWord_bit (a b mask : UInt64) (i : Nat) :
    (contractWord a b mask).toBitVec.getLsbD i =
      (if mask.toBitVec.getLsbD i then b.toBitVec.getLsbD i else a.toBitVec.getLsbD i) := by
  change (a ^^^ ((a ^^^ b) &&& mask)).toBitVec.getLsbD i = _
  simp only [UInt64.toBitVec_xor, UInt64.toBitVec_and, BitVec.getLsbD_xor, BitVec.getLsbD_and]
  cases a.toBitVec.getLsbD i <;> cases b.toBitVec.getLsbD i <;> cases mask.toBitVec.getLsbD i <;> decide

theorem contractWord_oneHot (a b mask : UInt64) (i : Nat) :
    ((contractWord a b mask).toBitVec.getLsbD i).toNat =
      (a.toBitVec.getLsbD i).toNat * (!mask.toBitVec.getLsbD i).toNat +
      (b.toBitVec.getLsbD i).toNat * (mask.toBitVec.getLsbD i).toNat := by
  rw [contractWord_bit]
  cases mask.toBitVec.getLsbD i <;> simp

def charged (s p m e : Nat) : Representation → Nat
  | .fill => 0 | .sparse => s | .palette => p | .raw => 4096 | .selector => m | .corrected => e

theorem chosen_is_minimum (s p m e : Nat) :
    let cost := charged s p m e (choose false s p m e)
    cost ≤ 4096 ∧ cost ≤ s ∧ cost ≤ p ∧ cost ≤ m ∧ cost ≤ e := by
  dsimp only
  simp only [choose, Bool.false_eq_true, if_false]
  split <;> (try split) <;> (try split) <;> (try split) <;>
    simp_all [charged, Nat.blt, Nat.ble] <;> omega

theorem cannot_discard_dirty_without_capture :
    step (.reclaim true true) = .capture ∧ step (.captured false) = .retain := ⟨rfl, rfl⟩

#print axioms contractWord_bit
#print axioms contractWord_oneHot
#print axioms chosen_is_minimum
#print axioms cannot_discard_dirty_without_capture
end Vram.Laws
