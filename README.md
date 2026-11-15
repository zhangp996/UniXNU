# UniXNU R2 baseline (OnePlus 8 Pro / instantnoodlep)

This branch is the working R2 baseline: the tree and device repository that produced the validated
bootEU.img (old R2 kernel + old A650 component, boots into iPadOS Setup/SpringBoard on the OnePlus 8 Pro).

Layout:

- r2/core: R2 UniXNU core sources (out/ and build/ excluded)
- r2/device: R2 device repository (UniXNU-instantnoodlep)
- r2/reference: A650 reference component used by the R2 build (Adreno650 + kits)
- r2/uefi: device UEFI overlay (BootShim, instantnoodlepPkg, config, DTB); the Mu-Silicium base is fetched by CI
- artifacts/original: validated bootEU.img and its embedded kernel/payload, split into 48 MiB parts
- tools/r2: R2 build wrappers (kernel, payload, boot packing)
- docs/MISSING-FILES.md: files the baseline does not carry and CI obtains

Reassemble and verify the artifacts:

    sh tools/join-artifacts.sh

Build (Linux host, after placing the local inputs described in docs/MISSING-FILES.md):

    bash tools/r2/build_kernel.sh
    bash tools/r2/build_payload.sh

The previous fresh-core attempt is preserved on the fresh-core branch.

## userdata (required before booting)

The root and SystemOS images live in the userdata partition. Rebuild and flash it after any IPSW or
layout change, otherwise the stale SHNGROOT windows make the storage driver return EINVAL and the
kernel panics in pmap_assert_free under load:

    python3 ./unixnu build userdata -D ../UniXNU-instantnoodlep
    fastboot flash userdata <out>/userdata-root.img

The sparse form (img2simg) is needed on Windows fastboot for images above ~4 GiB.