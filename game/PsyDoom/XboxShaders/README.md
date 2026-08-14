# NV2A shaders for the hardware present

`vs.inl` is a compiled NV2A vertex program: a passthrough that takes a position already in clip space and a texture
coordinate, and hands both straight to the rasteriser. Two instructions. It is all a fullscreen quad needs, because
there is nothing to transform - the quad is the screen.

It is checked in rather than built, because it will not change and building it needs a toolchain that took some
assembling.

## Rebuilding it, if it ever needs to change

Two stages: Cg to vp20 assembly, then vp20 assembly to NV2A microcode.

```
$NXDK_DIR/tools/cg/win/cgc.exe -profile vp20 -o vs.tmp vs.vs.cg
vp20compiler.exe vs.tmp > vs.inl
```

`cgc.exe` ships with nxdk for Windows and works as-is. `vp20compiler` does not: nxdk carries the source but it will
not build against a current GCC. It needs the fixes listed below before it will build.

What it needed:

- `#include <assert.h>` and `#include <string.h>` at the top of `nvvertparse.c`, which older compilers supplied by
  accident through other headers.
- Building through a login shell (`bash -lc`). A plain non-login shell has no include paths set, and the failure it
  gives is `no include path in which to search for stdio.h`, which reads like a missing header rather than a missing
  environment.

## What is deliberately not here

There is no pixel shader. `fp20compiler` needs flex to generate its parsers and does not build out of the box, and a
plain textured quad has no use for one - the register combiners can pass the texture straight through, which is all
this needs. If a pixel shader is ever genuinely wanted, that is the point at which to sort out flex.

## GameCube

None of this ports. NV2A microcode is Xbox only. The GameCube equivalent is GX, where a screen-space textured quad
needs no shader at all.
