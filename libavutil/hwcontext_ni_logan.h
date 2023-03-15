/*
* This file is part of FFmpeg.
*
* FFmpeg is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* FFmpeg is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with FFmpeg; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifndef AVUTIL_HWCONTEXT_NI_LOGAN_H
#define AVUTIL_HWCONTEXT_NI_LOGAN_H

#include "hwcontext.h"
#include <ni_device_api_logan.h>
#include <ni_rsrc_api_logan.h>

enum
{
  NI_LOGAN_MEMTYPE_VIDEO_MEMORY_NONE,
  NI_LOGAN_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET,
  NI_LOGAN_MEMTYPE_VIDEO_MEMORY_HWUPLOAD_TARGET,
};

typedef struct NILOGANFramesContext {
  ni_logan_hwframe_surface_t    *surfaces_internal;
  int                     nb_surfaces_used;
  ni_logan_hwframe_surface_t    **surface_ptrs;
  ni_logan_device_context_t     *rsrc_ctx;  /* resource management context */
  ni_logan_session_data_io_t    *src_session_io_data;
  ni_logan_session_context_t    api_ctx;//for down/uploading frames
  int                     pc_height, pc_width, pc_crop_bottom, pc_crop_right; //precropped values
  ni_device_handle_t      suspended_device_handle;
} NILOGANFramesContext;

/**
* This struct is allocated as AVHWDeviceContext.hwctx
*/
typedef struct AVNILOGANDeviceContext {
  int device_idx;
} AVNILOGANDeviceContext;

/**
* This struct is allocated as AVHWFramesContext.hwctx
*/
typedef struct AVNILOGANFramesContext {
  ni_logan_hwframe_surface_t  *surfaces;
  int                   nb_surfaces;
  int                   bit_depth;
  int                   frame_type;
} AVNILOGANFramesContext;

#endif /* AVUTIL_HWCONTEXT_NI_LOGAN_H */
