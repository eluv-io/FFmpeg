//=============================================================================================
//  Copyright (c) 2026 uniqFEED Ltd. All rights reserved.
//=============================================================================================
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>


/** Context
 * @{
 */

/// Rendering context
struct UfContext;

enum UfImageFormat
{
    R8G8B8_UINT = 102,       // 8 bits 3 channels (RGB)
    B8G8R8_UINT = 103,       // 8 bits 3 channels (BGR)
    U8Y8V8Y8_422_UINT = 200, // packed YUV 4:2:2, 16bpp, (Cb Y0 Cr Y1)
    UNSPECIFIED = -1,        // unknown or non-specified format
};

/// Create a rendering context. The result is returned on success
UfContext* uFCreateContext(const char *projectPath);

/// Destroy a rendering context
int uFDestroyContext(UfContext *ctx);

/// Get the native video resolution of the context. Right now, input images have to arrive at
/// this size, but this limitation will likely be removed.
int uFGetContextResolution(const UfContext *ctx, uint32_t *width, uint32_t *height);

/// Get the native video frame rate (as fraction nFrames/duration_s) of the context. Input
/// images can be processed at any speed, this is just for informational purposes.
int uFGetContextFramerate(const UfContext *ctx, uint32_t *nFrames, uint32_t *duration_s);

/** @} */


/** Image
 * @{
 */

struct UfImage;

/// Create UfImage
UfImage* uFCreateImage(uint32_t width, uint32_t height, UfImageFormat format);

/// Convert src image to destinationFormat. The result is returned on success. For realtime
/// performance, this function should be avoided
UfImage* uFConvertImage(const UfImage *src, UfImageFormat destinationFormat);

/// Destroy a image. Return 0 on success
int uFDestroyImage(UfImage *image);

/// Retrieve image size. Return 0 on success
int uFGetImageSize(const UfImage *image, uint32_t *width, uint32_t *height);

/// Retrieve image format. Return 0 on success
int uFGetImageFormat(const UfImage *image, UfImageFormat *format);

/// Retrieve host buffer. Return 0 on success
int uFGetImageHostBuffer(const UfImage *img, void **buffer);

/// Retrieve stride. Return 0 on success
int uFGetImageStride(const UfImage *img, uint32_t *stride);

/** @} */


/** Metadata
 * @{
 */

struct UfMetadata;

/// Create metadata object. Data is stored inside the object. If data is a nullptr or the size
/// is empty, metadata for unaltered rendering is returned
UfMetadata* uFCreateMetadata(const void *data, uint32_t data_size);

/// Destroy metadata object and release memory. Return 0 on success
int uFDestroyMetadata(UfMetadata *metadata);

/** @} */


/** Feeds
 * @{
 */

/// Multiple rendered feeds
struct UfFeeds;

/// Start rendering onto image using metadata.  Returns null as long as the internal pipeline
/// is not filled yet.  After it has been filled, it will never return null and always return
/// the virtualized images in execution order. tid is an arbitrary temporal identificator (e.g.
/// the pts) which is can be retrieved from UfFeeds again.
///
/// Limitations:
/// - image must be int R8G8B8_UINT or B8G8R8_UINT format
UfFeeds* uFRenderFeeds(UfContext *ctx, const UfMetadata *metadata, int64_t tid,
                       const UfImage *image);

/// Destroy recieved feeds. Return 0 on success
int uFDestroyFeeds(UfFeeds *feeds);

/// Receive the temporal identificator tid and the number of images in feeds. Return 0 on
/// success
int uFGetFeedsProperties(const UfFeeds *feeds, uint32_t *feed_count, int64_t *tid);

/// Return an image of a feed. Data is owned by feeds. Null is returned on error
const UfImage* uFGetFeedsImage(const UfFeeds *feeds, uint32_t idx);

/** @} */


#ifdef __cplusplus
}
#endif
