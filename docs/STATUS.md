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
