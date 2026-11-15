# r2-prelink

The qualified prelink candidate the device display builders audit:

- `native-owner-mobile-graphics-candidate.macho` (124727296 bytes) split into parts
- `native-owner-mobile-graphics-candidate.json` (16981306 bytes)

Both names used by the builders refer to the same image:
`native-owner-mobile-graphics-candidate.*` and
`native-owner-ufs-cpu-desktop-mobile-graphics-candidate.*`.

`tools/join-r2-prelink.sh <tree>` joins them into
`<tree>/build/xnu-prelink/` and verifies SHA256SUMS. The CI workflow runs that step before
the kernel build; a fresh tree has no candidate of its own.

The candidate is assembled from the original Apple kernelcache, so it is never uploaded as an
image: only the text logs and these parts are published.
