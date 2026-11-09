# opengx `ut99` branch: changes against upstream

Base: devkitPro/opengx `23c12e6` (v0.17.0-1). Every change is a separate commit on `ut99`, written so it could be
sent upstream on its own. Licence unchanged (BSD-3-Clause, see the file headers).

## Texture environment

### `GL_RGB_SCALE` / `GL_ALPHA_SCALE` (1, 2, 4)
- `glTexEnv{if}(GL_TEXTURE_ENV, GL_RGB_SCALE|GL_ALPHA_SCALE, v)` is stored per texture unit; values other than
  1, 2, 4 give `GL_INVALID_VALUE` (as the spec requires).
- GX mechanism: the TEV stage that computes the unit's `GL_COMBINE` result gets the scale as its output scale
  (`GX_SetTevColorOp`/`GX_SetTevAlphaOp` `GX_CS_SCALE_1/2/4`). The TEV scales before clamping, the same order
  as OpenGL. Like the spec, the scale only applies in `GL_COMBINE` mode.
- Bug fix found on the way: `setup_texture_stage()` tested `if (!tu->mode != GL_COMBINE)`, which is always true,
  so the generic `GX_TEV_ADD`/`GX_TB_ZERO` op overwrote what `setup_combine_operation()` had set. That broke
  `GL_SUBTRACT` (ran as ADD) and `GL_ADD_SIGNED` (lost its -0.5 bias), and would have dropped the new scale.
