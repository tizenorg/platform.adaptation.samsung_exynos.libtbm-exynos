/**************************************************************************

libtbm_exynos

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
#include <exynos_drm.h>
#include <pthread.h>
#include <tbm_surface.h>
#include <tbm_surface_internal.h>

#define DEBUG
#define USE_DMAIMPORT
#define TBM_COLOR_FORMAT_COUNT 8

#ifdef DEBUG
#define LOG_TAG	"TBM_BACKEND"
#include <dlog.h>
static int bDebug=0;

char* target_name()
{
    FILE *f;
    char *slash;
    static int 	initialized = 0;
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
#define TBM_EXYNOS_LOG(fmt, args...) LOGE("\033[31m"  "[%s]" fmt "\033[0m", target_name(), ##args)
#define DBG(fmt, args...)  if(bDebug&01) LOGE(fmt, ##args)
#else
#define TBM_EXYNOS_LOG(...)
#define DBG(...)
#endif

#define SIZE_ALIGN( value, base ) (((value) + ((base) - 1)) & ~((base) - 1))

#define TBM_SURFACE_ALIGNMENT_PLANE (64)
#define TBM_SURFACE_ALIGNMENT_PITCH_RGB (64)
#define TBM_SURFACE_ALIGNMENT_PITCH_YUV (16)


/* check condition */
#define EXYNOS_RETURN_IF_FAIL(cond) {\
    if (!(cond)) {\
        TBM_EXYNOS_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return;\
    }\
}
#define EXYNOS_RETURN_VAL_IF_FAIL(cond, val) {\
    if (!(cond)) {\
        TBM_EXYNOS_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return val;\
    }\
}

struct dma_buf_info {
	unsigned long	size;
	unsigned int	fence_supported;
	unsigned int	padding;
};

#define DMA_BUF_ACCESS_READ		0x1
#define DMA_BUF_ACCESS_WRITE		0x2
#define DMA_BUF_ACCESS_DMA		0x4
#define DMA_BUF_ACCESS_MAX		0x8

#define DMA_FENCE_LIST_MAX 		5

struct dma_buf_fence {
	unsigned long		ctx;
	unsigned int		type;
};

#define DMABUF_IOCTL_BASE	'F'
#define DMABUF_IOWR(nr, type)	_IOWR(DMABUF_IOCTL_BASE, nr, type)

#define DMABUF_IOCTL_GET_INFO	DMABUF_IOWR(0x00, struct dma_buf_info)
#define DMABUF_IOCTL_GET_FENCE	DMABUF_IOWR(0x01, struct dma_buf_fence)
#define DMABUF_IOCTL_PUT_FENCE	DMABUF_IOWR(0x02, struct dma_buf_fence)

typedef struct _tbm_bufmgr_exynos *tbm_bufmgr_exynos;
typedef struct _tbm_bo_exynos *tbm_bo_exynos;

typedef struct _exynos_private
{
    int ref_count;
} PrivGem;

/* tbm buffor object for exynos */
struct _tbm_bo_exynos
{
    int fd;

    unsigned int name;    /* FLINK ID */

    unsigned int gem;     /* GEM Handle */

    unsigned int dmabuf;  /* fd for dmabuf */

    void *pBase;          /* virtual address */

    unsigned int size;

    unsigned int flags_exynos;
    unsigned int flags_tbm;

    PrivGem* private;

    pthread_mutex_t mutex;
    struct dma_buf_fence dma_fence[DMA_FENCE_LIST_MAX];
    int device;
    int opt;
};

/* tbm bufmgr private for exynos */
struct _tbm_bufmgr_exynos
{
    int fd;
    int isLocal;
    void* hashBos;

    int use_dma_fence;
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


uint32_t tbm_exynos_color_format_list[TBM_COLOR_FORMAT_COUNT] = {   TBM_FORMAT_RGBA8888,
																		TBM_FORMAT_BGRA8888,
																		TBM_FORMAT_RGBX8888,
																		TBM_FORMAT_RGB888,
																		TBM_FORMAT_NV12,
																		TBM_FORMAT_NV21,
																		TBM_FORMAT_YUV420,
																		TBM_FORMAT_YVU420 };


static unsigned int
_get_exynos_flag_from_tbm (unsigned int ftbm)
{
    unsigned int flags = 0;

    if (ftbm & TBM_BO_SCANOUT)
        flags |= EXYNOS_BO_CONTIG;
    else
        flags |= EXYNOS_BO_NONCONTIG;

    if (ftbm & TBM_BO_WC)
        flags |= EXYNOS_BO_WC;
    else if (ftbm & TBM_BO_NONCACHABLE)
        flags |= EXYNOS_BO_NONCACHABLE;
    else
        flags |= EXYNOS_BO_CACHABLE;

    return flags;
}

static unsigned int
_get_tbm_flag_from_exynos (unsigned int fexynos)
{
    unsigned int flags = 0;

    if (fexynos & EXYNOS_BO_NONCONTIG)
        flags |= TBM_BO_DEFAULT;
    else
        flags |= TBM_BO_SCANOUT;

    if (fexynos & EXYNOS_BO_WC)
        flags |= TBM_BO_WC;
    else if (fexynos & EXYNOS_BO_CACHABLE)
        flags |= TBM_BO_DEFAULT;
    else
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
        TBM_EXYNOS_LOG ("error fail to get flink from gem:%d (DRM_IOCTL_GEM_FLINK)\n",
                gem);
        return 0;
    }

    return (unsigned int)arg.name;
}

static tbm_bo_handle
_exynos_bo_handle (tbm_bo_exynos bo_exynos, int device)
{
    tbm_bo_handle bo_handle;
    memset (&bo_handle, 0x0, sizeof (uint64_t));

    switch(device)
    {
    case TBM_DEVICE_DEFAULT:
    case TBM_DEVICE_2D:
        bo_handle.u32 = (uint32_t)bo_exynos->gem;
        break;
    case TBM_DEVICE_CPU:
        if (!bo_exynos->pBase)
        {
            struct drm_exynos_gem_map arg = {0,};
            void *map = NULL;

            arg.handle = bo_exynos->gem;
            if (drmCommandWriteRead (bo_exynos->fd, DRM_EXYNOS_GEM_MAP, &arg, sizeof(arg)))
            {
               TBM_EXYNOS_LOG ("error Cannot map_dumb gem=%d\n", bo_exynos->gem);
               return (tbm_bo_handle) NULL;
            }

            map = mmap (NULL, bo_exynos->size, PROT_READ|PROT_WRITE, MAP_SHARED,
                              bo_exynos->fd, arg.offset);
            if (map == MAP_FAILED)
            {
                TBM_EXYNOS_LOG ("error Cannot usrptr gem=%d\n", bo_exynos->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_exynos->pBase = map;
        }
        bo_handle.ptr = (void *)bo_exynos->pBase;
        break;
    case TBM_DEVICE_3D:
#ifdef USE_DMAIMPORT
        if (bo_exynos->dmabuf)
        {
            bo_handle.u32 = (uint32_t)bo_exynos->dmabuf;
            break;
        }

        if (!bo_exynos->dmabuf)
        {
            struct drm_prime_handle arg = {0, };

            arg.handle = bo_exynos->gem;
            if (drmIoctl (bo_exynos->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
                TBM_EXYNOS_LOG ("error Cannot dmabuf=%d\n", bo_exynos->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_exynos->dmabuf = arg.fd;
        }

        bo_handle.u32 = (uint32_t)bo_exynos->dmabuf;
#endif
        break;
    case TBM_DEVICE_MM:
        if (!bo_exynos->dmabuf)
        {
            struct drm_prime_handle arg = {0, };

            arg.handle = bo_exynos->gem;
            if (drmIoctl (bo_exynos->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
                TBM_EXYNOS_LOG ("error Cannot dmabuf=%d\n", bo_exynos->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_exynos->dmabuf = arg.fd;
        }

        bo_handle.u32 = (uint32_t)bo_exynos->dmabuf;
        break;
    default:
        TBM_EXYNOS_LOG ("error Not supported device:%d\n", device);
        bo_handle.ptr = (void *) NULL;
        break;
    }

    return bo_handle;
}

static int
_exynos_cache_flush (int fd, tbm_bo_exynos bo_exynos, int flags)
{
#ifdef ENABLE_CACHECRTL
    struct drm_exynos_gem_cache_op cache_op = {0, };
    int ret;

    /* if bo_exynos is null, do cache_flush_all */
    if(bo_exynos)
    {
        cache_op.flags = 0;
        cache_op.usr_addr = (uint64_t)((uint32_t)bo_exynos->pBase);
        cache_op.size = bo_exynos->size;
    }
    else
    {
        flags = TBM_CACHE_FLUSH_ALL;
        cache_op.flags = 0;
        cache_op.usr_addr = 0;
        cache_op.size = 0;
    }

    if (flags & TBM_CACHE_INV)
    {
        if(flags & TBM_CACHE_ALL)
            cache_op.flags |= EXYNOS_DRM_CACHE_INV_ALL;
        else
            cache_op.flags |= EXYNOS_DRM_CACHE_INV_RANGE;
    }

    if (flags & TBM_CACHE_CLN)
    {
        if(flags & TBM_CACHE_ALL)
            cache_op.flags |= EXYNOS_DRM_CACHE_CLN_ALL;
        else
            cache_op.flags |= EXYNOS_DRM_CACHE_CLN_RANGE;
    }

    if(flags & TBM_CACHE_ALL)
        cache_op.flags |= EXYNOS_DRM_ALL_CACHES_CORES;

    ret = drmCommandWriteRead (fd, DRM_EXYNOS_GEM_CACHE_OP, &cache_op, sizeof(cache_op));
    if (ret)
    {
        TBM_EXYNOS_LOG ("error fail to flush the cache.\n");
        return 0;
    }
#else
    TBM_EXYNOS_LOG ("warning fail to enable the cache flush.\n");
#endif
    return 1;
}

static int
tbm_exynos_bo_size (tbm_bo bo)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, 0);

    return bo_exynos->size;
}

static void *
tbm_exynos_bo_alloc (tbm_bo bo, int size, int flags)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos bo_exynos;
    tbm_bufmgr_exynos bufmgr_exynos;
    unsigned int exynos_flags;

    bufmgr_exynos = (tbm_bufmgr_exynos)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bufmgr_exynos!=NULL, 0);

    bo_exynos = calloc (1, sizeof(struct _tbm_bo_exynos));
    if (!bo_exynos)
    {
        TBM_EXYNOS_LOG ("error fail to allocate the bo private\n");
        return 0;
    }

    exynos_flags = _get_exynos_flag_from_tbm (flags);
    if((flags & TBM_BO_SCANOUT) &&
        size <= 4*1024)
    {
        exynos_flags |= EXYNOS_BO_NONCONTIG;
    }

    struct drm_exynos_gem_create arg = {0, };
    arg.size = size;
    arg.flags = exynos_flags;
    if (drmCommandWriteRead(bufmgr_exynos->fd, DRM_EXYNOS_GEM_CREATE, &arg, sizeof(arg)))
    {
        TBM_EXYNOS_LOG ("error Cannot create bo(flag:%x, size:%d)\n", arg.flags, (unsigned int)arg.size);
        free (bo_exynos);
        return 0;
    }

    bo_exynos->fd = bufmgr_exynos->fd;
    bo_exynos->gem = arg.handle;
    bo_exynos->size = size;
    bo_exynos->flags_tbm = flags;
    bo_exynos->flags_exynos = exynos_flags;
    bo_exynos->name = _get_name (bo_exynos->fd, bo_exynos->gem);

    pthread_mutex_init(&bo_exynos->mutex, NULL);

    if (bufmgr_exynos->use_dma_fence
        && !bo_exynos->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_exynos->gem;
        if (drmIoctl (bo_exynos->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_EXYNOS_LOG ("error Cannot dmabuf=%d\n", bo_exynos->gem);
            free (bo_exynos);
            return 0;
        }
        bo_exynos->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem* privGem = calloc (1, sizeof(PrivGem));
    if (!privGem)
    {
        TBM_EXYNOS_LOG ("[libtbm-exynos:%d] "
                "error %s:%d Fail to calloc privGem\n",
                getpid(), __FUNCTION__, __LINE__);
        free (bo_exynos);
        return 0;
    }

    privGem->ref_count = 1;
    if (drmHashInsert(bufmgr_exynos->hashBos, bo_exynos->name, (void *)privGem) < 0)
    {
        TBM_EXYNOS_LOG ("error Cannot insert bo to Hash(%d)\n", bo_exynos->name);
    }

    DBG ("     [%s] bo:%p, gem:%d(%d), flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         flags, exynos_flags,
         bo_exynos->size);

    return (void *)bo_exynos;
}

static void
tbm_exynos_bo_free(tbm_bo bo)
{
    tbm_bo_exynos bo_exynos;
    tbm_bufmgr_exynos bufmgr_exynos;

    if (!bo)
        return;

    bufmgr_exynos = (tbm_bufmgr_exynos)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS_RETURN_IF_FAIL (bufmgr_exynos!=NULL);

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_IF_FAIL (bo_exynos!=NULL);

    DBG ("      [%s] bo:%p, gem:%d(%d), fd:%d, size:%d\n",target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         bo_exynos->dmabuf,
         bo_exynos->size);

    if (bo_exynos->pBase)
    {
        if (munmap(bo_exynos->pBase, bo_exynos->size) == -1)
        {
            TBM_EXYNOS_LOG ("error bo:%p fail to munmap(%s)\n",
                bo, strerror(errno));
        }
    }

    /* close dmabuf */
    if (bo_exynos->dmabuf)
    {
        close (bo_exynos->dmabuf);
        bo_exynos->dmabuf = 0;
    }

    /* delete bo from hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_exynos->hashBos, bo_exynos->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count--;
        if (privGem->ref_count == 0)
        {
            drmHashDelete (bufmgr_exynos->hashBos, bo_exynos->name);
            free (privGem);
            privGem = NULL;
        }
    }
    else
    {
        TBM_EXYNOS_LOG ("warning Cannot find bo to Hash(%d), ret=%d\n", bo_exynos->name, ret);
    }

    /* Free gem handle */
    struct drm_gem_close arg = {0, };
    memset (&arg, 0, sizeof(arg));
    arg.handle = bo_exynos->gem;
    if (drmIoctl (bo_exynos->fd, DRM_IOCTL_GEM_CLOSE, &arg))
    {
        TBM_EXYNOS_LOG ("error bo:%p fail to gem close.(%s)\n",
            bo, strerror(errno));
    }

    free (bo_exynos);
}


static void *
tbm_exynos_bo_import (tbm_bo bo, unsigned int key)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_exynos bufmgr_exynos;
    tbm_bo_exynos bo_exynos;

    bufmgr_exynos = (tbm_bufmgr_exynos)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bufmgr_exynos!=NULL, 0);

    struct drm_gem_open arg = {0, };
    struct drm_exynos_gem_info info = {0, };

    arg.name = key;
    if (drmIoctl(bufmgr_exynos->fd, DRM_IOCTL_GEM_OPEN, &arg))
    {
        TBM_EXYNOS_LOG ("error Cannot open gem name=%d\n", key);
        return 0;
    }

    info.handle = arg.handle;
    if (drmCommandWriteRead(bufmgr_exynos->fd,
                           DRM_EXYNOS_GEM_GET,
                           &info,
                           sizeof(struct drm_exynos_gem_info)))
    {
        TBM_EXYNOS_LOG ("error Cannot get gem info=%d\n", key);
        return 0;
    }

    bo_exynos = calloc (1, sizeof(struct _tbm_bo_exynos));
    if (!bo_exynos)
    {
        TBM_EXYNOS_LOG ("error fail to allocate the bo private\n");
        return 0;
    }

    bo_exynos->fd = bufmgr_exynos->fd;
    bo_exynos->gem = arg.handle;
    bo_exynos->size = arg.size;
    bo_exynos->flags_exynos = info.flags;
    bo_exynos->name = key;
    bo_exynos->flags_tbm = _get_tbm_flag_from_exynos (bo_exynos->flags_exynos);

    if (!bo_exynos->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_exynos->gem;
        if (drmIoctl (bo_exynos->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_EXYNOS_LOG ("error Cannot dmabuf=%d\n", bo_exynos->gem);
            free (bo_exynos);
            return 0;
        }
        bo_exynos->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_exynos->hashBos, bo_exynos->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count++;
    }
    else if (ret == 1)
    {
        privGem = calloc (1, sizeof(PrivGem));
        if (!privGem)
        {
            TBM_EXYNOS_LOG ("[libtbm-exynos:%d] "
                    "error %s:%d Fail to calloc privGem\n",
                    getpid(), __FUNCTION__, __LINE__);
            free (bo_exynos);
            return 0;
        }

        privGem->ref_count = 1;
        if (drmHashInsert (bufmgr_exynos->hashBos, bo_exynos->name, (void *)privGem) < 0)
        {
            TBM_EXYNOS_LOG ("error Cannot insert bo to Hash(%d)\n", bo_exynos->name);
        }
    }
    else
    {
        TBM_EXYNOS_LOG ("error Cannot insert bo to Hash(%d)\n", bo_exynos->name);
    }

    DBG ("    [%s] bo:%p, gem:%d(%d), fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         bo_exynos->dmabuf,
         bo_exynos->flags_tbm, bo_exynos->flags_exynos,
         bo_exynos->size);

    return (void *)bo_exynos;
}

static void *
tbm_exynos_bo_import_fd (tbm_bo bo, tbm_fd key)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_exynos bufmgr_exynos;
    tbm_bo_exynos bo_exynos;

    bufmgr_exynos = (tbm_bufmgr_exynos)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bufmgr_exynos!=NULL, 0);

    unsigned int gem = 0;
    unsigned int real_size = -1;
    struct drm_exynos_gem_info info = {0, };

	//getting handle from fd
    struct drm_prime_handle arg = {0, };

	arg.fd = key;
	arg.flags = 0;
    if (drmIoctl (bufmgr_exynos->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg))
    {
        TBM_EXYNOS_LOG ("error bo:%p Cannot get gem handle from fd:%d (%s)\n",
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

    info.handle = gem;
    if (drmCommandWriteRead(bufmgr_exynos->fd,
                           DRM_EXYNOS_GEM_GET,
                           &info,
                           sizeof(struct drm_exynos_gem_info)))
    {
        TBM_EXYNOS_LOG ("error bo:%p Cannot get gem info from gem:%d, fd:%d (%s)\n",
                bo, gem, key, strerror(errno));
        return 0;
    }

    if (real_size == -1)
    	real_size = info.size;

    bo_exynos = calloc (1, sizeof(struct _tbm_bo_exynos));
    if (!bo_exynos)
    {
        TBM_EXYNOS_LOG ("error bo:%p fail to allocate the bo private\n", bo);
        return 0;
    }

    bo_exynos->fd = bufmgr_exynos->fd;
    bo_exynos->gem = gem;
    bo_exynos->size = real_size;
    bo_exynos->flags_exynos = info.flags;
    bo_exynos->flags_tbm = _get_tbm_flag_from_exynos (bo_exynos->flags_exynos);

    bo_exynos->name = _get_name(bo_exynos->fd, bo_exynos->gem);
    if (!bo_exynos->name)
    {
        TBM_EXYNOS_LOG ("error bo:%p Cannot get name from gem:%d, fd:%d (%s)\n",
            bo, gem, key, strerror(errno));
        free (bo_exynos);
        return 0;
    }

    /* add bo to hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_exynos->hashBos, bo_exynos->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count++;
    }
    else if (ret == 1)
    {
        privGem = calloc (1, sizeof(PrivGem));
        if (!privGem)
        {
            TBM_EXYNOS_LOG ("[libtbm-exynos:%d] "
                    "error %s:%d Fail to calloc privGem\n",
                    getpid(), __FUNCTION__, __LINE__);
            free (bo_exynos);
            return 0;
        }

        privGem->ref_count = 1;
        if (drmHashInsert (bufmgr_exynos->hashBos, bo_exynos->name, (void *)privGem) < 0)
        {
            TBM_EXYNOS_LOG ("error bo:%p Cannot insert bo to Hash(%d) from gem:%d, fd:%d\n",
                bo, bo_exynos->name, gem, key);
        }
    }
    else
    {
        TBM_EXYNOS_LOG ("error bo:%p Cannot insert bo to Hash(%d) from gem:%d, fd:%d\n",
                bo, bo_exynos->name, gem, key);
    }

    DBG (" [%s] bo:%p, gem:%d(%d), fd:%d, key_fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         bo_exynos->dmabuf,
         key,
         bo_exynos->flags_tbm, bo_exynos->flags_exynos,
         bo_exynos->size);

    return (void *)bo_exynos;
}

static unsigned int
tbm_exynos_bo_export (tbm_bo bo)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, 0);

    if (!bo_exynos->name)
    {
        bo_exynos->name = _get_name(bo_exynos->fd, bo_exynos->gem);
        if (!bo_exynos->name)
        {
            TBM_EXYNOS_LOG ("error Cannot get name\n");
            return 0;
        }
    }

    DBG ("    [%s] bo:%p, gem:%d(%d), fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         bo_exynos->dmabuf,
         bo_exynos->flags_tbm, bo_exynos->flags_exynos,
         bo_exynos->size);

    return (unsigned int)bo_exynos->name;
}

tbm_fd
tbm_exynos_bo_export_fd (tbm_bo bo)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, -1);

    tbm_bo_exynos bo_exynos;
    int ret;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, -1);

    struct drm_prime_handle arg = {0, };

    arg.handle = bo_exynos->gem;
    ret = drmIoctl (bo_exynos->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg);
    if (ret)
    {
        TBM_EXYNOS_LOG ("error bo:%p Cannot dmabuf=%d (%s)\n",
            bo, bo_exynos->gem, strerror(errno));
        return (tbm_fd) ret;
    }

    DBG (" [%s] bo:%p, gem:%d(%d), fd:%d, key_fd:%d, flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         bo_exynos->dmabuf,
         arg.fd,
         bo_exynos->flags_tbm, bo_exynos->flags_exynos,
         bo_exynos->size);

    return (tbm_fd)arg.fd;
}

static tbm_bo_handle
tbm_exynos_bo_get_handle (tbm_bo bo, int device)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, (tbm_bo_handle) NULL);

    if (!bo_exynos->gem)
    {
        TBM_EXYNOS_LOG ("error Cannot map gem=%d\n", bo_exynos->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("[%s] bo:%p, gem:%d(%d), fd:%d, flags:%d(%d), size:%d, %s\n", target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         bo_exynos->dmabuf,
         bo_exynos->flags_tbm, bo_exynos->flags_exynos,
         bo_exynos->size,
         STR_DEVICE[device]);

    /*Get mapped bo_handle*/
    bo_handle = _exynos_bo_handle (bo_exynos, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_EXYNOS_LOG ("error Cannot get handle: gem:%d, device:%d\n", bo_exynos->gem, device);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static tbm_bo_handle
tbm_exynos_bo_map (tbm_bo bo, int device, int opt)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, (tbm_bo_handle) NULL);

    if (!bo_exynos->gem)
    {
        TBM_EXYNOS_LOG ("error Cannot map gem=%d\n", bo_exynos->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("       [%s] bo:%p, gem:%d(%d), fd:%d, %s, %s\n", target_name(),
         bo,
         bo_exynos->gem, bo_exynos->name,
         bo_exynos->dmabuf,
         STR_DEVICE[device],
         STR_OPT[opt]);

    /*Get mapped bo_handle*/
    bo_handle = _exynos_bo_handle (bo_exynos, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_EXYNOS_LOG ("error Cannot get handle: gem:%d, device:%d, opt:%d\n", bo_exynos->gem, device, opt);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static int
tbm_exynos_bo_unmap (tbm_bo bo)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, 0);

    if (!bo_exynos->gem)
        return 0;

    DBG ("     [%s] bo:%p, gem:%d(%d), fd:%d\n", target_name(),
          bo,
          bo_exynos->gem, bo_exynos->name,
          bo_exynos->dmabuf);

    return 1;
}

static int
tbm_exynos_bo_cache_flush (tbm_bo bo, int flags)
{
    tbm_bufmgr_exynos bufmgr_exynos = (tbm_bufmgr_exynos)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bufmgr_exynos!=NULL, 0);

    /* cache flush is managed by kernel side when using dma-fence. */
    if (bufmgr_exynos->use_dma_fence)
       return 1;

    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, 0);

    if (!_exynos_cache_flush(bo_exynos->fd, bo_exynos, flags))
        return 0;

    return 1;
}

static int
tbm_exynos_bo_get_global_key (tbm_bo bo)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, 0);

    if (!bo_exynos->name)
    {
        if (!bo_exynos->gem)
            return 0;

        bo_exynos->name = _get_name(bo_exynos->fd, bo_exynos->gem);
    }

    return bo_exynos->name;
}

static int
tbm_exynos_bo_lock(tbm_bo bo, int device, int opt)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_exynos bufmgr_exynos;
    tbm_bo_exynos bo_exynos;
    struct dma_buf_fence fence;
    struct flock filelock;
    int ret=0;

    if (device != TBM_DEVICE_3D && device != TBM_DEVICE_CPU)
    {
        DBG ("[libtbm-exynos:%d] %s not support device type,\n", getpid(), __FUNCTION__);
	    return 0;
    }

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, 0);

    bufmgr_exynos = (tbm_bufmgr_exynos)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bufmgr_exynos!=NULL, 0);

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
        TBM_EXYNOS_LOG ("error Invalid argument\n");
        return 0;
    }

    /* Check if the tbm manager supports dma fence or not. */
    if (!bufmgr_exynos->use_dma_fence)
    {
        TBM_EXYNOS_LOG ("error Not support DMA FENCE(%s)\n", strerror(errno) );
        return 0;

    }

    if (device == TBM_DEVICE_3D)
    {
        ret = ioctl(bo_exynos->dmabuf, DMABUF_IOCTL_GET_FENCE, &fence);
        if (ret < 0)
        {
            TBM_EXYNOS_LOG ("error Cannot set GET FENCE(%s)\n", strerror(errno) );
            return 0;
        }
    } else
    {
	if (opt & TBM_OPTION_WRITE)
	    filelock.l_type = F_WRLCK;
	else
	    filelock.l_type = F_RDLCK;

	filelock.l_whence = SEEK_CUR;
	filelock.l_start = 0;
	filelock.l_len = 0;

	if (-1 == fcntl(bo_exynos->dmabuf, F_SETLKW, &filelock))
        {
	    return 0;
	}
    }

    pthread_mutex_lock(&bo_exynos->mutex);

    if (device == TBM_DEVICE_3D)
    {
        int i;
        for (i = 0; i < DMA_FENCE_LIST_MAX; i++)
        {
            if (bo_exynos->dma_fence[i].ctx == 0)
            {
                bo_exynos->dma_fence[i].type = fence.type;
                bo_exynos->dma_fence[i].ctx = fence.ctx;
                break;
            }
        }

        if (i == DMA_FENCE_LIST_MAX)
        {
            //TODO: if dma_fence list is full, it needs realloc. I will fix this. by minseok3.kim
            TBM_EXYNOS_LOG ("error fence list is full\n");
        }
    }

    pthread_mutex_unlock(&bo_exynos->mutex);

    DBG ("[%s] DMABUF_IOCTL_GET_FENCE! bo:%p, gem:%d(%d), fd:%ds\n", target_name(),
          bo,
          bo_exynos->gem, bo_exynos->name,
          bo_exynos->dmabuf);

    return 1;
}

static int
tbm_exynos_bo_unlock(tbm_bo bo)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos bo_exynos;
    struct dma_buf_fence fence;
    struct flock filelock;
    unsigned int dma_type = 0;
    int ret=0;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos!=NULL, 0);

    if (bo_exynos->dma_fence[0].type & DMA_BUF_ACCESS_DMA)
	    dma_type = 1;

    if (!bo_exynos->dma_fence[0].ctx && dma_type)
    {
        DBG ("[libtbm-exynos:%d] %s FENCE not support or ignored,\n", getpid(), __FUNCTION__);
        return 0;
    }

    if (!bo_exynos->dma_fence[0].ctx && dma_type)
    {
        DBG ("[libtbm-exynos:%d] %s device type is not 3D/CPU,\n", getpid(), __FUNCTION__);
        return 0;
    }

    pthread_mutex_lock(&bo_exynos->mutex);

    if (dma_type)
    {
        fence.type = bo_exynos->dma_fence[0].type;
        fence.ctx = bo_exynos->dma_fence[0].ctx;
        int i;
        for (i = 1; i < DMA_FENCE_LIST_MAX; i++)
        {
            bo_exynos->dma_fence[i-1].type = bo_exynos->dma_fence[i].type;
            bo_exynos->dma_fence[i-1].ctx = bo_exynos->dma_fence[i].ctx;
        }
        bo_exynos->dma_fence[DMA_FENCE_LIST_MAX-1].type = 0;
        bo_exynos->dma_fence[DMA_FENCE_LIST_MAX-1].ctx = 0;
    }
    pthread_mutex_unlock(&bo_exynos->mutex);

    if (dma_type)
    {
        ret = ioctl(bo_exynos->dmabuf, DMABUF_IOCTL_PUT_FENCE, &fence);
        if (ret < 0)
        {
            TBM_EXYNOS_LOG ("error Can not set PUT FENCE(%s)\n", strerror(errno));
            return 0;
        }
    } else
    {
        filelock.l_type = F_UNLCK;
	filelock.l_whence = SEEK_CUR;
	filelock.l_start = 0;
	filelock.l_len = 0;

	if (-1 == fcntl(bo_exynos->dmabuf, F_SETLKW, &filelock))
        {
	    return 0;
	}
    }

    DBG ("[%s] DMABUF_IOCTL_PUT_FENCE! bo:%p, gem:%d(%d), fd:%ds\n", target_name(),
          bo,
          bo_exynos->gem, bo_exynos->name,
          bo_exynos->dmabuf);

    return 1;
}

static void
tbm_exynos_bufmgr_deinit (void *priv)
{
    EXYNOS_RETURN_IF_FAIL (priv!=NULL);

    tbm_bufmgr_exynos bufmgr_exynos;

    bufmgr_exynos = (tbm_bufmgr_exynos)priv;

    if (bufmgr_exynos->hashBos)
    {
        unsigned long key;
        void *value;

        while (drmHashFirst(bufmgr_exynos->hashBos, &key, &value) > 0)
        {
            free (value);
            drmHashDelete (bufmgr_exynos->hashBos, key);
        }

        drmHashDestroy (bufmgr_exynos->hashBos);
        bufmgr_exynos->hashBos = NULL;
    }

    free (bufmgr_exynos);
}

int
tbm_exynos_surface_supported_format(uint32_t **formats, uint32_t *num)
{
    uint32_t* color_formats=NULL;

    color_formats = (uint32_t*)calloc (1,sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT);

    if( color_formats == NULL )
    {
        return 0;
    }
    memcpy( color_formats, tbm_exynos_color_format_list , sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT );


    *formats = color_formats;
    *num = TBM_COLOR_FORMAT_COUNT;

    fprintf (stderr, "tbm_exynos_surface_supported_format  count = %d \n",*num);

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
tbm_exynos_surface_get_plane_data(tbm_surface_h surface, int width, int height, tbm_format format, int plane_idx, uint32_t *size, uint32_t *offset, uint32_t *pitch, int *bo_idx)
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
                _offset = 0;
				_pitch = SIZE_ALIGN( width ,TBM_SURFACE_ALIGNMENT_PITCH_YUV/2);
				_size = SIZE_ALIGN(_pitch*(height/2),TBM_SURFACE_ALIGNMENT_PLANE);
                _bo_idx = 1;
            }
            break;
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
            _bo_idx = 0;
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
tbm_exynos_surface_get_num_bos(tbm_format format)
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
tbm_exynos_surface_get_size(tbm_surface_h surface, int width, int height, tbm_format format)
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
tbm_exynos_fd_to_handle(tbm_bufmgr bufmgr, tbm_fd fd, int device)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bufmgr!=NULL, (tbm_bo_handle) NULL);
    EXYNOS_RETURN_VAL_IF_FAIL (fd > 0, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    memset (&bo_handle, 0x0, sizeof (uint64_t));

    tbm_bufmgr_exynos bufmgr_exynos = (tbm_bufmgr_exynos)tbm_backend_get_priv_from_bufmgr(bufmgr);

    switch(device)
    {
    case TBM_DEVICE_DEFAULT:
    case TBM_DEVICE_2D:
    {
        //getting handle from fd
        struct drm_prime_handle arg = {0, };

        arg.fd = fd;
        arg.flags = 0;
        if (drmIoctl (bufmgr_exynos->fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &arg))
        {
            TBM_EXYNOS_LOG ("error Cannot get gem handle from fd:%d (%s)\n",
                     arg.fd, strerror(errno));
            return (tbm_bo_handle) NULL;
        }

        bo_handle.u32 = (uint32_t)arg.handle;;
        break;
    }
    case TBM_DEVICE_CPU:
        TBM_EXYNOS_LOG ("Not supported device:%d\n", device);
        bo_handle.ptr = (void *) NULL;
        break;
    case TBM_DEVICE_3D:
    case TBM_DEVICE_MM:
        bo_handle.u32 = (uint32_t)fd;
        break;
    default:
        TBM_EXYNOS_LOG ("error Not supported device:%d\n", device);
        bo_handle.ptr = (void *) NULL;
        break;
    }

    return bo_handle;
}

int
tbm_exynos_bo_get_flags (tbm_bo bo)
{
    EXYNOS_RETURN_VAL_IF_FAIL (bo != NULL, 0);

    tbm_bo_exynos bo_exynos;

    bo_exynos = (tbm_bo_exynos)tbm_backend_get_bo_priv(bo);
    EXYNOS_RETURN_VAL_IF_FAIL (bo_exynos != NULL, 0);

    return bo_exynos->flags_tbm;
}

MODULEINITPPROTO (init_tbm_bufmgr_priv);

static TBMModuleVersionInfo ExynosVersRec =
{
    "exynos",
    "Samsung",
    TBM_ABI_VERSION,
};

TBMModuleData tbmModuleData = { &ExynosVersRec, init_tbm_bufmgr_priv};

int
init_tbm_bufmgr_priv (tbm_bufmgr bufmgr, int fd)
{
    tbm_bufmgr_exynos bufmgr_exynos;
    tbm_bufmgr_backend bufmgr_backend;

    if (!bufmgr)
        return 0;

    bufmgr_exynos = calloc (1, sizeof(struct _tbm_bufmgr_exynos));
    if (!bufmgr_exynos)
    {
        TBM_EXYNOS_LOG ("error: Fail to alloc bufmgr_exynos!\n");
        return 0;
    }

    bufmgr_exynos->fd = fd;
    if (bufmgr_exynos->fd < 0)
    {
        TBM_EXYNOS_LOG ("error: Fail to create drm!\n");
        free (bufmgr_exynos);
        return 0;
    }

    //Create Hash Table
    bufmgr_exynos->hashBos = drmHashCreate ();

    //Check if the tbm manager supports dma fence or not.
    int fp = open("/sys/module/dmabuf_sync/parameters/enabled", O_RDONLY);
    int length;
    char buf[1];
    if (fp != -1)
    {
        length = read(fp, buf, 1);

        if (length == 1 && buf[0] == '1')
            bufmgr_exynos->use_dma_fence = 1;

        close(fp);
    }

    bufmgr_backend = tbm_backend_alloc();
    if (!bufmgr_backend)
    {
        TBM_EXYNOS_LOG ("error: Fail to create drm!\n");
        if (bufmgr_exynos->hashBos)
            drmHashDestroy (bufmgr_exynos->hashBos);
        free (bufmgr_exynos);
        return 0;
    }

    bufmgr_backend->priv = (void *)bufmgr_exynos;
    bufmgr_backend->bufmgr_deinit = tbm_exynos_bufmgr_deinit,
    bufmgr_backend->bo_size = tbm_exynos_bo_size,
    bufmgr_backend->bo_alloc = tbm_exynos_bo_alloc,
    bufmgr_backend->bo_free = tbm_exynos_bo_free,
    bufmgr_backend->bo_import = tbm_exynos_bo_import,
    bufmgr_backend->bo_import_fd = tbm_exynos_bo_import_fd,
    bufmgr_backend->bo_export = tbm_exynos_bo_export,
    bufmgr_backend->bo_export_fd = tbm_exynos_bo_export_fd,
    bufmgr_backend->bo_get_handle = tbm_exynos_bo_get_handle,
    bufmgr_backend->bo_map = tbm_exynos_bo_map,
    bufmgr_backend->bo_unmap = tbm_exynos_bo_unmap,
    bufmgr_backend->bo_cache_flush = tbm_exynos_bo_cache_flush,
    bufmgr_backend->bo_get_global_key = tbm_exynos_bo_get_global_key;
    bufmgr_backend->surface_get_plane_data = tbm_exynos_surface_get_plane_data;
    bufmgr_backend->surface_get_size = tbm_exynos_surface_get_size;
    bufmgr_backend->surface_supported_format = tbm_exynos_surface_supported_format;
    bufmgr_backend->fd_to_handle = tbm_exynos_fd_to_handle;
    bufmgr_backend->surface_get_num_bos = tbm_exynos_surface_get_num_bos;
    bufmgr_backend->bo_get_flags = tbm_exynos_bo_get_flags;

    if (bufmgr_exynos->use_dma_fence)
    {
        bufmgr_backend->flags = (TBM_LOCK_CTRL_BACKEND | TBM_CACHE_CTRL_BACKEND);
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_lock2 = tbm_exynos_bo_lock;
        bufmgr_backend->bo_unlock = tbm_exynos_bo_unlock;
    }
    else
    {
        bufmgr_backend->flags = 0;
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_unlock = NULL;
    }

    if (!tbm_backend_init (bufmgr, bufmgr_backend))
    {
        TBM_EXYNOS_LOG ("error: Fail to init backend!\n");
        tbm_backend_free (bufmgr_backend);
        free (bufmgr_exynos);
        return 0;
    }

#ifdef DEBUG
    {
        char* env;
        env = getenv ("TBM_EXYNOS_DEBUG");
        if (env)
        {
            bDebug = atoi (env);
            TBM_EXYNOS_LOG ("TBM_EXYNOS_DEBUG=%s\n", env);
        }
        else
        {
            bDebug = 0;
        }
    }
#endif

    DBG ("[%s] DMABUF FENCE is %s\n", target_name(),
          bufmgr_exynos->use_dma_fence ? "supported!" : "NOT supported!");

    DBG ("[%s] drm_fd:%d\n", target_name(),
          bufmgr_exynos->fd);

    return 1;
}


