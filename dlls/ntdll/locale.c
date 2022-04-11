/*
 * Locale functions
 *
 * Copyright 2004, 2019 Alexandre Julliard
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

#define NONAMELESSUNION

#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "ntdll_misc.h"
#include "locale_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(nls);

UINT NlsAnsiCodePage = 0;
BYTE NlsMbCodePageTag = 0;
BYTE NlsMbOemCodePageTag = 0;

static const WCHAR *locale_strings;
static NLSTABLEINFO nls_info;
static struct norm_table *norm_tables[16];
static const NLS_LOCALE_LCID_INDEX *lcids_index;
static const NLS_LOCALE_LCNAME_INDEX *lcnames_index;
static const NLS_LOCALE_HEADER *locale_table;


static DWORD mbtowc_size( const CPTABLEINFO *info, LPCSTR str, UINT len )
{
    DWORD res;

    if (!info->DBCSCodePage) return len;

    for (res = 0; len; len--, str++, res++)
    {
        if (info->DBCSOffsets[(unsigned char)*str] && len > 1)
        {
            str++;
            len--;
        }
    }
    return res;
}


static DWORD wctomb_size( const CPTABLEINFO *info, LPCWSTR str, UINT len )
{
    if (info->DBCSCodePage)
    {
        WCHAR *uni2cp = info->WideCharTable;
        DWORD res;

        for (res = 0; len; len--, str++, res++)
            if (uni2cp[*str] & 0xff00) res++;
        return res;
    }
    else return len;
}


static WCHAR casemap( USHORT *table, WCHAR ch )
{
    return ch + table[table[table[ch >> 8] + ((ch >> 4) & 0x0f)] + (ch & 0x0f)];
}


static WCHAR casemap_ascii( WCHAR ch )
{
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    return ch;
}


static NTSTATUS load_norm_table( ULONG form, const struct norm_table **info )
{
    unsigned int i;
    USHORT *data, *tables;
    SIZE_T size;
    NTSTATUS status;

    if (!form) return STATUS_INVALID_PARAMETER;
    if (form >= ARRAY_SIZE(norm_tables)) return STATUS_OBJECT_NAME_NOT_FOUND;

    if (!norm_tables[form])
    {
        if ((status = NtGetNlsSectionPtr( NLS_SECTION_NORMALIZE, form, NULL, (void **)&data, &size )))
            return status;

        /* sanity checks */

        if (size <= 0x44) goto invalid;
        if (data[0x14] != form) goto invalid;
        tables = data + 0x1a;
        for (i = 0; i < 8; i++)
        {
            if (tables[i] > size / sizeof(USHORT)) goto invalid;
            if (i && tables[i] < tables[i-1]) goto invalid;
        }

        if (InterlockedCompareExchangePointer( (void **)&norm_tables[form], data, NULL ))
            NtUnmapViewOfSection( GetCurrentProcess(), data );
    }
    *info = norm_tables[form];
    return STATUS_SUCCESS;

invalid:
    NtUnmapViewOfSection( GetCurrentProcess(), data );
    return STATUS_INVALID_PARAMETER;
}


static int compare_locale_names( const WCHAR *n1, const WCHAR *n2 )
{
    for (;;)
    {
        WCHAR ch1 = casemap_ascii( *n1++ );
        WCHAR ch2 = casemap_ascii( *n2++ );
        if (ch1 == '_') ch1 = '-';
        if (ch2 == '_') ch2 = '-';
        if (!ch1 || ch1 != ch2) return ch1 - ch2;
    }
}


static const NLS_LOCALE_LCNAME_INDEX *find_lcname_entry( const WCHAR *name )
{
    int min = 0, max = locale_table->nb_lcnames - 1;

    if (!name) return NULL;
    while (min <= max)
    {
        int res, pos = (min + max) / 2;
        const WCHAR *str = locale_strings + lcnames_index[pos].name;
        res = compare_locale_names( name, str + 1 );
        if (res < 0) max = pos - 1;
        else if (res > 0) min = pos + 1;
        else return &lcnames_index[pos];
    }
    return NULL;
}


static const NLS_LOCALE_LCID_INDEX *find_lcid_entry( LCID lcid )
{
    int min = 0, max = locale_table->nb_lcids - 1;

    while (min <= max)
    {
        int pos = (min + max) / 2;
        if (lcid < lcids_index[pos].id) max = pos - 1;
        else if (lcid > lcids_index[pos].id) min = pos + 1;
        else return &lcids_index[pos];
    }
    return NULL;
}


static const NLS_LOCALE_DATA *get_locale_data( UINT idx )
{
    ULONG offset = locale_table->locales_offset + idx * locale_table->locale_size;
    return (const NLS_LOCALE_DATA *)((const char *)locale_table + offset);
}


void locale_init(void)
{
    WCHAR locale[LOCALE_NAME_MAX_LENGTH];
    UNICODE_STRING name, value;
    LARGE_INTEGER unused;
    LCID system_lcid, user_lcid = 0;
    NTSTATUS status;
    struct
    {
        UINT ctypes;
        UINT unknown1;
        UINT unknown2;
        UINT unknown3;
        UINT locales;
        UINT charmaps;
        UINT geoids;
        UINT scripts;
    } *header;

    status = RtlGetLocaleFileMappingAddress( (void **)&header, &system_lcid, &unused );
    if (status)
    {
        ERR( "locale init failed %x\n", status );
        return;
    }
    locale_table = (const NLS_LOCALE_HEADER *)((char *)header + header->locales);
    lcids_index = (const NLS_LOCALE_LCID_INDEX *)((char *)locale_table + locale_table->lcids_offset);
    lcnames_index = (const NLS_LOCALE_LCNAME_INDEX *)((char *)locale_table + locale_table->lcnames_offset);
    locale_strings = (const WCHAR *)((char *)locale_table + locale_table->strings_offset);

    value.Buffer = locale;
    value.MaximumLength = sizeof(locale);
    RtlInitUnicodeString( &name, L"WINELOCALE" );
    if (!RtlQueryEnvironmentVariable_U( NULL, &name, &value ))
    {
        const NLS_LOCALE_LCNAME_INDEX *entry = find_lcname_entry( locale );
        if (entry) system_lcid = get_locale_data( entry->idx )->idefaultlanguage;
    }
    RtlInitUnicodeString( &name, L"WINEUSERLOCALE" );
    if (!RtlQueryEnvironmentVariable_U( NULL, &name, &value ))
    {
        const NLS_LOCALE_LCNAME_INDEX *entry = find_lcname_entry( locale );
        if (entry) user_lcid = get_locale_data( entry->idx )->idefaultlanguage;
    }
    if (!system_lcid) system_lcid = MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT );
    if (!user_lcid) user_lcid = system_lcid;
    NtSetDefaultUILanguage( user_lcid );
    NtSetDefaultLocale( TRUE, user_lcid );
    NtSetDefaultLocale( FALSE, system_lcid );
}


static NTSTATUS get_dummy_preferred_ui_language( DWORD flags, LANGID lang, ULONG *count,
                                                 WCHAR *buffer, ULONG *size )
{
    WCHAR name[LOCALE_NAME_MAX_LENGTH + 2];
    NTSTATUS status;
    ULONG len;

    FIXME("(0x%x %p %p %p) returning a dummy value (current locale)\n", flags, count, buffer, size);

    if (flags & MUI_LANGUAGE_ID) swprintf( name, ARRAY_SIZE(name), L"%04lX", lang );
    else
    {
        UNICODE_STRING str;

        str.Buffer = name;
        str.MaximumLength = sizeof(name);
        status = RtlLcidToLocaleName( lang, &str, 0, FALSE );
        if (status) return status;
    }

    len = wcslen( name ) + 2;
    name[len - 1] = 0;
    if (buffer)
    {
        if (len > *size)
        {
            *size = len;
            return STATUS_BUFFER_TOO_SMALL;
        }
        memcpy( buffer, name, len * sizeof(WCHAR) );
    }
    *size = len;
    *count = 1;
    TRACE("returned variable content: %d, \"%s\", %d\n", *count, debugstr_w(buffer), *size);
    return STATUS_SUCCESS;

}

/**************************************************************************
 *      RtlGetProcessPreferredUILanguages   (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetProcessPreferredUILanguages( DWORD flags, ULONG *count, WCHAR *buffer, ULONG *size )
{
    LANGID ui_language;

    FIXME( "%08x, %p, %p %p\n", flags, count, buffer, size );

    NtQueryDefaultUILanguage( &ui_language );
    return get_dummy_preferred_ui_language( flags, ui_language, count, buffer, size );
}


/**************************************************************************
 *      RtlGetSystemPreferredUILanguages   (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetSystemPreferredUILanguages( DWORD flags, ULONG unknown, ULONG *count,
                                                  WCHAR *buffer, ULONG *size )
{
    LANGID ui_language;

    if (flags & ~(MUI_LANGUAGE_NAME | MUI_LANGUAGE_ID | MUI_MACHINE_LANGUAGE_SETTINGS)) return STATUS_INVALID_PARAMETER;
    if ((flags & MUI_LANGUAGE_NAME) && (flags & MUI_LANGUAGE_ID)) return STATUS_INVALID_PARAMETER;
    if (*size && !buffer) return STATUS_INVALID_PARAMETER;

    NtQueryInstallUILanguage( &ui_language );
    return get_dummy_preferred_ui_language( flags, ui_language, count, buffer, size );
}


/**************************************************************************
 *      RtlGetThreadPreferredUILanguages   (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetThreadPreferredUILanguages( DWORD flags, ULONG *count, WCHAR *buffer, ULONG *size )
{
    LANGID ui_language;

    FIXME( "%08x, %p, %p %p\n", flags, count, buffer, size );

    NtQueryDefaultUILanguage( &ui_language );
    return get_dummy_preferred_ui_language( flags, ui_language, count, buffer, size );
}


/**************************************************************************
 *      RtlGetUserPreferredUILanguages   (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetUserPreferredUILanguages( DWORD flags, ULONG unknown, ULONG *count,
                                                WCHAR *buffer, ULONG *size )
{
    LANGID ui_language;

    if (flags & ~(MUI_LANGUAGE_NAME | MUI_LANGUAGE_ID)) return STATUS_INVALID_PARAMETER;
    if ((flags & MUI_LANGUAGE_NAME) && (flags & MUI_LANGUAGE_ID)) return STATUS_INVALID_PARAMETER;
    if (*size && !buffer) return STATUS_INVALID_PARAMETER;

    NtQueryDefaultUILanguage( &ui_language );
    return get_dummy_preferred_ui_language( flags, ui_language, count, buffer, size );
}


/**************************************************************************
 *      RtlSetProcessPreferredUILanguages   (NTDLL.@)
 */
NTSTATUS WINAPI RtlSetProcessPreferredUILanguages( DWORD flags, PCZZWSTR buffer, ULONG *count )
{
    FIXME( "%u, %p, %p\n", flags, buffer, count );
    return STATUS_SUCCESS;
}


/**************************************************************************
 *      RtlSetThreadPreferredUILanguages   (NTDLL.@)
 */
NTSTATUS WINAPI RtlSetThreadPreferredUILanguages( DWORD flags, PCZZWSTR buffer, ULONG *count )
{
    FIXME( "%u, %p, %p\n", flags, buffer, count );
    return STATUS_SUCCESS;
}


/******************************************************************
 *      RtlInitCodePageTable   (NTDLL.@)
 */
void WINAPI RtlInitCodePageTable( USHORT *ptr, CPTABLEINFO *info )
{
    USHORT hdr_size = ptr[0];

    if (ptr[1] == CP_UTF8)
    {
        static const CPTABLEINFO utf8_cpinfo = { CP_UTF8, 4, '?', 0xfffd, '?', '?' };
        *info = utf8_cpinfo;
        return;
    }

    info->CodePage             = ptr[1];
    info->MaximumCharacterSize = ptr[2];
    info->DefaultChar          = ptr[3];
    info->UniDefaultChar       = ptr[4];
    info->TransDefaultChar     = ptr[5];
    info->TransUniDefaultChar  = ptr[6];
    memcpy( info->LeadByte, ptr + 7, sizeof(info->LeadByte) );
    ptr += hdr_size;

    info->WideCharTable = ptr + ptr[0] + 1;
    info->MultiByteTable = ++ptr;
    ptr += 256;
    if (*ptr++) ptr += 256;  /* glyph table */
    info->DBCSRanges = ptr;
    if (*ptr)  /* dbcs ranges */
    {
        info->DBCSCodePage = 1;
        info->DBCSOffsets  = ptr + 1;
    }
    else
    {
        info->DBCSCodePage = 0;
        info->DBCSOffsets  = NULL;
    }
}


/**************************************************************************
 *      RtlInitNlsTables   (NTDLL.@)
 */
void WINAPI RtlInitNlsTables( USHORT *ansi, USHORT *oem, USHORT *casetable, NLSTABLEINFO *info )
{
    RtlInitCodePageTable( ansi, &info->AnsiTableInfo );
    RtlInitCodePageTable( oem, &info->OemTableInfo );
    info->UpperCaseTable = casetable + 2;
    info->LowerCaseTable = casetable + casetable[1] + 2;
}


/**************************************************************************
 *      RtlResetRtlTranslations   (NTDLL.@)
 */
void WINAPI RtlResetRtlTranslations( const NLSTABLEINFO *info )
{
    NlsAnsiCodePage     = info->AnsiTableInfo.CodePage;
    NlsMbCodePageTag    = info->AnsiTableInfo.DBCSCodePage;
    NlsMbOemCodePageTag = info->OemTableInfo.DBCSCodePage;
    nls_info = *info;
}


/**************************************************************************
 *      RtlGetLocaleFileMappingAddress   (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetLocaleFileMappingAddress( void **ptr, LCID *lcid, LARGE_INTEGER *size )
{
    static void *cached_ptr;
    static LCID cached_lcid;

    if (!cached_ptr)
    {
        void *addr;
        NTSTATUS status = NtInitializeNlsFiles( &addr, &cached_lcid, size );

        if (status) return status;
        if (InterlockedCompareExchangePointer( &cached_ptr, addr, NULL ))
            NtUnmapViewOfSection( GetCurrentProcess(), addr );
    }
    *ptr = cached_ptr;
    *lcid = cached_lcid;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *      RtlAnsiCharToUnicodeChar   (NTDLL.@)
 */
WCHAR WINAPI RtlAnsiCharToUnicodeChar( char **ansi )
{
    if (nls_info.AnsiTableInfo.DBCSOffsets)
    {
        USHORT off = nls_info.AnsiTableInfo.DBCSOffsets[(unsigned char)**ansi];
        if (off)
        {
            (*ansi)++;
            return nls_info.AnsiTableInfo.DBCSOffsets[off + (unsigned char)*(*ansi)++];
        }
    }
    return nls_info.AnsiTableInfo.MultiByteTable[(unsigned char)*(*ansi)++];
}


/******************************************************************************
 *	RtlCompareUnicodeStrings   (NTDLL.@)
 */
LONG WINAPI RtlCompareUnicodeStrings( const WCHAR *s1, SIZE_T len1, const WCHAR *s2, SIZE_T len2,
                                      BOOLEAN case_insensitive )
{
    LONG ret = 0;
    SIZE_T len = min( len1, len2 );

    if (case_insensitive)
    {
        if (nls_info.UpperCaseTable)
        {
            while (!ret && len--) ret = casemap( nls_info.UpperCaseTable, *s1++ ) -
                                        casemap( nls_info.UpperCaseTable, *s2++ );
        }
        else  /* locale not setup yet */
        {
            while (!ret && len--) ret = casemap_ascii( *s1++ ) - casemap_ascii( *s2++ );
        }
    }
    else
    {
        while (!ret && len--) ret = *s1++ - *s2++;
    }
    if (!ret) ret = len1 - len2;
    return ret;
}


/**************************************************************************
 *	RtlPrefixUnicodeString   (NTDLL.@)
 */
BOOLEAN WINAPI RtlPrefixUnicodeString( const UNICODE_STRING *s1, const UNICODE_STRING *s2,
                                       BOOLEAN ignore_case )
{
    unsigned int i;

    if (s1->Length > s2->Length) return FALSE;
    if (ignore_case)
    {
        for (i = 0; i < s1->Length / sizeof(WCHAR); i++)
            if (casemap( nls_info.UpperCaseTable, s1->Buffer[i] ) !=
                casemap( nls_info.UpperCaseTable, s2->Buffer[i] )) return FALSE;
    }
    else
    {
        for (i = 0; i < s1->Length / sizeof(WCHAR); i++)
            if (s1->Buffer[i] != s2->Buffer[i]) return FALSE;
    }
    return TRUE;
}



/******************************************************************************
 *	RtlHashUnicodeString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlHashUnicodeString( const UNICODE_STRING *string, BOOLEAN case_insensitive,
                                      ULONG alg, ULONG *hash )
{
    unsigned int i;

    if (!string || !hash) return STATUS_INVALID_PARAMETER;

    switch (alg)
    {
    case HASH_STRING_ALGORITHM_DEFAULT:
    case HASH_STRING_ALGORITHM_X65599:
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    *hash = 0;
    if (!case_insensitive)
        for (i = 0; i < string->Length / sizeof(WCHAR); i++)
            *hash = *hash * 65599 + string->Buffer[i];
    else if (nls_info.UpperCaseTable)
        for (i = 0; i < string->Length / sizeof(WCHAR); i++)
            *hash = *hash * 65599 + casemap( nls_info.UpperCaseTable, string->Buffer[i] );
    else  /* locale not setup yet */
        for (i = 0; i < string->Length / sizeof(WCHAR); i++)
            *hash = *hash * 65599 + casemap_ascii( string->Buffer[i] );
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlCustomCPToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlCustomCPToUnicodeN( CPTABLEINFO *info, WCHAR *dst, DWORD dstlen, DWORD *reslen,
                                       const char *src, DWORD srclen )
{
    DWORD i, ret;

    dstlen /= sizeof(WCHAR);
    if (info->DBCSOffsets)
    {
        for (i = dstlen; srclen && i; i--, srclen--, src++, dst++)
        {
            USHORT off = info->DBCSOffsets[(unsigned char)*src];
            if (off && srclen > 1)
            {
                src++;
                srclen--;
                *dst = info->DBCSOffsets[off + (unsigned char)*src];
            }
            else *dst = info->MultiByteTable[(unsigned char)*src];
        }
        ret = dstlen - i;
    }
    else
    {
        ret = min( srclen, dstlen );
        for (i = 0; i < ret; i++) dst[i] = info->MultiByteTable[(unsigned char)src[i]];
    }
    if (reslen) *reslen = ret * sizeof(WCHAR);
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUnicodeToCustomCPN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToCustomCPN( CPTABLEINFO *info, char *dst, DWORD dstlen, DWORD *reslen,
                                       const WCHAR *src, DWORD srclen )
{
    DWORD i, ret;

    srclen /= sizeof(WCHAR);
    if (info->DBCSCodePage)
    {
        WCHAR *uni2cp = info->WideCharTable;

        for (i = dstlen; srclen && i; i--, srclen--, src++)
        {
            if (uni2cp[*src] & 0xff00)
            {
                if (i == 1) break;  /* do not output a partial char */
                i--;
                *dst++ = uni2cp[*src] >> 8;
            }
            *dst++ = (char)uni2cp[*src];
        }
        ret = dstlen - i;
    }
    else
    {
        char *uni2cp = info->WideCharTable;
        ret = min( srclen, dstlen );
        for (i = 0; i < ret; i++) dst[i] = uni2cp[src[i]];
    }
    if (reslen) *reslen = ret;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlMultiByteToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlMultiByteToUnicodeN( WCHAR *dst, DWORD dstlen, DWORD *reslen,
                                        const char *src, DWORD srclen )
{
    if (nls_info.AnsiTableInfo.WideCharTable)
        return RtlCustomCPToUnicodeN( &nls_info.AnsiTableInfo, dst, dstlen, reslen, src, srclen );

    /* locale not setup yet */
    dstlen = min( srclen, dstlen / sizeof(WCHAR) );
    if (reslen) *reslen = dstlen * sizeof(WCHAR);
    while (dstlen--) *dst++ = *src++ & 0x7f;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *      RtlMultiByteToUnicodeSize   (NTDLL.@)
 */
NTSTATUS WINAPI RtlMultiByteToUnicodeSize( DWORD *size, const char *str, DWORD len )
{
    *size = mbtowc_size( &nls_info.AnsiTableInfo, str, len ) * sizeof(WCHAR);
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlOemToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlOemToUnicodeN( WCHAR *dst, DWORD dstlen, DWORD *reslen,
                                  const char *src, DWORD srclen )
{
    return RtlCustomCPToUnicodeN( &nls_info.OemTableInfo, dst, dstlen, reslen, src, srclen );
}


/**************************************************************************
 *      RtlOemStringToUnicodeSize   (NTDLL.@)
 *      RtlxOemStringToUnicodeSize  (NTDLL.@)
 */
DWORD WINAPI RtlOemStringToUnicodeSize( const STRING *str )
{
    return (mbtowc_size( &nls_info.OemTableInfo, str->Buffer, str->Length ) + 1) * sizeof(WCHAR);
}


/**************************************************************************
 *      RtlUnicodeStringToOemSize   (NTDLL.@)
 *      RtlxUnicodeStringToOemSize  (NTDLL.@)
 */
DWORD WINAPI RtlUnicodeStringToOemSize( const UNICODE_STRING *str )
{
    return wctomb_size( &nls_info.OemTableInfo, str->Buffer, str->Length / sizeof(WCHAR) ) + 1;
}


/**************************************************************************
 *	RtlUnicodeToMultiByteN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToMultiByteN( char *dst, DWORD dstlen, DWORD *reslen,
                                        const WCHAR *src, DWORD srclen )
{
    if (nls_info.AnsiTableInfo.WideCharTable)
        return RtlUnicodeToCustomCPN( &nls_info.AnsiTableInfo, dst, dstlen, reslen, src, srclen );

    /* locale not setup yet */
    dstlen = min( srclen / sizeof(WCHAR), dstlen );
    if (reslen) *reslen = dstlen;
    while (dstlen--)
    {
        WCHAR ch = *src++;
        if (ch > 0x7f) ch = '?';
        *dst++ = ch;
    }
    return STATUS_SUCCESS;
}


/**************************************************************************
 *      RtlUnicodeToMultiByteSize   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToMultiByteSize( DWORD *size, const WCHAR *str, DWORD len )
{
    *size = wctomb_size( &nls_info.AnsiTableInfo, str, len / sizeof(WCHAR) );
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUnicodeToOemN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToOemN( char *dst, DWORD dstlen, DWORD *reslen,
                                  const WCHAR *src, DWORD srclen )
{
    return RtlUnicodeToCustomCPN( &nls_info.OemTableInfo, dst, dstlen, reslen, src, srclen );
}


/**************************************************************************
 *	RtlDowncaseUnicodeChar   (NTDLL.@)
 */
WCHAR WINAPI RtlDowncaseUnicodeChar( WCHAR wch )
{
    if (nls_info.LowerCaseTable) return casemap( nls_info.LowerCaseTable, wch );
    if (wch >= 'A' && wch <= 'Z') wch += 'a' - 'A';
    return wch;
}


/**************************************************************************
 *	RtlDowncaseUnicodeString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlDowncaseUnicodeString( UNICODE_STRING *dest, const UNICODE_STRING *src,
                                          BOOLEAN alloc )
{
    DWORD i, len = src->Length;

    if (alloc)
    {
        dest->MaximumLength = len;
        if (!(dest->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, len ))) return STATUS_NO_MEMORY;
    }
    else if (len > dest->MaximumLength) return STATUS_BUFFER_OVERFLOW;

    for (i = 0; i < len / sizeof(WCHAR); i++)
        dest->Buffer[i] = casemap( nls_info.LowerCaseTable, src->Buffer[i] );
    dest->Length = len;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUpcaseUnicodeChar   (NTDLL.@)
 */
WCHAR WINAPI RtlUpcaseUnicodeChar( WCHAR wch )
{
    return casemap( nls_info.UpperCaseTable, wch );
}


/**************************************************************************
 *	RtlUpcaseUnicodeString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeString( UNICODE_STRING *dest, const UNICODE_STRING *src,
                                        BOOLEAN alloc )
{
    DWORD i, len = src->Length;

    if (alloc)
    {
        dest->MaximumLength = len;
        if (!(dest->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, len ))) return STATUS_NO_MEMORY;
    }
    else if (len > dest->MaximumLength) return STATUS_BUFFER_OVERFLOW;

    for (i = 0; i < len / sizeof(WCHAR); i++)
        dest->Buffer[i] = casemap( nls_info.UpperCaseTable, src->Buffer[i] );
    dest->Length = len;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUpcaseUnicodeToCustomCPN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeToCustomCPN( CPTABLEINFO *info, char *dst, DWORD dstlen, DWORD *reslen,
                                             const WCHAR *src, DWORD srclen )
{
    DWORD i, ret;

    srclen /= sizeof(WCHAR);
    if (info->DBCSCodePage)
    {
        WCHAR *uni2cp = info->WideCharTable;

        for (i = dstlen; srclen && i; i--, srclen--, src++)
        {
            WCHAR ch = casemap( nls_info.UpperCaseTable, *src );
            if (uni2cp[ch] & 0xff00)
            {
                if (i == 1) break;  /* do not output a partial char */
                i--;
                *dst++ = uni2cp[ch] >> 8;
            }
            *dst++ = (char)uni2cp[ch];
        }
        ret = dstlen - i;
    }
    else
    {
        char *uni2cp = info->WideCharTable;
        ret = min( srclen, dstlen );
        for (i = 0; i < ret; i++) dst[i] = uni2cp[casemap( nls_info.UpperCaseTable, src[i] )];
    }
    if (reslen) *reslen = ret;
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUpcaseUnicodeToMultiByteN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeToMultiByteN( char *dst, DWORD dstlen, DWORD *reslen,
                                              const WCHAR *src, DWORD srclen )
{
    return RtlUpcaseUnicodeToCustomCPN( &nls_info.AnsiTableInfo, dst, dstlen, reslen, src, srclen );
}


/**************************************************************************
 *	RtlUpcaseUnicodeToOemN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUpcaseUnicodeToOemN( char *dst, DWORD dstlen, DWORD *reslen,
                                        const WCHAR *src, DWORD srclen )
{
    if (nls_info.OemTableInfo.WideCharTable)
        return RtlUpcaseUnicodeToCustomCPN( &nls_info.OemTableInfo, dst, dstlen, reslen, src, srclen );

    /* locale not setup yet */
    dstlen = min( srclen / sizeof(WCHAR), dstlen );
    if (reslen) *reslen = dstlen;
    while (dstlen--)
    {
        WCHAR ch = *src++;
        if (ch > 0x7f) ch = '?';
        else ch = casemap_ascii( ch );
        *dst++ = ch;
    }
    return STATUS_SUCCESS;
}


/*********************************************************************
 *	towlower   (NTDLL.@)
 */
WCHAR __cdecl towlower( WCHAR ch )
{
    if (ch >= 0x100) return ch;
    return casemap( nls_info.LowerCaseTable, ch );
}


/*********************************************************************
 *           towupper    (NTDLL.@)
 */
WCHAR __cdecl towupper( WCHAR ch )
{
    if (nls_info.UpperCaseTable) return casemap( nls_info.UpperCaseTable, ch );
    return casemap_ascii( ch );
}


/******************************************************************
 *      RtlIsValidLocaleName   (NTDLL.@)
 */
BOOLEAN WINAPI RtlIsValidLocaleName( const WCHAR *name, ULONG flags )
{
    const NLS_LOCALE_LCNAME_INDEX *entry = find_lcname_entry( name );

    if (!entry) return FALSE;
    /* reject neutral locale unless flag 2 is set */
    if (!(flags & 2) && !get_locale_data( entry->idx )->inotneutral) return FALSE;
    return TRUE;
}


/******************************************************************
 *      RtlLcidToLocaleName   (NTDLL.@)
 */
NTSTATUS WINAPI RtlLcidToLocaleName( LCID lcid, UNICODE_STRING *str, ULONG flags, BOOLEAN alloc )
{
    const NLS_LOCALE_LCID_INDEX *entry;
    const WCHAR *name;
    ULONG len;

    if (!str) return STATUS_INVALID_PARAMETER_2;

    switch (lcid)
    {
    case LOCALE_USER_DEFAULT:
        NtQueryDefaultLocale( TRUE, &lcid );
        break;
    case LOCALE_SYSTEM_DEFAULT:
    case LOCALE_CUSTOM_DEFAULT:
        NtQueryDefaultLocale( FALSE, &lcid );
        break;
    case LOCALE_CUSTOM_UI_DEFAULT:
        return STATUS_UNSUCCESSFUL;
    case LOCALE_CUSTOM_UNSPECIFIED:
        return STATUS_INVALID_PARAMETER_1;
    }

    if (!(entry = find_lcid_entry( lcid ))) return STATUS_INVALID_PARAMETER_1;
    /* reject neutral locale unless flag 2 is set */
    if (!(flags & 2) && !get_locale_data( entry->idx )->inotneutral) return STATUS_INVALID_PARAMETER_1;

    name = locale_strings + entry->name;
    len = *name++;

    if (alloc)
    {
        if (!(str->Buffer = RtlAllocateHeap( GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR) )))
            return STATUS_NO_MEMORY;
        str->MaximumLength = (len + 1) * sizeof(WCHAR);
    }
    else if (str->MaximumLength < (len + 1) * sizeof(WCHAR)) return STATUS_BUFFER_TOO_SMALL;

    wcscpy( str->Buffer, name );
    str->Length = len * sizeof(WCHAR);
    TRACE( "%04x -> %s\n", lcid, debugstr_us(str) );
    return STATUS_SUCCESS;
}


/******************************************************************
 *      RtlLocaleNameToLcid   (NTDLL.@)
 */
NTSTATUS WINAPI RtlLocaleNameToLcid( const WCHAR *name, LCID *lcid, ULONG flags )
{
    const NLS_LOCALE_LCNAME_INDEX *entry = find_lcname_entry( name );

    if (!entry) return STATUS_INVALID_PARAMETER_1;
    /* reject neutral locale unless flag 2 is set */
    if (!(flags & 2) && !get_locale_data( entry->idx )->inotneutral) return STATUS_INVALID_PARAMETER_1;
    *lcid = entry->id;
    TRACE( "%s -> %04x\n", debugstr_w(name), *lcid );
    return STATUS_SUCCESS;
}


/**************************************************************************
 *	RtlUTF8ToUnicodeN   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUTF8ToUnicodeN( WCHAR *dst, DWORD dstlen, DWORD *reslen, const char *src, DWORD srclen )
{
    unsigned int res, len;
    NTSTATUS status = STATUS_SUCCESS;
    const char *srcend = src + srclen;
    WCHAR *dstend;

    if (!src) return STATUS_INVALID_PARAMETER_4;
    if (!reslen) return STATUS_INVALID_PARAMETER;

    dstlen /= sizeof(WCHAR);
    dstend = dst + dstlen;
    if (!dst)
    {
        for (len = 0; src < srcend; len++)
        {
            unsigned char ch = *src++;
            if (ch < 0x80) continue;
            if ((res = decode_utf8_char( ch, &src, srcend )) > 0x10ffff)
                status = STATUS_SOME_NOT_MAPPED;
            else
                if (res > 0xffff) len++;
        }
        *reslen = len * sizeof(WCHAR);
        return status;
    }

    while ((dst < dstend) && (src < srcend))
    {
        unsigned char ch = *src++;
        if (ch < 0x80)  /* special fast case for 7-bit ASCII */
        {
            *dst++ = ch;
            continue;
        }
        if ((res = decode_utf8_char( ch, &src, srcend )) <= 0xffff)
        {
            *dst++ = res;
        }
        else if (res <= 0x10ffff)  /* we need surrogates */
        {
            res -= 0x10000;
            *dst++ = 0xd800 | (res >> 10);
            if (dst == dstend) break;
            *dst++ = 0xdc00 | (res & 0x3ff);
        }
        else
        {
            *dst++ = 0xfffd;
            status = STATUS_SOME_NOT_MAPPED;
        }
    }
    if (src < srcend) status = STATUS_BUFFER_TOO_SMALL;  /* overflow */
    *reslen = (dstlen - (dstend - dst)) * sizeof(WCHAR);
    return status;
}


/**************************************************************************
 *	RtlUnicodeToUTF8N   (NTDLL.@)
 */
NTSTATUS WINAPI RtlUnicodeToUTF8N( char *dst, DWORD dstlen, DWORD *reslen, const WCHAR *src, DWORD srclen )
{
    char *end;
    unsigned int val, len;
    NTSTATUS status = STATUS_SUCCESS;

    if (!src) return STATUS_INVALID_PARAMETER_4;
    if (!reslen) return STATUS_INVALID_PARAMETER;
    if (dst && (srclen & 1)) return STATUS_INVALID_PARAMETER_5;

    srclen /= sizeof(WCHAR);

    if (!dst)
    {
        for (len = 0; srclen; srclen--, src++)
        {
            if (*src < 0x80) len++;  /* 0x00-0x7f: 1 byte */
            else if (*src < 0x800) len += 2;  /* 0x80-0x7ff: 2 bytes */
            else
            {
                if (!get_utf16( src, srclen, &val ))
                {
                    val = 0xfffd;
                    status = STATUS_SOME_NOT_MAPPED;
                }
                if (val < 0x10000) len += 3; /* 0x800-0xffff: 3 bytes */
                else   /* 0x10000-0x10ffff: 4 bytes */
                {
                    len += 4;
                    src++;
                    srclen--;
                }
            }
        }
        *reslen = len;
        return status;
    }

    for (end = dst + dstlen; srclen; srclen--, src++)
    {
        WCHAR ch = *src;

        if (ch < 0x80)  /* 0x00-0x7f: 1 byte */
        {
            if (dst > end - 1) break;
            *dst++ = ch;
            continue;
        }
        if (ch < 0x800)  /* 0x80-0x7ff: 2 bytes */
        {
            if (dst > end - 2) break;
            dst[1] = 0x80 | (ch & 0x3f);
            ch >>= 6;
            dst[0] = 0xc0 | ch;
            dst += 2;
            continue;
        }
        if (!get_utf16( src, srclen, &val ))
        {
            val = 0xfffd;
            status = STATUS_SOME_NOT_MAPPED;
        }
        if (val < 0x10000)  /* 0x800-0xffff: 3 bytes */
        {
            if (dst > end - 3) break;
            dst[2] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[1] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[0] = 0xe0 | val;
            dst += 3;
        }
        else   /* 0x10000-0x10ffff: 4 bytes */
        {
            if (dst > end - 4) break;
            dst[3] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[2] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[1] = 0x80 | (val & 0x3f);
            val >>= 6;
            dst[0] = 0xf0 | val;
            dst += 4;
            src++;
            srclen--;
        }
    }
    if (srclen) status = STATUS_BUFFER_TOO_SMALL;
    *reslen = dstlen - (end - dst);
    return status;
}


/******************************************************************************
 *      RtlIsNormalizedString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlIsNormalizedString( ULONG form, const WCHAR *str, INT len, BOOLEAN *res )
{
    const struct norm_table *info;
    NTSTATUS status;
    BYTE props, class, last_class = 0;
    unsigned int ch;
    int i, r, result = 1;

    if ((status = load_norm_table( form, &info ))) return status;

    if (len == -1) len = wcslen( str );

    for (i = 0; i < len && result; i += r)
    {
        if (!(r = get_utf16( str + i, len - i, &ch ))) return STATUS_NO_UNICODE_TRANSLATION;
        if (info->comp_size)
        {
            if ((ch >= HANGUL_VBASE && ch < HANGUL_VBASE + HANGUL_VCOUNT) ||
                (ch >= HANGUL_TBASE && ch < HANGUL_TBASE + HANGUL_TCOUNT))
            {
                result = -1;  /* QC=Maybe */
                continue;
            }
        }
        else if (ch >= HANGUL_SBASE && ch < HANGUL_SBASE + HANGUL_SCOUNT)
        {
            result = 0;  /* QC=No */
            break;
        }
        props = get_char_props( info, ch );
        class = props & 0x3f;
        if (class == 0x3f)
        {
            last_class = 0;
            if (props == 0xbf) result = 0;  /* QC=No */
            else if (props == 0xff)
            {
                /* ignore other chars in Hangul range */
                if (ch >= HANGUL_LBASE && ch < HANGUL_LBASE + 0x100) continue;
                if (ch >= HANGUL_SBASE && ch < HANGUL_SBASE + 0x2c00) continue;
                /* allow final null */
                if (!ch && i == len - 1) continue;
                return STATUS_NO_UNICODE_TRANSLATION;
            }
        }
        else if (props & 0x80)
        {
            if ((props & 0xc0) == 0xc0) result = -1;  /* QC=Maybe */
            if (class && class < last_class) result = 0;  /* QC=No */
            last_class = class;
        }
        else last_class = 0;
    }

    if (result == -1)
    {
        int dstlen = len * 4;
        NTSTATUS status;
        WCHAR *buffer = RtlAllocateHeap( GetProcessHeap(), 0, dstlen * sizeof(WCHAR) );
        if (!buffer) return STATUS_NO_MEMORY;
        status = RtlNormalizeString( form, str, len, buffer, &dstlen );
        result = !status && (dstlen == len) && !wcsncmp( buffer, str, len );
        RtlFreeHeap( GetProcessHeap(), 0, buffer );
    }
    *res = result;
    return STATUS_SUCCESS;
}


/******************************************************************************
 *      RtlNormalizeString   (NTDLL.@)
 */
NTSTATUS WINAPI RtlNormalizeString( ULONG form, const WCHAR *src, INT src_len, WCHAR *dst, INT *dst_len )
{
    int buf_len;
    WCHAR *buf = NULL;
    const struct norm_table *info;
    NTSTATUS status = STATUS_SUCCESS;

    TRACE( "%x %s %d %p %d\n", form, debugstr_wn(src, src_len), src_len, dst, *dst_len );

    if ((status = load_norm_table( form, &info ))) return status;

    if (src_len == -1) src_len = wcslen(src) + 1;

    if (!*dst_len)
    {
        *dst_len = src_len * info->len_factor;
        if (*dst_len > 64) *dst_len = max( 64, src_len + src_len / 8 );
        return STATUS_SUCCESS;
    }
    if (!src_len)
    {
        *dst_len = 0;
        return STATUS_SUCCESS;
    }

    if (!info->comp_size) return decompose_string( info, src, src_len, dst, dst_len );

    buf_len = src_len * 4;
    for (;;)
    {
        buf = RtlAllocateHeap( GetProcessHeap(), 0, buf_len * sizeof(WCHAR) );
        if (!buf) return STATUS_NO_MEMORY;
        status = decompose_string( info, src, src_len, buf, &buf_len );
        if (status != STATUS_BUFFER_TOO_SMALL) break;
        RtlFreeHeap( GetProcessHeap(), 0, buf );
    }
    if (!status)
    {
        buf_len = compose_string( info, buf, buf_len );
        if (*dst_len >= buf_len) memcpy( dst, buf, buf_len * sizeof(WCHAR) );
        else status = STATUS_BUFFER_TOO_SMALL;
    }
    RtlFreeHeap( GetProcessHeap(), 0, buf );
    *dst_len = buf_len;
    return status;
}


/* Punycode parameters */
enum { BASE = 36, TMIN = 1, TMAX = 26, SKEW = 38, DAMP = 700 };

static BOOL check_invalid_chars( const struct norm_table *info, DWORD flags,
                                 const unsigned int *buffer, int len )
{
    int i;

    for (i = 0; i < len; i++)
    {
        switch (buffer[i])
        {
        case 0x200c:  /* zero-width non-joiner */
        case 0x200d:  /* zero-width joiner */
            if (!i || get_combining_class( info, buffer[i - 1] ) != 9) return TRUE;
            break;
        case 0x2260:  /* not equal to */
        case 0x226e:  /* not less than */
        case 0x226f:  /* not greater than */
            if (flags & IDN_USE_STD3_ASCII_RULES) return TRUE;
            break;
        }
        switch (get_char_props( info, buffer[i] ))
        {
        case 0xbf:
            return TRUE;
        case 0xff:
            if (buffer[i] >= HANGUL_SBASE && buffer[i] < HANGUL_SBASE + 0x2c00) break;
            return TRUE;
        case 0x7f:
            if (!(flags & IDN_ALLOW_UNASSIGNED)) return TRUE;
            break;
        }
    }

    if ((flags & IDN_USE_STD3_ASCII_RULES) && len && (buffer[0] == '-' || buffer[len - 1] == '-'))
        return TRUE;

    return FALSE;
}


/******************************************************************************
 *      RtlIdnToAscii   (NTDLL.@)
 */
NTSTATUS WINAPI RtlIdnToAscii( DWORD flags, const WCHAR *src, INT srclen, WCHAR *dst, INT *dstlen )
{
    static const WCHAR prefixW[] = {'x','n','-','-'};
    const struct norm_table *info;
    NTSTATUS status;
    WCHAR normstr[256], res[256];
    unsigned int ch, buffer[64];
    int i, len, start, end, out_label, out = 0, normlen = ARRAY_SIZE(normstr);

    TRACE( "%x %s %p %d\n", flags, debugstr_wn(src, srclen), dst, *dstlen );

    if ((status = load_norm_table( 13, &info ))) return status;

    if ((status = RtlIdnToNameprepUnicode( flags, src, srclen, normstr, &normlen ))) return status;

    /* implementation of Punycode based on RFC 3492 */

    for (start = 0; start < normlen; start = end + 1)
    {
        int n = 0x80, bias = 72, delta = 0, b = 0, h, buflen = 0;

        out_label = out;
        for (i = start; i < normlen; i += len)
        {
            if (!(len = get_utf16( normstr + i, normlen - i, &ch ))) break;
            if (!ch || ch == '.') break;
            if (ch < 0x80) b++;
            buffer[buflen++] = ch;
        }
        end = i;

        if (b == end - start)
        {
            if (end < normlen) b++;
            if (out + b > ARRAY_SIZE(res)) return STATUS_INVALID_IDN_NORMALIZATION;
            memcpy( res + out, normstr + start, b * sizeof(WCHAR) );
            out += b;
            continue;
        }

        if (buflen >= 4 && buffer[2] == '-' && buffer[3] == '-') return STATUS_INVALID_IDN_NORMALIZATION;
        if (check_invalid_chars( info, flags, buffer, buflen )) return STATUS_INVALID_IDN_NORMALIZATION;

        if (out + 5 + b > ARRAY_SIZE(res)) return STATUS_INVALID_IDN_NORMALIZATION;
        memcpy( res + out, prefixW, sizeof(prefixW) );
        out += ARRAY_SIZE(prefixW);
        if (b)
        {
            for (i = start; i < end; i++) if (normstr[i] < 0x80) res[out++] = normstr[i];
            res[out++] = '-';
        }

        for (h = b; h < buflen; delta++, n++)
        {
            int m = 0x10ffff, q, k;

            for (i = 0; i < buflen; i++) if (buffer[i] >= n && m > buffer[i]) m = buffer[i];
            delta += (m - n) * (h + 1);
            n = m;

            for (i = 0; i < buflen; i++)
            {
                if (buffer[i] == n)
                {
                    for (q = delta, k = BASE; ; k += BASE)
                    {
                        int t = k <= bias ? TMIN : k >= bias + TMAX ? TMAX : k - bias;
                        int disp = q < t ? q : t + (q - t) % (BASE - t);
                        if (out + 1 > ARRAY_SIZE(res)) return STATUS_INVALID_IDN_NORMALIZATION;
                        res[out++] = disp <= 25 ? 'a' + disp : '0' + disp - 26;
                        if (q < t) break;
                        q = (q - t) / (BASE - t);
                    }
                    delta /= (h == b ? DAMP : 2);
                    delta += delta / (h + 1);
                    for (k = 0; delta > ((BASE - TMIN) * TMAX) / 2; k += BASE) delta /= BASE - TMIN;
                    bias = k + ((BASE - TMIN + 1) * delta) / (delta + SKEW);
                    delta = 0;
                    h++;
                }
                else if (buffer[i] < n) delta++;
            }
        }

        if (out - out_label > 63) return STATUS_INVALID_IDN_NORMALIZATION;

        if (end < normlen)
        {
            if (out + 1 > ARRAY_SIZE(res)) return STATUS_INVALID_IDN_NORMALIZATION;
            res[out++] = normstr[end];
        }
    }

    if (*dstlen)
    {
        if (out <= *dstlen) memcpy( dst, res, out * sizeof(WCHAR) );
        else status = STATUS_BUFFER_TOO_SMALL;
    }
    *dstlen = out;
    return status;
}


/******************************************************************************
 *      RtlIdnToNameprepUnicode   (NTDLL.@)
 */
NTSTATUS WINAPI RtlIdnToNameprepUnicode( DWORD flags, const WCHAR *src, INT srclen,
                                         WCHAR *dst, INT *dstlen )
{
    const struct norm_table *info;
    unsigned int ch;
    NTSTATUS status;
    WCHAR buf[256];
    int i, start, len, buflen = ARRAY_SIZE(buf);

    if (flags & ~(IDN_ALLOW_UNASSIGNED | IDN_USE_STD3_ASCII_RULES)) return STATUS_INVALID_PARAMETER;
    if (!src || srclen < -1) return STATUS_INVALID_PARAMETER;

    TRACE( "%x %s %p %d\n", flags, debugstr_wn(src, srclen), dst, *dstlen );

    if ((status = load_norm_table( 13, &info ))) return status;

    if (srclen == -1) srclen = wcslen(src) + 1;

    for (i = 0; i < srclen; i++) if (src[i] < 0x20 || src[i] >= 0x7f) break;

    if (i == srclen || (i == srclen - 1 && !src[i]))  /* ascii only */
    {
        if (srclen > buflen) return STATUS_INVALID_IDN_NORMALIZATION;
        memcpy( buf, src, srclen * sizeof(WCHAR) );
        buflen = srclen;
    }
    else if ((status = RtlNormalizeString( 13, src, srclen, buf, &buflen )))
    {
        if (status == STATUS_NO_UNICODE_TRANSLATION) status = STATUS_INVALID_IDN_NORMALIZATION;
        return status;
    }

    for (i = start = 0; i < buflen; i += len)
    {
        if (!(len = get_utf16( buf + i, buflen - i, &ch ))) break;
        if (!ch) break;
        if (ch == '.')
        {
            if (start == i) return STATUS_INVALID_IDN_NORMALIZATION;
            /* maximal label length is 63 characters */
            if (i - start > 63) return STATUS_INVALID_IDN_NORMALIZATION;
            if ((flags & IDN_USE_STD3_ASCII_RULES) && (buf[start] == '-' || buf[i-1] == '-'))
                return STATUS_INVALID_IDN_NORMALIZATION;
            start = i + 1;
            continue;
        }
        if (flags & IDN_USE_STD3_ASCII_RULES)
        {
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-') continue;
            return STATUS_INVALID_IDN_NORMALIZATION;
        }
        if (!(flags & IDN_ALLOW_UNASSIGNED))
        {
            if (get_char_props( info, ch ) == 0x7f) return STATUS_INVALID_IDN_NORMALIZATION;
        }
    }
    if (!i || i - start > 63) return STATUS_INVALID_IDN_NORMALIZATION;
    if ((flags & IDN_USE_STD3_ASCII_RULES) && (buf[start] == '-' || buf[i-1] == '-'))
        return STATUS_INVALID_IDN_NORMALIZATION;

    if (*dstlen)
    {
        if (buflen <= *dstlen) memcpy( dst, buf, buflen * sizeof(WCHAR) );
        else status = STATUS_BUFFER_TOO_SMALL;
    }
    *dstlen = buflen;
    return status;
}


/******************************************************************************
 *      RtlIdnToUnicode   (NTDLL.@)
 */
NTSTATUS WINAPI RtlIdnToUnicode( DWORD flags, const WCHAR *src, INT srclen, WCHAR *dst, INT *dstlen )
{
    const struct norm_table *info;
    int i, buflen, start, end, out_label, out = 0;
    NTSTATUS status;
    UINT buffer[64];
    WCHAR ch;

    if (!src || srclen < -1) return STATUS_INVALID_PARAMETER;
    if (srclen == -1) srclen = wcslen( src ) + 1;

    TRACE( "%x %s %p %d\n", flags, debugstr_wn(src, srclen), dst, *dstlen );

    if ((status = load_norm_table( 13, &info ))) return status;

    for (start = 0; start < srclen; )
    {
        int n = 0x80, bias = 72, pos = 0, old_pos, w, k, t, delim = 0, digit, delta;

        out_label = out;
        for (i = start; i < srclen; i++)
        {
            ch = src[i];
            if (ch > 0x7f || (i != srclen - 1 && !ch)) return STATUS_INVALID_IDN_NORMALIZATION;
            if (!ch || ch == '.') break;
            if (ch == '-') delim = i;

            if (!(flags & IDN_USE_STD3_ASCII_RULES)) continue;
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-')
                continue;
            return STATUS_INVALID_IDN_NORMALIZATION;
        }
        end = i;

        /* last label may be empty */
        if (start == end && ch) return STATUS_INVALID_IDN_NORMALIZATION;

        if (end - start < 4 ||
            (src[start] != 'x' && src[start] != 'X') ||
            (src[start + 1] != 'n' && src[start + 1] != 'N') ||
            src[start + 2] != '-' || src[start + 3] != '-')
        {
            if (end - start > 63) return STATUS_INVALID_IDN_NORMALIZATION;

            if ((flags & IDN_USE_STD3_ASCII_RULES) && (src[start] == '-' || src[end - 1] == '-'))
                return STATUS_INVALID_IDN_NORMALIZATION;

            if (end < srclen) end++;
            if (*dstlen)
            {
                if (out + end - start <= *dstlen)
                    memcpy( dst + out, src + start, (end - start) * sizeof(WCHAR));
                else return STATUS_BUFFER_TOO_SMALL;
            }
            out += end - start;
            start = end;
            continue;
        }

        if (delim == start + 3) delim++;
        buflen = 0;
        for (i = start + 4; i < delim && buflen < ARRAY_SIZE(buffer); i++) buffer[buflen++] = src[i];
        if (buflen) i++;
        while (i < end)
        {
            old_pos = pos;
            w = 1;
            for (k = BASE; ; k += BASE)
            {
                if (i >= end) return STATUS_INVALID_IDN_NORMALIZATION;
                ch = src[i++];
                if (ch >= 'a' && ch <= 'z') digit = ch - 'a';
                else if (ch >= 'A' && ch <= 'Z') digit = ch - 'A';
                else if (ch >= '0' && ch <= '9') digit = ch - '0' + 26;
                else return STATUS_INVALID_IDN_NORMALIZATION;
                pos += digit * w;
                t = k <= bias ? TMIN : k >= bias + TMAX ? TMAX : k - bias;
                if (digit < t) break;
                w *= BASE - t;
            }

            delta = (pos - old_pos) / (!old_pos ? DAMP : 2);
            delta += delta / (buflen + 1);
            for (k = 0; delta > ((BASE - TMIN) * TMAX) / 2; k += BASE) delta /= BASE - TMIN;
            bias = k + ((BASE - TMIN + 1) * delta) / (delta + SKEW);
            n += pos / (buflen + 1);
            pos %= buflen + 1;

            if (buflen >= ARRAY_SIZE(buffer) - 1) return STATUS_INVALID_IDN_NORMALIZATION;
            memmove( buffer + pos + 1, buffer + pos, (buflen - pos) * sizeof(*buffer) );
            buffer[pos++] = n;
            buflen++;
        }

        if (check_invalid_chars( info, flags, buffer, buflen )) return STATUS_INVALID_IDN_NORMALIZATION;

        for (i = 0; i < buflen; i++)
        {
            int len = 1 + (buffer[i] >= 0x10000);
            if (*dstlen)
            {
                if (out + len <= *dstlen) put_utf16( dst + out, buffer[i] );
                else return STATUS_BUFFER_TOO_SMALL;
            }
            out += len;
        }

        if (out - out_label > 63) return STATUS_INVALID_IDN_NORMALIZATION;

        if (end < srclen)
        {
            if (*dstlen)
            {
                if (out + 1 <= *dstlen) dst[out] = src[end];
                else return STATUS_BUFFER_TOO_SMALL;
            }
            out++;
        }
        start = end + 1;
    }
    *dstlen = out;
    return STATUS_SUCCESS;
}
