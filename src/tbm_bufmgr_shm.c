/**************************************************************************

libtbm_shm

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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <linux/unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <tbm_bufmgr.h>
#include <tbm_bufmgr_backend.h>
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
#define TBM_SHM_LOG(fmt, args...) LOGE("\033[31m"  "[%s]" fmt "\033[0m", target_name(), ##args)
#define DBG(fmt, args...)  if(bDebug&01) LOGE(fmt, ##args)
#else
#define TBM_SHM_LOG(...)
#define DBG(...)
#endif

#define SIZE_ALIGN( value, base ) (((value) + ((base) - 1)) & ~((base) - 1))

#define TBM_SURFACE_ALIGNMENT_PLANE (64)
#define TBM_SURFACE_ALIGNMENT_PITCH_RGB (64)
#define TBM_SURFACE_ALIGNMENT_PITCH_YUV (16)


/* check condition */
#define SHM_RETURN_IF_FAIL(cond) {\
    if (!(cond)) {\
        TBM_SHM_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return;\
    }\
}
#define SHM_RETURN_VAL_IF_FAIL(cond, val) {\
    if (!(cond)) {\
        TBM_SHM_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return val;\
    }\
}

typedef struct _tbm_bufmgr_shm *tbm_bufmgr_shm;
typedef struct _tbm_bo_shm *tbm_bo_shm;

/* tbm buffor object for shm */
struct _tbm_bo_shm
{
    int shmid;
    key_t key;
    void * pBase;

    unsigned int size;

    unsigned int flags_shm;
    unsigned int flags_tbm;

    pthread_mutex_t mutex;
    int device;
    int opt;
};

/* tbm bufmgr private for shm */
struct _tbm_bufmgr_shm
{
    int isLocal;
    void* hashBos;
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


uint32_t tbm_shm_color_format_list[TBM_COLOR_FORMAT_COUNT] = {   TBM_FORMAT_RGBA8888,
																		TBM_FORMAT_BGRA8888,
																		TBM_FORMAT_RGBX8888,
																		TBM_FORMAT_RGB888,
																		TBM_FORMAT_NV12,
																		TBM_FORMAT_NV21,
																		TBM_FORMAT_YUV420,
																		TBM_FORMAT_YVU420 };

static tbm_bo_handle
_shm_bo_handle (tbm_bo_shm bo_shm, int device)
{
    tbm_bo_handle bo_handle;
    memset (&bo_handle, 0x0, sizeof (uint64_t));

    switch(device)
    {
    case TBM_DEVICE_DEFAULT:
    case TBM_DEVICE_2D:
        bo_handle.u32 = (uint32_t)bo_shm->shmid;
        break;
    case TBM_DEVICE_CPU:
        bo_handle.ptr = (void *)bo_shm->pBase;
        break;
    case TBM_DEVICE_3D:
    case TBM_DEVICE_MM:
    default:
        TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Not supported device:%d\n",
                getpid(), __FUNCTION__, __LINE__, device);
        bo_handle.ptr = (void *) NULL;
        break;
    }

    return bo_handle;
}

static int
tbm_shm_bo_size (tbm_bo bo)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_shm bo_shm;

    bo_shm = (tbm_bo_shm)tbm_backend_get_bo_priv(bo);

    return bo_shm->size;
}

static void *
tbm_shm_bo_alloc (tbm_bo bo, int size, int flags)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_shm bo_shm;
    tbm_bufmgr_shm bufmgr_shm;

    static key_t key = 1;

    bufmgr_shm = (tbm_bufmgr_shm)tbm_backend_get_bufmgr_priv(bo);
    SHM_RETURN_VAL_IF_FAIL (bufmgr_shm!=NULL, 0);

    if (flags & TBM_BO_SCANOUT)
    {
        TBM_SHM_LOG ("[libtbm-shm:%d] warning %s:%d TBM_BO_SCANOUT ins't supported\n",
                getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    bo_shm = calloc (1, sizeof(struct _tbm_bo_shm));
    if (!bo_shm)
    {
        TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to allocate the bo private\n",
                getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    //get next key for a allocated shm segment
    key += 1;

    while ((bo_shm->shmid = shmget(key, size,  IPC_CREAT | IPC_EXCL | 0666)) < 0)
    {
        if ( errno != EEXIST)
        {
            TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to allocate the shared memory segment (%s)\n",
                    getpid(), __FUNCTION__, __LINE__, strerror(errno));
            free (bo_shm);
            return 0;
        }

        //try to allocate the segment with next key;
        key += 1;
    }

    bo_shm->key = key;

    if ((bo_shm->pBase = shmat(bo_shm->shmid, NULL, 0)) == (char *) -1) {
        TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to attach the shared memory segment (%s)\n",
                getpid(), __FUNCTION__, __LINE__,  strerror(errno));
        free(bo_shm);
        return 0;
    }

    bo_shm->size = size;
    bo_shm->flags_shm = 0;
    bo_shm->flags_tbm = TBM_BO_DEFAULT | TBM_BO_NONCACHABLE;

    DBG ("     [%s] bo:%p, shmid:%d(%d), flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_shm->shmid, bo_shm->key,
         bo_shm->flags_tbm, bo_shm->flags_shm,
         bo_shm->size);

    return (void *)bo_shm;
}

static void
tbm_shm_bo_free(tbm_bo bo)
{
    tbm_bo_shm bo_shm;
    tbm_bufmgr_shm bufmgr_shm;

    if (!bo)
        return;

    bufmgr_shm = (tbm_bufmgr_shm)tbm_backend_get_bufmgr_priv(bo);
    SHM_RETURN_IF_FAIL (bufmgr_shm!=NULL);

    bo_shm = (tbm_bo_shm)tbm_backend_get_bo_priv(bo);
    SHM_RETURN_IF_FAIL (bo_shm!=NULL);

    DBG ("      [%s] bo:%p, gem:%d(%d), size:%d\n",target_name(),
         bo,
         bo_shm->shmid, bo_shm->key,
         bo_shm->size);

    /* Free shm object*/
    if (shmdt(bo_shm->pBase) == -1)
    {
    	TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to detach shared memory segment bo:%p (%s)\n",
    			getpid(), __FUNCTION__, __LINE__, bo, strerror(errno));
    }

    free (bo_shm);
}


static void *
tbm_shm_bo_import (tbm_bo bo, unsigned int key)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_shm bo_shm;
    struct shmid_ds buf;

    bo_shm = calloc (1, sizeof(struct _tbm_bo_shm));
    if (!bo_shm)
    {
        TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to allocate the bo private\n",
                getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    if((bo_shm->shmid = shmget(key, 0,  0666)) < 0)
    {
        TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to import the shared memory segment key=%d (%s)\n",
                getpid(), __FUNCTION__, __LINE__, key, strerror(errno));
        free(bo_shm);
        return 0;
    }

    if ((bo_shm->pBase = shmat(bo_shm->shmid, NULL, 0)) == (char *) -1) {
        TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to attach the shared memory segment (%s)\n",
                getpid(), __FUNCTION__, __LINE__,  strerror(errno));
        free(bo_shm);
        return 0;
    }

    /*Get the original data for this shmid data structure first.*/
    if (shmctl(bo_shm->shmid, IPC_STAT, &buf) < 0) {
        TBM_SHM_LOG ("[libtbm-shm:%d] error %s:%d Fail to get IPC_STAT(%s)\n",
                getpid(), __FUNCTION__, __LINE__,  strerror(errno));
        free(bo_shm);
        return 0;
    }

    bo_shm->size = buf.shm_segsz;
    bo_shm->flags_shm = 0;
    bo_shm->key = key;
    bo_shm->flags_tbm = TBM_BO_DEFAULT | TBM_BO_NONCACHABLE;

    DBG ("    [%s] bo:%p, shm:%d(%d), flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_shm->shmid, bo_shm->key,
         bo_shm->flags_tbm, bo_shm->flags_shm,
         bo_shm->size);

    return (void *)bo_shm;
}

static unsigned int
tbm_shm_bo_export (tbm_bo bo)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_shm bo_shm;

    bo_shm = (tbm_bo_shm)tbm_backend_get_bo_priv(bo);
    SHM_RETURN_VAL_IF_FAIL (bo_shm!=NULL, 0);

    DBG ("    [%s] bo:%p, shm:%d(%d), flags:%d(%d), size:%d\n", target_name(),
         bo,
         bo_shm->shmid, bo_shm->key,
         bo_shm->flags_tbm, bo_shm->flags_shm,
         bo_shm->size);

    return (unsigned int)bo_shm->key;
}

static tbm_bo_handle
tbm_shm_bo_get_handle (tbm_bo bo, int device)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_shm bo_shm;

    bo_shm = (tbm_bo_shm)tbm_backend_get_bo_priv(bo);
    SHM_RETURN_VAL_IF_FAIL (bo_shm!=NULL, (tbm_bo_handle) NULL);

    DBG ("[%s] bo:%p, shm:%d(%d), flags:%d(%d), size:%d, %s\n", target_name(),
         bo,
         bo_shm->shmid, bo_shm->key,
         bo_shm->flags_tbm, bo_shm->flags_shm,
         bo_shm->size,
         STR_DEVICE[device]);

    /*Get mapped bo_handle*/
    bo_handle = _shm_bo_handle (bo_shm, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_SHM_LOG ("error Cannot get handle: shm:%d, device:%d\n", bo_shm->shmid, device);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static tbm_bo_handle
tbm_shm_bo_map (tbm_bo bo, int device, int opt)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_shm bo_shm;

    bo_shm = (tbm_bo_shm)tbm_backend_get_bo_priv(bo);
    SHM_RETURN_VAL_IF_FAIL (bo_shm!=NULL, (tbm_bo_handle) NULL);

    DBG ("       [%s] bo:%p, shm:%d(%d), %s, %s\n", target_name(),
         bo,
         bo_shm->shmid, bo_shm->key,
         STR_DEVICE[device],
         STR_OPT[opt]);

    /*Get mapped bo_handle*/
    bo_handle = _shm_bo_handle (bo_shm, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_SHM_LOG ("error Cannot get handle: shm:%d, device:%d, opt:%d\n", bo_shm->shmid, device, opt);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static int
tbm_shm_bo_unmap (tbm_bo bo)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_shm bo_shm;

    bo_shm = (tbm_bo_shm)tbm_backend_get_bo_priv(bo);
    SHM_RETURN_VAL_IF_FAIL (bo_shm!=NULL, 0);

    DBG ("     [%s] bo:%p, shm:%d(%d)\n", target_name(),
          bo,
          bo_shm->shmid, bo_shm->key);

    return 1;
}

static int
tbm_shm_bo_get_global_key (tbm_bo bo)
{
    SHM_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_shm bo_shm;

    bo_shm = (tbm_bo_shm)tbm_backend_get_bo_priv(bo);
    SHM_RETURN_VAL_IF_FAIL (bo_shm!=NULL, 0);

    return bo_shm->key;
}

static void
tbm_shm_bufmgr_deinit (void *priv)
{
    SHM_RETURN_IF_FAIL (priv!=NULL);

    tbm_bufmgr_shm bufmgr_shm;

    bufmgr_shm = (tbm_bufmgr_shm)priv;

    free (bufmgr_shm);
}

int
tbm_shm_surface_supported_format(uint32_t **formats, uint32_t *num)
{
    uint32_t* color_formats=NULL;

    color_formats = (uint32_t*)calloc (1,sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT);

    if( color_formats == NULL )
    {
        return 0;
    }
    memcpy( color_formats, tbm_shm_color_format_list , sizeof(uint32_t)*TBM_COLOR_FORMAT_COUNT );


    *formats = color_formats;
    *num = TBM_COLOR_FORMAT_COUNT;

    fprintf (stderr, "tbm_shm_surface_supported_format  count = %d \n",*num);

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
tbm_shm_surface_get_plane_data(tbm_surface_h surface, int width, int height, tbm_format format, int plane_idx, uint32_t *size, uint32_t *offset, uint32_t *pitch, int *bo_idx)
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
tbm_shm_surface_get_num_bos(tbm_format format)
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
tbm_shm_surface_get_size(tbm_surface_h surface, int width, int height, tbm_format format)
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

MODULEINITPPROTO (init_tbm_bufmgr_priv);

static TBMModuleVersionInfo DumbVersRec =
{
    "shm",
    "Samsung",
    TBM_ABI_VERSION,
};

TBMModuleData tbmModuleData = { &DumbVersRec, init_tbm_bufmgr_priv};

int
init_tbm_bufmgr_priv (tbm_bufmgr bufmgr, int fd)
{
    tbm_bufmgr_shm bufmgr_shm;
    tbm_bufmgr_backend bufmgr_backend;

    if (!bufmgr)
        return 0;

    bufmgr_shm = calloc (1, sizeof(struct _tbm_bufmgr_shm));
    if (!bufmgr_shm)
    {
        TBM_SHM_LOG ("error: Fail to alloc bufmgr_shm!\n");
        return 0;
    }

    bufmgr_backend = tbm_backend_alloc();
    if (!bufmgr_backend)
    {
        TBM_SHM_LOG ("error: Fail to create drm!\n");
        free (bufmgr_shm);
        return 0;
    }

    bufmgr_backend->priv = (void *)bufmgr_shm;
    bufmgr_backend->bufmgr_deinit = tbm_shm_bufmgr_deinit,
    bufmgr_backend->bo_size = tbm_shm_bo_size,
    bufmgr_backend->bo_alloc = tbm_shm_bo_alloc,
    bufmgr_backend->bo_free = tbm_shm_bo_free,
    bufmgr_backend->bo_import = tbm_shm_bo_import,
    bufmgr_backend->bo_import_fd = NULL,
    bufmgr_backend->bo_export = tbm_shm_bo_export,
    bufmgr_backend->bo_export_fd = NULL,
    bufmgr_backend->bo_get_handle = tbm_shm_bo_get_handle,
    bufmgr_backend->bo_map = tbm_shm_bo_map,
    bufmgr_backend->bo_unmap = tbm_shm_bo_unmap,
    bufmgr_backend->bo_cache_flush = NULL,
    bufmgr_backend->bo_get_global_key = tbm_shm_bo_get_global_key;
    bufmgr_backend->surface_get_plane_data = tbm_shm_surface_get_plane_data;
    bufmgr_backend->surface_get_size = tbm_shm_surface_get_size;
    bufmgr_backend->surface_supported_format = tbm_shm_surface_supported_format;
    bufmgr_backend->fd_to_handle = NULL;
    bufmgr_backend->surface_get_num_bos = tbm_shm_surface_get_num_bos;

    bufmgr_backend->flags = 0;
    bufmgr_backend->bo_lock = NULL;
    bufmgr_backend->bo_unlock = NULL;

    if (!tbm_backend_init (bufmgr, bufmgr_backend))
    {
        TBM_SHM_LOG ("error: Fail to init backend!\n");
        tbm_backend_free (bufmgr_backend);
        free (bufmgr_shm);
        return 0;
    }

#ifdef DEBUG
    {
        char* env;
        env = getenv ("TBM_SHM_DEBUG");
        if (env)
        {
            bDebug = atoi (env);
            TBM_SHM_LOG ("TBM_SHM_DEBUG=%s\n", env);
        }
        else
        {
            bDebug = 0;
        }
    }
#endif

    return 1;
}


