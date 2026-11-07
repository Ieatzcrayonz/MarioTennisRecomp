"""libultra (2.0I) functions identified in Mario Tennis (USA) main code, by behaviour.

Only names that N64Recomp/N64ModernRuntime treat specially (reimplemented or ignored) matter;
a wrong name there swaps in a runtime implementation with different semantics, so anything not
verified is left as func_XXXXXXXX. See NOTES.md for the evidence and the unresolved list.
"""

LIBULTRA = {
    # --- interrupts / cop0 ---
    0x80032F60: "__osRestoreInt",
    0x80032F80: "__osDisableInt",
    0x80042250: "osSetIntMask",
    0x80032C74: "osGetCount",
    0x80048650: "__osGetCause",
    0x80048660: "__osGetSR",
    0x80048670: "__osSetCompare",
    0x80048680: "__osSetFpcCsr",
    0x80048690: "__osSetSR",
    0x800486A0: "__osSetWatchLo",
    # --- TLB ---
    0x80032BC0: "osMapTLB",
    0x80033178: "__osProbeTLB",
    0x80033160: "mt_osVirtualToPhysical",  # replaced in src/game/mt_tlb.cpp (runtime ignores the TLB)
    0x80045E80: "osUnmapTLBAll",
    0x80048850: "osMapTLBRdb",
    # --- exceptions / scheduler ---
    0x800418E0: "__osException",
    0x80041E6C: "send_mesg",
    0x80041F5C: "__osEnqueueAndYield",
    0x80042064: "__osEnqueueThread",
    0x800420AC: "__osPopThread",
    0x800420BC: "__osNop",
    0x800420C4: "__osDispatchThread",
    # --- threads ---
    0x80045590: "osCreateThread",
    0x8003A980: "osStartThread",
    0x80045850: "osStopThread",
    0x80045660: "osDestroyThread",
    0x80045950: "osYieldThread",
    0x80045780: "osSetThreadPri",
    0x80045760: "osGetThreadPri",
    0x80045740: "osGetThreadId",
    0x80045910: "__osDequeueThread",
    # --- messages ---
    0x80045070: "osCreateMesgQueue",
    0x80032DE0: "osRecvMesg",
    0x80032EA0: "osSendMesg",
    # --- timers ---
    0x800459A0: "osGetTime",
    0x80045A30: "osSetTimer",
    0x80045B60: "__osTimerServicesInit",
    0x80045BB4: "__osTimerInterrupt",
    0x80045CF0: "__osSetTimerIntr",
    0x80045D70: "__osInsertTimer",
    # --- VI ---
    0x80045F50: "osCreateViManager",
    0x800460F0: "viMgrMain",
    0x80046290: "osViSetEvent",
    0x800462F0: "osViSetMode",
    0x80046340: "osViSetSpecialFeatures",
    0x800464B0: "osViSetXScale",
    0x80046580: "osViSetYScale",
    0x800465D0: "osViSwapBuffer",
    0x80046620: "__osViSwapContext",
    0x80046930: "osViBlack",
    0x80045ED0: "osViGetCurrentFramebuffer",
    0x80045F10: "osViGetNextFramebuffer",
    0x800488B0: "__osViInit",
    0x800489C0: "__osViGetCurrentContext",
    # --- AI ---
    0x80042D40: "osAiGetLength",
    0x80042D50: "osAiGetStatus",
    0x80042D60: "osAiSetFrequency",
    0x80042E80: "osAiSetNextBuffer",
    0x80048620: "__osAiDeviceBusy",
    # --- SP / RSP tasks ---
    0x80045290: "__osSpDeviceBusy",
    0x800452B0: "__osSpSetStatus",
    0x800452C0: "__osSpSetPc",
    0x800452F0: "__osSpRawStartDma",
    0x800486B0: "__osSpGetStatus",
    0x80045380: "osSpTaskYield",
    0x800453A0: "osSpTaskYielded",
    0x8003AA70: "osSpTaskStartGo",
    0x8003AA9C: "osSpTaskLoad",
    # --- caches ---
    0x8003AC10: "osWritebackDCacheAll",
    0x8003AC64: "osWritebackDCache",
    0x8003AD40: "osInvalDCache",
    0x80042F20: "osInvalICache",
    # --- PI ---
    0x80042480: "osEPiRawStartDma",
    0x80042670: "osEPiStartDma",
    0x800422F0: "osCreatePiManager",
    0x80042710: "osCartRomInit",
    0x80042890: "__osDevMgrMain",
    0x80042C50: "__osPiCreateAccessQueue",
    0x80042CA4: "__osPiGetAccess",
    0x80042D10: "__osPiRelAccess",
    0x80048250: "osPiRawStartDma",
    0x80048320: "osPiGetCmdQueue",
    0x80048340: "osEPiRawReadIo",
    0x800484B0: "osEPiRawWriteIo",
    # --- SI / EEPROM / rumble ---
    0x800453F0: "__osSiRawStartDma",
    0x800454A0: "__osSiCreateAccessQueue",
    0x800454F4: "__osSiGetAccess",
    0x80045560: "__osSiRelAccess",
    0x800486C0: "__osSiRawReadIo",
    0x80048710: "__osSiRawWriteIo",
    0x80049A60: "__osSiDeviceBusy",
    0x80041700: "osEepromProbe",
    0x80047BF0: "osEepromRead",
    0x80047D88: "__osPackEepReadData",
    0x80047E10: "osEepromWrite",
    0x80047F78: "__osPackEepWriteData",
    0x80048020: "__osEepStatus",
    0x80041870: "osEepromLongRead",
    0x80041780: "osEepromLongWrite",
    0x80046FD0: "__osMotorAccess",
    # --- controllers (osCont*) ---
    0x80043220: "osContInit",
    0x80042FA0: "osContStartQuery",
    0x80043020: "osContGetQuery",
    0x80043040: "osContStartReadData",
    0x800430C8: "osContGetReadData",
    0x800433BC: "__osContGetInitData",
    0x80043470: "__osPackRequestData",
    0x80043164: "__osPackReadData",
    # --- messages (more) ---
    0x800451E0: "osSetEventMesg",
    0x800450A0: "osJamMesg",
    # --- controller pak / rumble ---
    0x80047114: "osMotorInit",
    0x80047360: "__osPfsSelectBank",
    0x800473D0: "__osContRamRead",
    0x800475C0: "__osContRamWrite",
    0x80048760: "__osContAddressCrc",
    0x800487CC: "__osContDataCrc",
    0x800489D0: "osPfsIsPlug",
    0x80048B4C: "__osPfsRequestData",
    0x80048BF0: "__osPfsGetInitData",
    0x80048CB0: "__osPfsGetStatus",
    0x80048DA0: "__osPfsRequestOneChannel",
    0x80048E34: "__osPfsGetOneChannelData",
    # --- init ---
    0x80047908: "osInitialize",
}

# Game functions replaced by hand in src/game/mt_libultra.cpp (listed as `ignored` in mt.us.toml):
#   0x80031C4C  second copy of osStartThread          -> forwards to the runtime's osStartThread
#   0x80032D20  thread-wake helper of osSend/RecvMesg -> unused once those are replaced
#   0x800332DC  exception-handler send_mesg copy       -> unused (exceptions are not emulated)
#   0x80048F38, 0x80049224, 0x80049388, 0x80049554, 0x8004960C, 0x80049950
#               Transfer Pak (osGbpak*) functions      -> report "no pak"
