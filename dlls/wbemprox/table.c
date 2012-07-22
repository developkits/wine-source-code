/*
 * Copyright 2012 Hans Leidekker for CodeWeavers
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

#define COBJMACROS

#include "config.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wbemcli.h"

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

HRESULT get_column_index( const struct table *table, const WCHAR *name, UINT *column )
{
    UINT i;
    for (i = 0; i < table->num_cols; i++)
    {
        if (!strcmpiW( table->columns[i].name, name ))
        {
            *column = i;
            return S_OK;
        }
    }
    return WBEM_E_INVALID_QUERY;
}

static UINT get_column_size( const struct table *table, UINT column )
{
    if (table->columns[column].type & CIM_FLAG_ARRAY) return sizeof(void *);

    switch (table->columns[column].type & COL_TYPE_MASK)
    {
    case CIM_SINT16:
    case CIM_UINT16:
        return sizeof(INT16);
    case CIM_SINT32:
    case CIM_UINT32:
        return sizeof(INT32);
    case CIM_SINT64:
    case CIM_UINT64:
        return sizeof(INT64);
    case CIM_DATETIME:
    case CIM_STRING:
        return sizeof(WCHAR *);
    default:
        ERR("unknown column type %u\n", table->columns[column].type & COL_TYPE_MASK);
        break;
    }
    return sizeof(INT32);
}

static UINT get_column_offset( const struct table *table, UINT column )
{
    UINT i, offset = 0;
    for (i = 0; i < column; i++) offset += get_column_size( table, i );
    return offset;
}

static UINT get_row_size( const struct table *table )
{
    return get_column_offset( table, table->num_cols - 1 ) + get_column_size( table, table->num_cols - 1 );
}

HRESULT get_value( const struct table *table, UINT row, UINT column, LONGLONG *val )
{
    UINT col_offset, row_size;
    const BYTE *ptr;

    col_offset = get_column_offset( table, column );
    row_size = get_row_size( table );
    ptr = table->data + row * row_size + col_offset;

    if (table->columns[column].type & CIM_FLAG_ARRAY)
    {
        *val = (LONGLONG)(INT_PTR)*(const void **)ptr;
        return S_OK;
    }
    switch (table->columns[column].type & COL_TYPE_MASK)
    {
    case CIM_DATETIME:
    case CIM_STRING:
        *val = (LONGLONG)(INT_PTR)*(const WCHAR **)ptr;
        break;
    case CIM_SINT16:
        *val = *(const INT16 *)ptr;
        break;
    case CIM_UINT16:
        *val = *(const UINT16 *)ptr;
        break;
    case CIM_SINT32:
        *val = *(const INT32 *)ptr;
        break;
    case CIM_UINT32:
        *val = *(const UINT32 *)ptr;
        break;
    case CIM_SINT64:
        *val = *(const INT64 *)ptr;
        break;
    case CIM_UINT64:
        *val = *(const UINT64 *)ptr;
        break;
    default:
        ERR("invalid column type %u\n", table->columns[column].type & COL_TYPE_MASK);
        *val = 0;
        break;
    }
    return S_OK;
}

BSTR get_value_bstr( const struct table *table, UINT row, UINT column )
{
    static const WCHAR fmt_signedW[] = {'%','d',0};
    static const WCHAR fmt_unsignedW[] = {'%','u',0};
    static const WCHAR fmt_signed64W[] = {'%','I','6','4','d',0};
    static const WCHAR fmt_unsigned64W[] = {'%','I','6','4','u',0};
    static const WCHAR fmt_strW[] = {'\"','%','s','\"',0};
    LONGLONG val;
    BSTR ret;
    WCHAR number[22];
    UINT len;

    if (table->columns[column].type & CIM_FLAG_ARRAY)
    {
        FIXME("array to string conversion not handled\n");
        return NULL;
    }
    if (get_value( table, row, column, &val ) != S_OK) return NULL;

    switch (table->columns[column].type & COL_TYPE_MASK)
    {
    case CIM_DATETIME:
    case CIM_STRING:
        len = strlenW( (const WCHAR *)(INT_PTR)val ) + 2;
        if (!(ret = SysAllocStringLen( NULL, len ))) return NULL;
        sprintfW( ret, fmt_strW, (const WCHAR *)(INT_PTR)val );
        return ret;

    case CIM_SINT16:
    case CIM_SINT32:
        sprintfW( number, fmt_signedW, val );
        return SysAllocString( number );

    case CIM_UINT16:
    case CIM_UINT32:
        sprintfW( number, fmt_unsignedW, val );
        return SysAllocString( number );

    case CIM_SINT64:
        wsprintfW( number, fmt_signed64W, val );
        return SysAllocString( number );

    case CIM_UINT64:
        wsprintfW( number, fmt_unsigned64W, val );
        return SysAllocString( number );

    default:
        FIXME("unhandled column type %u\n", table->columns[column].type & COL_TYPE_MASK);
        break;
    }
    return NULL;
}

static void clear_table( struct table *table )
{
    UINT i, j, type;
    LONGLONG val;

    if (!table->data) return;

    for (i = 0; i < table->num_rows; i++)
    {
        for (j = 0; j < table->num_cols; j++)
        {
            if (!(table->columns[j].type & COL_FLAG_DYNAMIC)) continue;

            type = table->columns[j].type & COL_TYPE_MASK;
            if (type == CIM_STRING || type == CIM_DATETIME || (type & CIM_FLAG_ARRAY))
            {
                if (get_value( table, i, j, &val ) == S_OK) heap_free( (void *)(INT_PTR)val );
            }
        }
    }
    table->num_rows = 0;
    if (table->fill)
    {
        heap_free( table->data );
        table->data = NULL;
    }
}

void free_columns( struct column *columns, UINT num_cols )
{
    UINT i;

    for (i = 0; i < num_cols; i++)
    {
        heap_free( (WCHAR *)columns[i].name );
    }
    heap_free( columns );
}

void free_table( struct table *table )
{
    if (!table) return;

    clear_table( table );
    if (table->flags & TABLE_FLAG_DYNAMIC)
    {
        heap_free( (WCHAR *)table->name );
        free_columns( (struct column *)table->columns, table->num_cols );
        heap_free( table );
    }
}

struct table *get_table( const WCHAR *name )
{
    struct table *table;

    LIST_FOR_EACH_ENTRY( table, table_list, struct table, entry )
    {
        if (!strcmpiW( table->name, name ))
        {
            if (table->fill && !table->data) table->fill( table );
            return table;
        }
    }
    return NULL;
}

struct table *create_table( const WCHAR *name, UINT num_cols, const struct column *columns,
                            UINT num_rows, BYTE *data, void (*fill)(struct table *) )
{
    struct table *table;

    if (!(table = heap_alloc( sizeof(*table) ))) return NULL;
    table->name     = name;
    table->num_cols = num_cols;
    table->columns  = columns;
    table->num_rows = num_rows;
    table->data     = data;
    table->fill     = fill;
    table->flags    = TABLE_FLAG_DYNAMIC;
    return table;
}

BOOL add_table( struct table *table )
{
    struct table *iter;

    LIST_FOR_EACH_ENTRY( iter, table_list, struct table, entry )
    {
        if (!strcmpiW( iter->name, table->name ))
        {
            TRACE("table %s already exists\n", debugstr_w(table->name));
            return FALSE;
        }
    }
    list_add_tail( table_list, &table->entry );
    return TRUE;
}