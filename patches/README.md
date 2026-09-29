# Patches against upstream BTT

Unified diffs from upstream
[Beat-and-Tempo-Tracking](https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking)
commit `c039090` to the vendored copies in `firmware/audioclock_5/src/`.

| File | Patches |
|---|---|
| `DFT.c.patch` | 1: magnitude-only rect-to-polar. 2: `sqrtf` fast path. |
| `BTT.c.patch` | 3: tempo decimation. 4: sub-bin tempo interpolation. |

These are here for review and for re-basing onto a newer upstream. You don't
need to apply them to build, because the sketch folder already contains the
patched files. For what each patch does and why, see
[docs/btt-patches.md](../docs/btt-patches.md).

To re-apply onto a fresh upstream checkout:

```sh
git clone https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking.git upstream
cd upstream
patch -p1 < ../patches/DFT.c.patch   # labels are upstream/src/…
patch -p1 < ../patches/BTT.c.patch
```
