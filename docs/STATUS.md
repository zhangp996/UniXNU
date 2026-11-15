# Porting status - R2 baseline

Repository content switched back to the R2 baseline on 2026-11-16 (the fresh-core attempt is on the
fresh-core branch).

## Validated baseline

| Artifact | SHA-256 | Notes |
| --- | --- | --- |
| bootEU.img (199,782,400 B) | e6386a23c3f1815f0fbc98029d1f99f60f9dd0e2352d699e52ea9dde801f6ae1 | boots to iPadOS Setup/SpringBoard; 480 s control run: 0 storage errors, 0 panics |
| embedded kernel.macho (125,304,832 B) | e9db497221428e6f830407a11adae18b4cba7b470fbfeda1762b657b3ce46020 | AMFI-patched at 0x6060d04 |
| embedded payload.img (184,549,376 B) | 384501274ab06458280fda43b5c73dae15519204aaa9588190023c386eed82ad | original payload files |

## Open issue

Payloads rebuilt with the current R2 wrappers reproduce storage EINVAL errors under load and a pmap
panic, even with the original kernel; the only file differing from the validated payload was
kernel.macho.adt. Next step: rebuild payloads with the original ADT (or fix the device tree) before
switching back to the A650 work.

## R2 baseline boot test (2026-11-16)

Booted the committed original bootEU.img on the OnePlus 8 Pro and captured 480 s of COM14 logs
(docs/evidence/2026-11-16-r2-baseline-boot.log, 761,343 bytes).

What works: display adoption and 2376-row presents, backlight service, A650 GPU (GPU56 mode9),
SpringBoard/backboardd/Setup running, AKS keyevents stage 3.

What fails after roughly ten minutes under Setup/iCloud I/O load: disk3 (Cache) and disk5 (Root)
return I/O timeout then EINVAL (error 22) on block reads, APFS reports them, Setup exits, and the
kernel finally panics in pmap_assert_free with a leftover User CPU mapping (pmap.c:2907).
This is pre-existing R2 storage-stack behaviour, not caused by the fresh rebuild: the same image
and the rebuilt kernel both show it, and the earlier 480 s control stopped before the failure window.