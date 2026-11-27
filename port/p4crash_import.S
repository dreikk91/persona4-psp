/* b523: the CFW's (ARK-4 and alike) SystemCtrlForKernel sctrlHENFindFunction as p4c_find_function (p4crash_prx.c);
   left unlinked without such a CFW, which p4crash_prx.c checks before calling it. */
	.set noreorder

#include "pspimport.s"

	IMPORT_START "SystemCtrlForKernel",0x00090000
	IMPORT_FUNC  "SystemCtrlForKernel",0x159AF5CC,p4c_find_function
