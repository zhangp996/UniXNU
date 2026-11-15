# Original bootable image (validated)

This directory carries the original bootEU.img that boots the OnePlus 8 Pro into iPadOS
Setup/SpringBoard with the old R2 kernel and the old A650 component (the control image used
for docs/evidence/2026-11-16-hybrid-boot.md).

The file is split into 48 MiB parts so it can live in git without exceeding the per-file limit.

Reassemble and verify:

    sh tools/join-artifacts.sh

The macOS-only deliverables (Metal addon/plugin, and any hdiutil-based seed growth) are
produced by the GitHub Actions workflow .github/workflows/macos-artifacts.yml; download them
from the run artifacts.
