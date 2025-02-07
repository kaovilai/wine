/*
 * Video Mixing Renderer for dx9
 *
 * Copyright 2004 Christian Costa
 * Copyright 2008 Maarten Lankhorst
 * Copyright 2012 Aric Stewart
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "quartz_private.h"

#include "uuids.h"
#include "vfwmsgs.h"
#include "amvideo.h"
#include "windef.h"
#include "winbase.h"
#include "dshow.h"
#include "evcode.h"
#include "strmif.h"
#include "ddraw.h"
#include "dvdmedia.h"
#include "d3d9.h"
#include "vmr9.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(quartz);

static inline const char *debugstr_normalized_rect(const VMR9NormalizedRect *rect)
{
    if (!rect) return "(null)";
    return wine_dbg_sprintf("(%.8e,%.8e)-(%.8e,%.8e)", rect->left, rect->top, rect->right, rect->bottom);
}

static const BITMAPINFOHEADER *get_bitmap_header(const AM_MEDIA_TYPE *mt)
{
    if (IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo))
        return &((VIDEOINFOHEADER *)mt->pbFormat)->bmiHeader;
    else
        return &((VIDEOINFOHEADER2 *)mt->pbFormat)->bmiHeader;
}

struct quartz_vmr
{
    struct strmbase_renderer renderer;
    struct video_window window;

    IAMCertifiedOutputProtection IAMCertifiedOutputProtection_iface;
    IAMFilterMiscFlags IAMFilterMiscFlags_iface;
    IVMRAspectRatioControl9 IVMRAspectRatioControl9_iface;
    IVMRFilterConfig IVMRFilterConfig_iface;
    IVMRFilterConfig9 IVMRFilterConfig9_iface;
    IVMRMixerBitmap9 IVMRMixerBitmap9_iface;
    IVMRMixerControl9 IVMRMixerControl9_iface;
    IVMRMonitorConfig IVMRMonitorConfig_iface;
    IVMRMonitorConfig9 IVMRMonitorConfig9_iface;
    IVMRSurfaceAllocatorNotify IVMRSurfaceAllocatorNotify_iface;
    IVMRSurfaceAllocatorNotify9 IVMRSurfaceAllocatorNotify9_iface;
    IVMRWindowlessControl IVMRWindowlessControl_iface;
    IVMRWindowlessControl9 IVMRWindowlessControl9_iface;

    /* Devil May Cry 3 releases the last IBaseFilter reference while still
     * holding an IVMRSurfaceAllocatorNotify9 reference, and depends on
     * IVMRSurfaceAllocator9::TerminateDevice() being called as a result.
     * Native uses a separate reference count for IVMRSurfaceAllocatorNotify9. */
    LONG IVMRSurfaceAllocatorNotify9_refcount;

    IOverlay IOverlay_iface;

    IVMRSurfaceAllocator9 *allocator;
    IVMRImagePresenter9 *presenter;

    DWORD stream_count;
    DWORD mixing_prefs;

    /*
     * The Video Mixing Renderer supports 3 modes, renderless, windowless and windowed
     * What I do is implement windowless as a special case of renderless, and then
     * windowed also as a special case of windowless. This is probably the easiest way.
     */
    VMR9Mode mode;
    BITMAPINFOHEADER bmiheader;

    HMODULE hD3d9;

    /* Presentation related members */
    IDirect3DDevice9 *allocator_d3d9_dev;
    IDirect3DSurface9 **surfaces;
    DWORD num_surfaces;
    DWORD cur_surface;
    DWORD_PTR cookie;

    HWND clipping_window;

    LONG VideoWidth;
    LONG VideoHeight;
    VMR9AspectRatioMode aspect_mode;
};

static inline BOOL is_vmr9(const struct quartz_vmr *filter)
{
    return IsEqualGUID(&filter->renderer.filter.clsid, &CLSID_VideoMixingRenderer9);
}

static inline struct quartz_vmr *impl_from_video_window(struct video_window *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, window);
}

static inline struct quartz_vmr *impl_from_IAMCertifiedOutputProtection(IAMCertifiedOutputProtection *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IAMCertifiedOutputProtection_iface);
}

static inline struct quartz_vmr *impl_from_IAMFilterMiscFlags(IAMFilterMiscFlags *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IAMFilterMiscFlags_iface);
}

static inline struct quartz_vmr *impl_from_IVMRFilterConfig(IVMRFilterConfig *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRFilterConfig_iface);
}

static inline struct quartz_vmr *impl_from_IVMRFilterConfig9(IVMRFilterConfig9 *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRFilterConfig9_iface);
}

static inline struct quartz_vmr *impl_from_IVMRMonitorConfig(IVMRMonitorConfig *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRMonitorConfig_iface);
}

static inline struct quartz_vmr *impl_from_IVMRMonitorConfig9(IVMRMonitorConfig9 *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRMonitorConfig9_iface);
}

static inline struct quartz_vmr *impl_from_IVMRSurfaceAllocatorNotify(IVMRSurfaceAllocatorNotify *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRSurfaceAllocatorNotify_iface);
}

static inline struct quartz_vmr *impl_from_IVMRSurfaceAllocatorNotify9(IVMRSurfaceAllocatorNotify9 *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRSurfaceAllocatorNotify9_iface);
}

static inline struct quartz_vmr *impl_from_IVMRWindowlessControl(IVMRWindowlessControl *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRWindowlessControl_iface);
}

static inline struct quartz_vmr *impl_from_IVMRWindowlessControl9(IVMRWindowlessControl9 *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRWindowlessControl9_iface);
}

struct default_presenter
{
    IVMRImagePresenter9 IVMRImagePresenter9_iface;
    IVMRSurfaceAllocator9 IVMRSurfaceAllocator9_iface;

    LONG refCount;

    IDirect3DDevice9 *d3d9_dev;
    IDirect3D9 *d3d9_ptr;
    IDirect3DSurface9 **d3d9_surfaces;
    HMONITOR hMon;
    DWORD num_surfaces;

    VMR9AllocationInfo info;

    struct quartz_vmr* pVMR9;
    IVMRSurfaceAllocatorNotify9 *SurfaceAllocatorNotify;
};

static inline struct default_presenter *impl_from_IVMRImagePresenter9(IVMRImagePresenter9 *iface)
{
    return CONTAINING_RECORD(iface, struct default_presenter, IVMRImagePresenter9_iface);
}

static inline struct default_presenter *impl_from_IVMRSurfaceAllocator9(IVMRSurfaceAllocator9 *iface)
{
    return CONTAINING_RECORD(iface, struct default_presenter, IVMRSurfaceAllocator9_iface);
}

static HRESULT VMR9DefaultAllocatorPresenterImpl_create(struct quartz_vmr *parent, LPVOID * ppv);

static inline struct quartz_vmr *impl_from_IBaseFilter(IBaseFilter *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, renderer.filter.IBaseFilter_iface);
}

static HRESULT vmr_render(struct strmbase_renderer *iface, IMediaSample *sample)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);
    unsigned int data_size, width, depth, src_pitch;
    const BITMAPINFOHEADER *bitmap_header;
    REFERENCE_TIME start_time, end_time;
    VMR9PresentationInfo info = {};
    D3DLOCKED_RECT locked_rect;
    BYTE *data = NULL;
    HRESULT hr;
    int height;

    TRACE("filter %p, sample %p.\n", filter, sample);

    /* It is possible that there is no device at this point */

    if (!filter->allocator || !filter->presenter)
    {
        ERR("NO PRESENTER!!\n");
        return S_FALSE;
    }

    info.dwFlags = VMR9Sample_SrcDstRectsValid;

    if (SUCCEEDED(hr = IMediaSample_GetTime(sample, &start_time, &end_time)))
        info.dwFlags |= VMR9Sample_TimeValid;

    if (IMediaSample_IsDiscontinuity(sample) == S_OK)
        info.dwFlags |= VMR9Sample_Discontinuity;

    if (IMediaSample_IsPreroll(sample) == S_OK)
        info.dwFlags |= VMR9Sample_Preroll;

    if (IMediaSample_IsSyncPoint(sample) == S_OK)
        info.dwFlags |= VMR9Sample_SyncPoint;

    if (FAILED(hr = IMediaSample_GetPointer(sample, &data)))
    {
        ERR("Failed to get pointer to sample data, hr %#x.\n", hr);
        return hr;
    }
    data_size = IMediaSample_GetActualDataLength(sample);

    bitmap_header = get_bitmap_header(&filter->renderer.sink.pin.mt);
    width = bitmap_header->biWidth;
    height = bitmap_header->biHeight;
    depth = bitmap_header->biBitCount;
    if (bitmap_header->biCompression == mmioFOURCC('N','V','1','2')
            || bitmap_header->biCompression == mmioFOURCC('Y','V','1','2'))
        src_pitch = width;
    else /* packed YUV (UYVY or YUY2) or RGB */
        src_pitch = ((width * depth / 8) + 3) & ~3;

    info.rtStart = start_time;
    info.rtEnd = end_time;
    info.szAspectRatio.cx = width;
    info.szAspectRatio.cy = height;
    info.lpSurf = filter->surfaces[(++filter->cur_surface) % filter->num_surfaces];

    if (FAILED(hr = IDirect3DSurface9_LockRect(info.lpSurf, &locked_rect, NULL, D3DLOCK_DISCARD)))
    {
        ERR("Failed to lock surface, hr %#x.\n", hr);
        return hr;
    }

    if (height > 0 && bitmap_header->biCompression == BI_RGB)
    {
        BYTE *dst = (BYTE *)locked_rect.pBits + (height * locked_rect.Pitch);
        const BYTE *src = data;

        TRACE("Inverting image.\n");

        while (height--)
        {
            dst -= locked_rect.Pitch;
            memcpy(dst, src, width * depth / 8);
            src += src_pitch;
        }
    }
    else if (locked_rect.Pitch != src_pitch)
    {
        BYTE *dst = locked_rect.pBits;
        const BYTE *src = data;

        height = abs(height);

        TRACE("Source pitch %u does not match dest pitch %u; copying manually.\n",
                src_pitch, locked_rect.Pitch);

        while (height--)
        {
            memcpy(dst, src, width * depth / 8);
            src += src_pitch;
            dst += locked_rect.Pitch;
        }
    }
    else
    {
        memcpy(locked_rect.pBits, data, data_size);
    }

    IDirect3DSurface9_UnlockRect(info.lpSurf);

    return IVMRImagePresenter9_PresentImage(filter->presenter, filter->cookie, &info);
}

static HRESULT vmr_query_accept(struct strmbase_renderer *iface, const AM_MEDIA_TYPE *mt)
{
    if (!IsEqualIID(&mt->majortype, &MEDIATYPE_Video) || !mt->pbFormat)
        return S_FALSE;

    if (!IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo)
            && !IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo2))
        return S_FALSE;

    return S_OK;
}

static HRESULT initialize_device(struct quartz_vmr *filter, VMR9AllocationInfo *info, DWORD count)
{
    HRESULT hr;
    DWORD i;

    if (FAILED(hr = IVMRSurfaceAllocator9_InitializeDevice(filter->allocator,
            filter->cookie, info, &count)))
    {
        WARN("Failed to initialize device (flags %#x), hr %#x.\n", info->dwFlags, hr);
        return hr;
    }

    for (i = 0; i < count; ++i)
    {
        if (FAILED(hr = IVMRSurfaceAllocator9_GetSurface(filter->allocator,
                filter->cookie, i, 0, &filter->surfaces[i])))
        {
            ERR("Failed to get surface %u, hr %#x.\n", i, hr);
            while (i--)
                IDirect3DSurface9_Release(filter->surfaces[i]);
            IVMRSurfaceAllocator9_TerminateDevice(filter->allocator, filter->cookie);
            return hr;
        }
    }

    return hr;
}

static HRESULT allocate_surfaces(struct quartz_vmr *filter, const AM_MEDIA_TYPE *mt)
{
    VMR9AllocationInfo info = {};
    HRESULT hr = E_FAIL;
    DWORD count = 1;
    unsigned int i;

    static const struct
    {
        const GUID *subtype;
        D3DFORMAT format;
        DWORD flags;
    }
    formats[] =
    {
        {&MEDIASUBTYPE_ARGB1555, D3DFMT_A1R5G5B5, VMR9AllocFlag_TextureSurface},
        {&MEDIASUBTYPE_ARGB32, D3DFMT_A8R8G8B8, VMR9AllocFlag_TextureSurface},
        {&MEDIASUBTYPE_ARGB4444, D3DFMT_A4R4G4B4, VMR9AllocFlag_TextureSurface},

        {&MEDIASUBTYPE_RGB24, D3DFMT_R8G8B8, VMR9AllocFlag_TextureSurface | VMR9AllocFlag_OffscreenSurface},
        {&MEDIASUBTYPE_RGB32, D3DFMT_X8R8G8B8, VMR9AllocFlag_TextureSurface | VMR9AllocFlag_OffscreenSurface},
        {&MEDIASUBTYPE_RGB555, D3DFMT_X1R5G5B5, VMR9AllocFlag_TextureSurface | VMR9AllocFlag_OffscreenSurface},
        {&MEDIASUBTYPE_RGB565, D3DFMT_R5G6B5, VMR9AllocFlag_TextureSurface | VMR9AllocFlag_OffscreenSurface},

        {&MEDIASUBTYPE_NV12, MAKEFOURCC('N','V','1','2'), VMR9AllocFlag_OffscreenSurface},
        {&MEDIASUBTYPE_UYVY, D3DFMT_UYVY, VMR9AllocFlag_OffscreenSurface},
        {&MEDIASUBTYPE_YUY2, D3DFMT_YUY2, VMR9AllocFlag_OffscreenSurface},
        {&MEDIASUBTYPE_YV12, MAKEFOURCC('Y','V','1','2'), VMR9AllocFlag_OffscreenSurface},
    };

    TRACE("Initializing in mode %u, our window %p, clipping window %p.\n",
            filter->mode, filter->window.hwnd, filter->clipping_window);

    if (filter->mode == VMR9Mode_Windowless && !filter->clipping_window)
        return S_OK;

    info.Pool = D3DPOOL_DEFAULT;
    info.MinBuffers = count;
    info.dwWidth = info.szAspectRatio.cx = info.szNativeSize.cx = filter->bmiheader.biWidth;
    info.dwHeight = info.szAspectRatio.cy = info.szNativeSize.cy = filter->bmiheader.biHeight;

    if (!(filter->surfaces = calloc(count, sizeof(IDirect3DSurface9 *))))
        return E_OUTOFMEMORY;
    filter->num_surfaces = count;
    filter->cur_surface = 0;

    if (!is_vmr9(filter))
    {
        switch (filter->bmiheader.biCompression)
        {
        case BI_RGB:
            switch (filter->bmiheader.biBitCount)
            {
                case 24: info.Format = D3DFMT_R8G8B8; break;
                case 32: info.Format = D3DFMT_X8R8G8B8; break;
                default:
                    FIXME("Unhandled bit depth %u.\n", filter->bmiheader.biBitCount);
                    free(filter->surfaces);
                    return VFW_E_TYPE_NOT_ACCEPTED;
            }

            info.dwFlags = VMR9AllocFlag_TextureSurface;
            break;

        case mmioFOURCC('N','V','1','2'):
        case mmioFOURCC('U','Y','V','Y'):
        case mmioFOURCC('Y','U','Y','2'):
        case mmioFOURCC('Y','V','1','2'):
            info.Format = filter->bmiheader.biCompression;
            info.dwFlags = VMR9AllocFlag_OffscreenSurface;
            break;

        default:
            WARN("Unhandled video compression %#x.\n", filter->bmiheader.biCompression);
            free(filter->surfaces);
            return VFW_E_TYPE_NOT_ACCEPTED;
        }
        if (FAILED(hr = initialize_device(filter, &info, count)))
            free(filter->surfaces);
        return hr;
    }

    for (i = 0; i < ARRAY_SIZE(formats); ++i)
    {
        if (IsEqualGUID(&mt->subtype, formats[i].subtype))
        {
            info.Format = formats[i].format;

            if (formats[i].flags & VMR9AllocFlag_TextureSurface)
            {
                info.dwFlags = VMR9AllocFlag_TextureSurface;
                if (SUCCEEDED(hr = initialize_device(filter, &info, count)))
                    return hr;
            }

            if (formats[i].flags & VMR9AllocFlag_OffscreenSurface)
            {
                info.dwFlags = VMR9AllocFlag_OffscreenSurface;
                if (SUCCEEDED(hr = initialize_device(filter, &info, count)))
                    return hr;
            }
        }
    }

    free(filter->surfaces);
    return VFW_E_TYPE_NOT_ACCEPTED;
}

static void vmr_init_stream(struct strmbase_renderer *iface)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);

    if (filter->window.hwnd && filter->window.AutoShow)
        ShowWindow(filter->window.hwnd, SW_SHOW);
}

static void vmr_start_stream(struct strmbase_renderer *iface)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);

    IVMRImagePresenter9_StartPresenting(filter->presenter, filter->cookie);
}

static void vmr_stop_stream(struct strmbase_renderer *iface)
{
    struct quartz_vmr *This = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);

    TRACE("(%p)\n", This);

    if (This->renderer.filter.state == State_Running)
        IVMRImagePresenter9_StopPresenting(This->presenter, This->cookie);
}

static HRESULT vmr_connect(struct strmbase_renderer *iface, const AM_MEDIA_TYPE *mt)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);
    const BITMAPINFOHEADER *bitmap_header = get_bitmap_header(mt);
    HWND window = filter->window.hwnd;
    HRESULT hr;
    RECT rect;

    filter->bmiheader = *bitmap_header;
    filter->VideoWidth = bitmap_header->biWidth;
    filter->VideoHeight = bitmap_header->biHeight;
    SetRect(&rect, 0, 0, filter->VideoWidth, filter->VideoHeight);
    filter->window.src = rect;

    AdjustWindowRectEx(&rect, GetWindowLongW(window, GWL_STYLE), FALSE,
            GetWindowLongW(window, GWL_EXSTYLE));
    SetWindowPos(window, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(window, &filter->window.dst);

    if (filter->mode
            || SUCCEEDED(hr = IVMRFilterConfig9_SetRenderingMode(&filter->IVMRFilterConfig9_iface, VMR9Mode_Windowed)))
        hr = allocate_surfaces(filter, mt);

    return hr;
}

static void vmr_disconnect(struct strmbase_renderer *This)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&This->filter.IBaseFilter_iface);
    DWORD i;

    if (filter->mode && filter->allocator && filter->presenter)
    {
        for (i = 0; i < filter->num_surfaces; ++i)
            IDirect3DSurface9_Release(filter->surfaces[i]);
        free(filter->surfaces);

        IVMRSurfaceAllocator9_TerminateDevice(filter->allocator, filter->cookie);
        filter->num_surfaces = 0;
    }
}

static void vmr_destroy(struct strmbase_renderer *iface)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);

    video_window_cleanup(&filter->window);

    /* Devil May Cry 3 releases the IVMRSurfaceAllocatorNotify9 interface from
     * TerminateDevice(). Artificially increase the reference count so that we
     * don't free the filter yet. */
    InterlockedIncrement(&filter->renderer.filter.refcount);

    if (filter->allocator)
    {
        IVMRSurfaceAllocator9_TerminateDevice(filter->allocator, filter->cookie);
        IVMRSurfaceAllocator9_Release(filter->allocator);
    }
    if (filter->presenter)
        IVMRImagePresenter9_Release(filter->presenter);

    filter->num_surfaces = 0;
    if (filter->allocator_d3d9_dev)
    {
        IDirect3DDevice9_Release(filter->allocator_d3d9_dev);
        filter->allocator_d3d9_dev = NULL;
    }

    FreeLibrary(filter->hD3d9);
    strmbase_renderer_cleanup(&filter->renderer);
    if (!filter->IVMRSurfaceAllocatorNotify9_refcount)
        free(filter);
}

static HRESULT vmr_query_interface(struct strmbase_renderer *iface, REFIID iid, void **out)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);

    if (IsEqualGUID(iid, &IID_IVideoWindow))
        *out = &filter->window.IVideoWindow_iface;
    else if (IsEqualGUID(iid, &IID_IBasicVideo))
        *out = &filter->window.IBasicVideo_iface;
    else if (IsEqualGUID(iid, &IID_IAMCertifiedOutputProtection))
        *out = &filter->IAMCertifiedOutputProtection_iface;
    else if (IsEqualGUID(iid, &IID_IAMFilterMiscFlags))
        *out = &filter->IAMFilterMiscFlags_iface;
    else if (IsEqualGUID(iid, &IID_IVMRAspectRatioControl9) && is_vmr9(filter))
        *out = &filter->IVMRAspectRatioControl9_iface;
    else if (IsEqualGUID(iid, &IID_IVMRFilterConfig) && !is_vmr9(filter))
        *out = &filter->IVMRFilterConfig_iface;
    else if (IsEqualGUID(iid, &IID_IVMRFilterConfig9) && is_vmr9(filter))
        *out = &filter->IVMRFilterConfig9_iface;
    else if (IsEqualGUID(iid, &IID_IVMRMixerBitmap9) && is_vmr9(filter))
        *out = &filter->IVMRMixerBitmap9_iface;
    else if (IsEqualGUID(iid, &IID_IVMRMixerControl9) && is_vmr9(filter) && filter->stream_count)
        *out = &filter->IVMRMixerControl9_iface;
    else if (IsEqualGUID(iid, &IID_IVMRMonitorConfig) && !is_vmr9(filter))
        *out = &filter->IVMRMonitorConfig_iface;
    else if (IsEqualGUID(iid, &IID_IVMRMonitorConfig9)
            && filter->mode != VMR9Mode_Renderless && is_vmr9(filter))
        *out = &filter->IVMRMonitorConfig9_iface;
    else if (IsEqualGUID(iid, &IID_IVMRSurfaceAllocatorNotify)
            && filter->mode == (VMR9Mode)VMRMode_Renderless && !is_vmr9(filter))
        *out = &filter->IVMRSurfaceAllocatorNotify_iface;
    else if (IsEqualGUID(iid, &IID_IVMRSurfaceAllocatorNotify9)
            && filter->mode == VMR9Mode_Renderless && is_vmr9(filter))
        *out = &filter->IVMRSurfaceAllocatorNotify9_iface;
    else if (IsEqualGUID(iid, &IID_IVMRWindowlessControl)
            && filter->mode == (VMR9Mode)VMRMode_Windowless && !is_vmr9(filter))
        *out = &filter->IVMRWindowlessControl_iface;
    else if (IsEqualGUID(iid, &IID_IVMRWindowlessControl9)
            && filter->mode == VMR9Mode_Windowless && is_vmr9(filter))
        *out = &filter->IVMRWindowlessControl9_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static HRESULT vmr_pin_query_interface(struct strmbase_renderer *iface, REFIID iid, void **out)
{
    struct quartz_vmr *filter = impl_from_IBaseFilter(&iface->filter.IBaseFilter_iface);

    if (IsEqualGUID(iid, &IID_IOverlay))
        *out = &filter->IOverlay_iface;
    else
        return E_NOINTERFACE;

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static const struct strmbase_renderer_ops renderer_ops =
{
    .renderer_query_accept = vmr_query_accept,
    .renderer_render = vmr_render,
    .renderer_init_stream = vmr_init_stream,
    .renderer_start_stream = vmr_start_stream,
    .renderer_stop_stream = vmr_stop_stream,
    .renderer_connect = vmr_connect,
    .renderer_disconnect = vmr_disconnect,
    .renderer_destroy = vmr_destroy,
    .renderer_query_interface = vmr_query_interface,
    .renderer_pin_query_interface = vmr_pin_query_interface,
};

static RECT vmr_get_default_rect(struct video_window *This)
{
    struct quartz_vmr *pVMR9 = impl_from_video_window(This);
    static RECT defRect;

    SetRect(&defRect, 0, 0, pVMR9->VideoWidth, pVMR9->VideoHeight);

    return defRect;
}

static HRESULT vmr_get_current_image(struct video_window *iface, LONG *size, LONG *image)
{
    struct quartz_vmr *filter = impl_from_video_window(iface);
    IDirect3DSurface9 *rt = NULL, *surface = NULL;
    D3DLOCKED_RECT locked_rect;
    IDirect3DDevice9 *device;
    unsigned int row_size;
    BITMAPINFOHEADER bih;
    LONG i, size_left;
    char *dst;
    HRESULT hr;

    EnterCriticalSection(&filter->renderer.filter.stream_cs);
    device = filter->allocator_d3d9_dev;

    bih = *get_bitmap_header(&filter->renderer.sink.pin.mt);
    bih.biSizeImage = bih.biWidth * bih.biHeight * bih.biBitCount / 8;

    if (!image)
    {
        *size = sizeof(BITMAPINFOHEADER) + bih.biSizeImage;
        LeaveCriticalSection(&filter->renderer.filter.stream_cs);
        return S_OK;
    }

    if (FAILED(hr = IDirect3DDevice9_GetRenderTarget(device, 0, &rt)))
        goto out;

    if (FAILED(hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, bih.biWidth,
            bih.biHeight, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, NULL)))
        goto out;

    if (FAILED(hr = IDirect3DDevice9_GetRenderTargetData(device, rt, surface)))
        goto out;

    if (FAILED(hr = IDirect3DSurface9_LockRect(surface, &locked_rect, NULL, D3DLOCK_READONLY)))
        goto out;

    size_left = *size;
    memcpy(image, &bih, min(size_left, sizeof(BITMAPINFOHEADER)));
    size_left -= sizeof(BITMAPINFOHEADER);

    dst = (char *)image + sizeof(BITMAPINFOHEADER);
    row_size = bih.biWidth * bih.biBitCount / 8;

    for (i = 0; i < bih.biHeight && size_left > 0; ++i)
    {
        memcpy(dst, (char *)locked_rect.pBits + (i * locked_rect.Pitch), min(row_size, size_left));
        dst += row_size;
        size_left -= row_size;
    }

    IDirect3DSurface9_UnlockRect(surface);

out:
    if (surface) IDirect3DSurface9_Release(surface);
    if (rt) IDirect3DSurface9_Release(rt);
    LeaveCriticalSection(&filter->renderer.filter.stream_cs);
    return hr;
}

static const struct video_window_ops window_ops =
{
    .get_default_rect = vmr_get_default_rect,
    .get_current_image = vmr_get_current_image,
};

static const IVideoWindowVtbl IVideoWindow_VTable =
{
    BaseControlWindowImpl_QueryInterface,
    BaseControlWindowImpl_AddRef,
    BaseControlWindowImpl_Release,
    BaseControlWindowImpl_GetTypeInfoCount,
    BaseControlWindowImpl_GetTypeInfo,
    BaseControlWindowImpl_GetIDsOfNames,
    BaseControlWindowImpl_Invoke,
    BaseControlWindowImpl_put_Caption,
    BaseControlWindowImpl_get_Caption,
    BaseControlWindowImpl_put_WindowStyle,
    BaseControlWindowImpl_get_WindowStyle,
    BaseControlWindowImpl_put_WindowStyleEx,
    BaseControlWindowImpl_get_WindowStyleEx,
    BaseControlWindowImpl_put_AutoShow,
    BaseControlWindowImpl_get_AutoShow,
    BaseControlWindowImpl_put_WindowState,
    BaseControlWindowImpl_get_WindowState,
    BaseControlWindowImpl_put_BackgroundPalette,
    BaseControlWindowImpl_get_BackgroundPalette,
    BaseControlWindowImpl_put_Visible,
    BaseControlWindowImpl_get_Visible,
    BaseControlWindowImpl_put_Left,
    BaseControlWindowImpl_get_Left,
    BaseControlWindowImpl_put_Width,
    BaseControlWindowImpl_get_Width,
    BaseControlWindowImpl_put_Top,
    BaseControlWindowImpl_get_Top,
    BaseControlWindowImpl_put_Height,
    BaseControlWindowImpl_get_Height,
    BaseControlWindowImpl_put_Owner,
    BaseControlWindowImpl_get_Owner,
    BaseControlWindowImpl_put_MessageDrain,
    BaseControlWindowImpl_get_MessageDrain,
    BaseControlWindowImpl_get_BorderColor,
    BaseControlWindowImpl_put_BorderColor,
    BaseControlWindowImpl_get_FullScreenMode,
    BaseControlWindowImpl_put_FullScreenMode,
    BaseControlWindowImpl_SetWindowForeground,
    BaseControlWindowImpl_NotifyOwnerMessage,
    BaseControlWindowImpl_SetWindowPosition,
    BaseControlWindowImpl_GetWindowPosition,
    BaseControlWindowImpl_GetMinIdealImageSize,
    BaseControlWindowImpl_GetMaxIdealImageSize,
    BaseControlWindowImpl_GetRestorePosition,
    BaseControlWindowImpl_HideCursor,
    BaseControlWindowImpl_IsCursorHidden
};

static HRESULT WINAPI AMCertifiedOutputProtection_QueryInterface(IAMCertifiedOutputProtection *iface,
                                                                 REFIID riid, void **ppv)
{
    struct quartz_vmr *This = impl_from_IAMCertifiedOutputProtection(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI AMCertifiedOutputProtection_AddRef(IAMCertifiedOutputProtection *iface)
{
    struct quartz_vmr *This = impl_from_IAMCertifiedOutputProtection(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI AMCertifiedOutputProtection_Release(IAMCertifiedOutputProtection *iface)
{
    struct quartz_vmr *This = impl_from_IAMCertifiedOutputProtection(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI AMCertifiedOutputProtection_KeyExchange(IAMCertifiedOutputProtection *iface,
                                                              GUID* pRandom, BYTE** VarLenCertGH,
                                                              DWORD* pdwLengthCertGH)
{
    struct quartz_vmr *This = impl_from_IAMCertifiedOutputProtection(iface);

    FIXME("(%p/%p)->(%p, %p, %p) stub\n", iface, This, pRandom, VarLenCertGH, pdwLengthCertGH);
    return VFW_E_NO_COPP_HW;
}

static HRESULT WINAPI AMCertifiedOutputProtection_SessionSequenceStart(IAMCertifiedOutputProtection *iface,
                                                                       AMCOPPSignature* pSig)
{
    struct quartz_vmr *This = impl_from_IAMCertifiedOutputProtection(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, pSig);
    return VFW_E_NO_COPP_HW;
}

static HRESULT WINAPI AMCertifiedOutputProtection_ProtectionCommand(IAMCertifiedOutputProtection *iface,
                                                                    const AMCOPPCommand* cmd)
{
    struct quartz_vmr *This = impl_from_IAMCertifiedOutputProtection(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, cmd);
    return VFW_E_NO_COPP_HW;
}

static HRESULT WINAPI AMCertifiedOutputProtection_ProtectionStatus(IAMCertifiedOutputProtection *iface,
                                                                   const AMCOPPStatusInput* pStatusInput,
                                                                   AMCOPPStatusOutput* pStatusOutput)
{
    struct quartz_vmr *This = impl_from_IAMCertifiedOutputProtection(iface);

    FIXME("(%p/%p)->(%p, %p) stub\n", iface, This, pStatusInput, pStatusOutput);
    return VFW_E_NO_COPP_HW;
}

static const IAMCertifiedOutputProtectionVtbl IAMCertifiedOutputProtection_Vtbl =
{
    AMCertifiedOutputProtection_QueryInterface,
    AMCertifiedOutputProtection_AddRef,
    AMCertifiedOutputProtection_Release,
    AMCertifiedOutputProtection_KeyExchange,
    AMCertifiedOutputProtection_SessionSequenceStart,
    AMCertifiedOutputProtection_ProtectionCommand,
    AMCertifiedOutputProtection_ProtectionStatus
};

static HRESULT WINAPI AMFilterMiscFlags_QueryInterface(IAMFilterMiscFlags *iface, REFIID riid, void **ppv) {
    struct quartz_vmr *This = impl_from_IAMFilterMiscFlags(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI AMFilterMiscFlags_AddRef(IAMFilterMiscFlags *iface) {
    struct quartz_vmr *This = impl_from_IAMFilterMiscFlags(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI AMFilterMiscFlags_Release(IAMFilterMiscFlags *iface) {
    struct quartz_vmr *This = impl_from_IAMFilterMiscFlags(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static ULONG WINAPI AMFilterMiscFlags_GetMiscFlags(IAMFilterMiscFlags *iface) {
    return AM_FILTER_MISC_FLAGS_IS_RENDERER;
}

static const IAMFilterMiscFlagsVtbl IAMFilterMiscFlags_Vtbl = {
    AMFilterMiscFlags_QueryInterface,
    AMFilterMiscFlags_AddRef,
    AMFilterMiscFlags_Release,
    AMFilterMiscFlags_GetMiscFlags
};

static HRESULT WINAPI VMR7FilterConfig_QueryInterface(IVMRFilterConfig *iface, REFIID riid,
                                                      void** ppv)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR7FilterConfig_AddRef(IVMRFilterConfig *iface)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI VMR7FilterConfig_Release(IVMRFilterConfig *iface)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI VMR7FilterConfig_SetImageCompositor(IVMRFilterConfig *iface,
                                                          IVMRImageCompositor *compositor)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, compositor);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7FilterConfig_SetNumberOfStreams(IVMRFilterConfig *iface, DWORD max)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);

    FIXME("(%p/%p)->(%u) stub\n", iface, This, max);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7FilterConfig_GetNumberOfStreams(IVMRFilterConfig *iface, DWORD *max)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, max);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7FilterConfig_SetRenderingPrefs(IVMRFilterConfig *iface, DWORD renderflags)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);

    FIXME("(%p/%p)->(%u) stub\n", iface, This, renderflags);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7FilterConfig_GetRenderingPrefs(IVMRFilterConfig *iface, DWORD *renderflags)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, renderflags);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7FilterConfig_SetRenderingMode(IVMRFilterConfig *iface, DWORD mode)
{
    struct quartz_vmr *filter = impl_from_IVMRFilterConfig(iface);

    TRACE("iface %p, mode %#x.\n", iface, mode);

    return IVMRFilterConfig9_SetRenderingMode(&filter->IVMRFilterConfig9_iface, mode);
}

static HRESULT WINAPI VMR7FilterConfig_GetRenderingMode(IVMRFilterConfig *iface, DWORD *mode)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig(iface);

    TRACE("(%p/%p)->(%p)\n", iface, This, mode);
    if (!mode) return E_POINTER;

    if (This->mode)
        *mode = This->mode;
    else
        *mode = VMRMode_Windowed;

    return S_OK;
}

static const IVMRFilterConfigVtbl VMR7_FilterConfig_Vtbl =
{
    VMR7FilterConfig_QueryInterface,
    VMR7FilterConfig_AddRef,
    VMR7FilterConfig_Release,
    VMR7FilterConfig_SetImageCompositor,
    VMR7FilterConfig_SetNumberOfStreams,
    VMR7FilterConfig_GetNumberOfStreams,
    VMR7FilterConfig_SetRenderingPrefs,
    VMR7FilterConfig_GetRenderingPrefs,
    VMR7FilterConfig_SetRenderingMode,
    VMR7FilterConfig_GetRenderingMode
};

struct get_available_monitors_args
{
    VMRMONITORINFO *info7;
    VMR9MonitorInfo *info9;
    DWORD arraysize;
    DWORD numdev;
};

static BOOL CALLBACK get_available_monitors_proc(HMONITOR hmon, HDC hdc, LPRECT lprc, LPARAM lparam)
{
    struct get_available_monitors_args *args = (struct get_available_monitors_args *)lparam;
    MONITORINFOEXW mi;

    if (args->info7 || args->info9)
    {

        if (!args->arraysize)
            return FALSE;

        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(hmon, (MONITORINFO*)&mi))
            return TRUE;

        /* fill VMRMONITORINFO struct */
        if (args->info7)
        {
            VMRMONITORINFO *info = args->info7++;
            memset(info, 0, sizeof(*info));

            if (args->numdev > 0)
            {
                info->guid.pGUID = &info->guid.GUID;
                info->guid.GUID.Data4[7] = args->numdev;
            }
            else
                info->guid.pGUID = NULL;

            info->rcMonitor     = mi.rcMonitor;
            info->hMon          = hmon;
            info->dwFlags       = mi.dwFlags;

            lstrcpynW(info->szDevice, mi.szDevice, ARRAY_SIZE(info->szDevice));

            /* FIXME: how to get these values? */
            info->szDescription[0] = 0;
        }

        /* fill VMR9MonitorInfo struct */
        if (args->info9)
        {
            VMR9MonitorInfo *info = args->info9++;
            memset(info, 0, sizeof(*info));

            info->uDevID        = 0; /* FIXME */
            info->rcMonitor     = mi.rcMonitor;
            info->hMon          = hmon;
            info->dwFlags       = mi.dwFlags;

            lstrcpynW(info->szDevice, mi.szDevice, ARRAY_SIZE(info->szDevice));

            /* FIXME: how to get these values? */
            info->szDescription[0] = 0;
            info->dwVendorId    = 0;
            info->dwDeviceId    = 0;
            info->dwSubSysId    = 0;
            info->dwRevision    = 0;
        }

        args->arraysize--;
    }

    args->numdev++;
    return TRUE;
}

static HRESULT WINAPI VMR7MonitorConfig_QueryInterface(IVMRMonitorConfig *iface, REFIID riid,
                                                       LPVOID * ppv)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR7MonitorConfig_AddRef(IVMRMonitorConfig *iface)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI VMR7MonitorConfig_Release(IVMRMonitorConfig *iface)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI VMR7MonitorConfig_SetMonitor(IVMRMonitorConfig *iface, const VMRGUID *pGUID)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, pGUID);

    if (!pGUID)
        return E_POINTER;

    return S_OK;
}

static HRESULT WINAPI VMR7MonitorConfig_GetMonitor(IVMRMonitorConfig *iface, VMRGUID *pGUID)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, pGUID);

    if (!pGUID)
        return E_POINTER;

    pGUID->pGUID = NULL; /* default DirectDraw device */
    return S_OK;
}

static HRESULT WINAPI VMR7MonitorConfig_SetDefaultMonitor(IVMRMonitorConfig *iface,
                                                          const VMRGUID *pGUID)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, pGUID);

    if (!pGUID)
        return E_POINTER;

    return S_OK;
}

static HRESULT WINAPI VMR7MonitorConfig_GetDefaultMonitor(IVMRMonitorConfig *iface, VMRGUID *pGUID)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, pGUID);

    if (!pGUID)
        return E_POINTER;

    pGUID->pGUID = NULL; /* default DirectDraw device */
    return S_OK;
}

static HRESULT WINAPI VMR7MonitorConfig_GetAvailableMonitors(IVMRMonitorConfig *iface,
                                                             VMRMONITORINFO *info, DWORD arraysize,
                                                             DWORD *numdev)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig(iface);
    struct get_available_monitors_args args;

    FIXME("(%p/%p)->(%p, %u, %p) semi-stub\n", iface, This, info, arraysize, numdev);

    if (!numdev)
        return E_POINTER;

    if (info && arraysize == 0)
        return E_INVALIDARG;

    args.info7      = info;
    args.info9      = NULL;
    args.arraysize  = arraysize;
    args.numdev     = 0;
    EnumDisplayMonitors(NULL, NULL, get_available_monitors_proc, (LPARAM)&args);

    *numdev = args.numdev;
    return S_OK;
}

static const IVMRMonitorConfigVtbl VMR7_MonitorConfig_Vtbl =
{
    VMR7MonitorConfig_QueryInterface,
    VMR7MonitorConfig_AddRef,
    VMR7MonitorConfig_Release,
    VMR7MonitorConfig_SetMonitor,
    VMR7MonitorConfig_GetMonitor,
    VMR7MonitorConfig_SetDefaultMonitor,
    VMR7MonitorConfig_GetDefaultMonitor,
    VMR7MonitorConfig_GetAvailableMonitors
};

static HRESULT WINAPI VMR9MonitorConfig_QueryInterface(IVMRMonitorConfig9 *iface, REFIID riid,
                                                       LPVOID * ppv)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR9MonitorConfig_AddRef(IVMRMonitorConfig9 *iface)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI VMR9MonitorConfig_Release(IVMRMonitorConfig9 *iface)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI VMR9MonitorConfig_SetMonitor(IVMRMonitorConfig9 *iface, UINT uDev)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);

    FIXME("(%p/%p)->(%u) stub\n", iface, This, uDev);

    return S_OK;
}

static HRESULT WINAPI VMR9MonitorConfig_GetMonitor(IVMRMonitorConfig9 *iface, UINT *uDev)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, uDev);

    if (!uDev)
        return E_POINTER;

    *uDev = 0;
    return S_OK;
}

static HRESULT WINAPI VMR9MonitorConfig_SetDefaultMonitor(IVMRMonitorConfig9 *iface, UINT uDev)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);

    FIXME("(%p/%p)->(%u) stub\n", iface, This, uDev);

    return S_OK;
}

static HRESULT WINAPI VMR9MonitorConfig_GetDefaultMonitor(IVMRMonitorConfig9 *iface, UINT *uDev)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, uDev);

    if (!uDev)
        return E_POINTER;

    *uDev = 0;
    return S_OK;
}

static HRESULT WINAPI VMR9MonitorConfig_GetAvailableMonitors(IVMRMonitorConfig9 *iface,
                                                             VMR9MonitorInfo *info, DWORD arraysize,
                                                             DWORD *numdev)
{
    struct quartz_vmr *This = impl_from_IVMRMonitorConfig9(iface);
    struct get_available_monitors_args args;

    FIXME("(%p/%p)->(%p, %u, %p) semi-stub\n", iface, This, info, arraysize, numdev);

    if (!numdev)
        return E_POINTER;

    if (info && arraysize == 0)
        return E_INVALIDARG;

    args.info7      = NULL;
    args.info9      = info;
    args.arraysize  = arraysize;
    args.numdev     = 0;
    EnumDisplayMonitors(NULL, NULL, get_available_monitors_proc, (LPARAM)&args);

    *numdev = args.numdev;
    return S_OK;
}

static const IVMRMonitorConfig9Vtbl VMR9_MonitorConfig_Vtbl =
{
    VMR9MonitorConfig_QueryInterface,
    VMR9MonitorConfig_AddRef,
    VMR9MonitorConfig_Release,
    VMR9MonitorConfig_SetMonitor,
    VMR9MonitorConfig_GetMonitor,
    VMR9MonitorConfig_SetDefaultMonitor,
    VMR9MonitorConfig_GetDefaultMonitor,
    VMR9MonitorConfig_GetAvailableMonitors
};

static HRESULT WINAPI VMR9FilterConfig_QueryInterface(IVMRFilterConfig9 *iface, REFIID riid, LPVOID * ppv)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR9FilterConfig_AddRef(IVMRFilterConfig9 *iface)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI VMR9FilterConfig_Release(IVMRFilterConfig9 *iface)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI VMR9FilterConfig_SetImageCompositor(IVMRFilterConfig9 *iface, IVMRImageCompositor9 *compositor)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, compositor);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9FilterConfig_SetNumberOfStreams(IVMRFilterConfig9 *iface, DWORD count)
{
    struct quartz_vmr *filter = impl_from_IVMRFilterConfig9(iface);

    FIXME("iface %p, count %u, stub!\n", iface, count);

    if (!count)
    {
        WARN("Application requested zero streams; returning E_INVALIDARG.\n");
        return E_INVALIDARG;
    }

    EnterCriticalSection(&filter->renderer.filter.filter_cs);

    if (filter->stream_count)
    {
        LeaveCriticalSection(&filter->renderer.filter.filter_cs);
        WARN("Stream count is already set; returning VFW_E_WRONG_STATE.\n");
        return VFW_E_WRONG_STATE;
    }

    filter->stream_count = count;

    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI VMR9FilterConfig_GetNumberOfStreams(IVMRFilterConfig9 *iface, DWORD *count)
{
    struct quartz_vmr *filter = impl_from_IVMRFilterConfig9(iface);

    TRACE("filter %p, count %p.\n", filter, count);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);

    if (!filter->stream_count)
    {
        LeaveCriticalSection(&filter->renderer.filter.filter_cs);
        return VFW_E_VMR_NOT_IN_MIXER_MODE;
    }

    *count = filter->stream_count;

    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI VMR9FilterConfig_SetRenderingPrefs(IVMRFilterConfig9 *iface, DWORD renderflags)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);

    FIXME("(%p/%p)->(%u) stub\n", iface, This, renderflags);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9FilterConfig_GetRenderingPrefs(IVMRFilterConfig9 *iface, DWORD *renderflags)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);

    FIXME("(%p/%p)->(%p) stub\n", iface, This, renderflags);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9FilterConfig_SetRenderingMode(IVMRFilterConfig9 *iface, DWORD mode)
{
    HRESULT hr = S_OK;
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);

    TRACE("(%p/%p)->(%u)\n", iface, This, mode);

    EnterCriticalSection(&This->renderer.filter.filter_cs);
    if (This->mode)
    {
        LeaveCriticalSection(&This->renderer.filter.filter_cs);
        return VFW_E_WRONG_STATE;
    }

    if (This->allocator)
        IVMRSurfaceAllocator9_Release(This->allocator);
    if (This->presenter)
        IVMRImagePresenter9_Release(This->presenter);

    This->allocator = NULL;
    This->presenter = NULL;

    switch (mode)
    {
    case VMR9Mode_Windowed:
    case VMR9Mode_Windowless:
        This->cookie = ~0;

        if (FAILED(hr = VMR9DefaultAllocatorPresenterImpl_create(This, (void **)&This->presenter)))
        {
            ERR("Failed to create default presenter, hr %#x.\n", hr);
            break;
        }

        if (FAILED(hr = IVMRImagePresenter9_QueryInterface(This->presenter,
                    &IID_IVMRSurfaceAllocator9, (void **)&This->allocator)))
        {
            ERR("Failed to query for IVMRSurfaceAllocator9, hr %#x.\n", hr);
            IVMRImagePresenter9_Release(This->presenter);
            This->allocator = NULL;
            This->presenter = NULL;
            break;
        }

        hr = IVMRSurfaceAllocator9_AdviseNotify(This->allocator, &This->IVMRSurfaceAllocatorNotify9_iface);
        break;
    case VMR9Mode_Renderless:
        break;
    default:
        LeaveCriticalSection(&This->renderer.filter.filter_cs);
        return E_INVALIDARG;
    }

    if (mode != VMR9Mode_Windowed)
        video_window_cleanup(&This->window);

    This->mode = mode;
    LeaveCriticalSection(&This->renderer.filter.filter_cs);
    return hr;
}

static HRESULT WINAPI VMR9FilterConfig_GetRenderingMode(IVMRFilterConfig9 *iface, DWORD *mode)
{
    struct quartz_vmr *This = impl_from_IVMRFilterConfig9(iface);

    TRACE("(%p/%p)->(%p)\n", iface, This, mode);
    if (!mode)
        return E_POINTER;

    if (This->mode)
        *mode = This->mode;
    else
        *mode = VMR9Mode_Windowed;

    return S_OK;
}

static const IVMRFilterConfig9Vtbl VMR9_FilterConfig_Vtbl =
{
    VMR9FilterConfig_QueryInterface,
    VMR9FilterConfig_AddRef,
    VMR9FilterConfig_Release,
    VMR9FilterConfig_SetImageCompositor,
    VMR9FilterConfig_SetNumberOfStreams,
    VMR9FilterConfig_GetNumberOfStreams,
    VMR9FilterConfig_SetRenderingPrefs,
    VMR9FilterConfig_GetRenderingPrefs,
    VMR9FilterConfig_SetRenderingMode,
    VMR9FilterConfig_GetRenderingMode
};

static HRESULT WINAPI VMR7WindowlessControl_QueryInterface(IVMRWindowlessControl *iface, REFIID riid,
                                                           LPVOID * ppv)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR7WindowlessControl_AddRef(IVMRWindowlessControl *iface)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI VMR7WindowlessControl_Release(IVMRWindowlessControl *iface)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI VMR7WindowlessControl_GetNativeVideoSize(IVMRWindowlessControl *iface,
        LONG *width, LONG *height, LONG *aspect_width, LONG *aspect_height)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl(iface);

    TRACE("filter %p, width %p, height %p, aspect_width %p, aspect_height %p.\n",
            filter, width, height, aspect_width, aspect_height);

    if (!width || !height)
        return E_POINTER;

    *width = filter->bmiheader.biWidth;
    *height = filter->bmiheader.biHeight;
    if (aspect_width)
        *aspect_width = filter->bmiheader.biWidth;
    if (aspect_height)
        *aspect_height = filter->bmiheader.biHeight;

    return S_OK;
}

static HRESULT WINAPI VMR7WindowlessControl_GetMinIdealVideoSize(IVMRWindowlessControl *iface,
                                                                 LONG *width, LONG *height)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_GetMaxIdealVideoSize(IVMRWindowlessControl *iface,
                                                                 LONG *width, LONG *height)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_SetVideoPosition(IVMRWindowlessControl *iface,
                                                             const RECT *source, const RECT *dest)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    TRACE("(%p/%p)->(%p, %p)\n", iface, This, source, dest);

    EnterCriticalSection(&This->renderer.filter.filter_cs);

    if (source)
        This->window.src = *source;
    if (dest)
        This->window.dst = *dest;

    LeaveCriticalSection(&This->renderer.filter.filter_cs);

    return S_OK;
}

static HRESULT WINAPI VMR7WindowlessControl_GetVideoPosition(IVMRWindowlessControl *iface,
                                                             RECT *source, RECT *dest)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    if (source)
        *source = This->window.src;

    if (dest)
        *dest = This->window.dst;

    FIXME("(%p/%p)->(%p/%p) stub\n", iface, This, source, dest);
    return S_OK;
}

static HRESULT WINAPI VMR7WindowlessControl_GetAspectRatioMode(IVMRWindowlessControl *iface,
                                                               DWORD *mode)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_SetAspectRatioMode(IVMRWindowlessControl *iface,
                                                               DWORD mode)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_SetVideoClippingWindow(IVMRWindowlessControl *iface, HWND window)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl(iface);

    TRACE("iface %p, window %p.\n", iface, window);

    return IVMRWindowlessControl9_SetVideoClippingWindow(&filter->IVMRWindowlessControl9_iface, window);
}

static HRESULT WINAPI VMR7WindowlessControl_RepaintVideo(IVMRWindowlessControl *iface,
                                                         HWND hwnd, HDC hdc)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_DisplayModeChanged(IVMRWindowlessControl *iface)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_GetCurrentImage(IVMRWindowlessControl *iface,
                                                            BYTE **dib)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_SetBorderColor(IVMRWindowlessControl *iface,
                                                           COLORREF color)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_GetBorderColor(IVMRWindowlessControl *iface,
                                                           COLORREF *color)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_SetColorKey(IVMRWindowlessControl *iface, COLORREF color)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7WindowlessControl_GetColorKey(IVMRWindowlessControl *iface, COLORREF *color)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static const IVMRWindowlessControlVtbl VMR7_WindowlessControl_Vtbl =
{
    VMR7WindowlessControl_QueryInterface,
    VMR7WindowlessControl_AddRef,
    VMR7WindowlessControl_Release,
    VMR7WindowlessControl_GetNativeVideoSize,
    VMR7WindowlessControl_GetMinIdealVideoSize,
    VMR7WindowlessControl_GetMaxIdealVideoSize,
    VMR7WindowlessControl_SetVideoPosition,
    VMR7WindowlessControl_GetVideoPosition,
    VMR7WindowlessControl_GetAspectRatioMode,
    VMR7WindowlessControl_SetAspectRatioMode,
    VMR7WindowlessControl_SetVideoClippingWindow,
    VMR7WindowlessControl_RepaintVideo,
    VMR7WindowlessControl_DisplayModeChanged,
    VMR7WindowlessControl_GetCurrentImage,
    VMR7WindowlessControl_SetBorderColor,
    VMR7WindowlessControl_GetBorderColor,
    VMR7WindowlessControl_SetColorKey,
    VMR7WindowlessControl_GetColorKey
};

static HRESULT WINAPI VMR9WindowlessControl_QueryInterface(IVMRWindowlessControl9 *iface, REFIID riid, LPVOID * ppv)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR9WindowlessControl_AddRef(IVMRWindowlessControl9 *iface)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI VMR9WindowlessControl_Release(IVMRWindowlessControl9 *iface)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI VMR9WindowlessControl_GetNativeVideoSize(IVMRWindowlessControl9 *iface,
        LONG *width, LONG *height, LONG *aspect_width, LONG *aspect_height)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl9(iface);

    TRACE("filter %p, width %p, height %p, aspect_width %p, aspect_height %p.\n",
            filter, width, height, aspect_width, aspect_height);

    if (!width || !height)
        return E_POINTER;

    *width = filter->bmiheader.biWidth;
    *height = filter->bmiheader.biHeight;
    if (aspect_width)
        *aspect_width = filter->bmiheader.biWidth;
    if (aspect_height)
        *aspect_height = filter->bmiheader.biHeight;

    return S_OK;
}

static HRESULT WINAPI VMR9WindowlessControl_GetMinIdealVideoSize(IVMRWindowlessControl9 *iface, LONG *width, LONG *height)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9WindowlessControl_GetMaxIdealVideoSize(IVMRWindowlessControl9 *iface, LONG *width, LONG *height)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9WindowlessControl_SetVideoPosition(IVMRWindowlessControl9 *iface,
        const RECT *src, const RECT *dst)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl9(iface);

    TRACE("filter %p, src %s, dst %s.\n", filter, wine_dbgstr_rect(src), wine_dbgstr_rect(dst));

    EnterCriticalSection(&filter->renderer.filter.filter_cs);

    if (src)
        filter->window.src = *src;
    if (dst)
        filter->window.dst = *dst;

    LeaveCriticalSection(&filter->renderer.filter.filter_cs);

    return S_OK;
}

static HRESULT WINAPI VMR9WindowlessControl_GetVideoPosition(IVMRWindowlessControl9 *iface, RECT *src, RECT *dst)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl9(iface);

    TRACE("filter %p, src %p, dst %p.\n", filter, src, dst);

    if (src)
        *src = filter->window.src;

    if (dst)
        *dst = filter->window.dst;

    return S_OK;
}

static HRESULT WINAPI VMR9WindowlessControl_GetAspectRatioMode(IVMRWindowlessControl9 *iface, DWORD *mode)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl9(iface);

    TRACE("filter %p, mode %p.\n", filter, mode);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);
    *mode = filter->aspect_mode;
    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI VMR9WindowlessControl_SetAspectRatioMode(IVMRWindowlessControl9 *iface, DWORD mode)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl9(iface);

    TRACE("filter %p, mode %u.\n", filter, mode);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);
    filter->aspect_mode = mode;
    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI VMR9WindowlessControl_SetVideoClippingWindow(IVMRWindowlessControl9 *iface, HWND window)
{
    struct quartz_vmr *filter = impl_from_IVMRWindowlessControl9(iface);
    HRESULT hr;

    TRACE("filter %p, window %p.\n", filter, window);

    if (!IsWindow(window))
    {
        WARN("Invalid window %p, returning E_INVALIDARG.\n", window);
        return E_INVALIDARG;
    }

    EnterCriticalSection(&filter->renderer.filter.filter_cs);

    if (filter->renderer.sink.pin.peer)
    {
        LeaveCriticalSection(&filter->renderer.filter.filter_cs);
        WARN("Attempt to set the clipping window while connected; returning VFW_E_WRONG_STATE.\n");
        return VFW_E_WRONG_STATE;
    }

    filter->clipping_window = window;

    hr = IVMRFilterConfig9_SetNumberOfStreams(&filter->IVMRFilterConfig9_iface, 4);

    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return hr;
}

static HRESULT WINAPI VMR9WindowlessControl_RepaintVideo(IVMRWindowlessControl9 *iface, HWND hwnd, HDC hdc)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);
    HRESULT hr;

    FIXME("(%p/%p)->(...) semi-stub\n", iface, This);

    EnterCriticalSection(&This->renderer.filter.filter_cs);
    if (hwnd != This->clipping_window)
    {
        ERR("Not handling changing windows yet!!!\n");
        LeaveCriticalSection(&This->renderer.filter.filter_cs);
        return S_OK;
    }

    if (!This->allocator_d3d9_dev)
    {
        ERR("No d3d9 device!\n");
        LeaveCriticalSection(&This->renderer.filter.filter_cs);
        return VFW_E_WRONG_STATE;
    }

    /* Windowless extension */
    hr = IDirect3DDevice9_Present(This->allocator_d3d9_dev, NULL, NULL, NULL, NULL);
    LeaveCriticalSection(&This->renderer.filter.filter_cs);

    return hr;
}

static HRESULT WINAPI VMR9WindowlessControl_DisplayModeChanged(IVMRWindowlessControl9 *iface)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9WindowlessControl_GetCurrentImage(IVMRWindowlessControl9 *iface, BYTE **dib)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9WindowlessControl_SetBorderColor(IVMRWindowlessControl9 *iface, COLORREF color)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR9WindowlessControl_GetBorderColor(IVMRWindowlessControl9 *iface, COLORREF *color)
{
    struct quartz_vmr *This = impl_from_IVMRWindowlessControl9(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static const IVMRWindowlessControl9Vtbl VMR9_WindowlessControl_Vtbl =
{
    VMR9WindowlessControl_QueryInterface,
    VMR9WindowlessControl_AddRef,
    VMR9WindowlessControl_Release,
    VMR9WindowlessControl_GetNativeVideoSize,
    VMR9WindowlessControl_GetMinIdealVideoSize,
    VMR9WindowlessControl_GetMaxIdealVideoSize,
    VMR9WindowlessControl_SetVideoPosition,
    VMR9WindowlessControl_GetVideoPosition,
    VMR9WindowlessControl_GetAspectRatioMode,
    VMR9WindowlessControl_SetAspectRatioMode,
    VMR9WindowlessControl_SetVideoClippingWindow,
    VMR9WindowlessControl_RepaintVideo,
    VMR9WindowlessControl_DisplayModeChanged,
    VMR9WindowlessControl_GetCurrentImage,
    VMR9WindowlessControl_SetBorderColor,
    VMR9WindowlessControl_GetBorderColor
};

static HRESULT WINAPI VMR7SurfaceAllocatorNotify_QueryInterface(IVMRSurfaceAllocatorNotify *iface,
                                                                REFIID riid, LPVOID * ppv)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR7SurfaceAllocatorNotify_AddRef(IVMRSurfaceAllocatorNotify *iface)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);
    return IUnknown_AddRef(This->renderer.filter.outer_unk);
}

static ULONG WINAPI VMR7SurfaceAllocatorNotify_Release(IVMRSurfaceAllocatorNotify *iface)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);
    return IUnknown_Release(This->renderer.filter.outer_unk);
}

static HRESULT WINAPI VMR7SurfaceAllocatorNotify_AdviseSurfaceAllocator(IVMRSurfaceAllocatorNotify *iface,
                                                                        DWORD_PTR id,
                                                                        IVMRSurfaceAllocator *alloc)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7SurfaceAllocatorNotify_SetDDrawDevice(IVMRSurfaceAllocatorNotify *iface,
                                                                IDirectDraw7 *device, HMONITOR monitor)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7SurfaceAllocatorNotify_ChangeDDrawDevice(IVMRSurfaceAllocatorNotify *iface,
                                                                   IDirectDraw7 *device, HMONITOR monitor)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7SurfaceAllocatorNotify_RestoreDDrawSurfaces(IVMRSurfaceAllocatorNotify *iface)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7SurfaceAllocatorNotify_NotifyEvent(IVMRSurfaceAllocatorNotify *iface, LONG code,
                                                             LONG_PTR param1, LONG_PTR param2)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static HRESULT WINAPI VMR7SurfaceAllocatorNotify_SetBorderColor(IVMRSurfaceAllocatorNotify *iface,
                                                                COLORREF clrBorder)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static const IVMRSurfaceAllocatorNotifyVtbl VMR7_SurfaceAllocatorNotify_Vtbl =
{
    VMR7SurfaceAllocatorNotify_QueryInterface,
    VMR7SurfaceAllocatorNotify_AddRef,
    VMR7SurfaceAllocatorNotify_Release,
    VMR7SurfaceAllocatorNotify_AdviseSurfaceAllocator,
    VMR7SurfaceAllocatorNotify_SetDDrawDevice,
    VMR7SurfaceAllocatorNotify_ChangeDDrawDevice,
    VMR7SurfaceAllocatorNotify_RestoreDDrawSurfaces,
    VMR7SurfaceAllocatorNotify_NotifyEvent,
    VMR7SurfaceAllocatorNotify_SetBorderColor
};

static HRESULT WINAPI VMR9SurfaceAllocatorNotify_QueryInterface(IVMRSurfaceAllocatorNotify9 *iface, REFIID riid, LPVOID * ppv)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify9(iface);
    return IUnknown_QueryInterface(This->renderer.filter.outer_unk, riid, ppv);
}

static ULONG WINAPI VMR9SurfaceAllocatorNotify_AddRef(IVMRSurfaceAllocatorNotify9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRSurfaceAllocatorNotify9(iface);
    ULONG refcount = InterlockedIncrement(&filter->IVMRSurfaceAllocatorNotify9_refcount);

    TRACE("%p increasing refcount to %u.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI VMR9SurfaceAllocatorNotify_Release(IVMRSurfaceAllocatorNotify9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRSurfaceAllocatorNotify9(iface);
    ULONG refcount = InterlockedDecrement(&filter->IVMRSurfaceAllocatorNotify9_refcount);

    TRACE("%p decreasing refcount to %u.\n", iface, refcount);

    if (!refcount && !filter->renderer.filter.refcount)
        free(filter);

    return refcount;
}

static HRESULT WINAPI VMR9SurfaceAllocatorNotify_AdviseSurfaceAllocator(
        IVMRSurfaceAllocatorNotify9 *iface, DWORD_PTR cookie, IVMRSurfaceAllocator9 *allocator)
{
    struct quartz_vmr *filter = impl_from_IVMRSurfaceAllocatorNotify9(iface);
    IVMRImagePresenter9 *presenter;

    TRACE("filter %p, cookie %#Ix, allocator %p.\n", filter, cookie, allocator);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);

    filter->cookie = cookie;

    if (filter->renderer.sink.pin.peer)
    {
        LeaveCriticalSection(&filter->renderer.filter.filter_cs);
        WARN("Attempt to set allocator while connected; returning VFW_E_WRONG_STATE.\n");
        return VFW_E_WRONG_STATE;
    }

    if (FAILED(IVMRSurfaceAllocator9_QueryInterface(allocator, &IID_IVMRImagePresenter9, (void **)&presenter)))
    {
        LeaveCriticalSection(&filter->renderer.filter.filter_cs);
        return E_NOINTERFACE;
    }

    if (filter->allocator)
    {
        IVMRImagePresenter9_Release(filter->presenter);
        IVMRSurfaceAllocator9_Release(filter->allocator);
    }
    filter->allocator = allocator;
    filter->presenter = presenter;
    IVMRSurfaceAllocator9_AddRef(allocator);

    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI VMR9SurfaceAllocatorNotify_SetD3DDevice(IVMRSurfaceAllocatorNotify9 *iface,
        IDirect3DDevice9 *device, HMONITOR monitor)
{
    struct quartz_vmr *filter = impl_from_IVMRSurfaceAllocatorNotify9(iface);

    TRACE("filter %p, device %p, monitor %p.\n", filter, device, monitor);

    if (filter->allocator_d3d9_dev)
        IDirect3DDevice9_Release(filter->allocator_d3d9_dev);
    filter->allocator_d3d9_dev = device;
    IDirect3DDevice9_AddRef(device);

    return S_OK;
}

static HRESULT WINAPI VMR9SurfaceAllocatorNotify_ChangeD3DDevice(IVMRSurfaceAllocatorNotify9 *iface, IDirect3DDevice9 *device, HMONITOR monitor)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify9(iface);

    FIXME("(%p/%p)->(...) semi-stub\n", iface, This);
    if (This->allocator_d3d9_dev)
        IDirect3DDevice9_Release(This->allocator_d3d9_dev);
    This->allocator_d3d9_dev = device;
    IDirect3DDevice9_AddRef(This->allocator_d3d9_dev);

    return S_OK;
}

static HRESULT WINAPI VMR9SurfaceAllocatorNotify_AllocateSurfaceHelper(IVMRSurfaceAllocatorNotify9 *iface, VMR9AllocationInfo *allocinfo, DWORD *numbuffers, IDirect3DSurface9 **surface)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify9(iface);
    DWORD i;
    HRESULT hr = S_OK;

    TRACE("filter %p, allocinfo %p, numbuffers %p, surface %p.\n", This, numbuffers, allocinfo, surface);

    if (!allocinfo || !numbuffers || !surface)
        return E_POINTER;

    TRACE("Flags %#x, size %ux%u, format %u (%#x), pool %u, minimum buffers %u.\n",
            allocinfo->dwFlags, allocinfo->dwWidth, allocinfo->dwHeight,
            allocinfo->Format, allocinfo->Format, allocinfo->Pool, allocinfo->MinBuffers);

    if ((allocinfo->dwFlags & VMR9AllocFlag_TextureSurface)
            && (allocinfo->dwFlags & VMR9AllocFlag_OffscreenSurface))
    {
        WARN("Invalid flags specified; returning E_INVALIDARG.\n");
        return E_INVALIDARG;
    }

    if (!allocinfo->Format)
    {
        IDirect3DSurface9 *backbuffer;
        D3DSURFACE_DESC desc;

        IDirect3DDevice9_GetBackBuffer(This->allocator_d3d9_dev, 0, 0,
                D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        IDirect3DSurface9_GetDesc(backbuffer, &desc);
        IDirect3DSurface9_Release(backbuffer);
        allocinfo->Format = desc.Format;
    }

    if (!*numbuffers || *numbuffers < allocinfo->MinBuffers)
    {
        WARN("%u surfaces requested (minimum %u); returning E_INVALIDARG.\n",
                *numbuffers, allocinfo->MinBuffers);
        return E_INVALIDARG;
    }

    if (!This->allocator_d3d9_dev)
    {
        WARN("No Direct3D device; returning VFW_E_WRONG_STATE.\n");
        return VFW_E_WRONG_STATE;
    }

    if (allocinfo->dwFlags == VMR9AllocFlag_OffscreenSurface)
    {
        for (i = 0; i < *numbuffers; ++i)
        {
            hr = IDirect3DDevice9_CreateOffscreenPlainSurface(This->allocator_d3d9_dev,  allocinfo->dwWidth, allocinfo->dwHeight,
                                                             allocinfo->Format, allocinfo->Pool, &surface[i], NULL);
            if (FAILED(hr))
                break;
        }
    }
    else if (allocinfo->dwFlags == VMR9AllocFlag_TextureSurface)
    {
        for (i = 0; i < *numbuffers; ++i)
        {
            IDirect3DTexture9 *texture;

            hr = IDirect3DDevice9_CreateTexture(This->allocator_d3d9_dev, allocinfo->dwWidth, allocinfo->dwHeight, 1, D3DUSAGE_DYNAMIC,
                                                allocinfo->Format, allocinfo->Pool, &texture, NULL);
            if (FAILED(hr))
                break;
            IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface[i]);
            IDirect3DTexture9_Release(texture);
        }
    }
    else if (allocinfo->dwFlags == VMR9AllocFlag_3DRenderTarget)
    {
        for (i = 0; i < *numbuffers; ++i)
        {
            if (FAILED(hr = IDirect3DDevice9_CreateRenderTarget(This->allocator_d3d9_dev,
                    allocinfo->dwWidth, allocinfo->dwHeight, allocinfo->Format,
                    D3DMULTISAMPLE_NONE, 0, FALSE, &surface[i], NULL)))
                break;
        }
    }
    else
    {
        FIXME("Unhandled flags %#x.\n", allocinfo->dwFlags);
        return E_NOTIMPL;
    }

    if (FAILED(hr))
        WARN("%u/%u surfaces allocated, hr %#x.\n", i, *numbuffers, hr);

    if (i >= allocinfo->MinBuffers)
    {
        hr = S_OK;
        *numbuffers = i;
    }
    else
    {
        for ( ; i > 0; --i) IDirect3DSurface9_Release(surface[i - 1]);
        *numbuffers = 0;
    }
    return hr;
}

static HRESULT WINAPI VMR9SurfaceAllocatorNotify_NotifyEvent(IVMRSurfaceAllocatorNotify9 *iface, LONG code, LONG_PTR param1, LONG_PTR param2)
{
    struct quartz_vmr *This = impl_from_IVMRSurfaceAllocatorNotify9(iface);

    FIXME("(%p/%p)->(...) stub\n", iface, This);
    return E_NOTIMPL;
}

static const IVMRSurfaceAllocatorNotify9Vtbl VMR9_SurfaceAllocatorNotify_Vtbl =
{
    VMR9SurfaceAllocatorNotify_QueryInterface,
    VMR9SurfaceAllocatorNotify_AddRef,
    VMR9SurfaceAllocatorNotify_Release,
    VMR9SurfaceAllocatorNotify_AdviseSurfaceAllocator,
    VMR9SurfaceAllocatorNotify_SetD3DDevice,
    VMR9SurfaceAllocatorNotify_ChangeD3DDevice,
    VMR9SurfaceAllocatorNotify_AllocateSurfaceHelper,
    VMR9SurfaceAllocatorNotify_NotifyEvent
};

static inline struct quartz_vmr *impl_from_IVMRMixerControl9(IVMRMixerControl9 *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRMixerControl9_iface);
}

static HRESULT WINAPI mixer_control9_QueryInterface(IVMRMixerControl9 *iface, REFIID iid, void **out)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);
    return IUnknown_QueryInterface(filter->renderer.filter.outer_unk, iid, out);
}

static ULONG WINAPI mixer_control9_AddRef(IVMRMixerControl9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);
    return IUnknown_AddRef(filter->renderer.filter.outer_unk);
}

static ULONG WINAPI mixer_control9_Release(IVMRMixerControl9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);
    return IUnknown_Release(filter->renderer.filter.outer_unk);
}

static HRESULT WINAPI mixer_control9_SetAlpha(IVMRMixerControl9 *iface, DWORD stream, float alpha)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, stream %u, alpha %.8e, stub!\n", filter, stream, alpha);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_GetAlpha(IVMRMixerControl9 *iface, DWORD stream, float *alpha)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, stream %u, alpha %p, stub!\n", filter, stream, alpha);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_SetZOrder(IVMRMixerControl9 *iface, DWORD stream, DWORD z)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, stream %u, z %u, stub!\n", filter, stream, z);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_GetZOrder(IVMRMixerControl9 *iface, DWORD stream, DWORD *z)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, stream %u, z %p, stub!\n", filter, stream, z);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_SetOutputRect(IVMRMixerControl9 *iface,
        DWORD stream, const VMR9NormalizedRect *rect)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, stream %u, rect %s, stub!\n", filter, stream, debugstr_normalized_rect(rect));

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_GetOutputRect(IVMRMixerControl9 *iface,
        DWORD stream, VMR9NormalizedRect *rect)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, stream %u, rect %p, stub!\n", filter, stream, rect);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_SetBackgroundClr(IVMRMixerControl9 *iface, COLORREF color)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, color #%06x, stub!\n", filter, color);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_GetBackgroundClr(IVMRMixerControl9 *iface, COLORREF *color)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, color %p, stub!\n", filter, color);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_SetMixingPrefs(IVMRMixerControl9 *iface, DWORD flags)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, flags %#x, stub!\n", filter, flags);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);
    filter->mixing_prefs = flags;
    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI mixer_control9_GetMixingPrefs(IVMRMixerControl9 *iface, DWORD *flags)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, flags %p, stub!\n", filter, flags);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);
    *flags = filter->mixing_prefs;
    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI mixer_control9_SetProcAmpControl(IVMRMixerControl9 *iface,
        DWORD stream, VMR9ProcAmpControl *settings)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, settings %p, stub!\n", filter, settings);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_GetProcAmpControl(IVMRMixerControl9 *iface,
        DWORD stream, VMR9ProcAmpControl *settings)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, settings %p, stub!\n", filter, settings);

    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_control9_GetProcAmpControlRange(IVMRMixerControl9 *iface,
        DWORD stream, VMR9ProcAmpControlRange *settings)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerControl9(iface);

    FIXME("filter %p, settings %p, stub!\n", filter, settings);

    return E_NOTIMPL;
}

static const IVMRMixerControl9Vtbl mixer_control9_vtbl =
{
    mixer_control9_QueryInterface,
    mixer_control9_AddRef,
    mixer_control9_Release,
    mixer_control9_SetAlpha,
    mixer_control9_GetAlpha,
    mixer_control9_SetZOrder,
    mixer_control9_GetZOrder,
    mixer_control9_SetOutputRect,
    mixer_control9_GetOutputRect,
    mixer_control9_SetBackgroundClr,
    mixer_control9_GetBackgroundClr,
    mixer_control9_SetMixingPrefs,
    mixer_control9_GetMixingPrefs,
    mixer_control9_SetProcAmpControl,
    mixer_control9_GetProcAmpControl,
    mixer_control9_GetProcAmpControlRange,
};

static inline struct quartz_vmr *impl_from_IVMRMixerBitmap9(IVMRMixerBitmap9 *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRMixerBitmap9_iface);
}

static HRESULT WINAPI mixer_bitmap9_QueryInterface(IVMRMixerBitmap9 *iface, REFIID iid, void **out)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerBitmap9(iface);
    return IUnknown_QueryInterface(filter->renderer.filter.outer_unk, iid, out);
}

static ULONG WINAPI mixer_bitmap9_AddRef(IVMRMixerBitmap9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerBitmap9(iface);
    return IUnknown_AddRef(filter->renderer.filter.outer_unk);
}

static ULONG WINAPI mixer_bitmap9_Release(IVMRMixerBitmap9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRMixerBitmap9(iface);
    return IUnknown_Release(filter->renderer.filter.outer_unk);
}

static HRESULT WINAPI mixer_bitmap9_SetAlphaBitmap(IVMRMixerBitmap9 *iface,
        const VMR9AlphaBitmap *bitmap)
{
    FIXME("iface %p, bitmap %p, stub!\n", iface, bitmap);
    TRACE("dwFlags %#x, hdc %p, pDDS %p, rSrc %s, rDest %s, fAlpha %.8e, clrSrcKey #%06x, dwFilterMode %#x.\n",
            bitmap->dwFlags, bitmap->hdc, bitmap->pDDS, wine_dbgstr_rect(&bitmap->rSrc),
            debugstr_normalized_rect(&bitmap->rDest), bitmap->fAlpha, bitmap->clrSrcKey, bitmap->dwFilterMode);
    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_bitmap9_UpdateAlphaBitmapParameters(IVMRMixerBitmap9 *iface,
        const VMR9AlphaBitmap *bitmap)
{
    FIXME("iface %p, bitmap %p, stub!\n", iface, bitmap);
    return E_NOTIMPL;
}

static HRESULT WINAPI mixer_bitmap9_GetAlphaBitmapParameters(IVMRMixerBitmap9 *iface,
        VMR9AlphaBitmap *bitmap)
{
    FIXME("iface %p, bitmap %p, stub!\n", iface, bitmap);
    return E_NOTIMPL;
}

static const IVMRMixerBitmap9Vtbl mixer_bitmap9_vtbl =
{
    mixer_bitmap9_QueryInterface,
    mixer_bitmap9_AddRef,
    mixer_bitmap9_Release,
    mixer_bitmap9_SetAlphaBitmap,
    mixer_bitmap9_UpdateAlphaBitmapParameters,
    mixer_bitmap9_GetAlphaBitmapParameters,
};

static inline struct quartz_vmr *impl_from_IVMRAspectRatioControl9(IVMRAspectRatioControl9 *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IVMRAspectRatioControl9_iface);
}

static HRESULT WINAPI aspect_ratio_control9_QueryInterface(IVMRAspectRatioControl9 *iface, REFIID iid, void **out)
{
    struct quartz_vmr *filter = impl_from_IVMRAspectRatioControl9(iface);
    return IUnknown_QueryInterface(filter->renderer.filter.outer_unk, iid, out);
}

static ULONG WINAPI aspect_ratio_control9_AddRef(IVMRAspectRatioControl9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRAspectRatioControl9(iface);
    return IUnknown_AddRef(filter->renderer.filter.outer_unk);
}

static ULONG WINAPI aspect_ratio_control9_Release(IVMRAspectRatioControl9 *iface)
{
    struct quartz_vmr *filter = impl_from_IVMRAspectRatioControl9(iface);
    return IUnknown_Release(filter->renderer.filter.outer_unk);
}

static HRESULT WINAPI aspect_ratio_control9_GetAspectRatioMode(IVMRAspectRatioControl9 *iface, DWORD *mode)
{
    struct quartz_vmr *filter = impl_from_IVMRAspectRatioControl9(iface);

    TRACE("filter %p, mode %p.\n", filter, mode);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);
    *mode = filter->aspect_mode;
    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static HRESULT WINAPI aspect_ratio_control9_SetAspectRatioMode(IVMRAspectRatioControl9 *iface, DWORD mode)
{
    struct quartz_vmr *filter = impl_from_IVMRAspectRatioControl9(iface);

    TRACE("filter %p, mode %u.\n", filter, mode);

    EnterCriticalSection(&filter->renderer.filter.filter_cs);
    filter->aspect_mode = mode;
    LeaveCriticalSection(&filter->renderer.filter.filter_cs);
    return S_OK;
}

static const IVMRAspectRatioControl9Vtbl aspect_ratio_control9_vtbl =
{
    aspect_ratio_control9_QueryInterface,
    aspect_ratio_control9_AddRef,
    aspect_ratio_control9_Release,
    aspect_ratio_control9_GetAspectRatioMode,
    aspect_ratio_control9_SetAspectRatioMode,
};

static inline struct quartz_vmr *impl_from_IOverlay(IOverlay *iface)
{
    return CONTAINING_RECORD(iface, struct quartz_vmr, IOverlay_iface);
}

static HRESULT WINAPI overlay_QueryInterface(IOverlay *iface, REFIID iid, void **out)
{
    struct quartz_vmr *filter = impl_from_IOverlay(iface);
    return IPin_QueryInterface(&filter->renderer.sink.pin.IPin_iface, iid, out);
}

static ULONG WINAPI overlay_AddRef(IOverlay *iface)
{
    struct quartz_vmr *filter = impl_from_IOverlay(iface);
    return IPin_AddRef(&filter->renderer.sink.pin.IPin_iface);
}

static ULONG WINAPI overlay_Release(IOverlay *iface)
{
    struct quartz_vmr *filter = impl_from_IOverlay(iface);
    return IPin_Release(&filter->renderer.sink.pin.IPin_iface);
}

static HRESULT WINAPI overlay_GetPalette(IOverlay *iface, DWORD *count, PALETTEENTRY **palette)
{
    FIXME("iface %p, count %p, palette %p, stub!\n", iface, count, palette);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_SetPalette(IOverlay *iface, DWORD count, PALETTEENTRY *palette)
{
    FIXME("iface %p, count %u, palette %p, stub!\n", iface, count, palette);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_GetDefaultColorKey(IOverlay *iface, COLORKEY *key)
{
    FIXME("iface %p, key %p, stub!\n", iface, key);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_GetColorKey(IOverlay *iface, COLORKEY *key)
{
    FIXME("iface %p, key %p, stub!\n", iface, key);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_SetColorKey(IOverlay *iface, COLORKEY *key)
{
    FIXME("iface %p, key %p, stub!\n", iface, key);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_GetWindowHandle(IOverlay *iface, HWND *window)
{
    struct quartz_vmr *filter = impl_from_IOverlay(iface);

    TRACE("filter %p, window %p.\n", filter, window);

    if (!filter->window.hwnd)
        return VFW_E_WRONG_STATE;

    *window = filter->window.hwnd;
    return S_OK;
}

static HRESULT WINAPI overlay_GetClipList(IOverlay *iface, RECT *source, RECT *dest, RGNDATA **region)
{
    FIXME("iface %p, source %p, dest %p, region %p, stub!\n", iface, source, dest, region);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_GetVideoPosition(IOverlay *iface, RECT *source, RECT *dest)
{
    FIXME("iface %p, source %p, dest %p, stub!\n", iface, source, dest);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_Advise(IOverlay *iface, IOverlayNotify *sink, DWORD flags)
{
    FIXME("iface %p, sink %p, flags %#x, stub!\n", iface, sink, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI overlay_Unadvise(IOverlay *iface)
{
    FIXME("iface %p, stub!\n", iface);
    return E_NOTIMPL;
}

static const IOverlayVtbl overlay_vtbl =
{
    overlay_QueryInterface,
    overlay_AddRef,
    overlay_Release,
    overlay_GetPalette,
    overlay_SetPalette,
    overlay_GetDefaultColorKey,
    overlay_GetColorKey,
    overlay_SetColorKey,
    overlay_GetWindowHandle,
    overlay_GetClipList,
    overlay_GetVideoPosition,
    overlay_Advise,
    overlay_Unadvise,
};

static HRESULT vmr_create(IUnknown *outer, IUnknown **out, const CLSID *clsid)
{
    struct quartz_vmr *object;
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->hD3d9 = LoadLibraryA("d3d9.dll");
    if (!object->hD3d9)
    {
        WARN("Could not load d3d9.dll\n");
        free(object);
        return VFW_E_DDRAW_CAPS_NOT_SUITABLE;
    }

    strmbase_renderer_init(&object->renderer, outer, clsid, L"VMR Input0", &renderer_ops);
    object->IAMCertifiedOutputProtection_iface.lpVtbl = &IAMCertifiedOutputProtection_Vtbl;
    object->IAMFilterMiscFlags_iface.lpVtbl = &IAMFilterMiscFlags_Vtbl;
    object->IVMRAspectRatioControl9_iface.lpVtbl = &aspect_ratio_control9_vtbl;
    object->IVMRFilterConfig_iface.lpVtbl = &VMR7_FilterConfig_Vtbl;
    object->IVMRFilterConfig9_iface.lpVtbl = &VMR9_FilterConfig_Vtbl;
    object->IVMRMixerBitmap9_iface.lpVtbl = &mixer_bitmap9_vtbl;
    object->IVMRMixerControl9_iface.lpVtbl = &mixer_control9_vtbl;
    object->IVMRMonitorConfig_iface.lpVtbl = &VMR7_MonitorConfig_Vtbl;
    object->IVMRMonitorConfig9_iface.lpVtbl = &VMR9_MonitorConfig_Vtbl;
    object->IVMRSurfaceAllocatorNotify_iface.lpVtbl = &VMR7_SurfaceAllocatorNotify_Vtbl;
    object->IVMRSurfaceAllocatorNotify9_iface.lpVtbl = &VMR9_SurfaceAllocatorNotify_Vtbl;
    object->IVMRWindowlessControl_iface.lpVtbl = &VMR7_WindowlessControl_Vtbl;
    object->IVMRWindowlessControl9_iface.lpVtbl = &VMR9_WindowlessControl_Vtbl;
    object->IOverlay_iface.lpVtbl = &overlay_vtbl;

    video_window_init(&object->window, &IVideoWindow_VTable,
            &object->renderer.filter, &object->renderer.sink.pin, &window_ops);

    if (FAILED(hr = video_window_create_window(&object->window)))
    {
        video_window_cleanup(&object->window);
        strmbase_renderer_cleanup(&object->renderer);
        FreeLibrary(object->hD3d9);
        free(object);
        return hr;
    }

    object->mixing_prefs = MixerPref9_NoDecimation | MixerPref9_ARAdjustXorY
            | MixerPref9_BiLinearFiltering | MixerPref9_RenderTargetRGB;

    TRACE("Created VMR %p.\n", object);
    *out = &object->renderer.filter.IUnknown_inner;
    return S_OK;
}

HRESULT vmr7_create(IUnknown *outer, IUnknown **out)
{
    return vmr_create(outer, out, &CLSID_VideoMixingRenderer);
}

HRESULT vmr9_create(IUnknown *outer, IUnknown **out)
{
    return vmr_create(outer, out, &CLSID_VideoMixingRenderer9);
}


static HRESULT WINAPI VMR9_ImagePresenter_QueryInterface(IVMRImagePresenter9 *iface, REFIID riid, void **ppv)
{
    struct default_presenter *This = impl_from_IVMRImagePresenter9(iface);

    TRACE("(%p/%p)->(%s, %p)\n", This, iface, qzdebugstr_guid(riid), ppv);

    *ppv = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IVMRImagePresenter9))
        *ppv = &This->IVMRImagePresenter9_iface;
    else if (IsEqualIID(riid, &IID_IVMRSurfaceAllocator9))
        *ppv = &This->IVMRSurfaceAllocator9_iface;

    if (*ppv)
    {
        IUnknown_AddRef((IUnknown *)(*ppv));
        return S_OK;
    }

    FIXME("No interface for %s\n", debugstr_guid(riid));

    return E_NOINTERFACE;
}

static ULONG WINAPI VMR9_ImagePresenter_AddRef(IVMRImagePresenter9 *iface)
{
    struct default_presenter *This = impl_from_IVMRImagePresenter9(iface);
    ULONG refCount = InterlockedIncrement(&This->refCount);

    TRACE("(%p)->() AddRef from %d\n", iface, refCount - 1);

    return refCount;
}

static ULONG WINAPI VMR9_ImagePresenter_Release(IVMRImagePresenter9 *iface)
{
    struct default_presenter *This = impl_from_IVMRImagePresenter9(iface);
    ULONG refCount = InterlockedDecrement(&This->refCount);

    TRACE("(%p)->() Release from %d\n", iface, refCount + 1);

    if (!refCount)
    {
        DWORD i;
        TRACE("Destroying\n");
        IDirect3D9_Release(This->d3d9_ptr);

        TRACE("Number of surfaces: %u\n", This->num_surfaces);
        for (i = 0; i < This->num_surfaces; ++i)
        {
            IDirect3DSurface9 *surface = This->d3d9_surfaces[i];
            TRACE("Releasing surface %p\n", surface);
            if (surface)
                IDirect3DSurface9_Release(surface);
        }

        if (This->d3d9_dev)
            IDirect3DDevice9_Release(This->d3d9_dev);
        free(This->d3d9_surfaces);
        This->d3d9_surfaces = NULL;
        This->num_surfaces = 0;
        free(This);
        return 0;
    }
    return refCount;
}

static HRESULT WINAPI VMR9_ImagePresenter_StartPresenting(IVMRImagePresenter9 *iface, DWORD_PTR cookie)
{
    struct default_presenter *presenter = impl_from_IVMRImagePresenter9(iface);

    TRACE("presenter %p, cookie %#Ix.\n", presenter, cookie);

    return S_OK;
}

static HRESULT WINAPI VMR9_ImagePresenter_StopPresenting(IVMRImagePresenter9 *iface, DWORD_PTR cookie)
{
    struct default_presenter *presenter = impl_from_IVMRImagePresenter9(iface);

    TRACE("presenter %p, cookie %#Ix.\n", presenter, cookie);

    return S_OK;
}

static HRESULT WINAPI VMR9_ImagePresenter_PresentImage(IVMRImagePresenter9 *iface,
        DWORD_PTR cookie, VMR9PresentationInfo *info)
{
    struct default_presenter *presenter = impl_from_IVMRImagePresenter9(iface);
    const struct quartz_vmr *filter = presenter->pVMR9;
    IDirect3DDevice9 *device = presenter->d3d9_dev;
    const RECT src = filter->window.src;
    IDirect3DSurface9 *backbuffer;
    RECT dst = filter->window.dst;
    HRESULT hr;

    TRACE("presenter %p, cookie %#Ix, info %p.\n", presenter, cookie, info);

    /* This might happen if we don't have active focus (eg on a different virtual desktop) */
    if (!device)
        return S_OK;

    if (FAILED(hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0)))
        ERR("Failed to clear, hr %#x.\n", hr);

    if (FAILED(hr = IDirect3DDevice9_BeginScene(device)))
        ERR("Failed to begin scene, hr %#x.\n", hr);

    if (FAILED(hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)))
    {
        ERR("Failed to get backbuffer, hr %#x.\n", hr);
        return hr;
    }

    if (FAILED(hr = IDirect3DDevice9_StretchRect(device, info->lpSurf, NULL, backbuffer, NULL, D3DTEXF_POINT)))
        ERR("Failed to blit image, hr %#x.\n", hr);
    IDirect3DSurface9_Release(backbuffer);

    if (FAILED(hr = IDirect3DDevice9_EndScene(device)))
        ERR("Failed to end scene, hr %#x.\n", hr);

    if (filter->aspect_mode == VMR9ARMode_LetterBox)
    {
        unsigned int src_width = src.right - src.left, src_height = src.bottom - src.top;
        unsigned int dst_width = dst.right - dst.left, dst_height = dst.bottom - dst.top;

        if (src_width * dst_height > dst_width * src_height)
        {
            /* src is "wider" than dst. */
            unsigned int dst_center = (dst.top + dst.bottom) / 2;
            unsigned int scaled_height = src_height * dst_width / src_width;

            dst.top = dst_center - scaled_height / 2;
            dst.bottom = dst.top + scaled_height;
        }
        else if (src_width * dst_height < dst_width * src_height)
        {
            /* src is "taller" than dst. */
            unsigned int dst_center = (dst.left + dst.right) / 2;
            unsigned int scaled_width = src_width * dst_height / src_height;

            dst.left = dst_center - scaled_width / 2;
            dst.right = dst.left + scaled_width;
        }
    }

    if (FAILED(hr = IDirect3DDevice9_Present(device, &src, &dst, NULL, NULL)))
        ERR("Failed to present, hr %#x.\n", hr);

    return S_OK;
}

static const IVMRImagePresenter9Vtbl VMR9_ImagePresenter =
{
    VMR9_ImagePresenter_QueryInterface,
    VMR9_ImagePresenter_AddRef,
    VMR9_ImagePresenter_Release,
    VMR9_ImagePresenter_StartPresenting,
    VMR9_ImagePresenter_StopPresenting,
    VMR9_ImagePresenter_PresentImage
};

static HRESULT WINAPI VMR9_SurfaceAllocator_QueryInterface(IVMRSurfaceAllocator9 *iface, REFIID iid, void **out)
{
    struct default_presenter *presenter = impl_from_IVMRSurfaceAllocator9(iface);
    return IVMRImagePresenter9_QueryInterface(&presenter->IVMRImagePresenter9_iface, iid, out);
}

static ULONG WINAPI VMR9_SurfaceAllocator_AddRef(IVMRSurfaceAllocator9 *iface)
{
    struct default_presenter *presenter = impl_from_IVMRSurfaceAllocator9(iface);
    return IVMRImagePresenter9_AddRef(&presenter->IVMRImagePresenter9_iface);
}

static ULONG WINAPI VMR9_SurfaceAllocator_Release(IVMRSurfaceAllocator9 *iface)
{
    struct default_presenter *presenter = impl_from_IVMRSurfaceAllocator9(iface);
    return IVMRImagePresenter9_Release(&presenter->IVMRImagePresenter9_iface);
}

static void adjust_surface_size(const D3DCAPS9 *caps, VMR9AllocationInfo *allocinfo)
{
    UINT width, height;

    /* There are no restrictions on the size of offscreen surfaces. */
    if (!(allocinfo->dwFlags & VMR9AllocFlag_TextureSurface))
        return;

    if (!(caps->TextureCaps & D3DPTEXTURECAPS_POW2) || (caps->TextureCaps & D3DPTEXTURECAPS_SQUAREONLY))
    {
        width = allocinfo->dwWidth;
        height = allocinfo->dwHeight;
    }
    else
    {
        width = height = 1;
        while (width < allocinfo->dwWidth)
            width *= 2;

        while (height < allocinfo->dwHeight)
            height *= 2;
        FIXME("NPOW2 support missing, not using proper surfaces!\n");
    }

    if (caps->TextureCaps & D3DPTEXTURECAPS_SQUAREONLY)
    {
        if (height > width)
            width = height;
        else
            height = width;
        FIXME("Square texture support required..\n");
    }

    allocinfo->dwHeight = height;
    allocinfo->dwWidth = width;
}

static UINT d3d9_adapter_from_hwnd(IDirect3D9 *d3d9, HWND hwnd, HMONITOR *mon_out)
{
    UINT d3d9_adapter;
    HMONITOR mon;

    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (!mon)
        d3d9_adapter = 0;
    else
    {
        for (d3d9_adapter = 0; d3d9_adapter < IDirect3D9_GetAdapterCount(d3d9); ++d3d9_adapter)
        {
            if (mon == IDirect3D9_GetAdapterMonitor(d3d9, d3d9_adapter))
                break;
        }
        if (d3d9_adapter >= IDirect3D9_GetAdapterCount(d3d9))
            d3d9_adapter = 0;
    }
    if (mon_out)
        *mon_out = mon;
    return d3d9_adapter;
}

static HRESULT WINAPI VMR9_SurfaceAllocator_InitializeDevice(IVMRSurfaceAllocator9 *iface,
        DWORD_PTR cookie, VMR9AllocationInfo *info, DWORD *numbuffers)
{
    struct default_presenter *This = impl_from_IVMRSurfaceAllocator9(iface);
    D3DPRESENT_PARAMETERS d3dpp;
    IDirect3DDevice9 *device;
    DWORD d3d9_adapter;
    D3DCAPS9 caps;
    HWND window;
    HRESULT hr;

    TRACE("presenter %p, cookie %#Ix, info %p, numbuffers %p.\n", This, cookie, info, numbuffers);

    This->info = *info;

    if (This->pVMR9->mode == VMR9Mode_Windowed)
        window = This->pVMR9->window.hwnd;
    else
        window = This->pVMR9->clipping_window;

    /* Obtain a monitor and d3d9 device */
    d3d9_adapter = d3d9_adapter_from_hwnd(This->d3d9_ptr, window, &This->hMon);

    /* Now try to create the d3d9 device */
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.Windowed = TRUE;
    d3dpp.hDeviceWindow = window;
    d3dpp.SwapEffect = D3DSWAPEFFECT_COPY;
    d3dpp.BackBufferWidth = info->dwWidth;
    d3dpp.BackBufferHeight = info->dwHeight;

    hr = IDirect3D9_CreateDevice(This->d3d9_ptr, d3d9_adapter, D3DDEVTYPE_HAL,
            NULL, D3DCREATE_MIXED_VERTEXPROCESSING, &d3dpp, &device);
    if (FAILED(hr))
    {
        ERR("Could not create device: %08x\n", hr);
        return hr;
    }

    IDirect3DDevice9_GetDeviceCaps(device, &caps);
    if (!(caps.DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES))
    {
        WARN("Device does not support blitting from textures.\n");
        IDirect3DDevice9_Release(device);
        return VFW_E_DDRAW_CAPS_NOT_SUITABLE;
    }

    This->d3d9_dev = device;
    IVMRSurfaceAllocatorNotify9_SetD3DDevice(This->SurfaceAllocatorNotify, This->d3d9_dev, This->hMon);

    if (!(This->d3d9_surfaces = calloc(*numbuffers, sizeof(IDirect3DSurface9 *))))
        return E_OUTOFMEMORY;

    adjust_surface_size(&caps, info);

    hr = IVMRSurfaceAllocatorNotify9_AllocateSurfaceHelper(This->SurfaceAllocatorNotify,
            info, numbuffers, This->d3d9_surfaces);
    if (FAILED(hr))
    {
        ERR("Failed to allocate surfaces, hr %#x.\n", hr);
        IVMRSurfaceAllocator9_TerminateDevice(This->pVMR9->allocator, This->pVMR9->cookie);
        return hr;
    }

    This->num_surfaces = *numbuffers;

    return S_OK;
}

static HRESULT WINAPI VMR9_SurfaceAllocator_TerminateDevice(IVMRSurfaceAllocator9 *iface, DWORD_PTR cookie)
{
    TRACE("iface %p, cookie %#lx.\n", iface, cookie);

    return S_OK;
}

static HRESULT WINAPI VMR9_SurfaceAllocator_GetSurface(IVMRSurfaceAllocator9 *iface,
        DWORD_PTR cookie, DWORD surfaceindex, DWORD flags, IDirect3DSurface9 **surface)
{
    struct default_presenter *This = impl_from_IVMRSurfaceAllocator9(iface);

    /* Update everything first, this is needed because the surface might be destroyed in the reset */
    if (!This->d3d9_dev)
    {
        TRACE("Device has left me!\n");
        return E_FAIL;
    }

    if (surfaceindex >= This->num_surfaces)
    {
        ERR("surfaceindex is greater than num_surfaces\n");
        return E_FAIL;
    }
    *surface = This->d3d9_surfaces[surfaceindex];
    IDirect3DSurface9_AddRef(*surface);

    return S_OK;
}

static HRESULT WINAPI VMR9_SurfaceAllocator_AdviseNotify(IVMRSurfaceAllocator9 *iface,
        IVMRSurfaceAllocatorNotify9 *notify)
{
    struct default_presenter *presenter = impl_from_IVMRSurfaceAllocator9(iface);

    TRACE("presenter %p, notify %p.\n", presenter, notify);

    /* No AddRef taken here or the base VMR9 filter would never be destroyed */
    presenter->SurfaceAllocatorNotify = notify;
    return S_OK;
}

static const IVMRSurfaceAllocator9Vtbl VMR9_SurfaceAllocator =
{
    VMR9_SurfaceAllocator_QueryInterface,
    VMR9_SurfaceAllocator_AddRef,
    VMR9_SurfaceAllocator_Release,
    VMR9_SurfaceAllocator_InitializeDevice,
    VMR9_SurfaceAllocator_TerminateDevice,
    VMR9_SurfaceAllocator_GetSurface,
    VMR9_SurfaceAllocator_AdviseNotify,
};

static IDirect3D9 *init_d3d9(HMODULE d3d9_handle)
{
    IDirect3D9 * (__stdcall * d3d9_create)(UINT SDKVersion);

    d3d9_create = (void *)GetProcAddress(d3d9_handle, "Direct3DCreate9");
    if (!d3d9_create) return NULL;

    return d3d9_create(D3D_SDK_VERSION);
}

static HRESULT VMR9DefaultAllocatorPresenterImpl_create(struct quartz_vmr *parent, LPVOID * ppv)
{
    struct default_presenter *object;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->d3d9_ptr = init_d3d9(parent->hD3d9);
    if (!object->d3d9_ptr)
    {
        WARN("Could not initialize d3d9.dll\n");
        free(object);
        return VFW_E_DDRAW_CAPS_NOT_SUITABLE;
    }

    object->IVMRImagePresenter9_iface.lpVtbl = &VMR9_ImagePresenter;
    object->IVMRSurfaceAllocator9_iface.lpVtbl = &VMR9_SurfaceAllocator;

    object->refCount = 1;
    object->pVMR9 = parent;

    TRACE("Created default presenter %p.\n", object);
    *ppv = &object->IVMRImagePresenter9_iface;
    return S_OK;
}
