/* WMA Decoder DMO / MF Transform
 *
 * Copyright 2022 Rémi Bernon for CodeWeavers
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

#include "gst_private.h"

#include "mfapi.h"
#include "mferror.h"
#include "mfobjects.h"
#include "mftransform.h"
#include "wmcodecdsp.h"

#include "wine/debug.h"
#include "wine/heap.h"

WINE_DEFAULT_DEBUG_CHANNEL(wmadec);

static const GUID *wma_decoder_input_types[] =
{
    &MEDIASUBTYPE_MSAUDIO1,
    &MFAudioFormat_WMAudioV8,
    &MFAudioFormat_WMAudioV9,
    &MFAudioFormat_WMAudio_Lossless,
};
static const GUID *wma_decoder_output_types[] =
{
    &MFAudioFormat_Float,
    &MFAudioFormat_PCM,
};

struct wma_decoder
{
    IUnknown IUnknown_inner;
    IMFTransform IMFTransform_iface;
    IMediaObject IMediaObject_iface;
    IPropertyBag IPropertyBag_iface;
    IUnknown *outer;
    LONG refcount;
    IMFMediaType *input_type;
    IMFMediaType *output_type;
};

static inline struct wma_decoder *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IUnknown_inner);
}

static HRESULT WINAPI unknown_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    struct wma_decoder *decoder = impl_from_IUnknown(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown))
        *out = &decoder->IUnknown_inner;
    else if (IsEqualGUID(iid, &IID_IMFTransform))
        *out = &decoder->IMFTransform_iface;
    else if (IsEqualGUID(iid, &IID_IMediaObject))
        *out = &decoder->IMediaObject_iface;
    else if (IsEqualIID(iid, &IID_IPropertyBag))
        *out = &decoder->IPropertyBag_iface;
    else
    {
        *out = NULL;
        WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*out);
    return S_OK;
}

static ULONG WINAPI unknown_AddRef(IUnknown *iface)
{
    struct wma_decoder *decoder = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedIncrement(&decoder->refcount);

    TRACE("iface %p increasing refcount to %u.\n", decoder, refcount);

    return refcount;
}

static ULONG WINAPI unknown_Release(IUnknown *iface)
{
    struct wma_decoder *decoder = impl_from_IUnknown(iface);
    ULONG refcount = InterlockedDecrement(&decoder->refcount);

    TRACE("iface %p decreasing refcount to %u.\n", decoder, refcount);

    if (!refcount)
    {
        if (decoder->input_type)
            IMFMediaType_Release(decoder->input_type);
        if (decoder->output_type)
            IMFMediaType_Release(decoder->output_type);
        free(decoder);
    }

    return refcount;
}

static const IUnknownVtbl unknown_vtbl =
{
    unknown_QueryInterface,
    unknown_AddRef,
    unknown_Release,
};

static struct wma_decoder *impl_from_IMFTransform(IMFTransform *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IMFTransform_iface);
}

static HRESULT WINAPI transform_QueryInterface(IMFTransform *iface, REFIID iid, void **out)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    return IUnknown_QueryInterface(decoder->outer, iid, out);
}

static ULONG WINAPI transform_AddRef(IMFTransform *iface)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    return IUnknown_AddRef(decoder->outer);
}

static ULONG WINAPI transform_Release(IMFTransform *iface)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    return IUnknown_Release(decoder->outer);
}

static HRESULT WINAPI transform_GetStreamLimits(IMFTransform *iface, DWORD *input_minimum,
        DWORD *input_maximum, DWORD *output_minimum, DWORD *output_maximum)
{
    FIXME("iface %p, input_minimum %p, input_maximum %p, output_minimum %p, output_maximum %p stub!\n",
            iface, input_minimum, input_maximum, output_minimum, output_maximum);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetStreamCount(IMFTransform *iface, DWORD *inputs, DWORD *outputs)
{
    FIXME("iface %p, inputs %p, outputs %p stub!\n", iface, inputs, outputs);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetStreamIDs(IMFTransform *iface, DWORD input_size, DWORD *inputs,
        DWORD output_size, DWORD *outputs)
{
    FIXME("iface %p, input_size %u, inputs %p, output_size %u, outputs %p stub!\n", iface,
            input_size, inputs, output_size, outputs);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStreamInfo(IMFTransform *iface, DWORD id, MFT_INPUT_STREAM_INFO *info)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    UINT32 block_alignment;
    HRESULT hr;

    TRACE("iface %p, id %u, info %p.\n", iface, id, info);

    if (!decoder->input_type || !decoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = IMFMediaType_GetUINT32(decoder->input_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &block_alignment)))
        return hr;

    info->hnsMaxLatency = 0;
    info->dwFlags = 0;
    info->cbSize = block_alignment;
    info->cbMaxLookahead = 0;
    info->cbAlignment = 1;

    return S_OK;
}

static HRESULT WINAPI transform_GetOutputStreamInfo(IMFTransform *iface, DWORD id, MFT_OUTPUT_STREAM_INFO *info)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    UINT32 channel_count, block_alignment;
    HRESULT hr;

    TRACE("iface %p, id %u, info %p.\n", iface, id, info);

    if (!decoder->input_type || !decoder->output_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = IMFMediaType_GetUINT32(decoder->output_type, &MF_MT_AUDIO_NUM_CHANNELS, &channel_count)))
        return hr;
    if (FAILED(hr = IMFMediaType_GetUINT32(decoder->output_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &block_alignment)))
        return hr;

    info->dwFlags = 0;
    info->cbSize = 1024 * block_alignment * channel_count;
    info->cbAlignment = 1;

    return S_OK;
}

static HRESULT WINAPI transform_GetAttributes(IMFTransform *iface, IMFAttributes **attributes)
{
    FIXME("iface %p, attributes %p stub!\n", iface, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    FIXME("iface %p, id %u, attributes %p stub!\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputStreamAttributes(IMFTransform *iface, DWORD id, IMFAttributes **attributes)
{
    FIXME("iface %p, id %u, attributes %p stub!\n", iface, id, attributes);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_DeleteInputStream(IMFTransform *iface, DWORD id)
{
    FIXME("iface %p, id %u stub!\n", iface, id);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_AddInputStreams(IMFTransform *iface, DWORD streams, DWORD *ids)
{
    FIXME("iface %p, streams %u, ids %p stub!\n", iface, streams, ids);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    FIXME("iface %p, id %u, index %u, type %p stub!\n", iface, id, index, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputAvailableType(IMFTransform *iface, DWORD id, DWORD index,
        IMFMediaType **type)
{
    UINT32 channel_count, sample_size, sample_rate, block_alignment;
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    IMFMediaType *media_type;
    const GUID *output_type;
    HRESULT hr;

    TRACE("iface %p, id %u, index %u, type %p.\n", iface, id, index, type);

    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    *type = NULL;

    if (index >= ARRAY_SIZE(wma_decoder_output_types))
        return MF_E_NO_MORE_TYPES;
    output_type = wma_decoder_output_types[index];

    if (FAILED(hr = MFCreateMediaType(&media_type)))
        return hr;

    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetGUID(media_type, &MF_MT_SUBTYPE, output_type)))
        goto done;

    if (IsEqualGUID(output_type, &MFAudioFormat_Float))
        sample_size = 32;
    else if (IsEqualGUID(output_type, &MFAudioFormat_PCM))
        sample_size = 16;
    else
    {
        FIXME("Subtype %s not implemented!\n", debugstr_guid(output_type));
        hr = E_NOTIMPL;
        goto done;
    }

    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, sample_size)))
        goto done;

    if (FAILED(hr = IMFMediaType_GetUINT32(decoder->input_type, &MF_MT_AUDIO_NUM_CHANNELS, &channel_count)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_NUM_CHANNELS, channel_count)))
        goto done;

    if (FAILED(hr = IMFMediaType_GetUINT32(decoder->input_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &sample_rate)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate)))
        goto done;

    block_alignment = sample_size * channel_count / 8;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, block_alignment)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sample_rate * block_alignment)))
        goto done;

    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_FIXED_SIZE_SAMPLES, 1)))
        goto done;
    if (FAILED(hr = IMFMediaType_SetUINT32(media_type, &MF_MT_AUDIO_PREFER_WAVEFORMATEX, 1)))
        goto done;

done:
    if (SUCCEEDED(hr))
        IMFMediaType_AddRef((*type = media_type));

    IMFMediaType_Release(media_type);
    return hr;
}

static HRESULT WINAPI transform_SetInputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    MF_ATTRIBUTE_TYPE item_type;
    GUID major, subtype;
    HRESULT hr;
    ULONG i;

    TRACE("iface %p, id %u, type %p, flags %#x.\n", iface, id, type, flags);

    if (FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) ||
        FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return hr;

    if (!IsEqualGUID(&major, &MFMediaType_Audio))
        return MF_E_INVALIDMEDIATYPE;

    for (i = 0; i < ARRAY_SIZE(wma_decoder_input_types); ++i)
        if (IsEqualGUID(&subtype, wma_decoder_input_types[i]))
            break;
    if (i == ARRAY_SIZE(wma_decoder_input_types))
        return MF_E_INVALIDMEDIATYPE;

    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_USER_DATA, &item_type)) ||
        item_type != MF_ATTRIBUTE_BLOB)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_NUM_CHANNELS, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;

    if (!decoder->input_type && FAILED(hr = MFCreateMediaType(&decoder->input_type)))
        return hr;

    if (decoder->output_type)
    {
        IMFMediaType_Release(decoder->output_type);
        decoder->output_type = NULL;
    }

    if (FAILED(hr = IMFMediaType_CopyAllItems(type, (IMFAttributes *)decoder->input_type)))
    {
        IMFMediaType_Release(decoder->input_type);
        decoder->input_type = NULL;
    }

    return hr;
}

static HRESULT WINAPI transform_SetOutputType(IMFTransform *iface, DWORD id, IMFMediaType *type, DWORD flags)
{
    struct wma_decoder *decoder = impl_from_IMFTransform(iface);
    MF_ATTRIBUTE_TYPE item_type;
    ULONG i, sample_size;
    GUID major, subtype;
    HRESULT hr;

    TRACE("iface %p, id %u, type %p, flags %#x.\n", iface, id, type, flags);

    if (!decoder->input_type)
        return MF_E_TRANSFORM_TYPE_NOT_SET;

    if (FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_MAJOR_TYPE, &major)) ||
        FAILED(hr = IMFMediaType_GetGUID(type, &MF_MT_SUBTYPE, &subtype)))
        return hr;

    if (!IsEqualGUID(&major, &MFMediaType_Audio))
        return MF_E_INVALIDMEDIATYPE;

    for (i = 0; i < ARRAY_SIZE(wma_decoder_output_types); ++i)
        if (IsEqualGUID(&subtype, wma_decoder_output_types[i]))
            break;
    if (i == ARRAY_SIZE(wma_decoder_output_types))
        return MF_E_INVALIDMEDIATYPE;

    if (IsEqualGUID(&subtype, &MFAudioFormat_Float))
        sample_size = 32;
    else if (IsEqualGUID(&subtype, &MFAudioFormat_PCM))
        sample_size = 16;
    else
    {
        FIXME("Subtype %s not implemented!\n", debugstr_guid(&subtype));
        hr = E_NOTIMPL;
        return hr;
    }

    if (FAILED(IMFMediaType_SetUINT32(decoder->input_type, &MF_MT_AUDIO_BITS_PER_SAMPLE, sample_size)))
        return MF_E_INVALIDMEDIATYPE;

    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_BITS_PER_SAMPLE, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_NUM_CHANNELS, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;
    if (FAILED(IMFMediaType_GetItemType(type, &MF_MT_AUDIO_BLOCK_ALIGNMENT, &item_type)) ||
        item_type != MF_ATTRIBUTE_UINT32)
        return MF_E_INVALIDMEDIATYPE;

    if (!decoder->output_type && FAILED(hr = MFCreateMediaType(&decoder->output_type)))
        return hr;

    if (FAILED(hr = IMFMediaType_CopyAllItems(type, (IMFAttributes *)decoder->output_type)))
        goto failed;

    return S_OK;

failed:
    IMFMediaType_Release(decoder->output_type);
    decoder->output_type = NULL;
    return hr;
}

static HRESULT WINAPI transform_GetInputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    FIXME("iface %p, id %u, type %p stub!\n", iface, id, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputCurrentType(IMFTransform *iface, DWORD id, IMFMediaType **type)
{
    FIXME("iface %p, id %u, type %p stub!\n", iface, id, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetInputStatus(IMFTransform *iface, DWORD id, DWORD *flags)
{
    FIXME("iface %p, id %u, flags %p stub!\n", iface, id, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_GetOutputStatus(IMFTransform *iface, DWORD *flags)
{
    FIXME("iface %p, flags %p stub!\n", iface, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_SetOutputBounds(IMFTransform *iface, LONGLONG lower, LONGLONG upper)
{
    FIXME("iface %p, lower %s, upper %s stub!\n", iface, wine_dbgstr_longlong(lower),
            wine_dbgstr_longlong(upper));
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessEvent(IMFTransform *iface, DWORD id, IMFMediaEvent *event)
{
    FIXME("iface %p, id %u, event %p stub!\n", iface, id, event);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessMessage(IMFTransform *iface, MFT_MESSAGE_TYPE message, ULONG_PTR param)
{
    FIXME("iface %p, message %#x, param %p stub!\n", iface, message, (void *)param);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessInput(IMFTransform *iface, DWORD id, IMFSample *sample, DWORD flags)
{
    FIXME("iface %p, id %u, sample %p, flags %#x stub!\n", iface, id, sample, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI transform_ProcessOutput(IMFTransform *iface, DWORD flags, DWORD count,
        MFT_OUTPUT_DATA_BUFFER *samples, DWORD *status)
{
    FIXME("iface %p, flags %#x, count %u, samples %p, status %p stub!\n", iface, flags, count, samples, status);
    return E_NOTIMPL;
}

static const IMFTransformVtbl transform_vtbl =
{
    transform_QueryInterface,
    transform_AddRef,
    transform_Release,
    transform_GetStreamLimits,
    transform_GetStreamCount,
    transform_GetStreamIDs,
    transform_GetInputStreamInfo,
    transform_GetOutputStreamInfo,
    transform_GetAttributes,
    transform_GetInputStreamAttributes,
    transform_GetOutputStreamAttributes,
    transform_DeleteInputStream,
    transform_AddInputStreams,
    transform_GetInputAvailableType,
    transform_GetOutputAvailableType,
    transform_SetInputType,
    transform_SetOutputType,
    transform_GetInputCurrentType,
    transform_GetOutputCurrentType,
    transform_GetInputStatus,
    transform_GetOutputStatus,
    transform_SetOutputBounds,
    transform_ProcessEvent,
    transform_ProcessMessage,
    transform_ProcessInput,
    transform_ProcessOutput,
};

static inline struct wma_decoder *impl_from_IMediaObject(IMediaObject *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IMediaObject_iface);
}

static HRESULT WINAPI media_object_QueryInterface(IMediaObject *iface, REFIID iid, void **obj)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    return IUnknown_QueryInterface(decoder->outer, iid, obj);
}

static ULONG WINAPI media_object_AddRef(IMediaObject *iface)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    return IUnknown_AddRef(decoder->outer);
}

static ULONG WINAPI media_object_Release(IMediaObject *iface)
{
    struct wma_decoder *decoder = impl_from_IMediaObject(iface);
    return IUnknown_Release(decoder->outer);
}

static HRESULT WINAPI media_object_GetStreamCount(IMediaObject *iface, DWORD *input, DWORD *output)
{
    FIXME("iface %p, input %p, output %p semi-stub!\n", iface, input, output);
    *input = *output = 1;
    return S_OK;
}

static HRESULT WINAPI media_object_GetInputStreamInfo(IMediaObject *iface, DWORD index, DWORD *flags)
{
    FIXME("iface %p, index %u, flags %p stub!\n", iface, index, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetOutputStreamInfo(IMediaObject *iface, DWORD index, DWORD *flags)
{
    FIXME("iface %p, index %u, flags %p stub!\n", iface, index, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetInputType(IMediaObject *iface, DWORD index, DWORD type_index,
        DMO_MEDIA_TYPE *type)
{
    FIXME("iface %p, index %u, type_index %u, type %p stub!\n", iface, index, type_index, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetOutputType(IMediaObject *iface, DWORD index, DWORD type_index,
        DMO_MEDIA_TYPE *type)
{
    FIXME("iface %p, index %u, type_index %u, type %p stub!\n", iface, index, type_index, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_SetInputType(IMediaObject *iface, DWORD index,
        const DMO_MEDIA_TYPE *type, DWORD flags)
{
    FIXME("iface %p, index %u, type %p, flags %#x stub!\n", iface, index, type, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_SetOutputType(IMediaObject *iface, DWORD index,
        const DMO_MEDIA_TYPE *type, DWORD flags)
{
    FIXME("iface %p, index %u, type %p, flags %#x stub!\n", iface, index, type, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetInputCurrentType(IMediaObject *iface, DWORD index, DMO_MEDIA_TYPE *type)
{
    FIXME("iface %p, index %u, type %p stub!\n", iface, index, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetOutputCurrentType(IMediaObject *iface, DWORD index, DMO_MEDIA_TYPE *type)
{
    FIXME("iface %p, index %u, type %p stub!\n", iface, index, type);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetInputSizeInfo(IMediaObject *iface, DWORD index, DWORD *size,
        DWORD *lookahead, DWORD *alignment)
{
    FIXME("iface %p, index %u, size %p, lookahead %p, alignment %p stub!\n", iface, index, size,
            lookahead, alignment);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetOutputSizeInfo(IMediaObject *iface, DWORD index, DWORD *size, DWORD *alignment)
{
    FIXME("iface %p, index %u, size %p, alignment %p stub!\n", iface, index, size, alignment);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetInputMaxLatency(IMediaObject *iface, DWORD index, REFERENCE_TIME *latency)
{
    FIXME("iface %p, index %u, latency %p stub!\n", iface, index, latency);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_SetInputMaxLatency(IMediaObject *iface, DWORD index, REFERENCE_TIME latency)
{
    FIXME("iface %p, index %u, latency %s stub!\n", iface, index, wine_dbgstr_longlong(latency));
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_Flush(IMediaObject *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_Discontinuity(IMediaObject *iface, DWORD index)
{
    FIXME("iface %p, index %u stub!\n", iface, index);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_AllocateStreamingResources(IMediaObject *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_FreeStreamingResources(IMediaObject *iface)
{
    FIXME("iface %p stub!\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_GetInputStatus(IMediaObject *iface, DWORD index, DWORD *flags)
{
    FIXME("iface %p, index %u, flags %p stub!\n", iface, index, flags);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_ProcessInput(IMediaObject *iface, DWORD index,
        IMediaBuffer *buffer, DWORD flags, REFERENCE_TIME timestamp, REFERENCE_TIME timelength)
{
    FIXME("iface %p, index %u, buffer %p, flags %#x, timestamp %s, timelength %s stub!\n", iface,
            index, buffer, flags, wine_dbgstr_longlong(timestamp), wine_dbgstr_longlong(timelength));
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_ProcessOutput(IMediaObject *iface, DWORD flags, DWORD count,
        DMO_OUTPUT_DATA_BUFFER *buffers, DWORD *status)
{
    FIXME("iface %p, flags %#x, count %u, buffers %p, status %p stub!\n", iface, flags, count, buffers, status);
    return E_NOTIMPL;
}

static HRESULT WINAPI media_object_Lock(IMediaObject *iface, LONG lock)
{
    FIXME("iface %p, lock %d stub!\n", iface, lock);
    return E_NOTIMPL;
}

static const IMediaObjectVtbl media_object_vtbl =
{
    media_object_QueryInterface,
    media_object_AddRef,
    media_object_Release,
    media_object_GetStreamCount,
    media_object_GetInputStreamInfo,
    media_object_GetOutputStreamInfo,
    media_object_GetInputType,
    media_object_GetOutputType,
    media_object_SetInputType,
    media_object_SetOutputType,
    media_object_GetInputCurrentType,
    media_object_GetOutputCurrentType,
    media_object_GetInputSizeInfo,
    media_object_GetOutputSizeInfo,
    media_object_GetInputMaxLatency,
    media_object_SetInputMaxLatency,
    media_object_Flush,
    media_object_Discontinuity,
    media_object_AllocateStreamingResources,
    media_object_FreeStreamingResources,
    media_object_GetInputStatus,
    media_object_ProcessInput,
    media_object_ProcessOutput,
    media_object_Lock,
};

static inline struct wma_decoder *impl_from_IPropertyBag(IPropertyBag *iface)
{
    return CONTAINING_RECORD(iface, struct wma_decoder, IPropertyBag_iface);
}

static HRESULT WINAPI property_bag_QueryInterface(IPropertyBag *iface, REFIID iid, void **out)
{
    struct wma_decoder *filter = impl_from_IPropertyBag(iface);
    return IUnknown_QueryInterface(filter->outer, iid, out);
}

static ULONG WINAPI property_bag_AddRef(IPropertyBag *iface)
{
    struct wma_decoder *filter = impl_from_IPropertyBag(iface);
    return IUnknown_AddRef(filter->outer);
}

static ULONG WINAPI property_bag_Release(IPropertyBag *iface)
{
    struct wma_decoder *filter = impl_from_IPropertyBag(iface);
    return IUnknown_Release(filter->outer);
}

static HRESULT WINAPI property_bag_Read(IPropertyBag *iface, const WCHAR *prop_name, VARIANT *value,
        IErrorLog *error_log)
{
    FIXME("iface %p, prop_name %s, value %p, error_log %p stub!\n", iface, debugstr_w(prop_name), value, error_log);
    return E_NOTIMPL;
}

static HRESULT WINAPI property_bag_Write(IPropertyBag *iface, const WCHAR *prop_name, VARIANT *value)
{
    FIXME("iface %p, prop_name %s, value %p stub!\n", iface, debugstr_w(prop_name), value);
    return S_OK;
}

static const IPropertyBagVtbl property_bag_vtbl =
{
    property_bag_QueryInterface,
    property_bag_AddRef,
    property_bag_Release,
    property_bag_Read,
    property_bag_Write,
};

HRESULT wma_decoder_create(IUnknown *outer, IUnknown **out)
{
    struct wma_decoder *decoder;

    TRACE("outer %p, out %p.\n", outer, out);

    if (!(decoder = calloc(1, sizeof(*decoder))))
        return E_OUTOFMEMORY;

    decoder->IUnknown_inner.lpVtbl = &unknown_vtbl;
    decoder->IMFTransform_iface.lpVtbl = &transform_vtbl;
    decoder->IMediaObject_iface.lpVtbl = &media_object_vtbl;
    decoder->IPropertyBag_iface.lpVtbl = &property_bag_vtbl;
    decoder->refcount = 1;
    decoder->outer = outer ? outer : &decoder->IUnknown_inner;

    *out = &decoder->IUnknown_inner;
    TRACE("Created decoder %p\n", *out);
    return S_OK;
}
