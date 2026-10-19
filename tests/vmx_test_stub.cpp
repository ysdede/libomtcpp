/**
 * vmx_test_stub.cpp - link-time stub for the VMX codec surface
 *
 * OMTSender.cpp references the libvmx entry points unconditionally, so a test
 * executable that pulls in the Sender object cannot link without them. The
 * tests never encode or decode video (they exercise the metadata/control
 * paths), so the stub only needs to satisfy the linker: VMX_Create hands back
 * a null "instance" and every encode reports failure, which makes Sender's
 * encode path bail out the same way it does when the codec reports an error.
 *
 * Signatures come from libvmx's vmxcodec.h (extern "C"); including the real
 * header keeps this file honest instead of duplicating the types.
 */

#include "vmxcodec.h"

extern "C" {

VMX_API VMX_INSTANCE* VMX_Create(VMX_SIZE dimensions, VMX_PROFILE profile, VMX_COLORSPACE colorSpace) {
    (void)dimensions; (void)profile; (void)colorSpace;
    return nullptr;
}

VMX_API void VMX_Destroy(VMX_INSTANCE* instance) {
    (void)instance;
}

VMX_API VMX_ERR VMX_EncodeUYVY(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_INVALID_INSTANCE;
}

VMX_API VMX_ERR VMX_EncodeYUY2(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_INVALID_INSTANCE;
}

VMX_API VMX_ERR VMX_EncodeNV12(VMX_INSTANCE* instance, BYTE* y, int yStride, BYTE* uv, int uvStride, int interlaced) {
    (void)instance; (void)y; (void)yStride; (void)uv; (void)uvStride; (void)interlaced;
    return VMX_ERR_INVALID_INSTANCE;
}

VMX_API VMX_ERR VMX_EncodeBGRA(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_INVALID_INSTANCE;
}

VMX_API VMX_ERR VMX_EncodeBGRX(VMX_INSTANCE* instance, BYTE* src, int stride, int interlaced) {
    (void)instance; (void)src; (void)stride; (void)interlaced;
    return VMX_ERR_INVALID_INSTANCE;
}

VMX_API int VMX_SaveTo(VMX_INSTANCE* instance, BYTE* dst, int maxLen) {
    (void)instance; (void)dst; (void)maxLen;
    return 0;
}

} // extern "C"
