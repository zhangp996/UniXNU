#!/usr/bin/env python3
"""Run the pinned ShengHidInput ABI checks using the SM8250 Linux toolchain."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(os.environ.get("SMUFS_BUILD_TREE", str(Path(__file__).resolve().parents[1])))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--kit", type=Path, default=ROOT / "kits/instantnoodlep")
parser.add_argument("--standalone", action="store_true")
options = parser.parse_args()
kit = options.kit.resolve()
out = ROOT / "build/sheng-hid-input"
out.mkdir(parents=True, exist_ok=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace(text, original, adapted):
    assert text.count(original) == 1, original
    return text.replace(original, adapted)


def qualify_edge_abi(read, plan):
    # These address points were recovered from the 21H461 class metaclass
    # getters. Addresses here are unslid, unlike the reference reader.
    slide = plan["slide"]
    driver = 0xfffffff006d06a68
    event = 0xfffffff006d07318
    for table, slots, methods in (
        (driver, 220, {7: 0xfffffff005af0a3c, 198: 0xfffffff005aface8, 219: 0xfffffff005af71f4}),
        (event, 37, {7: 0xfffffff005afb1b0, 15: 0xfffffff005afcee0, 18: 0xfffffff005afcf90,
                     23: 0xfffffff005afcfb8, 24: 0xfffffff005afefdc, 25: 0xfffffff005afe4c4}),
    ):
        entries = struct.unpack(f"<{slots + 3}Q", read(table + slide - 16, (slots + 3) * 8))
        assert entries[:2] == (0, 0) and all(entries[2:-1]) and entries[-1] == 0
        assert all(entries[slot + 2] == address + slide for slot, address in methods.items())
    # appendChild reads the OSArray pointer at +24.
    assert read(0xfffffff005afcf04 + slide, 4) == bytes.fromhex("000c40f9")


def compile_source(source, obj):
    record = ROOT / "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json"
    recipe = json.loads(record.read_text().strip().rstrip(","))
    arguments = iter(recipe["arguments"])
    command = []
    for arg in arguments:
        if arg == recipe["file"]:
            command.append(str(source))
        elif arg in ("-o", "-MF", "-MT", "-MJ"):
            next(arguments)
        elif arg == "-Xclang":
            opt = next(arguments)
            if not opt.startswith("-W"):
                command += [arg, opt]
        elif not arg.startswith("-W"):
            command.append(arg)
    command += ["-o", str(obj), "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                "-Wno-missing-designated-field-initializers", "-Wno-c99-designator",
                "-Wno-reorder-init-list", "-Wno-deprecated-enum-enum-conversion",
                "-Wno-unnecessary-virtual-specifier", "-I", str(out), "-I", str(kit),
                "-I", str(ROOT / ".cache/xnu-work/osfmk"), "-mgeneral-regs-only"]
    result = subprocess.run(command, cwd=recipe["directory"], capture_output=True, text=True)
    log = result.stdout + result.stderr
    (out / (obj.stem + ".log")).write_text(log)
    if result.returncode:
        raise SystemExit(log[-7000:])


source = kit / "NativeHidInput.cpp"
assert sha(source) == "7effbe160d96074738f94f0fe7f223e1203d968cd840a0b6b9e4a88a3fb4711a"
text = source.read_text()
text = replace(text, "transformCoordinate(xs[i],30480),y=transformCoordinate(ys[i],20320)",
               "transformCoordinate(xs[i],SmTouchMaxX+1),y=transformCoordinate(ys[i],SmTouchMaxY+1)")
text = replace(text, '#include "OriginalHidDeviceABI.h"',
               '#include "OriginalHidDeviceABI.h"\n#include "Sm8250TouchFrame.h"')
text = replace(text, 'touchOnly?"SPI":"Virtual"', '(touchOnly||touchEnabled)?"I2C":"Virtual"')
text = replace(text, 'extern "C" void native_hid_input_observed(unsigned,int);',
               'extern "C" void native_hid_input_observed(unsigned,int);\n'
               'extern "C" void native_hid_buttons_observed(uint32_t,int);')
text = replace(text, "native_hid_input_observed(4,result);", "native_hid_buttons_observed(self->buttons,result);")
# Reuse the upstream gesture handling, with only the board geometry and
# 21H461 dispatch ABI adapted. 1080x2376 is the TOUCH44 scanout.
text = replace(text, "kSlots=256,kDispatch=198", "kSlots=220,kDispatch=198")
text = replace(text, "kEdgeX=48*65536/3048,kEdgeY=48*65536/2032",
               "kEdgeX=48*65536/1080,kEdgeY=48*65536/2376")
text = replace(text, "static uintptr_t originalDispatch;",
               "static uintptr_t originalDispatch;\nstatic uintptr_t eventVptr;")
text = replace(text, "if(event && call<uint32_t,IOOptionBits>(event,kGetType,0)==kDigitizer)",
               "if(event && *reinterpret_cast<uintptr_t *>(event)==eventVptr && "
               "call<uint32_t,IOOptionBits>(event,kGetType,0)==kDigitizer)")
text = replace(text, 'if(child && child->metaCast("IOHIDEvent"))any|=mark(child);',
               "if(child && *reinterpret_cast<uintptr_t *>(child)==eventVptr)any|=mark(child);")
text = replace(text, "    uintptr_t *copy=static_cast<uintptr_t *>(IOMalloc(kSlots*sizeof(uintptr_t)));\n"
                     "    if(!copy)return;\n"
                     "    memcpy(copy,table,kSlots*sizeof(uintptr_t));",
               "    const uintptr_t address=reinterpret_cast<uintptr_t>(table);\n"
               "    if(table[7]!=address-0x121602c || table[kDispatch]!=address-0x120bd80) {\n"
               "        printf(\"TOUCH-EDGE 21H461 driver ABI refused\\n\");return;\n"
               "    }\n"
               "    uintptr_t *storage=static_cast<uintptr_t *>(IOMalloc((kSlots+2)*sizeof(uintptr_t)));\n"
               "    if(!storage)return;\n"
               "    memcpy(storage,table-2,(kSlots+2)*sizeof(uintptr_t));\n"
               "    uintptr_t *copy=storage+2;\n"
               "    eventVptr=address+0x8b0;")
text = replace(text, "    native_touch_edge_stats[0]=1;",
               "    native_touch_edge_stats[0]=1;\n"
               "    printf(\"TOUCH-EDGE 21H461 installed\\n\");")
text = replace(text, "if(!self->hasEventDriverCached())return;\n        uint32_t before",
               "if(!self->eventDriverFound)native_hid_live_checkpoint(self);\n"
               "        if(!self->hasEventDriverCached())return;\n        uint32_t before")
text = replace(text, "        self->lastSeq=before;\n", "")
before = text.index("        for(unsigned j=0;j<TOUCH_FINGERS;++j) {\n            self->prevIds[j]=")
after = text.index("        if(!slot)return;", before)
commit_state = text[before:after]
text = text[:before] + text[after:]
text = replace(text, "        if(!slot)return;", "        if(!slot){self->lastSeq=before;return;}")
text = replace(text, "        native_hid_input_observed(6,result);",
               "        if(!result) {\n            self->lastSeq=before;\n" +
               "".join("    " + line for line in commit_state.splitlines(keepends=True)) +
               "        }\n        native_hid_input_observed(6,result);")
text = replace(text, "if(self->touchOnly){source->setTimeoutMS(1000);return;}",
               "if(self->touchOnly||self->touchEnabled)self->touchSend();\n"
               "        if(self->touchOnly){source->setTimeoutMS(1000);return;}")
adapted = out / "NativeHidInput.cpp"
adapted.write_text(text)

reference = ROOT.parent / "kernel/kernel.macho"
builder = ROOT / "scripts/build-sheng-hid-input.py"
assert sha(builder) == "764d5a61e936c290af43765f34aa9cb32a082f1404a426535d38a01a6f345ea6"
script = builder.read_text()
script = replace(script, "ROOT=Path(__file__).resolve().parents[1]", "ROOT=board_root")
start = script.index("p=ROOT/'build/xnu-prelink/")
end = script.index("b=p.read_bytes();", start)
script = script[:start] + "p=board_reference\n" + script[end:]
script = replace(script, "print(run(['python3',ROOT/'scripts/build-arm-driver.py','NativeHidInput'],text=True),end='')",
                 "board_compile(board_source, OUT/'NativeHidInput.o')")
script = replace(script, "names={n:a for a,ns in exports(b,m).items() for n in ns}",
                 "board_edge_abi(read, plan)\nnames={n:a for a,ns in exports(b,m).items() for n in ns}")
start = script.index("sdk=run(['xcrun'")
end = script.index("def code(data,image):", start)
script = script[:start] + """run(['clang','-target','arm64-apple-ios17.0','-c',asm,'-o',OUT/'hid-original-bindings.o'])
late=OUT/'NativeHidInput.late.o'
run(['llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeHidInput.cpp',obj,late])
""" + script[end:]
script = replace(script, "(ROOT/'drivers/ShengHidInput/NativeHidInput.cpp').read_bytes()", "board_source.read_bytes()")
exec(compile(script, str(builder), "exec"), {
    "board_root": ROOT, "board_reference": reference, "board_compile": compile_source,
    "board_source": adapted, "board_edge_abi": qualify_edge_abi,
})
board = kit / "Sm8250TouchHid.cpp"
board_object = out / "Sm8250TouchHid.o"
compile_source(board, board_object)
members = [out / "NativeHidInput.late.o", out / "hid-original-bindings.o"]
if not options.standalone:
    archive = ROOT / "build/native-legacy-display/libNativeLegacyDisplay.late.a"
    report_path = archive.with_name("build-report.json")
    report = json.loads(report_path.read_text())
    assert sha(archive) == report["archive_sha256"]
    before = subprocess.check_output(["llvm-ar", "t", archive], text=True).splitlines()
    assert not {m.name for m in members}.intersection(before)
    subprocess.run(["llvm-ar", "r", archive, *members], check=True)
    assert subprocess.check_output(["llvm-ar", "t", archive], text=True).splitlines() == before + [m.name for m in members]
    report["archive_sha256"] = sha(archive)
    report["source_sha256"].update({str(p): sha(p) for p in (source, adapted, board, kit / "Sm8250TouchFrame.h")})
    report["touch_hid"] = {"members": [m.name for m in members], "abi": str(out / "abi-audit.json")}
    report_path.write_text(json.dumps(report, indent=2) + "\n")
print("PASS SM8250 ShengHidInput and board publication compiled")
