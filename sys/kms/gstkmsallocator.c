/* GStreamer
 *
 * Copyright (C) 2016 Igalia
 * Copyright (C) Microchip Technology Inc.
 *
 * Authors:
 *  Víctor Manuel Jáquez Leal <vjaquez@igalia.com>
 *  Javier Martin <javiermartin@by.com.es>
 *  Sandeep Sheriker M <sandeepsheriker.mallikarjun@microchip.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* it needs to be below because is internal to libdrm */
#include <drm.h>

#include "gstkmsallocator.h"
#include "gstkmsutils.h"
#include "gstg1kmssink.h"
#include "gstg1allocator.h"

#define GST_CAT_DEFAULT kmsallocator_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define GST_KMS_MEMORY_TYPE "KMSMemory"

#define DRM_ATMEL_GEM_GET		0x00

#define DRM_IOCTL_ATMEL_GEM_GET		DRM_IOWR(DRM_COMMAND_BASE + \
					DRM_ATMEL_GEM_GET, struct drm_mode_map_dumb)

struct kms_bo
{
  void *ptr;
  size_t size;
  size_t offset;
  size_t pitch;
  unsigned handle;
  unsigned int refs;
};

struct _GstKMSAllocatorPrivate
{
  int fd;
};

#define parent_class gst_kms_allocator_parent_class
G_DEFINE_TYPE_WITH_CODE (GstKMSAllocator, gst_kms_allocator, GST_TYPE_ALLOCATOR,
    G_ADD_PRIVATE (GstKMSAllocator);
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "kmsallocator", 0,
        "KMS allocator"));

enum
{
  PROP_DRM_FD = 1,
  PROP_N,
};

static GParamSpec *g_props[PROP_N] = { NULL, };

gboolean
gst_is_kms_memory (GstMemory * mem)
{
  return gst_memory_is_type (mem, GST_KMS_MEMORY_TYPE);
}

guint32
gst_kms_memory_get_fb_id (GstMemory * mem)
{
  if (!gst_is_kms_memory (mem))
    return 0;
  return ((GstKMSMemory *) mem)->fb_id;
}

static gboolean
check_fd (GstKMSAllocator * alloc)
{
  return alloc->priv->fd > -1;
}

static void
gst_kms_allocator_memory_reset (GstKMSAllocator * allocator, GstKMSMemory * mem)
{
  int err;
  struct drm_mode_destroy_dumb arg = { 0, };

  if (!check_fd (allocator))
    return;

  if (allocator->kmssink->Ismaster) {
    if (mem->fb_id) {
      GST_DEBUG_OBJECT (allocator, "removing fb id %d", mem->fb_id);
      drmModeRmFB (allocator->priv->fd, mem->fb_id);
      mem->fb_id = 0;
    }
  }
  if (!mem->bo)
    return;

  if (mem->bo->ptr != NULL) {
    GST_WARNING_OBJECT (allocator, "destroying mapped bo (refcount=%d)",
        mem->bo->refs);
    munmap (mem->bo->ptr, mem->bo->size);
    mem->bo->ptr = NULL;
  }

  if (allocator->kmssink->Ismaster) {
    arg.handle = mem->bo->handle;
    err = drmIoctl (allocator->priv->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
    if (err)
      GST_WARNING_OBJECT (allocator,
          "Failed to destroy dumb buffer object: %s %d", strerror (errno),
          errno);
  }
  g_free (mem->bo);
  mem->bo = NULL;
}

static gboolean
gst_kms_allocator_memory_create (GstKMSAllocator * allocator,
    GstKMSMemory * kmsmem, GstVideoInfo * vinfo)
{
  gint ret;
  struct drm_mode_create_dumb arg = { 0, };
  struct drm_mode_map_dumb mreq = { 0, };
  guint32 physaddress;
  guint32 fmt;

  if (kmsmem->bo)
    return TRUE;

  if (!check_fd (allocator))
    return FALSE;

  kmsmem->bo = g_malloc0 (sizeof (*kmsmem->bo));
  if (!kmsmem->bo)
    return FALSE;

  if (allocator->kmssink->Ismaster) {
    fmt = gst_drm_format_from_video (GST_VIDEO_INFO_FORMAT (vinfo));
    arg.bpp = gst_drm_bpp_from_drm (fmt);
    arg.width = GST_VIDEO_INFO_WIDTH (vinfo);
    arg.height = gst_drm_height_from_drm (fmt, GST_VIDEO_INFO_HEIGHT (vinfo));

    ret = drmIoctl (allocator->priv->fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
    if (ret)
      goto create_failed;

    kmsmem->bo->handle = arg.handle;
    kmsmem->bo->size = arg.size;
    kmsmem->bo->pitch = arg.pitch;
  } else {
    kmsmem->bo->handle = allocator->kmssink->gemhandle;
    kmsmem->bo->size = allocator->kmssink->gemsize;
  }

  /* Atmel specific IOCTL to get the physical address of gem object
   * This is very much required as the decoder API is expecting
   * physical address of buffer, Otherwise memcpy operation
   * is performed which affects the overall system performance
   */
  if (allocator->kmssink->zero_copy) {
    memset (&mreq, 0, sizeof (mreq));
    mreq.handle = kmsmem->bo->handle;
    ret = drmIoctl (allocator->priv->fd, DRM_IOCTL_ATMEL_GEM_GET, &mreq);
    if (ret) {
      GST_ERROR_OBJECT (allocator, "DRM buffer get physical address failed:"
          " %s %d", strerror (-ret), ret);
    }

    physaddress = (guint32) mreq.offset;
    gst_g1_gem_set_physical ((unsigned int) physaddress);
    GST_DEBUG_OBJECT (allocator, "Physaddress for PP = 0x%08x \n", physaddress);
  }
  return TRUE;

  /* ERRORS */
create_failed:
  {
    GST_ERROR_OBJECT (allocator, "Failed to create buffer object: %s (%d)",
        strerror (-ret), ret);
    g_free (kmsmem->bo);
    kmsmem->bo = NULL;
    return FALSE;
  }
}

static void
gst_kms_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstKMSAllocator *alloc;
  GstKMSMemory *kmsmem;

  alloc = GST_KMS_ALLOCATOR (allocator);
  kmsmem = (GstKMSMemory *) mem;

  gst_kms_allocator_memory_reset (alloc, kmsmem);
  g_slice_free (GstKMSMemory, kmsmem);
}

static void
gst_kms_allocator_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstKMSAllocator *alloc;

  alloc = GST_KMS_ALLOCATOR (object);

  switch (prop_id) {
    case PROP_DRM_FD:{
      int fd = g_value_get_int (value);
      if (fd > -1)
        alloc->priv->fd = dup (fd);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_kms_allocator_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstKMSAllocator *alloc;

  alloc = GST_KMS_ALLOCATOR (object);

  switch (prop_id) {
    case PROP_DRM_FD:
      g_value_set_int (value, alloc->priv->fd);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_kms_allocator_finalize (GObject * obj)
{
  GstKMSAllocator *alloc;

  alloc = GST_KMS_ALLOCATOR (obj);

  if (check_fd (alloc))
    close (alloc->priv->fd);

  G_OBJECT_CLASS (parent_class)->finalize (obj);
}

static void
gst_kms_allocator_class_init (GstKMSAllocatorClass * klass)
{
  GObjectClass *gobject_class;
  GstAllocatorClass *allocator_class;

  allocator_class = GST_ALLOCATOR_CLASS (klass);
  gobject_class = G_OBJECT_CLASS (klass);

  allocator_class->free = gst_kms_allocator_free;

  gobject_class->set_property = gst_kms_allocator_set_property;
  gobject_class->get_property = gst_kms_allocator_get_property;
  gobject_class->finalize = gst_kms_allocator_finalize;

  g_props[PROP_DRM_FD] = g_param_spec_int ("drm-fd", "DRM fd",
      "DRM file descriptor", -1, G_MAXINT, -1,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT);

  g_object_class_install_properties (gobject_class, PROP_N, g_props);
}

static gpointer
gst_kms_memory_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  GstKMSMemory *kmsmem;
  GstKMSAllocator *alloc;
  int err;
  gpointer out;
  struct drm_mode_map_dumb arg = { 0, };
  size_t size = 0;

  alloc = (GstKMSAllocator *) mem->allocator;

  if (!check_fd (alloc))
    return NULL;

  kmsmem = (GstKMSMemory *) mem;
  if (!kmsmem->bo)
    return NULL;

  arg.handle = kmsmem->bo->handle;
  size = kmsmem->bo->size;

  /* Reuse existing buffer object mapping if possible */
  if (kmsmem->bo->ptr != NULL) {
    goto out;
  }

  err = drmIoctl (alloc->priv->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
  if (err) {
    GST_ERROR_OBJECT (alloc, "Failed to get offset of buffer object: %s %d",
        strerror (-err), err);
    return NULL;
  }

  out = mmap (0, size, PROT_READ | PROT_WRITE, MAP_SHARED,
      alloc->priv->fd, arg.offset);
  if (out == MAP_FAILED) {
    GST_ERROR_OBJECT (alloc, "Failed to map dumb buffer object: %s %d",
        strerror (errno), errno);
    return NULL;
  }

  kmsmem->bo->ptr = out;

out:
  g_atomic_int_inc (&kmsmem->bo->refs);
  return kmsmem->bo->ptr;
}

static void
gst_kms_memory_unmap (GstMemory * mem)
{
  GstKMSMemory *kmsmem;

  if (!check_fd ((GstKMSAllocator *) mem->allocator))
    return;

  kmsmem = (GstKMSMemory *) mem;
  if (!kmsmem->bo)
    return;

  if (g_atomic_int_dec_and_test (&kmsmem->bo->refs)) {
    munmap (kmsmem->bo->ptr, kmsmem->bo->size);
    kmsmem->bo->ptr = NULL;
  }
}

static void
gst_kms_allocator_init (GstKMSAllocator * allocator)
{
  GstAllocator *alloc;

  alloc = GST_ALLOCATOR_CAST (allocator);

  allocator->priv = gst_kms_allocator_get_instance_private (allocator);
  allocator->priv->fd = -1;

  alloc->mem_type = GST_KMS_MEMORY_TYPE;
  alloc->mem_map = gst_kms_memory_map;
  alloc->mem_unmap = gst_kms_memory_unmap;
  /* Use the default, fallback copy function */

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

GstAllocator *
gst_kms_allocator_new (GstG1KMSSink * self)
{
  GstKMSAllocator *alloc = g_object_new (GST_TYPE_KMS_ALLOCATOR, "name",
      "KMSMemory::allocator", "drm-fd", self->fd, NULL);

  alloc->kmssink = self;

  return GST_ALLOCATOR_CAST (alloc);
}

/* The mem_offsets are relative to the GstMemory start, unlike the vinfo->offset
 * which are relative to the GstBuffer start. */
static gboolean
gst_kms_allocator_add_fb (GstKMSAllocator * alloc, GstKMSMemory * kmsmem,
    gsize mem_offsets[GST_VIDEO_MAX_PLANES], GstVideoInfo * vinfo)
{
  int i, ret;
  gint num_planes = GST_VIDEO_INFO_N_PLANES (vinfo);
  guint32 w, h, fmt, pitch = 0, bo_handles[4] = { 0, };
  guint32 offsets[4] = { 0, };
  guint32 pitches[4] = { 0, };
  struct drm_mode_map_dumb arg = { 0, };
  gpointer out;

  if (kmsmem->fb_id)
    return TRUE;

  w = GST_VIDEO_INFO_WIDTH (vinfo);
  h = GST_VIDEO_INFO_HEIGHT (vinfo);
  fmt = gst_drm_format_from_video (GST_VIDEO_INFO_FORMAT (vinfo));

  if (kmsmem->bo) {
    bo_handles[0] = kmsmem->bo->handle;
    for (i = 1; i < num_planes; i++)
      bo_handles[i] = bo_handles[0];

    /* Get the bo pitch calculated by the kms driver.
     * If it's defined, it will overwrite the video info's stride.
     * Since the API is completely undefined for planar formats,
     * only do this for interleaved formats.
     */
    if (num_planes == 1)
      pitch = kmsmem->bo->pitch;
  } else {
    for (i = 0; i < num_planes; i++)
      bo_handles[i] = kmsmem->gem_handle[i];
  }

  GST_DEBUG_OBJECT (alloc, "bo handles: %d, %d, %d, %d", bo_handles[0],
      bo_handles[1], bo_handles[2], bo_handles[3]);

  for (i = 0; i < num_planes; i++) {
    offsets[i] = mem_offsets[i];
    if (pitch)
      GST_VIDEO_INFO_PLANE_STRIDE (vinfo, i) = pitch;
    pitches[i] = GST_VIDEO_INFO_PLANE_STRIDE (vinfo, i);
    GST_DEBUG_OBJECT (alloc, "Create FB plane %i with stride %u and offset %u",
        i, pitches[i], offsets[i]);
  }

  if (alloc->kmssink->Ismaster) {
    ret = drmModeAddFB2 (alloc->priv->fd, w, h, fmt, bo_handles, pitches,
        offsets, &kmsmem->fb_id, 0);
    if (ret) {
      GST_ERROR_OBJECT (alloc, "Failed to bind to framebuffer: %s (%d)",
          strerror (-ret), ret);
      return FALSE;
    }
  }
  arg.handle = kmsmem->bo->handle;

  ret = drmIoctl (alloc->priv->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
  if (ret) {
    GST_ERROR_OBJECT (alloc, "Failed to get offset of buffer object: %s %d",
        strerror (-ret), ret);
    return FALSE;
  }

  out = mmap (0, kmsmem->bo->size,
      PROT_READ | PROT_WRITE, MAP_SHARED, alloc->priv->fd, arg.offset);
  if (out == MAP_FAILED) {
    GST_ERROR_OBJECT (alloc, "Failed to map dumb buffer object: %s %d",
        strerror (errno), errno);
    return FALSE;
  }

  return TRUE;
}

static GstMemory *
gst_kms_allocator_alloc_empty (GstAllocator * allocator, GstVideoInfo * vinfo)
{
  GstKMSMemory *kmsmem;
  GstMemory *mem;

  kmsmem = g_slice_new0 (GstKMSMemory);
  if (!kmsmem)
    return NULL;
  mem = GST_MEMORY_CAST (kmsmem);

  gst_memory_init (mem, GST_MEMORY_FLAG_NO_SHARE, allocator, NULL,
      GST_VIDEO_INFO_SIZE (vinfo), 0, 0, GST_VIDEO_INFO_SIZE (vinfo));

  return mem;
}

GstMemory *
gst_kms_allocator_bo_alloc (GstAllocator * allocator, GstVideoInfo * vinfo)
{
  GstKMSAllocator *alloc;
  GstKMSMemory *kmsmem;
  GstMemory *mem;
  GstG1KMSSink *self;
  GstVideoRectangle src = { 0, };
  GstVideoRectangle dst = { 0, };
  GstVideoRectangle result;
  gint ret;

  mem = gst_kms_allocator_alloc_empty (allocator, vinfo);
  if (!mem)
    return NULL;

  alloc = GST_KMS_ALLOCATOR (allocator);
  kmsmem = (GstKMSMemory *) mem;
  self = alloc->kmssink;

  if (!gst_kms_allocator_memory_create (alloc, kmsmem, vinfo))
    goto fail;
  if (!gst_kms_allocator_add_fb (alloc, kmsmem, vinfo->offset, vinfo))
    goto fail;

  if (self->Ismaster) {
    src.w = GST_VIDEO_SINK_WIDTH (self);
    src.h = GST_VIDEO_SINK_HEIGHT (self);

    dst.w = self->hdisplay;
    dst.h = self->vdisplay;

  retry_set_plane:
    gst_video_sink_center_rect (src, dst, &result, self->can_scale);

    src.w = GST_VIDEO_INFO_WIDTH (&self->vinfo);
    src.h = GST_VIDEO_INFO_HEIGHT (&self->vinfo);

    GST_TRACE_OBJECT (alloc,
        "drmModeSetPlane at (%i,%i) %ix%i sourcing at (%i,%i) %ix%i",
        result.x, result.y, result.w, result.h, src.x, src.y, src.w, src.h);

    /* source/cropping coordinates are given in Q16 */
    ret = drmModeSetPlane (self->fd, self->plane_id, self->crtc_id,
        kmsmem->fb_id, 0, result.x, result.y, result.w, result.h,
        src.x << 16, src.y << 16, src.w << 16, src.h << 16);
    if (ret) {
      if (self->can_scale) {
        self->can_scale = FALSE;
        goto retry_set_plane;
      }
      GST_DEBUG_OBJECT (alloc, "result = { %d, %d, %d, %d} / "
          "src = { %d, %d, %d %d } / dst = { %d, %d, %d %d }", result.x,
          result.y, result.w, result.h, src.x, src.y, src.w, src.h, dst.x,
          dst.y, dst.w, dst.h);
      GST_ELEMENT_ERROR (alloc, RESOURCE, FAILED, (NULL),
          ("drmModeSetPlane failed: %s (%d)", strerror (-ret), ret));
      goto fail;
    }
  }

  return mem;

  /* ERRORS */
fail:
  gst_memory_unref (mem);
  return NULL;
}

GstKMSMemory *
gst_kms_allocator_dmabuf_import (GstAllocator * allocator, gint * prime_fds,
    gint n_planes, gsize offsets[GST_VIDEO_MAX_PLANES], GstVideoInfo * vinfo)
{
  GstKMSAllocator *alloc;
  GstMemory *mem;
  GstKMSMemory *tmp;
  gint i, ret;

  g_return_val_if_fail (n_planes <= GST_VIDEO_MAX_PLANES, FALSE);

  mem = gst_kms_allocator_alloc_empty (allocator, vinfo);
  if (!mem)
    return FALSE;

  tmp = (GstKMSMemory *) mem;
  alloc = GST_KMS_ALLOCATOR (allocator);
  for (i = 0; i < n_planes; i++) {
    ret = drmPrimeFDToHandle (alloc->priv->fd, prime_fds[i],
        &tmp->gem_handle[i]);
    if (ret)
      goto import_fd_failed;
  }

  if (!gst_kms_allocator_add_fb (alloc, tmp, offsets, vinfo))
    goto failed;

  for (i = 0; i < n_planes; i++) {
    struct drm_gem_close arg = { tmp->gem_handle[i], };
    gint err;

    err = drmIoctl (alloc->priv->fd, DRM_IOCTL_GEM_CLOSE, &arg);
    if (err)
      GST_WARNING_OBJECT (allocator,
          "Failed to close GEM handle: %s %d", strerror (errno), errno);

    tmp->gem_handle[i] = 0;
  }

  return tmp;

  /* ERRORS */
import_fd_failed:
  {
    GST_ERROR_OBJECT (alloc, "Failed to import prime fd %d: %s (%d)",
        prime_fds[i], strerror (-ret), ret);
    /* fallback */
  }

failed:
  {
    gst_memory_unref (mem);
    return NULL;
  }
}
