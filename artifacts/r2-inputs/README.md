# R2 baseline payload inputs

These are the exact inputs of the validated bootEU.img payload (extracted from its FAT image and
from the R2 output tree). They let a Linux host rebuild the validated payload without any
macOS-only step; the Metal plugin/Turnip pieces are produced by CI instead.

| File | Size | SHA-256 | Meaning |
| --- | --- | --- | --- |
| kernel.macho.adt | 14048 | ba17a4b3b70bafb8273d1a9865a6dedfe2a60609c35ef735eeb60d33b4bc7695 | ADT of the validated payload (use this one for stable payloads) |
| adt-generated-differs.dtb | 14048 | f11da7016253730c852958cd4b0c86a65129b227c46e8df543685de2f5096d0d | the ADT our board DTS currently generates (differs; candidate cause of storage EINVAL) |
| kernel.macho.trustcache | 61432 | b440aad2e2a4b5335256e048d28b1776853f2f036842251af2afe7333f7be589 | original merged trust cache (owner trust bundle) |
| owner-trust.bundle | 61432 | b440aad2e2a4b5335256e048d28b1776853f2f036842251af2afe7333f7be589 | same trust cache as produced by our build |
| scratch-data.img | 1196160 | 414a8465c7051b5889278e6dbb49b4c617c09347476b437c32be9f1a4c47bc66 | original Data seed embedded in the payload |
| kernel.macho.gpuaddon | 773232 | a3c869222ec9cd1b19d821e848eb51d7ddf870d569ee0efb07c76277351f744a | original GPU addon embedded in the payload |
| gpu-addon-merged.bin | 773232 | a3c869222ec9cd1b19d821e848eb51d7ddf870d569ee0efb07c76277351f744a | same addon as built here (no Metal plugin inside) |
| xpc-addon.bin / .json | 773152 | 00af5218869c6cd073f17bab78a1a6dd273a8579a2adc906e654f94600de1b9d | launchd cache addon merged into the GPU addon |
| unixnu.toml | 1339 | 22f81ae00b2c315de3946149d1bfae37b88f88724ef6893bea35df32b6d68941 | payload metadata of the validated image |
| payload-reference.json | 1741 | 6ebe563e90d9e7f26493f9d8c4dc506645d23837f02636b5c4aebc695b7fdba2 | payload report (hashes and kernel args) |
| data-seed.dbr / owner-data-*.dbr | - | see SHA256SUMS | alternative Data seeds from the R2 output tree |

Verify with: cd artifacts/r2-inputs && sha256sum -c SHA256SUMS
