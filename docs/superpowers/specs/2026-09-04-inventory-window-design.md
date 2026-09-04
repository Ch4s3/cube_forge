# Inventory window with drag-and-drop

`I` opens a modal inventory: a 27-slot backpack above the 9 hotbar slots, drawn
over the dimmed world. Mouselook releases, a cursor appears, and stacks are
moved by pressing on a slot, dragging, and releasing on another. `I` or `Esc`
closes.

## Why the data model has to change first

Today a slot's *position is* its identity. `Inv` is a flat 14-field variant with
one `Int` per block type, and `block_of_slot(4)` returns water because it is
written that way:

```march
fn block_of_slot(slot : Int) : Int do
  if slot == 4 do C.water() else ... end
end
```

Drag-and-drop cannot be built on that: a slot has to hold an (item, count) pair.
The replacement is an array:

```march
type Inv = Inv(NativeIntArr, Int, Float, Int, Int, Int)
--            slots,        sel, progress, target x, y, z
```

72 ints, two per slot: item id then count. Hotbar occupies slots 0-8, backpack
9-35. `block_of_slot` and `slot_of_block` are deleted; a slot's item is read
from the array.

This is the right shape independent of the feature. A 36-slot inventory as a
flat variant would be 72 fields, and the 14-field version is already awkward —
every `add` rewrites all fourteen. It also lands where March just got faster:
since `bfb16dac`, `NativeArray` reads borrow their array, so scanning slots
costs no refcount traffic.

## Rules

**Stacks are unbounded.** A slot holds any number of one item. This is a
deliberate call with a known consequence: with 9 item types and 9 hotbar slots,
**the backpack never fills on its own**. It exists so you can move items off the
hotbar and keep it to a chosen loadout, and it starts mattering when there are
more item types than hotbar slots.

**Pickup** on breaking a block, in order: a hotbar slot already holding that
item; else the first empty hotbar slot; else the same two rules over the
backpack; else the block is lost. Mining stays playable without opening the
window.

**Drag** is press-move-release, not click-to-pick-then-click-to-place. Pressing
on a non-empty slot lifts its whole stack onto the cursor and the slot renders
empty. Releasing over a slot places it; if that slot is occupied, the two swap.
Releasing anywhere else — outside the panel, on the world, on the dimmed
background — returns the stack to the slot it came from. Nothing is ever
destroyed and nothing is dropped into the world.

**While open** the world keeps ticking, so water still flows, but no mouse or
keyboard input reaches the player: no look, no movement, no breaking, no
placing. This matches how the map view already behaves and avoids introducing a
pause.

## Units, the one trap worth naming

GLFW reports the cursor in **window** coordinates. `cf_win_fb_w` returns
**framebuffer** pixels, and on this display they differ by 2x. Hit-testing
against the wrong one lands half a screen away and reads like a layout bug
rather than a units bug. The shim exposes the cursor already converted to
framebuffer pixels, so March only ever sees one coordinate system.

## Shim additions

- `cf_in_mouse_x` / `cf_in_mouse_y` — the position is already tracked in `g_in`,
  just never exposed. Returned in framebuffer pixels, per above.
- `cf_in_button_released` — a released-edge to end a drag, mirroring the
  existing `cf_in_button_pressed`.

No new GL state. The panel reuses the existing HUD quad builders and two free
buffer slots (248 background and text, 247 the dragged stack), drawn after the
hotbar so the dragged stack is on top.

## Layout

While the panel is open the hotbar is drawn as its **top row** and the strip at
the bottom of the screen is hidden. Everything is then one contiguous four-row
grid, so moving a stack between the backpack and the hotbar is a drag of one row
rather than a reach to the bottom of the screen. An extra gap under the top row
keeps the active slots reading as their own group.

The backpack is 9 columns by 3 rows sharing the hotbar's 0.125 NDC pitch, so
every column lines up top to bottom and a backpack slot sits directly under the
hotbar slot it drags into. Closed, the hotbar returns to the bottom strip
unchanged. The panel is a dark translucent rect behind the grid, with the world
dimmed under it.

## Testing

The logic is pure and runs without a window:

- **Pickup routing** — into a matching slot, into an empty one, spilling to the
  backpack when the hotbar is full, and dropping the item when everything is
  full.
- **Move and swap** — moving to an empty slot, swapping two occupied ones, and
  returning to origin on a release that hits nothing.
- **Conservation** — the property that would silently duplicate or delete items:
  no sequence of moves changes the total count of any item id. Worth a
  randomised sequence rather than a handful of cases.
- **Hit-testing** — known cursor positions map to the expected slot index,
  including positions in the gaps between slots (which must resolve to no slot,
  not to a neighbour) and under a 2x framebuffer scale.
