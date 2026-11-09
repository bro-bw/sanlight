#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
	#include "AE_General.r"
#endif

resource 'PiPL' (16000) {
	{	/* array properties */
		Kind {
			AEEffect
		},
		Name {
			"三渲二光源 SanLight"
		},
		Category {
			"三渲二工具"
		},
#ifdef AE_OS_WIN
	#ifdef AE_PROC_INTELx64
		CodeWin64X86 {"EffectMain"},
	#endif
#else
	#ifdef AE_OS_MAC
		CodeMacIntel64 {"EffectMain"},
		CodeMacARM64 {"EffectMain"},
	#endif
#endif
		AE_PiPL_Version {
			2,
			0
		},
		AE_Effect_Spec_Version {
			PF_PLUG_IN_VERSION,
			PF_PLUG_IN_SUBVERS
		},
		AE_Effect_Version {
			1048577	/* 1.0 */
		},
		AE_Effect_Info_Flags {
			0
		},
		AE_Effect_Global_OutFlags {
			0x2000400	/* PIX_INDEPENDENT | DEEP_COLOR_AWARE */
		},
		AE_Effect_Global_OutFlags_2 {
			0x8001400	/* SMART_RENDER | FLOAT_COLOR_AWARE | THREADED_RENDERING */
		},
		AE_Effect_Match_Name {
			"San Light"
		},
		AE_Reserved_Info {
			0
		}
	}
};
