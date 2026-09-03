# Hotbar, inventory, break delay — design

## Inventory
- `Inv(grass, dirt, stone, sel)`: counts for the three solid blocks, selected
  slot 1–4 (4 = water, unlimited). Start empty.
- Breaking grass/dirt/stone adds one of that type; breaking any water cell
  adds nothing. Placing a solid needs count > 0 and decrements; placing water
  is free. Keys 1–4 select; the previous "place stone" default is gone.

## Break delay
- Holding the left button on a targeted block accumulates progress at
  1 / 0.4 s; it resets when the targeted block changes or the button is
  released; at 1.0 the block breaks (edit + inventory). Right click places
  immediately.

## UI (option B: textured icons)
- Texture array grows to 4 layers: 0 grass checker, 1 water, 2 dirt, 3 stone;
  the mesher uses them per block id (a visual upgrade for the world too).
- Two HUD buffers, rebuilt in place only when a state key changes:
  - untextured (slot 252): 4 slot backgrounds bottom-centre, a bright border
    on the selected slot, count digits (3x5 font) in solid slots, a crosshair,
    and a progress bar under the crosshair while breaking;
  - textured (slot 251): one icon quad per slot sampling the block's layer.
- Vertex colour in the untextured path is `(u, v, layer) * shade`, so slot
  and border colours are plain RGB per vertex.

## Verification
- Tests: inventory add/remove rules, break progress reset on target change.
- `CF_AUTOEDIT=<f>`: hold break from f for 60 frames (the block must NOT be
  gone before 0.4 s and must be gone after), then place the harvested block
  at f+120 (count goes to 0), then water at f+180. Prints the inventory.
  Frame dump shows hotbar, icons, crosshair.
- Per-frame allocation stays at 1 outside HUD rebuilds.
