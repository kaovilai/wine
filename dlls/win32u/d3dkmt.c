/*
 * Copyright 2024 Rémi Bernon for CodeWeavers
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntgdi_private.h"
#include "win32u_private.h"
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

struct d3dkmt_adapter
{
    D3DKMT_HANDLE handle;               /* Kernel mode graphics adapter handle */
    struct list entry;                  /* List entry */
    VkPhysicalDevice vk_device;         /* Vulkan physical device */
};

struct d3dkmt_device
{
    D3DKMT_HANDLE handle;               /* Kernel mode graphics device handle*/
    struct list entry;                  /* List entry */
};

static pthread_mutex_t d3dkmt_lock = PTHREAD_MUTEX_INITIALIZER;
static struct list d3dkmt_adapters = LIST_INIT( d3dkmt_adapters );
static struct list d3dkmt_devices = LIST_INIT( d3dkmt_devices );

/******************************************************************************
 *           NtGdiDdDDIOpenAdapterFromHdc    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenAdapterFromHdc( D3DKMT_OPENADAPTERFROMHDC *desc )
{
    FIXME( "(%p): stub\n", desc );
    return STATUS_NO_MEMORY;
}

/******************************************************************************
 *           NtGdiDdDDIEscape    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIEscape( const D3DKMT_ESCAPE *desc )
{
    FIXME( "(%p): stub\n", desc );
    return STATUS_NO_MEMORY;
}

/******************************************************************************
 *           NtGdiDdDDICloseAdapter    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICloseAdapter( const D3DKMT_CLOSEADAPTER *desc )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    struct d3dkmt_adapter *adapter;

    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hAdapter) return STATUS_INVALID_PARAMETER;

    if (get_display_driver()->pD3DKMTCloseAdapter)
        get_display_driver()->pD3DKMTCloseAdapter( desc );

    pthread_mutex_lock( &d3dkmt_lock );
    LIST_FOR_EACH_ENTRY( adapter, &d3dkmt_adapters, struct d3dkmt_adapter, entry )
    {
        if (adapter->handle == desc->hAdapter)
        {
            list_remove( &adapter->entry );
            free( adapter );
            status = STATUS_SUCCESS;
            break;
        }
    }
    pthread_mutex_unlock( &d3dkmt_lock );

    return status;
}

/******************************************************************************
 *           NtGdiDdDDIOpenAdapterFromDeviceName    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenAdapterFromDeviceName( D3DKMT_OPENADAPTERFROMDEVICENAME *desc )
{
    D3DKMT_OPENADAPTERFROMLUID desc_luid;
    NTSTATUS status;

    FIXME( "desc %p stub.\n", desc );

    if (!desc || !desc->pDeviceName) return STATUS_INVALID_PARAMETER;

    memset( &desc_luid, 0, sizeof(desc_luid) );
    if ((status = NtGdiDdDDIOpenAdapterFromLuid( &desc_luid ))) return status;

    desc->AdapterLuid = desc_luid.AdapterLuid;
    desc->hAdapter = desc_luid.hAdapter;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDIOpenAdapterFromLuid    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIOpenAdapterFromLuid( D3DKMT_OPENADAPTERFROMLUID *desc )
{
    static D3DKMT_HANDLE handle_start = 0;
    struct d3dkmt_adapter *adapter;

    if (!(adapter = malloc( sizeof(*adapter) ))) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &d3dkmt_lock );
    desc->hAdapter = adapter->handle = ++handle_start;
    list_add_tail( &d3dkmt_adapters, &adapter->entry );
    pthread_mutex_unlock( &d3dkmt_lock );

    if (get_display_driver()->pD3DKMTOpenAdapterFromLuid)
        get_display_driver()->pD3DKMTOpenAdapterFromLuid( desc );

    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDICreateDevice    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICreateDevice( D3DKMT_CREATEDEVICE *desc )
{
    static D3DKMT_HANDLE handle_start = 0;
    struct d3dkmt_adapter *adapter;
    struct d3dkmt_device *device;
    BOOL found = FALSE;

    TRACE( "(%p)\n", desc );

    if (!desc) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &d3dkmt_lock );
    LIST_FOR_EACH_ENTRY( adapter, &d3dkmt_adapters, struct d3dkmt_adapter, entry )
    {
        if (adapter->handle == desc->hAdapter)
        {
            found = TRUE;
            break;
        }
    }
    pthread_mutex_unlock( &d3dkmt_lock );

    if (!found) return STATUS_INVALID_PARAMETER;

    if (desc->Flags.LegacyMode || desc->Flags.RequestVSync || desc->Flags.DisableGpuTimeout)
        FIXME( "Flags unsupported.\n" );

    device = calloc( 1, sizeof(*device) );
    if (!device) return STATUS_NO_MEMORY;

    pthread_mutex_lock( &d3dkmt_lock );
    device->handle = ++handle_start;
    list_add_tail( &d3dkmt_devices, &device->entry );
    pthread_mutex_unlock( &d3dkmt_lock );

    desc->hDevice = device->handle;
    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDIDestroyDevice    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIDestroyDevice( const D3DKMT_DESTROYDEVICE *desc )
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    D3DKMT_SETVIDPNSOURCEOWNER set_owner_desc;
    struct d3dkmt_device *device;

    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hDevice) return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock( &d3dkmt_lock );
    LIST_FOR_EACH_ENTRY( device, &d3dkmt_devices, struct d3dkmt_device, entry )
    {
        if (device->handle == desc->hDevice)
        {
            memset( &set_owner_desc, 0, sizeof(set_owner_desc) );
            set_owner_desc.hDevice = desc->hDevice;
            NtGdiDdDDISetVidPnSourceOwner( &set_owner_desc );
            list_remove( &device->entry );
            free( device );
            status = STATUS_SUCCESS;
            break;
        }
    }
    pthread_mutex_unlock( &d3dkmt_lock );

    return status;
}

/******************************************************************************
 *           NtGdiDdDDIQueryAdapterInfo    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryAdapterInfo( D3DKMT_QUERYADAPTERINFO *desc )
{
    if (!desc) return STATUS_INVALID_PARAMETER;

    FIXME( "desc %p, type %d stub\n", desc, desc->Type );
    return STATUS_NOT_IMPLEMENTED;
}

/******************************************************************************
 *           NtGdiDdDDIQueryStatistics    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryStatistics( D3DKMT_QUERYSTATISTICS *stats )
{
    FIXME( "(%p): stub\n", stats );
    return STATUS_SUCCESS;
}

/******************************************************************************
 *           NtGdiDdDDIQueryVideoMemoryInfo    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDIQueryVideoMemoryInfo( D3DKMT_QUERYVIDEOMEMORYINFO *desc )
{
    OBJECT_BASIC_INFORMATION info;
    NTSTATUS status;

    TRACE( "(%p)\n", desc );

    if (!desc || !desc->hAdapter ||
        (desc->MemorySegmentGroup != D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL &&
         desc->MemorySegmentGroup != D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL))
        return STATUS_INVALID_PARAMETER;

    /* FIXME: Wine currently doesn't support linked adapters */
    if (desc->PhysicalAdapterIndex > 0) return STATUS_INVALID_PARAMETER;

    status = NtQueryObject( desc->hProcess ? desc->hProcess : GetCurrentProcess(),
                            ObjectBasicInformation, &info, sizeof(info), NULL );
    if (status != STATUS_SUCCESS) return status;
    if (!(info.GrantedAccess & PROCESS_QUERY_INFORMATION)) return STATUS_ACCESS_DENIED;

    if (!get_display_driver()->pD3DKMTQueryVideoMemoryInfo) return STATUS_PROCEDURE_NOT_FOUND;
    return get_display_driver()->pD3DKMTQueryVideoMemoryInfo( desc );
}

/******************************************************************************
 *           NtGdiDdDDISetQueuedLimit    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDISetQueuedLimit( D3DKMT_SETQUEUEDLIMIT *desc )
{
    FIXME( "(%p): stub\n", desc );
    return STATUS_NOT_IMPLEMENTED;
}

/******************************************************************************
 *           NtGdiDdDDISetVidPnSourceOwner    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDISetVidPnSourceOwner( const D3DKMT_SETVIDPNSOURCEOWNER *desc )
{
    TRACE( "(%p)\n", desc );

    if (!get_display_driver()->pD3DKMTSetVidPnSourceOwner) return STATUS_PROCEDURE_NOT_FOUND;

    if (!desc || !desc->hDevice || (desc->VidPnSourceCount && (!desc->pType || !desc->pVidPnSourceId)))
        return STATUS_INVALID_PARAMETER;

    /* Store the VidPN source ownership info in the graphics driver because
     * the graphics driver needs to change ownership sometimes. For example,
     * when a new window is moved to a VidPN source with an exclusive owner,
     * such an exclusive owner will be released before showing the new window */
    return get_display_driver()->pD3DKMTSetVidPnSourceOwner( desc );
}

/******************************************************************************
 *           NtGdiDdDDICheckVidPnExclusiveOwnership    (win32u.@)
 */
NTSTATUS WINAPI NtGdiDdDDICheckVidPnExclusiveOwnership( const D3DKMT_CHECKVIDPNEXCLUSIVEOWNERSHIP *desc )
{
    TRACE( "(%p)\n", desc );

    if (!get_display_driver()->pD3DKMTCheckVidPnExclusiveOwnership)
        return STATUS_PROCEDURE_NOT_FOUND;

    if (!desc || !desc->hAdapter) return STATUS_INVALID_PARAMETER;

    return get_display_driver()->pD3DKMTCheckVidPnExclusiveOwnership( desc );
}
