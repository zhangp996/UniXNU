# AGENTS.md

## Responsibility

Keep this repository a valid UniXNU device repository for the OnePlus 8 Pro (SM8250, Adreno 650).
Follow https://unixnu.com/zh/porting/steps and the driver guides; the GPU work follows
https://unixnu.com/zh/porting/gpu, the driver interface and build integration follow
https://unixnu.com/zh/porting/drivers#build-integration.

## Rules

- board.toml, feeds.toml, configs, dts, proprietary-files.txt and porting.toml describe this board only.
- The A650 backend and its kits are a companion component; reference them by pinned source, do not
  copy third-party device-port trees.
- Every capability added to board.toml needs a recipe (scripts/), the matching archive in
  [kernel].archives, the configuration fragment, and a porting.toml record with evidence.
- Keep the ordered archive list complete: it replaces, not extends, inherited values.
- Record device observations separately from host build results, with kernel/payload hashes.
