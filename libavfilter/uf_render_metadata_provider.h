#ifndef AVFILTER_UF_RENDER_METADATA_PROVIDER_H
#define AVFILTER_UF_RENDER_METADATA_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

typedef struct AVFrame AVFrame;

typedef struct UfMetadataProviderVTable {
    int (*init)(const char *project_path,
                const char *metadata_dir,
                void **provider_opaque);

    int (*get_metadata_blob)(uint64_t frame_index,
                             unsigned int stream_index,
                             int64_t render_tid,
                             const AVFrame *filtered_frame,
                             uint8_t **metadata_blob,
                             size_t *metadata_blob_size,
                             void *provider_opaque);

    void (*release_metadata_blob)(uint8_t *metadata_blob,
                                  size_t metadata_blob_size,
                                  void *provider_opaque);

    void (*close)(void *provider_opaque);
} UfMetadataProviderVTable;

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
const UfMetadataProviderVTable *uFGetExternalMetadataProviderV1(void);

#endif
