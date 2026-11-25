# b495: the one KUBridge call the crash catcher needs (crash.c). KUBridge is the CFW's user-to-kernel bridge
# (M33, PRO, ME, ARK: SystemControl exports it); NID and flags from pspdev/psp-cfw-sdk src/KUBridge/KUBridge.S.
# Without such a CFW (PPSSPP, OFW) the stub stays unlinked: crash.c checks it before the call.
	.set noreorder

#include "pspimport.s"

	IMPORT_START "KUBridge",0x40090000
	IMPORT_FUNC  "KUBridge",0x4C25EA72,kuKernelLoadModule
