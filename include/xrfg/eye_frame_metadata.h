#ifndef OFXR_EYE_FRAME_METADATA_H
#define OFXR_EYE_FRAME_METADATA_H

#include <stdint.h>
#include <openxr/openxr.h>

// In-process, experimental OFXR API. Resolve these names from the already
// loaded OFXR layer DLL. Call through the application's normal release chain.
#define OFXR_RELEASE_WITH_EYE_METADATA_V1 "ofxrReleaseSwapchainImageWithEyeMetadataV1"
#define OFXR_GET_RELEASED_EYE_METADATA_V1 "ofxrGetReleasedEyeFrameMetadataV1"

#if defined(_WIN32)
#define OFXR_EYE_METADATA_CALL __cdecl
#else
#define OFXR_EYE_METADATA_CALL
#endif

enum {
    OFXR_EYE_SOURCE_REAL_VR = 1,
    OFXR_EYE_SOURCE_UEVR = 2,
    OFXR_EYE_SOURCE_MUTAR = 3
};

typedef struct OfxrEyeSourceV1 {
    // Allocate a new nonzero stream ID when render numbering resets. Preserve
    // this pair when an old image is copied forward, including across slots.
    uint64_t stream_id;
    uint64_t render_id; // IDs in the same stream identify the same render time across eyes.
    uint32_t guide_token; // OFXR colour-origin token, or zero when unavailable.
    uint32_t eye; // 0 = left, 1 = right, independent of the subimage layout.
} OfxrEyeSourceV1;

typedef struct OfxrEyeFrameMetadataV1 {
    uint32_t size;
    uint32_t producer;
    uint32_t image_index; // Returned by xrAcquireSwapchainImage.
    uint32_t eye_count; // One for a split-eye image, up to two for a packed image.
    OfxrEyeSourceV1 eyes[2];
} OfxrEyeFrameMetadataV1;

typedef struct OfxrReleasedEyeFrameMetadataV1 {
    uint64_t release_serial; // OFXR's release serial, independent of render_id.
    uint64_t capture_serial; // Zero when OFXR did not retain a colour capture.
    OfxrEyeFrameMetadataV1 frame;
} OfxrReleasedEyeFrameMetadataV1;

// The OpenXR release callback retains the platform's XRAPI_CALL convention.
typedef XrResult (OFXR_EYE_METADATA_CALL *PFN_ofxrReleaseSwapchainImageWithEyeMetadataV1)(
    PFN_xrReleaseSwapchainImage release_image, XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info, const OfxrEyeFrameMetadataV1* frame);

// Shadow-probe accessor. Success with release_serial == 0 means the latest
// release has no accepted metadata. This does not query the runtime.
typedef XrResult (OFXR_EYE_METADATA_CALL *PFN_ofxrGetReleasedEyeFrameMetadataV1)(
    XrSwapchain swapchain, OfxrReleasedEyeFrameMetadataV1* released);

#endif
