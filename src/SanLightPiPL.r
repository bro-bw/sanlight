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
#ifdef SL_UTF8_BUILD
			"SanLight v1.3 UTF8"
#else
			"SanLight v1.3"
#endif
		},
		Category {
			"SanLight"
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
			624151	/* PF_VERSION(1, 3, 0, 3, 23) */
		},
		AE_Effect_Info_Flags {
			0
		},
		AE_Effect_Global_OutFlags {
			0x6000406	/* UPDATE_PARAMS_UI | PIX_INDEPENDENT | DEEP_COLOR_AWARE | WIDE_TIME | NON_PARAM_VARY */
		},
		AE_Effect_Global_OutFlags_2 {
			0x821400	/* SMART_RENDER | FLOAT_COLOR_AWARE | AUTOMATIC_WIDE_TIME_INPUT | GET_FLATTENED_SEQUENCE_DATA */
		},
		AE_Effect_Match_Name {
#ifdef SL_UTF8_BUILD
			"San Light 1.3 UTF8"
#else
			"San Light 1.3"
#endif
		},
		AE_Reserved_Info {
			0
		}
	}
};
