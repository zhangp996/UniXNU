
from pathlib import Path
import subprocess

tree = Path("/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree/drivers/GopFramebuffer/NativeLegacyDisplay.cpp")
live = tree.read_text()

# reconstruct the un-instrumented component source: drop the DPUDBG lines and restore the frames line
lines = []
for line in live.splitlines(keepends=True):
    if "DPUDBG" in line:
        continue
    if 'IOLog("DISPLAY frames=%u direct flip=' in line:
        lines.append('if((consoleFrames%128)==0)IOLog("DISPLAY frames=%u\\n",consoleFrames);\n')
        continue
    lines.append(line)
orig = "".join(lines)
Path("/tmp/gop-original.cpp").write_text(orig)

patch_dir = Path("/home/dev/unixnu/full-project/r2/device/patches/display")
patch_dir.mkdir(parents=True, exist_ok=True)
out = subprocess.run(["diff", "-u",
                      "--label", "a/drivers/GopFramebuffer/NativeLegacyDisplay.cpp",
                      "--label", "b/drivers/GopFramebuffer/NativeLegacyDisplay.cpp",
                      "/tmp/gop-original.cpp", str(tree)],
                     capture_output=True, text=True).stdout
(patch_dir / "zzz-gop-dpu-diagnostics.patch").write_text(out)
print("patch bytes", len(out))
print(out[:1200])
