#pragma once
#include "SL_InstanceID.h"
#include <cstring>
#include <objbase.h>
#pragma comment(lib, "ole32.lib")

struct SL_SequenceData {
    std::uint32_t magic, version;
    SL_InstanceID instance;
};
static_assert(sizeof(SL_SequenceData) == 24, "Sequence data must be flat and fixed-size");

inline bool SL_ValidSequence(const SL_SequenceData& data) {
    return data.magic == 0x534C5351 && data.version == 1;
}

inline SL_SequenceData SL_NewSequence() {
    SL_SequenceData data{0x534C5351, 1, {}};
    GUID guid{};
    // GUID creation needs no COM apartment. Failure disables stable-owner reuse.
    if (SUCCEEDED(CoCreateGuid(&guid))) std::memcpy(&data.instance, &guid, sizeof(guid));
    return data;
}

inline bool SL_ReadSequence(PF_InData* in, SL_SequenceData& data) {
    if (!in || !in->utils || !in->sequence_data || !in->utils->host_get_handle_size ||
        !in->utils->host_lock_handle || !in->utils->host_unlock_handle ||
        in->utils->host_get_handle_size(in->sequence_data) != sizeof(data)) return false;
    const void* bytes = in->utils->host_lock_handle(in->sequence_data);
    if (!bytes) return false;
    std::memcpy(&data, bytes, sizeof(data));
    in->utils->host_unlock_handle(in->sequence_data);
    return SL_ValidSequence(data);
}

inline SL_InstanceID SL_ReadInstanceID(PF_InData* in) {
    SL_SequenceData data{};
    if (SL_ReadSequence(in, data)) return data.instance;
    // The current build keeps MFR disabled. Accept the const-suite path as well
    // for hosts supplying read-only sequence copies; never mutate render data.
    if (!in || !in->pica_basicP || !in->utils || !in->utils->host_get_handle_size) return {};
    const PF_EffectSequenceDataSuite1* suite = nullptr;
    if (in->pica_basicP->AcquireSuite(kPFEffectSequenceDataSuite,
        kPFEffectSequenceDataSuiteVersion1, (const void**)&suite) || !suite) return {};
    PF_ConstHandle handle = nullptr;
    if (suite->PF_GetConstSequenceData && !suite->PF_GetConstSequenceData(in->effect_ref, &handle) &&
        handle && *handle && in->utils->host_get_handle_size((PF_Handle)handle) == sizeof(data)) {
        std::memcpy(&data, *handle, sizeof(data));
    }
    in->pica_basicP->ReleaseSuite(kPFEffectSequenceDataSuite, kPFEffectSequenceDataSuiteVersion1);
    return SL_ValidSequence(data) ? data.instance : SL_InstanceID{};
}

inline PF_Err SL_WriteSequence(PF_InData* in, PF_OutData* out,
                              const SL_SequenceData& data, bool replace) {
    if (!in->utils || !in->utils->host_new_handle || !in->utils->host_lock_handle ||
        !in->utils->host_unlock_handle || !in->utils->host_dispose_handle)
        return PF_Err_INVALID_CALLBACK;
    PF_Handle handle = in->utils->host_new_handle(sizeof(data));
    if (!handle) return PF_Err_OUT_OF_MEMORY;
    void* bytes = in->utils->host_lock_handle(handle);
    if (!bytes) {
        in->utils->host_dispose_handle(handle);
        return PF_Err_OUT_OF_MEMORY;
    }
    std::memcpy(bytes, &data, sizeof(data));
    in->utils->host_unlock_handle(handle);
    // Retain the old handle if allocation/locking fails.
    if (replace && in->sequence_data) in->utils->host_dispose_handle(in->sequence_data);
    out->sequence_data = handle;
    return PF_Err_NONE;
}

inline PF_Err SL_SequenceCommand(PF_Cmd cmd, PF_InData* in, PF_OutData* out) {
    if (cmd == PF_Cmd_SEQUENCE_SETDOWN) {
        if (in->sequence_data) {
            if (!in->utils || !in->utils->host_dispose_handle) return PF_Err_INVALID_CALLBACK;
            in->utils->host_dispose_handle(in->sequence_data);
        }
        out->sequence_data = nullptr;
        return PF_Err_NONE;
    }
    if (cmd == PF_Cmd_SEQUENCE_FLATTEN) {
        // The live representation is already flat; ownership stays with AE.
        out->sequence_data = in->sequence_data;
        return PF_Err_NONE;
    }
    SL_SequenceData data{};
    const bool valid = SL_ReadSequence(in, data);
    if (cmd == PF_Cmd_GET_FLATTENED_SEQUENCE_DATA) {
        out->sequence_data = nullptr;
        return valid ? SL_WriteSequence(in, out, data, false) : PF_Err_NONE;
    }
    if (cmd == PF_Cmd_SEQUENCE_SETUP || cmd == PF_Cmd_SEQUENCE_RESETUP) {
        // UI resetup also occurs for load, duplicate, undo and precompose. Give
        // those instances fresh identities. Render-only clones keep the UI ID.
        if (cmd == PF_Cmd_SEQUENCE_SETUP || !(in->in_flags & PF_InFlag_PROJECT_IS_RENDER_ONLY) || !valid)
            data = SL_NewSequence();
        return SL_WriteSequence(in, out, data, cmd == PF_Cmd_SEQUENCE_RESETUP);
    }
    return PF_Err_NONE;
}
