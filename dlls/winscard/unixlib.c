/*
 * Copyright 2022 Hans Leidekker for CodeWeavers
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

#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winbase.h"

#include "wine/debug.h"
#include "wine/unixlib.h"
#include "unixlib.h"

LONG SCardEstablishContext( UINT64, const void *, const void *, UINT64 * );
LONG SCardReleaseContext( UINT64 );
LONG SCardIsValidContext( UINT64 );
LONG SCardGetStatusChange( UINT64, UINT64, struct reader_state *, UINT64 );
LONG SCardCancel( UINT64 );
LONG SCardListReaders( UINT64, const char *, char *, UINT64 * );
LONG SCardListReaderGroups( UINT64, char *, UINT64 * );
LONG SCardConnect( UINT64, const char *, UINT64, UINT64, UINT64 *, UINT64 * );

static NTSTATUS scard_establish_context( void *args )
{
    struct scard_establish_context_params *params = args;
    return SCardEstablishContext( params->scope, NULL, NULL, params->handle );
}

static NTSTATUS scard_release_context( void *args )
{
    struct scard_release_context_params *params = args;
    return SCardReleaseContext( params->handle );
}

static NTSTATUS scard_is_valid_context( void *args )
{
    struct scard_is_valid_context_params *params = args;
    return SCardIsValidContext( params->handle );
}

static NTSTATUS scard_get_status_change( void *args )
{
    struct scard_get_status_change_params *params = args;
    return SCardGetStatusChange( params->handle, params->timeout, params->states, params->count );
}

static NTSTATUS scard_cancel( void *args )
{
    struct scard_cancel_params *params = args;
    return SCardCancel( params->handle );
}

static NTSTATUS scard_list_readers( void *args )
{
    struct scard_list_readers_params *params = args;
    return SCardListReaders( params->handle, params->groups, params->readers, params->readers_len );
}

static NTSTATUS scard_list_reader_groups( void *args )
{
    struct scard_list_reader_groups_params *params = args;
    return SCardListReaderGroups( params->handle, params->groups, params->groups_len );
}

static NTSTATUS scard_connect( void *args )
{
    struct scard_connect_params *params = args;
    return SCardConnect( params->context_handle, params->reader, params->share_mode, params->preferred_protocols,
                         params->connect_handle, params->protocol );
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    scard_establish_context,
    scard_release_context,
    scard_is_valid_context,
    scard_get_status_change,
    scard_cancel,
    scard_list_readers,
    scard_list_reader_groups,
    scard_connect,
};