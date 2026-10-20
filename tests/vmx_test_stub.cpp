/**
 * vmx_test_stub.cpp - link-time stub for the VMX codec surface
 *
 * Provides a working dummy implementation for unit tests and benchmarks.
 */

#include "vmxcodec.h"
#include <cstring>

static int g_dummyInstance = 42;

extern "C" {

VMX_API VMX_INSTANCE* VMX_Create(VMX_SIZE dimensions, VMX_PROFILE profile, VMX_COLORSPACE colorSpace) {
    (void)dimensions; (void)profile; (void)colorSpace;
    return reinterpret_cast<VMX_INSTANCE*>(&g_dummyInstance);
}

VMX_API void VMX_Destroy(VMX_INSTANCE* instance) {
    (void)instance;
}

VMX_API VMX_ERR VMX_EncodeUYVY(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_OK;
}

VMX_API VMX_ERR VMX_EncodeYUY2(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_OK;
}

VMX_API VMX_ERR VMX_EncodeNV12(VMX_INSTANCE* instance, BYTE* y, int yStride, BYTE* uv, int uvStride, int interlaced) {
    (void)instance; (void)y; (void)yStride; (void)uv; (void)uvStride; (void)interlaced;
    return VMX_ERR_OK;
}

VMX_API VMX_ERR VMX_EncodeBGRA(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_OK;
}

VMX_API VMX_ERR VMX_EncodeBGRX(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_OK;
}

VMX_API int VMX_SaveTo(VMX_INSTANCE* instance, BYTE* dst, int maxLen) {
    (void)instance;
    // Simulate 200KB encoded frame payload
    int payloadSize = 200 * 1024;
    if (maxLen < payloadSize) payloadSize = maxLen;
    if (dst && payloadSize > 0) {
        std::memset(dst, 0xAB, payloadSize);
    }
    return payloadSize;
}

} // extern "C"
