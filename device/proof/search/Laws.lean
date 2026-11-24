module
public import Vram.Main
import Std
public section
namespace Vram.SearchLaws
open Vram.Main

theorem publication (old new : Nat) (exact : Bool) :
    search (.replace old new exact) = true ↔ exact = true ∧ new < old := by
  simp [search, Nat.blt]
  omega

theorem probe_on_expiration (t : Bool) : schedule true 0 t = .probe := by rfl

theorem credits_decrease (n : Nat) : advance false (n+1) = n := by
  change (n+1)-1=n
  omega

#print axioms publication
#print axioms probe_on_expiration
#print axioms credits_decrease
end Vram.SearchLaws
