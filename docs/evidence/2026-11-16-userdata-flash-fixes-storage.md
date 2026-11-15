# 2026-11-16: userdata reflash fixes the storage EINVAL/panic

The failures recorded in 2026-11-16-r2-baseline-boot.log came from a stale userdata partition
whose SHNGROOT windows no longer matched the IPSW, not from the kernel or payload.

Rebuild and flash per https://unixnu.com/zh/concepts/userdata:

    cd UniXNU && python3 ./unixnu build userdata -D ../UniXNU-instantnoodlep
    fastboot flash userdata userdata-root.img   # sparse form works around the Windows bad_alloc

userdata-root.img sha256: fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093

Booted the committed original bootEU.img after the flash and captured 600 s of COM14 logs
(2026-11-16-r2-after-userdata-flash.log, 874455 bytes at the snapshot):

- invalid argument: 0 (was 56)
- I/O timeout: 0
- QCOM PANIC: 0 (was 1)
- SpringBoard: 97
- DISPLAY present: 9, BACKLIGHT: 5
