
from pathlib import Path

p = Path("/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree/drivers/GopFramebuffer/NativeLegacyDisplay.cpp")
src = p.read_text()
if "SWAPDBG" in src:
    print("already instrumented"); raise SystemExit(0)

def rep(old, new, name):
    global src
    assert old in src, ("anchor", name)
    src = src.replace(old, new, 1)

rep("    bool kicking=false,rekick=false;",
    "    bool kicking=false,rekick=false;\n"
    "    // Bounded swap-lifecycle counters and state prints (SWAPDBG) for locating a presentation stall.\n"
    "    unsigned dbgKickq=0,dbgKick=0,dbgCopy=0,dbgRetire=0,dbgRetireOk=0,dbgValidate=0,dbgBlocked=0,dbgVsync=0;\n"
    "    void dbgSwap(const char *event,const void *request,int a=0,int b=0) {\n"
    "        static unsigned shown;\n"
    "        if(shown++<60)printf(\"SWAPDBG %s req=%p a=%d b=%d kickq=%u kick=%u copy=%u retire=%u ok=%u val=%u blk=%u transferred=%d pending=%d\\n\",\n"
    "            event,request,a,b,dbgKickq,dbgKick,dbgCopy,dbgRetire,dbgRetireOk,dbgValidate,dbgBlocked,transferred?1:0,pendingSwapRequest()?1:0);\n"
    "    }", "latch")

rep("    void kickSwapQueue() override {\n        armVsync();",
    "    void kickSwapQueue() override {\n        dbgKickq++;dbgSwap(\"kickq\",pendingSwapRequest());\n        armVsync();", "kickq")

rep("    void kickOnce() {\n        unsigned enabled=0;PE_parse_boot_argn(\"offline-legacy-transfer\",&enabled,sizeof(enabled));\n        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;\n        const OriginalLegacySwapRequest *request=pendingSwapRequest();\n        if(!request || !isValidated(request))return;",
    "    void kickOnce() {\n"
    "        unsigned enabled=0;PE_parse_boot_argn(\"offline-legacy-transfer\",&enabled,sizeof(enabled));\n"
    "        dbgKick++;\n"
    "        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary) {\n"
    "            dbgBlocked++;dbgSwap(\"kick-blocked\",pendingSwapRequest(),(int)enabled,(transferred?1:0)|((!logicalPM?2:0))|((!bootMapping?4:0))|((!primary?8:0)));return;\n"
    "        }\n"
    "        const OriginalLegacySwapRequest *request=pendingSwapRequest();\n"
    "        if(!request || !isValidated(request)) {dbgSwap(\"kick-noreq\",request);return;}", "kickOnce")

rep("        OSSafeReleaseNULL(source);transferred=true;\n        native_legacy_transfer_observed(kIOReturnSuccess,request,length);\n        finishTransfer(request);",
    "        OSSafeReleaseNULL(source);transferred=true;\n        dbgCopy++;dbgSwap(\"copied\",request);\n        native_legacy_transfer_observed(kIOReturnSuccess,request,length);\n        finishTransfer(request);", "copy")

rep("    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {\n        unsigned retire=0,cpuPresent=0;",
    "    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {\n        dbgRetire++;\n        unsigned retire=0,cpuPresent=0;", "retire")

rep("        if(!activateOnlyPendingSwap(request))return false;\n        removeValidated(request);",
    "        if(!activateOnlyPendingSwap(request)) {dbgSwap(\"retire-nohead\",request,force?1:0);return false;}\n        dbgRetireOk++;dbgSwap(\"retired\",request,force?1:0);\n        removeValidated(request);", "retireok")

rep("        if(accepted)addValidated(request);else removeValidated(request);\n        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;",
    "        dbgValidate++;dbgSwap(accepted?\"validated\":\"rejected\",request,(int)reason);\n"
    "        if(accepted)addValidated(request);else removeValidated(request);\n"
    "        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;", "validate")

rep("        vsyncNext+=vsyncInterval;\n        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}\n        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);",
    "        vsyncNext+=vsyncInterval;\n"
    "        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}\n"
    "        if((++dbgVsync%120)==0){\n"
    "            static unsigned shown;\n"
    "            if(shown++<40)printf(\"SWAPDBG vsync alive tick=%u kickq=%u kick=%u copy=%u retire=%u ok=%u val=%u blk=%u transferred=%d pending=%d\\n\",\n"
    "                dbgVsync,dbgKickq,dbgKick,dbgCopy,dbgRetire,dbgRetireOk,dbgValidate,dbgBlocked,transferred?1:0,pendingSwapRequest()?1:0);\n"
    "        }\n"
    "        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);", "vsync")

p.write_text(src)
print("instrumented component source", len(src))
