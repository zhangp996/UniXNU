# 2026-11-16 hybrid boot evidence (old R2 kernel + newly adapted A650 GPU)

Boot image spliced into the validated fastboot skeleton and booted on the OnePlus 8 Pro.

| Artifact | SHA-256 / size |
| --- | --- |
| R2 kernel rebuilt with the new A650 component | e134e9cfb05a36250ad428bfef48ec1808ec28c39140fc2d871867646d61a194 |
| AMFI-patched kernel used in the payload | bc1f72eb4a875ff57d1f6d55d7b4d5dfbdbbe053ffd42568deea3a2b7f72d745 |
| payload.img | 7b76d478dd60f6c5a547d4440df963157cb4daed4759d56f969684d36484da5a (184549376 bytes) |
| boot-fresh7.img | f939c9e23d3bd77432b42bfac02f8a01b36b4876c8f13950b22837af9d40f223 (199782400 bytes) |
| serial log (this directory) | 2026-11-16-hybrid-boot.log (337647 bytes) |

Serial console: COM14 (VID_1209&PID_316D), 0 panics in the capture.

Key observations from the log:
- APFS mounted disk1s1/disk5s1; launchd, SpringBoard, backboardd, mediaserverd and WidgetRenderer running.
- "DISPLAY: adopted default surface sid=1"; "DISPLAY: present copied 2376 rows x 4320 bytes"; transfer rc=0x0.
- "GPU56 mode9 result status=0 routes=4 fence=0 seen=2 marker=a6500c0d fault=0".
- GPUDIAG: SpringBoard/backboardd/mediaserverd open the accelerator service; hundreds of calls, hundreds of tunnel methods.
