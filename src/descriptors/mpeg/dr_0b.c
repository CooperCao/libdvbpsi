/*****************************************************************************
 * dr_0b.c
 * Copyright (C) 2001-2011 VideoLAN
 * $Id: dr_0b.c,v 1.3 2003/07/25 20:20:40 fenrir Exp $
 *
 * Authors: Arnaud de Bossoreille de Ribou <bozo@via.ecp.fr>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#if defined(HAVE_INTTYPES_H)
#include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#include <stdint.h>
#endif

#include "../../dvbpsi.h"
#include "../../dvbpsi_private.h"
#include "../../descriptor.h"

#include "dr_0b.h"


/*****************************************************************************
 * dvbpsi_decode_mpeg_system_clock_dr
 *****************************************************************************/
dvbpsi_mpeg_system_clock_dr_t * dvbpsi_decode_mpeg_system_clock_dr(
                                        dvbpsi_descriptor_t * p_descriptor)
{
    dvbpsi_mpeg_system_clock_dr_t * p_decoded;

    /* Check the tag */
    if (!dvbpsi_CanDecodeAsDescriptor(p_descriptor, 0x0b))
        return NULL;

    /* Don't decode twice */
    if (dvbpsi_IsDescriptorDecoded(p_descriptor))
        return p_descriptor->p_decoded;

    /* Check the length */
    if (p_descriptor->i_length != 2)
        return NULL;

    /* Allocate memory */
    p_decoded =
            (dvbpsi_mpeg_system_clock_dr_t*)malloc(sizeof(dvbpsi_mpeg_system_clock_dr_t));
    if (!p_decoded)
        return NULL;

    p_decoded->b_external_clock_ref = (p_descriptor->p_data[0] & 0x80) ? true : false;
    p_decoded->i_clock_accuracy_integer = p_descriptor->p_data[0] & 0x3f;
    p_decoded->i_clock_accuracy_exponent = (p_descriptor->p_data[1] & 0xe0) >> 5;

    p_descriptor->p_decoded = (void*)p_decoded;

    return p_decoded;
}

/*****************************************************************************
 * dvbpsi_gen_mpeg_system_clock_dr
 *****************************************************************************/
dvbpsi_descriptor_t * dvbpsi_gen_mpeg_system_clock_dr(dvbpsi_mpeg_system_clock_dr_t * p_decoded,
                                              bool b_duplicate)
{
    /* Create the descriptor */
    dvbpsi_descriptor_t * p_descriptor =
            dvbpsi_NewDescriptor(0x0b, 2, NULL);
    if (!p_descriptor)
        return NULL;

    /* Encode data */
    p_descriptor->p_data[0] =
            0x40 | (p_decoded->i_clock_accuracy_integer & 0x3f);
    if (p_decoded->b_external_clock_ref)
        p_descriptor->p_data[0] |= 0x80;
    p_descriptor->p_data[1] = 0x1f | p_decoded->i_clock_accuracy_exponent << 5;

    if (b_duplicate)
    {
        /* Duplicate decoded data */
        p_descriptor->p_decoded =
                dvbpsi_DuplicateDecodedDescriptor(p_decoded,
                                                  sizeof(dvbpsi_mpeg_system_clock_dr_t));
    }

    return p_descriptor;
}
