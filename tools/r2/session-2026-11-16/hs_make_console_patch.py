from pathlib import Path
import shutil, subprocess

T = Path("/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree/.cache/xnu-work")
W = Path("/tmp/console-off")
if W.exists(): shutil.rmtree(W)
(W/"a").mkdir(parents=True); (W/"b").mkdir(parents=True)
files = ["osfmk/arm/arm_init.c", "osfmk/console/video_console.c"]
for f in files:
    dst = W/"a"/f
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(T/f, dst)

# arm_init.c: set the console-write early-exit flag once the video console exists
src = (W/"a"/"osfmk/arm/arm_init.c").read_text()
old = "\t/* setup console output */\n\tPE_init_printf(FALSE);\n"
new = (
    "\t/* setup console output */\n"
    "\tPE_init_printf(FALSE);\n"
    "\n"
    "\t/*\n"
    "\t * The panel is this board console: once the video console is registered the kernel log is\n"
    "\t * drawn over the UI. disableConsoleOutput is the early exit of console_write (the same flag\n"
    "\t * gc_pause/gc_enable use), so writing it leaves the console ops and the frame buffer alone.\n"
    "\t * The USB debug hook copies every byte before that check, so the serial log keeps its lines.\n"
    "\t * offline-legacy-console=1 restores the old screen output.\n"
    "\t */\n"
    "\t{\n"
    "\t\tunsigned keep_console = 0;\n"
    "\t\tif (!PE_parse_boot_argn(\"offline-legacy-console\", &keep_console, sizeof(keep_console)) ||\n"
    "\t\t    keep_console == 0) {\n"
    "\t\t\tdisableConsoleOutput = 1;\n"
    "\t\t}\n"
    "\t}\n"
)
assert old in src, "arm_init anchor"
Path(W/"b"/"osfmk/arm").mkdir(parents=True, exist_ok=True)
(W/"b"/"osfmk/arm/arm_init.c").write_text(src.replace(old, new, 1))

# video_console.c: gc_enable(TRUE) must not clear the flag unless the switch asks for it
src = (W/"a"/"osfmk/console/video_console.c").read_text()
old = "\t\tgc_ops.enable(TRUE);\n\t\tgc_enabled           = TRUE;\n\t\tdisableConsoleOutput = FALSE;\n"
new = (
    "\t\tgc_ops.enable(TRUE);\n"
    "\t\tgc_enabled           = TRUE;\n"
    "\t\t{\n"
    "\t\t\tunsigned keep_console = 0;\n"
    "\t\t\tif (PE_parse_boot_argn(\"offline-legacy-console\", &keep_console, sizeof(keep_console)) &&\n"
    "\t\t\t    keep_console != 0) {\n"
    "\t\t\t\tdisableConsoleOutput = FALSE;   /* Allow printfs to happen */\n"
    "\t\t\t}\n"
    "\t\t}\n"
)
assert old in src, "video_console anchor"
Path(W/"b"/"osfmk/console").mkdir(parents=True, exist_ok=True)
(W/"b"/"osfmk/console/video_console.c").write_text(src.replace(old, new, 1))

parts = []
for f in files:
    proc = subprocess.run(["diff", "-u", "--label", "a/" + f, "--label", "b/" + f, str(W/"a"/f), str(W/"b"/f)], capture_output=True, text=True)
    parts.append(proc.stdout)
patch = "".join(parts)
Path("/home/dev/unixnu/UniXNU-instantnoodlep/patches/xnu/zzz-console-output-off.patch").write_text(patch)
Path("/home/dev/unixnu/full-project/r2/device/patches/xnu/zzz-console-output-off.patch").write_text(patch)
print(patch)
print("bytes", len(patch))
