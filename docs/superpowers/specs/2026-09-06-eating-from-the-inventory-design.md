# Eating from the inventory — design

A mushroom cap is food: eating one starts a thirty-second effect by species
(`2026-09-05-fungus-save-fields-and-food`, `CubeForge.Effects`). Today there is
exactly one way to do it — hold a cap in the selected hotbar slot, aim at
**nothing**, and right-click — and it is the worst of both worlds. It is
undiscoverable, it fails whenever the ground is within reach, and it silently
eats a cap when you meant to place a block and happened to be facing the sky.

This gives eating two deliberate gestures and removes the accidental one.

## 1. One rule for what is food

`Inventory.edible(id) : Bool` — true for `Chunk.is_fruit_cap(id)`, false for
everything else. Stems stay building blocks and small mushrooms still yield
nothing when broken; neither is in scope.

Both gestures ask this one function, so they cannot disagree about what a bite
is. It also gives the tests a single thing to pin.

## 2. The eat key

`E` (GLFW 69, unused today) eats one from the **selected hotbar slot** when it
holds an edible item, whatever the player is aiming at. Pressed on an empty or
inedible slot it does nothing at all: no bite, no message, no consumed item.

It is read where the other play-mode keys are read, so it is inert while the
inventory window or the escape menu is open — the window has its own gesture
(§3), and a key that ate through a menu would be its own trap.

## 3. Right-click a slot in the open inventory

While the inventory window is open, **right-click on any slot** — hotbar or
backpack — eats one from that slot. The slot under the cursor is the one
`Hud.slot_at` already reports for drag and drop.

Left-click keeps its meaning: lift a stack, drop it on another slot. Right
click does nothing in the window today, because `interact` is skipped entirely
while a panel is open, so there is nothing to collide with.

This is the half that makes a full backpack usable. Caps accumulate there, and
today they must be dragged into the hotbar before they can be eaten at all.

**A right-click on an empty or inedible slot does nothing**, like the key.

## 4. What a bite does is unchanged

One item consumed from the slot it was eaten from, and
`Effects.apply(Effects.kind_for(sp), now)` — thirty seconds, refreshing rather
than stacking. The heads-up display already shows the active effects with the
seconds left, so a bite is visible without new UI. The existing stdout line
(`ate a Frostcap cap: JUMP 30`) stays: it is what the headless runs read.

Eating from the backpack needs one new inventory function,
`Inventory.consume_at(i, slot)` — `consume` today is hard-wired to the selected
hotbar slot.

## 5. The old gesture goes

The `place_now && !Ray.is_hit(hit) && is_fruit_cap(...)` branch in `interact`
is removed. Right-clicking at the sky with a cap in hand now does what
right-clicking at the sky with anything else in hand does: nothing.

## 6. Verification

`CF_AUTOEAT=<frame>` currently calls `eat` directly, which tests the effect and
nothing about the gesture. It becomes a scripted **key press** on the frame,
and `CF_AUTOEAT_SLOT=<slot>` scripts the inventory right-click on that slot
instead, so both real paths are exercised headless.

Tests:

- `edible` is true for every species' cap and false for stems, small
  mushrooms, spores, blocks and 0.
- Eating from a backpack slot consumes from **that** slot and leaves the
  selected hotbar slot alone.
- Eating the last item of a slot empties it.
- Eating an inedible or empty slot changes neither the inventory nor the
  effects.
- The species-to-effect mapping is already covered by `effects_test`.

## 7. Not in scope

Hunger or health, so eating is still only a buff. Eating anything that is not
a cap. A dedicated food UI. Stack-splitting or eating a whole stack at once.
