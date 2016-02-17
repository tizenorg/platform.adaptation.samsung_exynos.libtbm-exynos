/**************************************************************************

libtbm_dumb

Copyright 2012 Samsung Electronics co., Ltd. All Rights Reserved.

Contact: SooChan Lim <sc1.lim@samsung.com>, Sangjin Lee <lsj119@samsung.com>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <xf86drm.h>
#include <tbm_bufmgr.h>
#include <tbm_bufmgr_backend.h>
#include <pthread.h>
#include <tbm_surface.h>
#include <tbm_surface_internal.h>
#include "tbm_wayland.h"

#define DEBUG
#define USE_DMAIMPORT
#define TBM_COLOR_FORMAT_COUNT 8

#ifdef DEBUG
#define LOG_TAG    "TBM_BACKEND"
#include <dlog.h>
static int bDebug=0;

char* target_name()
{
    FILE *f;
    char *slash;
    static int     initialized = 0;
    static char app_name[128];

    if ( initialized )
        return app_name;

    /* get the application name */
    f = fopen("/proc/self/cmdline", "r");

    if ( !f )
    {
        return 0;
    }

    memset(app_name, 0x00, sizeof(app_name));

    if ( fgets(app_name, 100, f) == NULL )
    {
        fclose(f);
        return 0;
    }

    fclose(f);

    if ( (slash=strrchr(app_name, '/')) != NULL )
    {
        memmove(app_name, slash+1, strlen(slash));
    }

    initialized = 1;

    return app_name;
}
#define TBM_DUMB_LOG(fmt, args...) LOGE("\033[31m"  "[%s]" fmt "\033[0m", target_name(), ##args)
#define DBG(fmt, args...)  if(bDebug&01) LOGE(fmt, ##args)
#else
#define TBM_DUMB_LOG(...)
#define DBG(...)
#endif

#define SIZE_ALIGN( value, base ) (((value) + ((base) - 1)) & ~((base) - 1))

#define TBM_SURFACE_ALIGNMENT_PLANE (64)
#define TBM_SURFACE_ALIGNMENT_PITCH_RGB (128)
#define TBM_SURFACE_ALIGNMENT_PITCH_YUV (16)


/* check condition */
#define DUMB_RETURN_IF_FAIL(cond) {\
    if (!(cond)) {\
        TBM_DUMB_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return;\
    }\
}
#define DUMB_RETURN_VAL_IF_FAIL(cond, val) {\
    if (!(cond)) {\
        TBM_DUMB_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return val;\
    }\
}

struct dma_buf_info {
    unsigned long   size;
    unsigned int    fence_supported;
    unsigned int    padding;
};

#define DMA_BUF_ACCESS_READ     0x1
#define DMA_BUF_ACCESS_WRITE    0x2
#define DMA_BUF_ACCESS_DMA      0x4
#define DMA_BUF_ACCESS_MAX      0x8

#define DMA_FENCE_LIST_MAX      5

struct dma_buf_fence {
    unsigned long       ctx;
    unsigned int        type;
};

#define DMABUF_IOCTL_BASE    'F'
#define DMABUF_IOWR(nr, type)   _IOWR(DMABUF_IOCTL_BASE, nr, type)

#define DMABUF_IOCTL_GET_INFO   DMABUF_IOWR(0x00, struct dma_buf_info)
#define DMABUF_IOCTL_GET_FENCE  DMABUF_IOWR(0x01, struct dma_buf_fence)
#define DMABUF_IOCTL_PUT_FENCE  DMABUF_IOWR(0x02, struct dma_buf_fence)

typedef struct _tbm_bufmgr_dumb *tbm_bufmgr_dumb;
typedef struct _tbm_bo_dumb *tbm_bo_dumb;

typedef struct _dumb_private
{
    int ref_count;
} PrivGem;

/* tbm buffor object for dumb */
struct _tbm_bo_dumb
{
    int fd;

    unsigned int name;    /* FLINK ID */

    unsigned int gem;     /* GEM Handle */

    unsigned int dmabuf;  /* fd for dmabuf */

    void *pBase;          /* virtual address */

    unsigned int size;

    unsigned int flags_dumb;
    unsigned int flags_tbm;

    PrivGem* private;

    pthread_mutex_t mutex;
    struct dma_buf_fence dma_fence[DMA_FENCE_LIST_MAX];
    int device;
    int opt;
};

/* tbm bufmgr private for dumb */
struct _tbm_bufmgr_dumb
{
    int fd;
    void* hashBos;

    int use_dma_fence;

    int fd_owner;
};

char *STR_DEVICE[]=
{
    "DEF",
    "CPU",
    "2D",
    "3D",
    "MM"
};

char *STR_OPT[]=
{
    "NONE",
    "RD",
    "WR",
    "RDWR"
};


uint32_t tbm_dumb_color_format_list[TBM_COLOR_FORMAT_COUNT] = {   TBM_FORMAT_RGBA8888,
                                                                        TBM_FORMAT_BGRA8888,
                                                                        TBM_FORMAT_RGBX8888,
                                                                        TBM_FORMAT_RGB888,
                                                                        TBM_FORMAT_NV12,
                                                                        TBM_FORMAT_NV21,
                                                                        TBM_FORMAT_YUV420,
                                                                        TBM_FORMAT_YVU420 };


static unsigned int
_get_dumb_flag_from_tbm (unsigned int ftbm)
{
    unsigned int flags = 0;
    return flags;
}

static unsigned int
_get_tbm_flag_from_dumb (unsigned int fdumb)
{
    unsigned int flags = 0;

    flags |= TBM_BO_SCANOUT;
    flags |= TBM_BO_NONCACHABLE;

    return flags;
}

static unsigned int
_get_name (int fd, unsigned int gem)
{
    struct drm_gem_flink arg = {0,};

    arg.handle = gem;
    if (drmIoctl (fd, DRM_IOCTL_GEM_FLINK, &arg))
    {
        TBM_DUMB_LOG ("error fail to get flink from gem:%d (DRM_IOCTL_GEM_FLINK)\n",
                gem);
        return 0;
    }

    return (unsigned int)arg.name;
}

static tbm_bo_handle
_dumb_bo_handle (tbm_bo_dumb bo_dumb, int device)
{
    tbm_bo_handle bo_handle;
    memset (&bo_handle, 0x0, sizeof (uint64_t));

    switch(device)
    {
    case TBM_DEVICE_DEFAULT:
    case TBM_DEVICE_2D:
        bo_handle.u32 = (uint32_t)bo_dumb->gem;
        break;
    case TBM_DEVICE_CPU:
        if (!bo_dumb->pBase)
        {
            struct drm_mode_map_dumb arg = {0,};
            void *map = NULL;

            arg.handle = bo_dumb->gem;
            if (drmIoctl (bo_dumb->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg))
            {
               TBM_DUMB_LOG ("error Cannot map_ gem=%d\n", bo_dumb->gem);
               return (tbm_bo_handle) NULL;
            }

            map = mmap (NULL, bo_dumb->size, PROT_READ|PROT_WRITE, MAP_SHARED,
                              bo_dumb->fd, arg.offset);
            if (map == MAP_FAILED)
            {
                TBM_DUMB_LOG ("error Cannot usrptr gem=%d\n", bo_dumb->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_dumb->pBase = map;
        }
        bo_handle.ptr = (void *)bo_dumb->pBase;
        break;
    case TBM_DEVICE_3D:
#ifdef USE_DMAIMPORT
        if (!bo_dumb->dmabuf)
        {
            struct drm_prime_handle arg = {0, };

            arg.handle = bo_dumb->gem;
            if (drmIoctl (bo_dumb->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
                TBM_DUMB_LOG ("error Cannot dmabuf=%d\n", bo_dumb->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_dumb->dmabuf = arg.fd;
        }

        bo_handle.u32 = (uint32_t)bo_dumb->dmabuf;
#endif
        break;
    case TBM_DEVICE_MM:
        if (!bo_dumb->dmabuf)
        {
            struct drm_prime_handle arg = {0, };

            arg.handle = bo_dumb->gem;
            if (drmIoctl (bo_dumb->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
                TBM_DUMB_LOG ("error Cannot dmabuf=%d\n", bo_dumb->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_dumb->dmabuf = arg.fd;
        }

        bo_handle.u32 = (uint32_t)bo_dumb->dmabuf;
        break;
    default:
        TBM_DUMB_LOG ("error Not supported device:%d\n", device);
        bo_handle.ptr = (void *) NULL;
        break;
    }

    return bo_handle;
}

#ifdef USE_CACHE
static int
_dumb_cache_flush (int fd, tbm_bo_dumb bo_dumb, int flags)
{
    TBM_DUMB_LOG ("warning fail to flush the cache.\n");
    return 1;
}
#endif

static int
tbm_dumb_bo_size (tbm_bo bo)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);

    return bo_dumb->size;
}

static void *
tbm_dumb_bo_alloc (tbm_bo bo, int size, int flags)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_dumb bo_dumb;
    tbm_bufmgr_dumb bufmgr_dumb;
    unsigned int dumb_flags;

    bufmgr_dumb = (tbm_bufmgr_dumb)tbm_backend_get_bufmgr_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bufmgr_dumb!=NULL, 0);

    bo_dumb = calloc (1, sizeof(struct _tbm_bo_dumb));
    if (!bo_dumb)
    {
        TBM_DUMB_LOG ("error fail to allocate the bo private\n");
        return 0;
    }

    dumb_flags = _get_dumb_flag_from_tbm (flags);

    struct drm_mode_create_dumb arg = {0, };
    //as we know only size for new bo set height=1 and bpp=8 and in this case
    //width will by equal to size in bytes;
    arg.height = 1;
    arg.bpp = 8;
    arg.width = size;
    arg.flags = dumb_flags;
    if (drmIoctl (bufmgr_dumb->fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg))
    {
        TBM_DUMB_LOG ("error Cannot create bo(flag:%x, size:%d)\n", arg.flags, (unsigned int)size);
        free (bo_dumb);
        return 0;
    }

    bo_dumb->fd = bufmgr_dumb->fd;
    bo_dumb->gem = arg.handle;
    bo_dumb->size = arg.size;
    bo_dumb->flags_tbm = flags;
    bo_dumb->flags_dumb = dumb_flags;
    bo_dumb->name = _get_name (bo_dumb->fd, bo_dumb->gem);

    pthread_mutex_init(&bo_dumb->mutex, NULL);

    if (bufmgr_dumb->use_dma_fence
        && !bo_dumb->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_dumb->gem;
        if (drmIoctl (bo_dumb->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_DUMB_LOG ("error Cannot dmabuf=%d\n", bo_dumb->gem);
            free (bo_dumb);
            return 0;
        }
        bo_dumb->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem* privGem = calloc (1, sizeof(PrivGem));
    if (!privGem)
    {
        TBM_DUMB_LOG ("[libtbm-dumb:%d] "
                "error %s:%d Fail to calloc privGem\n",
                getpid(), __FUNCTION__, __LINE__);
        free (bo_dumb);
        return 0;
    }

    privGem->ref_count = 1;
    if (drmHashInsert(bufmgr_dumb->hashBos, bo_dumb->name, (void *)privGem) < 0)
    {
        TBM_DUMB_LOG ("error Cannot insert bo to Hash(%d)\n", bo_dumb->name);
    }

    DBG ("     [%s] bo:%p, gem:%d(%d), flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         flags, dumb_flags,
         bo_dumb->size);

    return (void *)bo_dumb;
}

static void
tbm_dumb_bo_free(tbm_bo bo)
{
    tbm_bo_dumb bo_dumb;
    tbm_bufmgr_dumb bufmgr_dumb;

    if (!bo)
        return;

    bufmgr_dumb = (tbm_bufmgr_dumb)tbm_backend_get_bufmgr_priv(bo);
    DUMB_RETURN_IF_FAIL (bufmgr_dumb!=NULL);

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_IF_FAIL (bo_dumb!=NULL);

    DBG ("      [%s] bo:%p, gem:%d(%d), fd:%d, size:%d\n",target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         bo_dumb->dmabuf,
         bo_dumb->size);

    if (bo_dumb->pBase)
    {
        if (munmap(bo_dumb->pBase, bo_dumb->size) == -1)
        {
            TBM_DUMB_LOG ("error bo:%p fail to munmap(%s)\n",
                bo, strerror(errno));
        }
    }

    /* close dmabuf */
    if (bo_dumb->dmabuf)
    {
        close (bo_dumb->dmabuf);
        bo_dumb->dmabuf = 0;
    }

    /* delete bo from hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_dumb->hashBos, bo_dumb->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count--;
        if (privGem->ref_count == 0)
        {
            drmHashDelete (bufmgr_dumb->hashBos, bo_dumb->name);
            free (privGem);
            privGem = NULL;
        }
    }
    else
    {
        TBM_DUMB_LOG ("warning Cannot find bo to Hash(%d), ret=%d\n", bo_dumb->name, ret);
    }

    /* Free gem handle */
    struct drm_gem_close arg = {0, };
    memset (&arg, 0, sizeof(arg));
    arg.handle = bo_dumb->gem;
    if (drmIoctl (bo_dumb->fd, DRM_IOCTL_GEM_CLOSE, &arg))
    {
        TBM_DUMB_LOG ("error bo:%p fail to gem close.(%s)\n",
            bo, strerror(errno));
    }

    free (bo_dumb);
}


static void *
tbm_dumb_bo_import (tbm_bo bo, unsigned int key)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_dumb bufmgr_dumb;
    tbm_bo_dumb bo_dumb;

    bufmgr_dumb = (tbm_bufmgr_dumb)tbm_backend_get_bufmgr_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bufmgr_dumb!=NULL, 0);

    struct drm_gem_open arg = {0, };

    arg.name = key;
    if (drmIoctl(bufmgr_dumb->fd, DRM_IOCTL_GEM_OPEN, &arg))
    {
        TBM_DUMB_LOG ("error Cannot open gem name=%d\n", key);
        return 0;
    }

    bo_dumb = calloc (1, sizeof(struct _tbm_bo_dumb));
    if (!bo_dumb)
    {
        TBM_DUMB_LOG ("error fail to allocate the bo private\n");
        return 0;
    }

    bo_dumb->fd = bufmgr_dumb->fd;
    bo_dumb->gem = arg.handle;
    bo_dumb->size = arg.size;
    bo_dumb->flags_dumb = 0;
    bo_dumb->name = key;
    bo_dumb->flags_tbm = _get_tbm_flag_from_dumb (bo_dumb->flags_dumb);

    if (!bo_dumb->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_dumb->gem;
        if (drmIoctl (bo_dumb->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_DUMB_LOG ("error Cannot dmabuf=%d\n", bo_dumb->gem);
            free (bo_dumb);
            return 0;
        }
        bo_dumb->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_dumb->hashBos, bo_dumb->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count++;
    }
    else if (ret == 1)
    {
        privGem = calloc (1, sizeof(PrivGem));
        if (!privGem)
        {
            TBM_DUMB_LOG ("error Fail to calloc privGem\n");
            free (bo_dumb);
            return 0;
        }

        privGem->ref_count = 1;
        if (drmHashInsert (bufmgr_dumb->hashBos, bo_dumb->name, (void *)privGem) < 0)
        {
            TBM_DUMB_LOG ("error Cannot insert bo to Hash(%d)\n", bo_dumb->name);
        }
    }
    else
    {
        TBM_DUMB_LOG ("error Cannot insert bo to Hash(%d)\n", bo_dumb->name);
    }

    DBG ("    [%s] bo:%p, gem:%d(%d), fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         bo_dumb->dmabuf,
         bo_dumb->flags_tbm, bo_dumb->flags_dumb,
         bo_dumb->size);

    return (void *)bo_dumb;
}

static void *
tbm_dumb_bo_import_fd (tbm_bo bo, tbm_fd key)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_dumb bufmgr_dumb;
    tbm_bo_dumb bo_dumb;

    bufmgr_dumb = (tbm_bufmgr_dumb)tbm_backend_get_bufmgr_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bufmgr_dumb!=NULL, 0);

    unsigned int gem = 0;
    unsigned int name = 0;
    unsigned int real_size = -1;

    //getting handle from fd
    struct drm_prime_handle arg = {0, };
    struct drm_gem_open gem_open = {0, };

    arg.fd = key;
    arg.flags = 0;
    if (drmIoctl (bufmgr_dumb->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg))
    {
        TBM_DUMB_LOG ("error bo:%p Cannot get gem handle from fd:%d (%s)\n",
                 bo, arg.fd, strerror(errno));
        return NULL;
    }
    gem = arg.handle;

    /* Determine size of bo.  The fd-to-handle ioctl really should
     * return the size, but it doesn't.  If we have kernel 3.12 or
     * later, we can lseek on the prime fd to get the size.  Older
     * kernels will just fail, in which case we fall back to the
     * provided (estimated or guess size). */
    real_size = lseek(key, 0, SEEK_END);

    name = _get_name(bufmgr_dumb->fd, gem);
    if (name == 0)
    {
        TBM_DUMB_LOG ("error bo:%p Cannot get name from gem:%d, fd:%d (%s)\n",
            bo, gem, key, strerror(errno));
        return 0;
    }    

    /* Open the same GEM object only for finding out its size */
    gem_open.name = name;
    if (drmIoctl(bufmgr_dumb->fd, DRM_IOCTL_GEM_OPEN, &gem_open))
    {
        TBM_DUMB_LOG ("error Cannot open gem name=%d\n", key);
        return 0;
    }
    /* Free gem handle to avoid a memory leak*/
    struct drm_gem_close gem_close;
    gem_close.handle = gem_open.handle;
    if (drmIoctl (bufmgr_dumb->fd, DRM_IOCTL_GEM_CLOSE, &gem_close))
    {
        TBM_DUMB_LOG ("error bo:%p fail to gem close.(%s)\n",
            bo, strerror(errno));
    }

    if (real_size == -1)
        real_size = gem_open.size;

    bo_dumb = calloc (1, sizeof(struct _tbm_bo_dumb));
    if (!bo_dumb)
    {
        TBM_DUMB_LOG ("error bo:%p fail to allocate the bo private\n", bo);
        return 0;
    }

    bo_dumb->fd = bufmgr_dumb->fd;
    bo_dumb->gem = gem;
    bo_dumb->dmabuf = key;
    bo_dumb->size = real_size;
    bo_dumb->flags_dumb = 0;
    bo_dumb->flags_tbm = _get_tbm_flag_from_dumb (bo_dumb->flags_dumb);
    bo_dumb->name = name;

    /* add bo to hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_dumb->hashBos, bo_dumb->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count++;
    }
    else if (ret == 1)
    {
        privGem = calloc (1, sizeof(PrivGem));
        if (!privGem)
        {
            TBM_DUMB_LOG ("error Fail to calloc privGem\n");
            free (bo_dumb);
            return 0;
        }

        privGem->ref_count = 1;
        if (drmHashInsert (bufmgr_dumb->hashBos, bo_dumb->name, (void *)privGem) < 0)
        {
            TBM_DUMB_LOG ("error bo:%p Cannot insert bo to Hash(%d) from gem:%d, fd:%d\n",
                bo, bo_dumb->name, gem, key);
        }
    }
    else
    {
        TBM_DUMB_LOG ("error bo:%p Cannot insert bo to Hash(%d) from gem:%d, fd:%d\n",
                bo, bo_dumb->name, gem, key);
    }

    DBG (" [%s] bo:%p, gem:%d(%d), fd:%d, key_fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         bo_dumb->dmabuf,
         key,
         bo_dumb->flags_tbm, bo_dumb->flags_dumb,
         bo_dumb->size);

    return (void *)bo_dumb;
}

static unsigned int
tbm_dumb_bo_export (tbm_bo bo)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, 0);

    if (!bo_dumb->name)
    {
        bo_dumb->name = _get_name(bo_dumb->fd, bo_dumb->gem);
        if (!bo_dumb->name)
        {
            TBM_DUMB_LOG ("error Cannot get name\n");
            return 0;
        }
    }

    DBG ("    [%s] bo:%p, gem:%d(%d), fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         bo_dumb->dmabuf,
         bo_dumb->flags_tbm, bo_dumb->flags_dumb,
         bo_dumb->size);

    return (unsigned int)bo_dumb->name;
}

tbm_fd
tbm_dumb_bo_export_fd (tbm_bo bo)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, -1);

    tbm_bo_dumb bo_dumb;
    int ret;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, -1);

    struct drm_prime_handle arg = {0, };

    arg.handle = bo_dumb->gem;
    ret = drmIoctl (bo_dumb->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg);
    if (ret)
    {
        TBM_DUMB_LOG ("error bo:%p Cannot dmabuf=%d (%s)\n",
            bo, bo_dumb->gem, strerror(errno));
        return (tbm_fd) ret;
    }

    DBG (" [%s] bo:%p, gem:%d(%d), fd:%d, key_fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         bo_dumb->dmabuf,
         arg.fd,
         bo_dumb->flags_tbm, bo_dumb->flags_dumb,
         bo_dumb->size);

    return (tbm_fd)arg.fd;
}

static tbm_bo_handle
tbm_dumb_bo_get_handle (tbm_bo bo, int device)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, (tbm_bo_handle) NULL);

    if (!bo_dumb->gem)
    {
        TBM_DUMB_LOG ("error Cannot map gem=%d\n", bo_dumb->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("[%s] bo:%p, gem:%d(%d), fd:%d, flags:%d(%d), size:%d, %s\n", target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         bo_dumb->dmabuf,
         bo_dumb->flags_tbm, bo_dumb->flags_dumb,
         bo_dumb->size,
         STR_DEVICE[device]);

    /*Get mapped bo_handle*/
    bo_handle = _dumb_bo_handle (bo_dumb, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_DUMB_LOG ("error Cannot get handle: gem:%d, device:%d\n", bo_dumb->gem, device);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static tbm_bo_handle
tbm_dumb_bo_map (tbm_bo bo, int device, int opt)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, (tbm_bo_handle) NULL);

    if (!bo_dumb->gem)
    {
        TBM_DUMB_LOG ("error Cannot map gem=%d\n", bo_dumb->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("       [%s] bo:%p, gem:%d(%d), fd:%d, %s, %s\n", target_name(),
         bo,
         bo_dumb->gem, bo_dumb->name,
         bo_dumb->dmabuf,
         STR_DEVICE[device],
         STR_OPT[opt]);

    /*Get mapped bo_handle*/
    bo_handle = _dumb_bo_handle (bo_dumb, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_DUMB_LOG ("error Cannot get handle: gem:%d, device:%d, opt:%d\n", bo_dumb->gem, device, opt);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static int
tbm_dumb_bo_unmap (tbm_bo bo)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, 0);

    if (!bo_dumb->gem)
        return 0;

    DBG ("     [%s] bo:%p, gem:%d(%d), fd:%d\n", target_name(),
          bo,
          bo_dumb->gem, bo_dumb->name,
          bo_dumb->dmabuf);

    return 1;
}

static int
tbm_dumb_bo_cache_flush (tbm_bo bo, int flags)
{
    tbm_bufmgr_dumb bufmgr_dumb = (tbm_bufmgr_dumb)tbm_backend_get_bufmgr_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bufmgr_dumb!=NULL, 0);

    /* cache flush is managed by kernel side when using dma-fence. */
    if (bufmgr_dumb->use_dma_fence)
       return 1;

    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, 0);

#ifdef USE_CACHE
    if (!_dumb_cache_flush(bo_dumb->fd, bo_dumb, flags))
        return 0;
#endif

    return 1;
}

static int
tbm_dumb_bo_get_global_key (tbm_bo bo)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, 0);

    if (!bo_dumb->name)
    {
        if (!bo_dumb->gem)
            return 0;

        bo_dumb->name = _get_name(bo_dumb->fd, bo_dumb->gem);
    }

    return bo_dumb->name;
}

static int
tbm_dumb_bo_lock(tbm_bo bo, int device, int opt)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

#if USE_BACKEND_LOCK
    tbm_bufmgr_dumb bufmgr_dumb;
    tbm_bo_dumb bo_dumb;
    struct dma_buf_fence fence;
    struct flock filelock;
    int ret=0;

    if (device != TBM_DEVICE_3D && device != TBM_DEVICE_CPU)
    {
        DBG ("[libtbm-dumb:%d] %s not support device type,\n", getpid(), __FUNCTION__);
        return 0;
    }

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, 0);

    bufmgr_dumb = (tbm_bufmgr_dumb)tbm_backend_get_bufmgr_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bufmgr_dumb!=NULL, 0);

    memset(&fence, 0, sizeof(struct dma_buf_fence));

    /* Check if the given type is valid or not. */
    if (opt & TBM_OPTION_WRITE)
    {
        if (device == TBM_DEVICE_3D)
            fence.type = DMA_BUF_ACCESS_WRITE | DMA_BUF_ACCESS_DMA;
    }
    else if (opt & TBM_OPTION_READ)
    {
        if (device == TBM_DEVICE_3D)
            fence.type = DMA_BUF_ACCESS_READ | DMA_BUF_ACCESS_DMA;
    }
    else
    {
        TBM_DUMB_LOG ("error Invalid argument\n");
        return 0;
    }

    /* Check if the tbm manager supports dma fence or not. */
    if (!bufmgr_dumb->use_dma_fence)
    {
        TBM_DUMB_LOG ("error Not support DMA FENCE(%s)\n", strerror(errno) );
        return 0;

    }

    if (device == TBM_DEVICE_3D)
    {
        ret = ioctl(bo_dumb->dmabuf, DMABUF_IOCTL_GET_FENCE, &fence);
        if (ret < 0)
        {
            TBM_DUMB_LOG ("error Cannot set GET FENCE(%s)\n", strerror(errno) );
            return 0;
        }
    }
    else
    {
        if (opt & TBM_OPTION_WRITE)
            filelock.l_type = F_WRLCK;
        else
            filelock.l_type = F_RDLCK;

        filelock.l_whence = SEEK_CUR;
        filelock.l_start = 0;
        filelock.l_len = 0;

        if (-1 == fcntl(bo_dumb->dmabuf, F_SETLKW, &filelock))
        {
            return 0;
        }
    }

    pthread_mutex_lock(&bo_dumb->mutex);

    if (device == TBM_DEVICE_3D)
    {
        int i;
        for (i = 0; i < DMA_FENCE_LIST_MAX; i++)
        {
            if (bo_dumb->dma_fence[i].ctx == 0)
            {
                bo_dumb->dma_fence[i].type = fence.type;
                bo_dumb->dma_fence[i].ctx = fence.ctx;
                break;
            }
        }

        if (i == DMA_FENCE_LIST_MAX)
        {
            //TODO: if dma_fence list is full, it needs realloc. I will fix this. by minseok3.kim
            TBM_DUMB_LOG ("error fence list is full\n");
        }
    }

    pthread_mutex_unlock(&bo_dumb->mutex);

    DBG ("[%s] DMABUF_IOCTL_GET_FENCE! bo:%p, gem:%d(%d), fd:%ds\n", target_name(),
          bo,
          bo_dumb->gem, bo_dumb->name,
          bo_dumb->dmabuf);

#endif
    return 1;
}

static int
tbm_dumb_bo_unlock(tbm_bo bo)
{
    DUMB_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

#if USE_BACKEND_LOCK
    tbm_bo_dumb bo_dumb;
    struct dma_buf_fence fence;
    struct flock filelock;
    unsigned int dma_type = 0;
    int ret=0;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb!=NULL, 0);

    if (bo_dumb->dma_fence[0].type & DMA_BUF_ACCESS_DMA)
        dma_type = 1;

    if (!bo_dumb->dma_fence[0].ctx && dma_type)
    {
        DBG ("error FENCE not support or ignored,\n");
        return 0;
    }

    if (!bo_dumb->dma_fence[0].ctx && dma_type)
    {
        DBG ("error device type is not 3D/CPU,\n");
        return 0;
    }

    pthread_mutex_lock(&bo_dumb->mutex);

    if (dma_type)
    {
        fence.type = bo_dumb->dma_fence[0].type;
        fence.ctx = bo_dumb->dma_fence[0].ctx;
        int i;
        for (i = 1; i < DMA_FENCE_LIST_MAX; i++)
        {
            bo_dumb->dma_fence[i-1].type = bo_dumb->dma_fence[i].type;
            bo_dumb->dma_fence[i-1].ctx = bo_dumb->dma_fence[i].ctx;
        }
        bo_dumb->dma_fence[DMA_FENCE_LIST_MAX-1].type = 0;
        bo_dumb->dma_fence[DMA_FENCE_LIST_MAX-1].ctx = 0;
    }
    pthread_mutex_unlock(&bo_dumb->mutex);

    if (dma_type)
    {
        ret = ioctl(bo_dumb->dmabuf, DMABUF_IOCTL_PUT_FENCE, &fence);
        if (ret < 0)
        {
            TBM_DUMB_LOG ("error Can not set PUT FENCE(%s)\n", strerror(errno));
            return 0;
        }
    }
    else
    {
        filelock.l_type = F_UNLCK;
        filelock.l_whence = SEEK_CUR;
        filelock.l_start = 0;
        filelock.l_len = 0;

        if (-1 == fcntl(bo_dumb->dmabuf, F_SETLKW, &filelock))
        {
            return 0;
        }
    }

    DBG ("[%s] DMABUF_IOCTL_PUT_FENCE! bo:%p, gem:%d(%d), fd:%ds\n", target_name(),
          bo,
          bo_dumb->gem, bo_dumb->name,
          bo_dumb->dmabuf);

#endif
    return 1;
}

static void
tbm_dumb_bufmgr_deinit (void *priv)
{
    DUMB_RETURN_IF_FAIL (priv!=NULL);

    tbm_bufmgr_dumb bufmgr_dumb;

    bufmgr_dumb = (tbm_bufmgr_dumb)priv;

    if (bufmgr_dumb->hashBos)
    {
        unsigned long key;
        void *value;

        while (drmHashFirst(bufmgr_dumb->hashBos, &key, &value) > 0)
        {
            free (value);
            drmHashDelete (bufmgr_dumb->hashBos, key);
        }

        drmHashDestroy (bufmgr_dumb->hashBos);
        bufmgr_dumb->hashBos = NULL;
    }

    if (bufmgr_dumb->fd_owner)
        close (bufmgr_dumb->fd);

    free (bufmgr_dumb);
}

int
tbm_dumb_surface_supported_format(uint32_t **formats, uint32_t *num)
{
    uint32_t* color_formats=NULL;

    color_formats = (uint32_t*)calloc (1,sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT);

    if( color_formats == NULL )
    {
        return 0;
    }
    memcpy( color_formats, tbm_dumb_color_format_list , sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT );


    *formats = color_formats;
    *num = TBM_COLOR_FORMAT_COUNT;

    return 1;
}


/**
 * @brief get the plane data of the surface.
 * @param[in] surface : the surface
 * @param[in] width : the width of the surface
 * @param[in] height : the height of the surface
 * @param[in] format : the format of the surface
 * @param[in] plane_idx : the format of the surface
 * @param[out] size : the size of the plane
 * @param[out] offset : the offset of the plane
 * @param[out] pitch : the pitch of the plane
 * @param[out] padding : the padding of the plane
 * @return 1 if this function succeeds, otherwise 0.
 */
int
tbm_dumb_surface_get_plane_data(tbm_surface_h surface, int width, int height, tbm_format format, int plane_idx, uint32_t *size, uint32_t *offset, uint32_t *pitch, int *bo_idx)
{
    int ret = 1;
    int bpp;
    int _offset =0;
    int _pitch =0;
    int _size =0;
    int _bo_idx = 0;

    switch(format)
    {
        /* 16 bpp RGB */
        case TBM_FORMAT_XRGB4444:
        case TBM_FORMAT_XBGR4444:
        case TBM_FORMAT_RGBX4444:
        case TBM_FORMAT_BGRX4444:
        case TBM_FORMAT_ARGB4444:
        case TBM_FORMAT_ABGR4444:
        case TBM_FORMAT_RGBA4444:
        case TBM_FORMAT_BGRA4444:
        case TBM_FORMAT_XRGB1555:
        case TBM_FORMAT_XBGR1555:
        case TBM_FORMAT_RGBX5551:
        case TBM_FORMAT_BGRX5551:
        case TBM_FORMAT_ARGB1555:
        case TBM_FORMAT_ABGR1555:
        case TBM_FORMAT_RGBA5551:
        case TBM_FORMAT_BGRA5551:
        case TBM_FORMAT_RGB565:
            bpp = 16;
            _offset = 0;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            _bo_idx = 0;
            break;
        /* 24 bpp RGB */
        case TBM_FORMAT_RGB888:
        case TBM_FORMAT_BGR888:
            bpp = 24;
            _offset = 0;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            _bo_idx = 0;
            break;
        /* 32 bpp RGB */
        case TBM_FORMAT_XRGB8888:
        case TBM_FORMAT_XBGR8888:
        case TBM_FORMAT_RGBX8888:
        case TBM_FORMAT_BGRX8888:
        case TBM_FORMAT_ARGB8888:
        case TBM_FORMAT_ABGR8888:
        case TBM_FORMAT_RGBA8888:
        case TBM_FORMAT_BGRA8888:
            bpp = 32;
            _offset = 0;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            _bo_idx = 0;
            break;

        /* packed YCbCr */
        case TBM_FORMAT_YUYV:
        case TBM_FORMAT_YVYU:
        case TBM_FORMAT_UYVY:
        case TBM_FORMAT_VYUY:
        case TBM_FORMAT_AYUV:
            bpp = 32;
            _offset = 0;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            _bo_idx = 0;
            break;

        /*
        * 2 plane YCbCr
        * index 0 = Y plane, [7:0] Y
        * index 1 = Cr:Cb plane, [15:0] Cr:Cb little endian
        * or
        * index 1 = Cb:Cr plane, [15:0] Cb:Cr little endian
        */
        case TBM_FORMAT_NV12:
        case TBM_FORMAT_NV21:
            bpp = 12;
            if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            else if( plane_idx ==1 )
            {
                _offset = width*height;
                _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            break;

        case TBM_FORMAT_NV16:
        case TBM_FORMAT_NV61:
            bpp = 16;
            //if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 0)
                    break;
            }
            //else if( plane_idx ==1 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            break;

        /*
        * 3 plane YCbCr
        * index 0: Y plane, [7:0] Y
        * index 1: Cb plane, [7:0] Cb
        * index 2: Cr plane, [7:0] Cr
        * or
        * index 1: Cr plane, [7:0] Cr
        * index 2: Cb plane, [7:0] Cb
        */
        /*
        NATIVE_BUFFER_FORMAT_YV12
        NATIVE_BUFFER_FORMAT_I420
        */
        case TBM_FORMAT_YUV410:
        case TBM_FORMAT_YVU410:
            bpp = 9;
            break;
        case TBM_FORMAT_YUV411:
        case TBM_FORMAT_YVU411:
        case TBM_FORMAT_YUV420:
        case TBM_FORMAT_YVU420:
            bpp = 12;
            //if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 0)
                    break;
            }
            //else if( plane_idx == 1 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 1)
                    break;
            }
            //else if (plane_idx == 2 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            break;
        case TBM_FORMAT_YUV422:
        case TBM_FORMAT_YVU422:
            bpp = 16;
            //if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 0)
                    break;
            }
            //else if( plane_idx == 1 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 1)
                    break;
            }
            //else if (plane_idx == 2 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size = SIZE_ALIGN(_pitch*(height),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
            }
            break;
        case TBM_FORMAT_YUV444:
        case TBM_FORMAT_YVU444:
            bpp = 24;
            //if(plane_idx == 0)
            {
                _offset = 0;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 0)
                    break;
            }
            //else if( plane_idx == 1 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 0;
                if(plane_idx == 1)
                    break;
            }
            //else if (plane_idx == 2 )
            {
                _offset += _size;
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
               _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
               _bo_idx = 0;
            }
            break;
        default:
            bpp = 0;
            break;
    }

    *size = _size;
    *offset = _offset;
    *pitch = _pitch;
    *bo_idx = _bo_idx;

    return ret;
}

int
tbm_dumb_surface_get_num_bos(tbm_format format)
{
    int num = 0;

    switch(format)
    {
        /* 16 bpp RGB */
        case TBM_FORMAT_XRGB4444:
        case TBM_FORMAT_XBGR4444:
        case TBM_FORMAT_RGBX4444:
        case TBM_FORMAT_BGRX4444:
        case TBM_FORMAT_ARGB4444:
        case TBM_FORMAT_ABGR4444:
        case TBM_FORMAT_RGBA4444:
        case TBM_FORMAT_BGRA4444:
        case TBM_FORMAT_XRGB1555:
        case TBM_FORMAT_XBGR1555:
        case TBM_FORMAT_RGBX5551:
        case TBM_FORMAT_BGRX5551:
        case TBM_FORMAT_ARGB1555:
        case TBM_FORMAT_ABGR1555:
        case TBM_FORMAT_RGBA5551:
        case TBM_FORMAT_BGRA5551:
        case TBM_FORMAT_RGB565:
        /* 24 bpp RGB */
        case TBM_FORMAT_RGB888:
        case TBM_FORMAT_BGR888:
        /* 32 bpp RGB */
        case TBM_FORMAT_XRGB8888:
        case TBM_FORMAT_XBGR8888:
        case TBM_FORMAT_RGBX8888:
        case TBM_FORMAT_BGRX8888:
        case TBM_FORMAT_ARGB8888:
        case TBM_FORMAT_ABGR8888:
        case TBM_FORMAT_RGBA8888:
        case TBM_FORMAT_BGRA8888:
        /* packed YCbCr */
        case TBM_FORMAT_YUYV:
        case TBM_FORMAT_YVYU:
        case TBM_FORMAT_UYVY:
        case TBM_FORMAT_VYUY:
        case TBM_FORMAT_AYUV:
        /*
        * 2 plane YCbCr
        * index 0 = Y plane, [7:0] Y
        * index 1 = Cr:Cb plane, [15:0] Cr:Cb little endian
        * or
        * index 1 = Cb:Cr plane, [15:0] Cb:Cr little endian
        */
        case TBM_FORMAT_NV21:
        case TBM_FORMAT_NV16:
        case TBM_FORMAT_NV61:
        /*
        * 3 plane YCbCr
        * index 0: Y plane, [7:0] Y
        * index 1: Cb plane, [7:0] Cb
        * index 2: Cr plane, [7:0] Cr
        * or
        * index 1: Cr plane, [7:0] Cr
        * index 2: Cb plane, [7:0] Cb
        */
        case TBM_FORMAT_YUV410:
        case TBM_FORMAT_YVU410:
        case TBM_FORMAT_YUV411:
        case TBM_FORMAT_YVU411:
        case TBM_FORMAT_YUV420:
        case TBM_FORMAT_YVU420:
        case TBM_FORMAT_YUV422:
        case TBM_FORMAT_YVU422:
        case TBM_FORMAT_YUV444:
        case TBM_FORMAT_YVU444:
            num = 1;
            break;

        case TBM_FORMAT_NV12:
            num = 2;
            break;

        default:
            num = 0;
            break;
    }

    return num;
}

/**
* @brief get the size of the surface with a format.
* @param[in] surface : the surface
* @param[in] width : the width of the surface
* @param[in] height : the height of the surface
* @param[in] format : the format of the surface
* @return size of the surface if this function succeeds, otherwise 0.
*/

int
tbm_dumb_surface_get_size(tbm_surface_h surface, int width, int height, tbm_format format)
{
    int ret = 0;
    int bpp = 0;
    int _pitch =0;
    int _size =0;
    int align =TBM_SURFACE_ALIGNMENT_PLANE;

    switch(format)
    {
        /* 16 bpp RGB */
        case TBM_FORMAT_XRGB4444:
        case TBM_FORMAT_XBGR4444:
        case TBM_FORMAT_RGBX4444:
        case TBM_FORMAT_BGRX4444:
        case TBM_FORMAT_ARGB4444:
        case TBM_FORMAT_ABGR4444:
        case TBM_FORMAT_RGBA4444:
        case TBM_FORMAT_BGRA4444:
        case TBM_FORMAT_XRGB1555:
        case TBM_FORMAT_XBGR1555:
        case TBM_FORMAT_RGBX5551:
        case TBM_FORMAT_BGRX5551:
        case TBM_FORMAT_ARGB1555:
        case TBM_FORMAT_ABGR1555:
        case TBM_FORMAT_RGBA5551:
        case TBM_FORMAT_BGRA5551:
        case TBM_FORMAT_RGB565:
            bpp = 16;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            break;
        /* 24 bpp RGB */
        case TBM_FORMAT_RGB888:
        case TBM_FORMAT_BGR888:
            bpp = 24;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            break;
        /* 32 bpp RGB */
        case TBM_FORMAT_XRGB8888:
        case TBM_FORMAT_XBGR8888:
        case TBM_FORMAT_RGBX8888:
        case TBM_FORMAT_BGRX8888:
        case TBM_FORMAT_ARGB8888:
        case TBM_FORMAT_ABGR8888:
        case TBM_FORMAT_RGBA8888:
        case TBM_FORMAT_BGRA8888:
            bpp = 32;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_RGB);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            break;
        /* packed YCbCr */
        case TBM_FORMAT_YUYV:
        case TBM_FORMAT_YVYU:
        case TBM_FORMAT_UYVY:
        case TBM_FORMAT_VYUY:
        case TBM_FORMAT_AYUV:
            bpp = 32;
            _pitch = SIZE_ALIGN((width*bpp)>>3,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
            _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            break;
        /*
        * 2 plane YCbCr
        * index 0 = Y plane, [7:0] Y
        * index 1 = Cr:Cb plane, [15:0] Cr:Cb little endian
        * or
        * index 1 = Cb:Cr plane, [15:0] Cb:Cr little endian
        */
        case TBM_FORMAT_NV12:
        case TBM_FORMAT_NV21:
            bpp = 12;
             //plane_idx == 0
             {
                 _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                 _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
             }
             //plane_idx ==1
             {
                 _pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                 _size += SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
             }
             break;

            break;
        case TBM_FORMAT_NV16:
        case TBM_FORMAT_NV61:
            bpp = 16;
            //plane_idx == 0
            {
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx ==1
            {
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }

            break;
        /*
        * 3 plane YCbCr
        * index 0: Y plane, [7:0] Y
        * index 1: Cb plane, [7:0] Cb
        * index 2: Cr plane, [7:0] Cr
        * or
        * index 1: Cr plane, [7:0] Cr
        * index 2: Cb plane, [7:0] Cb
        */
        case TBM_FORMAT_YUV410:
        case TBM_FORMAT_YVU410:
            bpp = 9;
        align = TBM_SURFACE_ALIGNMENT_PITCH_YUV;
            break;
        case TBM_FORMAT_YUV411:
        case TBM_FORMAT_YVU411:
        case TBM_FORMAT_YUV420:
        case TBM_FORMAT_YVU420:
            bpp = 12;
            //plane_idx == 0
            {
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 1
            {
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 2
            {
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
            }

            break;
        case TBM_FORMAT_YUV422:
        case TBM_FORMAT_YVU422:
            bpp = 16;
            //plane_idx == 0
            {
                _pitch = SIZE_ALIGN(width,TBM_SURFACE_ALIGNMENT_PITCH_YUV);
                _size = SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 1
            {
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            //plane_idx == 2
            {
                _pitch = SIZE_ALIGN(width/2,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
                _size += SIZE_ALIGN(_pitch*height,TBM_SURFACE_ALIGNMENT_PLANE);
            }
            break;
        case TBM_FORMAT_YUV444:
        case TBM_FORMAT_YVU444:
            bpp = 24;
            align = TBM_SURFACE_ALIGNMENT_PITCH_YUV;
            break;

        default:
            bpp = 0;
            break;
    }

    if(_size > 0)
        ret = _size;
    else
        ret =  SIZE_ALIGN( (width * height * bpp) >> 3, align);

    return ret;

}

tbm_bo_handle
tbm_dumb_fd_to_handle(tbm_bufmgr bufmgr, tbm_fd fd, int device)
{
    DUMB_RETURN_VAL_IF_FAIL (bufmgr!=NULL, (tbm_bo_handle) NULL);
    DUMB_RETURN_VAL_IF_FAIL (fd > 0, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    memset (&bo_handle, 0x0, sizeof (uint64_t));

    tbm_bufmgr_dumb bufmgr_dumb = (tbm_bufmgr_dumb)tbm_backend_get_priv_from_bufmgr(bufmgr);

    switch(device)
    {
    case TBM_DEVICE_DEFAULT:
    case TBM_DEVICE_2D:
    {
        //getting handle from fd
        struct drm_prime_handle arg = {0, };

        arg.fd = fd;
        arg.flags = 0;
        if (drmIoctl (bufmgr_dumb->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg))
        {
            TBM_DUMB_LOG ("error Cannot get gem handle from fd:%d (%s)\n",
                     arg.fd, strerror(errno));
            return (tbm_bo_handle) NULL;
        }

        bo_handle.u32 = (uint32_t)arg.handle;;
        break;
    }
    case TBM_DEVICE_CPU:
        TBM_DUMB_LOG ("Not supported device:%d\n", device);
        bo_handle.ptr = (void *) NULL;
        break;
    case TBM_DEVICE_3D:
    case TBM_DEVICE_MM:
        bo_handle.u32 = (uint32_t)fd;
        break;
    default:
        TBM_DUMB_LOG ("error Not supported device:%d\n", device);
        bo_handle.ptr = (void *) NULL;
        break;
    }

    return bo_handle;
}

int
tbm_dumb_bo_get_flags (tbm_bo bo)
{
    DUMB_RETURN_VAL_IF_FAIL (bo != NULL, 0);

    tbm_bo_dumb bo_dumb;

    bo_dumb = (tbm_bo_dumb)tbm_backend_get_bo_priv(bo);
    DUMB_RETURN_VAL_IF_FAIL (bo_dumb != NULL, 0);

    return bo_dumb->flags_tbm;
}

MODULEINITPPROTO (init_tbm_bufmgr_priv);

static TBMModuleVersionInfo DumbVersRec =
{
    "dumb",
    "Samsung",
    TBM_ABI_VERSION,
};

TBMModuleData tbmModuleData = { &DumbVersRec, init_tbm_bufmgr_priv};

int
init_tbm_bufmgr_priv (tbm_bufmgr bufmgr, int fd)
{
    tbm_bufmgr_dumb bufmgr_dumb;
    tbm_bufmgr_backend bufmgr_backend;
    uint64_t cap = 0;
    uint32_t ret;

    if (!bufmgr)
        return 0;

    ret = drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap);
    if (ret || cap == 0) {
        TBM_DUMB_LOG ("error: drm  buffer isn't supported !\n");
        return 0;
    }

    bufmgr_dumb = calloc (1, sizeof(struct _tbm_bufmgr_dumb));
    if (!bufmgr_dumb)
    {
        TBM_DUMB_LOG ("error: Fail to alloc bufmgr_dumb!\n");
        return 0;
    }

    if (fd < 0)
    {
        bufmgr_dumb->fd = tbm_bufmgr_get_drm_fd_wayland();
        bufmgr_dumb->fd_owner = 1;
    }
    else
        bufmgr_dumb->fd = fd;

    if (bufmgr_dumb->fd  < 0)
    {
        TBM_DUMB_LOG ("error: Fail to create drm!\n");
        free (bufmgr_dumb);
        return 0;
    }

    //Create Hash Table
    bufmgr_dumb->hashBos = drmHashCreate ();

    //Check if the tbm manager supports dma fence or not.
    int fp = open("/sys/module/dmabuf_sync/parameters/enabled", O_RDONLY);
    int length;
    char buf[1];
    if (fp != -1)
    {
        length = read(fp, buf, 1);

        if (length == 1 && buf[0] == '1')
            bufmgr_dumb->use_dma_fence = 1;

        close(fp);
    }

    bufmgr_backend = tbm_backend_alloc();
    if (!bufmgr_backend)
    {
        TBM_DUMB_LOG ("error: Fail to create drm!\n");
        if (bufmgr_dumb->hashBos)
            drmHashDestroy (bufmgr_dumb->hashBos);

        if (bufmgr_dumb->fd_owner)
            close(bufmgr_dumb->fd);

        free (bufmgr_dumb);
        return 0;
    }

    bufmgr_backend->priv = (void *)bufmgr_dumb;
    bufmgr_backend->bufmgr_deinit = tbm_dumb_bufmgr_deinit,
    bufmgr_backend->bo_size = tbm_dumb_bo_size,
    bufmgr_backend->bo_alloc = tbm_dumb_bo_alloc,
    bufmgr_backend->bo_free = tbm_dumb_bo_free,
    bufmgr_backend->bo_import = tbm_dumb_bo_import,
    bufmgr_backend->bo_import_fd = tbm_dumb_bo_import_fd,
    bufmgr_backend->bo_export = tbm_dumb_bo_export,
    bufmgr_backend->bo_export_fd = tbm_dumb_bo_export_fd,
    bufmgr_backend->bo_get_handle = tbm_dumb_bo_get_handle,
    bufmgr_backend->bo_map = tbm_dumb_bo_map,
    bufmgr_backend->bo_unmap = tbm_dumb_bo_unmap,
    bufmgr_backend->bo_cache_flush = tbm_dumb_bo_cache_flush,
    bufmgr_backend->bo_get_global_key = tbm_dumb_bo_get_global_key;
    bufmgr_backend->surface_get_plane_data = tbm_dumb_surface_get_plane_data;
    bufmgr_backend->surface_get_size = tbm_dumb_surface_get_size;
    bufmgr_backend->surface_supported_format = tbm_dumb_surface_supported_format;
    bufmgr_backend->fd_to_handle = tbm_dumb_fd_to_handle;
    bufmgr_backend->surface_get_num_bos = tbm_dumb_surface_get_num_bos;
    bufmgr_backend->bo_get_flags = tbm_dumb_bo_get_flags;

    if (bufmgr_dumb->use_dma_fence)
    {
        bufmgr_backend->flags = (TBM_LOCK_CTRL_BACKEND | TBM_CACHE_CTRL_BACKEND);
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_lock2 = tbm_dumb_bo_lock;
        bufmgr_backend->bo_unlock = tbm_dumb_bo_unlock;
    }
    else
    {
        bufmgr_backend->flags = 0;
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_unlock = NULL;
    }

    if (!tbm_backend_init (bufmgr, bufmgr_backend))
    {
        TBM_DUMB_LOG ("error: Fail to init backend!\n");

        if (bufmgr_dumb->fd_owner)
            close(bufmgr_dumb->fd);

        tbm_backend_free (bufmgr_backend);
        free (bufmgr_dumb);
        return 0;
    }

#ifdef DEBUG
    {
        char* env;
        env = getenv ("TBM_DUMB_DEBUG");
        if (env)
        {
            bDebug = atoi (env);
            TBM_DUMB_LOG ("TBM_DUMB_DEBUG=%s\n", env);
        }
        else
        {
            bDebug = 0;
        }
    }
#endif

    DBG ("[%s] DMABUF FENCE is %s\n", target_name(),
          bufmgr_dumb->use_dma_fence ? "supported!" : "NOT supported!");

    DBG ("[%s] drm_fd:%d\n", target_name(),
          bufmgr_dumb->fd);

    return 1;
}


