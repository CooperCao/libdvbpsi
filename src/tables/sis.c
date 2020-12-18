/*****************************************************************************
 * sis.c: SIS decoder/generator
 *----------------------------------------------------------------------------
 * Copyright (C) 2010-2011 VideoLAN
 * $Id:$
 *
 * Authors: Jean-Paul Saman <jpsaman@videolan.org>
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
 *----------------------------------------------------------------------------
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

#include <assert.h>

#include "../dvbpsi.h"
#include "../dvbpsi_private.h"
#include "../psi.h"
#include "../descriptor.h"
#include "../chain.h"

#include "sis.h"
#include "sis_private.h"

/*****************************************************************************
 * dvbpsi_sis_attach
 *****************************************************************************
 * Initialize a SIS subtable decoder.
 *****************************************************************************/
bool dvbpsi_sis_attach(dvbpsi_t *p_dvbpsi, uint8_t i_table_id, uint16_t i_extension,
                      dvbpsi_sis_callback pf_callback, void* p_priv)
{
    assert(p_dvbpsi);

    i_extension = 0;
    dvbpsi_decoder_t *p_dec = dvbpsi_decoder_chain_get(p_dvbpsi, i_table_id, i_extension);
    if (p_dec != NULL)
    {
        dvbpsi_error(p_dvbpsi, "SIS decoder",
                         "Already a decoder for (table_id == 0x%02x,"
                         "extension == 0x%02x)",
                         i_table_id, i_extension);
        return false;
    }

    dvbpsi_sis_decoder_t*  p_sis_decoder;
    p_sis_decoder = (dvbpsi_sis_decoder_t*) dvbpsi_decoder_new(dvbpsi_sis_sections_gather,
                                                4096, true, sizeof(dvbpsi_sis_decoder_t));
    if (p_sis_decoder == NULL)
        return false;

    /* SIS decoder information */
    p_sis_decoder->pf_sis_callback = pf_callback;
    p_sis_decoder->p_priv = p_priv;
    p_sis_decoder->p_building_sis = NULL;

    p_sis_decoder->i_table_id = i_table_id;
    p_sis_decoder->i_extension = i_extension;

    /* Add SIS decoder to decoder chain */
    if (!dvbpsi_decoder_chain_add(p_dvbpsi, DVBPSI_DECODER(p_sis_decoder)))
    {
        dvbpsi_decoder_delete(DVBPSI_DECODER(p_sis_decoder));
        return false;
    }
    return true;
}

/*****************************************************************************
 * dvbpsi_sis_detach
 *****************************************************************************
 * Close a SIS decoder.
 *****************************************************************************/
void dvbpsi_sis_detach(dvbpsi_t *p_dvbpsi, uint8_t i_table_id, uint16_t i_extension)
{
    assert(p_dvbpsi);

    i_extension = 0;
    dvbpsi_decoder_t *p_dec = dvbpsi_decoder_chain_get(p_dvbpsi, i_table_id, i_extension);
    if (p_dec == NULL)
    {
        dvbpsi_error(p_dvbpsi, "SIS Decoder",
                         "No such SIS decoder (table_id == 0x%02x,"
                         "extension == 0x%02x)",
                         i_table_id, i_extension);
        return;
    }

    /* Remove table decoder from decoder chain */
    if (!dvbpsi_decoder_chain_remove(p_dvbpsi, p_dec))
    {
        dvbpsi_error(p_dvbpsi, "SIS Decoder",
                     "Failed to remove"
                     "extension == 0x%02x)",
                      i_table_id, i_extension);
        return;
    }

    dvbpsi_sis_decoder_t* p_sis_decoder = (dvbpsi_sis_decoder_t*)p_dec;
    if (p_sis_decoder->p_building_sis)
        dvbpsi_sis_delete(p_sis_decoder->p_building_sis);
    p_sis_decoder->p_building_sis = NULL;
    dvbpsi_decoder_delete(p_dec);
    p_dec = NULL;
}

/*****************************************************************************
 * dvbpsi_sis_init
 *****************************************************************************
 * Initialize a pre-allocated dvbpsi_sis_t structure.
 *****************************************************************************/
void dvbpsi_sis_init(dvbpsi_sis_t *p_sis, uint8_t i_table_id, uint16_t i_extension,
                     uint8_t i_version, bool b_current_next, uint8_t i_protocol_version)
{
    p_sis->i_table_id = i_table_id;
    p_sis->i_extension = i_extension;

    p_sis->i_version = i_version;
    p_sis->b_current_next = b_current_next;

    assert(i_protocol_version == 0);
    p_sis->i_protocol_version = 0; /* must be 0 */

    /* encryption */
    p_sis->b_encrypted_packet = false;
    p_sis->i_encryption_algorithm = 0;

    p_sis->i_pts_adjustment = (uint64_t)0;
    p_sis->cw_index = 0;

    /* splice command */
    p_sis->i_splice_command_length = 0;
    p_sis->i_splice_command_type = 0x00;

    /* FIXME: splice_info_section comes here */

    /* descriptors */
    p_sis->i_descriptors_length = 0;
    p_sis->p_first_descriptor = NULL;

    /* FIXME: alignment stuffing */

    p_sis->i_ecrc = 0;
}

/*****************************************************************************
 * dvbpsi_sis_new
 *****************************************************************************
 * Allocate and Initialize a new dvbpsi_sis_t structure.
 *****************************************************************************/
dvbpsi_sis_t* dvbpsi_sis_new(uint8_t i_table_id, uint16_t i_extension, uint8_t i_version,
                             bool b_current_next, uint8_t i_protocol_version)
{
    dvbpsi_sis_t* p_sis = (dvbpsi_sis_t*)malloc(sizeof(dvbpsi_sis_t));
    if (p_sis != NULL)
        dvbpsi_sis_init(p_sis, i_table_id, i_extension, i_version,
                        b_current_next, i_protocol_version);
    return p_sis;
}

/*****************************************************************************
 * dvbpsi_sis_empty
 *****************************************************************************
 * Clean a dvbpsi_sis_t structure.
 *****************************************************************************/
void dvbpsi_sis_empty(dvbpsi_sis_t* p_sis)
{
    switch (p_sis->i_splice_command_type)
    {
        case 0x00: /* splice_null */
        case 0x04: /* splice_schedule */
            cmd_splice_schedule_cleanup(p_sis->p_splice_command);
            break;
        case 0x05: /* splice_insert */
            cmd_splice_insert_cleanup(p_sis->p_splice_command);
            break;
        case 0x06: /* time_signal */
            cmd_time_signal_cleanup(p_sis->p_splice_command);
            break;
        case 0x07: /* bandwidth_reservation */
            break;
        default:
            break;
    }
    p_sis->p_splice_command = NULL;

    dvbpsi_DeleteDescriptors(p_sis->p_first_descriptor);
    p_sis->p_first_descriptor = NULL;

    /* FIXME: free alignment stuffing */
}

/*****************************************************************************
 * dvbpsi_sis_delete
 *****************************************************************************
 * Clean and Delete a dvbpsi_sis_t structure.
 *****************************************************************************/
void dvbpsi_sis_delete(dvbpsi_sis_t *p_sis)
{
    if (p_sis)
        dvbpsi_sis_empty(p_sis);
    free(p_sis);
}

/*****************************************************************************
 * dvbpsi_sis_descriptor_add
 *****************************************************************************
 * Add a descriptor in the SIS service description.
 *****************************************************************************/
dvbpsi_descriptor_t *dvbpsi_sis_descriptor_add(dvbpsi_sis_t *p_sis,
                                             uint8_t i_tag, uint8_t i_length,
                                             uint8_t *p_data)
{
    dvbpsi_descriptor_t * p_descriptor;
    p_descriptor = dvbpsi_NewDescriptor(i_tag, i_length, p_data);
    if (p_descriptor == NULL)
        return NULL;

    p_sis->p_first_descriptor = dvbpsi_AddDescriptor(p_sis->p_first_descriptor,
                                                     p_descriptor);
    assert(p_sis->p_first_descriptor);
    if (p_sis->p_first_descriptor == NULL)
        return NULL;

    return p_descriptor;
}

/* */
static void dvbpsi_ReInitSIS(dvbpsi_sis_decoder_t* p_decoder, const bool b_force)
{
    assert(p_decoder);

    dvbpsi_decoder_reset(DVBPSI_DECODER(p_decoder), b_force);

    /* Force redecoding */
    if (b_force)
    {
        /* Free structures */
        if (p_decoder->p_building_sis)
            dvbpsi_sis_delete(p_decoder->p_building_sis);
    }
    p_decoder->p_building_sis = NULL;
}

static bool dvbpsi_CheckSIS(dvbpsi_t *p_dvbpsi, dvbpsi_sis_decoder_t* p_sis_decoder,
                            dvbpsi_psi_section_t *p_section)
{
    bool b_reinit = false;
    assert(p_dvbpsi);
    assert(p_sis_decoder);

    if (p_sis_decoder->p_building_sis->i_protocol_version != 0)
    {
        dvbpsi_error(p_dvbpsi, "SIS decoder",
                     "'protocol_version' differs"
                     " while no discontinuity has occurred");
        b_reinit = true;
    }
    else if (p_sis_decoder->p_building_sis->i_extension != p_section->i_extension)
    {
        dvbpsi_error(p_dvbpsi, "SIS decoder",
                "'transport_stream_id' differs"
                " whereas no discontinuity has occurred");
        b_reinit = true;
    }
    else if (p_sis_decoder->p_building_sis->i_version != p_section->i_version)
    {
        /* version_number */
        dvbpsi_error(p_dvbpsi, "SIS decoder",
                "'version_number' differs"
                " whereas no discontinuity has occurred");
        b_reinit = true;
    }
    else if (p_sis_decoder->i_last_section_number != p_section->i_last_number)
    {
        /* last_section_number */
        dvbpsi_error(p_dvbpsi, "SIS decoder",
                "'last_section_number' differs"
                " whereas no discontinuity has occurred");
        b_reinit = true;
    }

    return b_reinit;
}

static bool dvbpsi_AddSectionSIS(dvbpsi_t *p_dvbpsi, dvbpsi_sis_decoder_t *p_sis_decoder,
                                 dvbpsi_psi_section_t* p_section)
{
    assert(p_dvbpsi);
    assert(p_sis_decoder);
    assert(p_section);

    /* Initialize the structures if it's the first section received */
    if (!p_sis_decoder->p_building_sis)
    {
        p_sis_decoder->p_building_sis = dvbpsi_sis_new(
                            p_section->i_table_id, p_section->i_extension,
                            p_section->i_version, p_section->b_current_next, 0);
        if (p_sis_decoder->p_building_sis == NULL)
            return false;
        p_sis_decoder->i_last_section_number = p_section->i_last_number;
    }

    /* Add to linked list of sections */
    if (dvbpsi_decoder_psi_section_add(DVBPSI_DECODER(p_sis_decoder), p_section))
        dvbpsi_debug(p_dvbpsi, "SIS decoder", "overwrite section number %d",
                     p_section->i_number);

    return true;
}

/*****************************************************************************
 * dvbpsi_sis_sections_gather
 *****************************************************************************
 * Callback for the subtable demultiplexor.
 *****************************************************************************/
void dvbpsi_sis_sections_gather(dvbpsi_t *p_dvbpsi, dvbpsi_psi_section_t * p_section)
{
    assert(p_dvbpsi);
    assert(p_dvbpsi->p_decoder);

    if (!dvbpsi_CheckPSISection(p_dvbpsi, p_section, 0xFC, "SIS decoder"))
    {
        dvbpsi_DeletePSISections(p_section);
        return;
    }

    /* */
    dvbpsi_sis_decoder_t *p_sis_decoder = (dvbpsi_sis_decoder_t*)
            dvbpsi_decoder_chain_get(p_dvbpsi, p_section->i_table_id, p_section->i_extension);
    if (!p_sis_decoder)
    {
        dvbpsi_DeletePSISections(p_section);
        return;
    }

    if (p_section->b_private_indicator)
    {
        /* Invalid private_syntax_indicator */
        dvbpsi_error(p_dvbpsi, "SIS decoder",
                     "invalid private section (private_syntax_indicator != false)");
        dvbpsi_DeletePSISections(p_section);
        return;
    }

    /* TS discontinuity check */
    if (p_sis_decoder->b_discontinuity)
    {
        dvbpsi_ReInitSIS(p_sis_decoder, true);
        p_sis_decoder->b_discontinuity = false;
    }
    else
    {
        /* Perform a few sanity checks */
        if (p_sis_decoder->p_building_sis)
        {
            if (dvbpsi_CheckSIS(p_dvbpsi, p_sis_decoder, p_section))
                dvbpsi_ReInitSIS(p_sis_decoder, true);
        }
    }

    /* Add section to SIS */
    if (!dvbpsi_AddSectionSIS(p_dvbpsi, p_sis_decoder, p_section))
    {
        dvbpsi_error(p_dvbpsi, "SIS decoder", "failed decoding section %d",
                     p_section->i_number);
        dvbpsi_DeletePSISections(p_section);
        return;
    }

    /* Check if we have all the sections */
    if (dvbpsi_decoder_psi_sections_completed(DVBPSI_DECODER(p_sis_decoder)))
    {
        assert(p_sis_decoder->pf_sis_callback);

        /* Save the current information */
        p_sis_decoder->current_sis = *p_sis_decoder->p_building_sis;
        p_sis_decoder->b_current_valid = true;
        /* Decode the sections */
        dvbpsi_sis_sections_decode(p_dvbpsi, p_sis_decoder->p_building_sis,
                                   p_sis_decoder->p_sections);
        /* signal the new SIS */
        p_sis_decoder->pf_sis_callback(p_sis_decoder->p_priv,
                                       p_sis_decoder->p_building_sis);
        /* Delete sections and Reinitialize the structures */
        dvbpsi_ReInitSIS(p_sis_decoder, false);
        assert(p_sis_decoder->p_sections == NULL);
    }
}


/*****************************************************************************
 * dvbpsi_sis_utc_splice_time
 *****************************************************************************
 * extract UTC time
 *****************************************************************************/
static inline uint64_t dvbpsi_sis_utc_splice_time(uint8_t *p_data)
{
    return (((uint32_t)p_data[0] << 24) |
            ((uint32_t)p_data[1] << 16) |
            ((uint32_t)p_data[2] << 8)  |
             (uint32_t)p_data[3]);
}

/*****************************************************************************
 * dvbpsi_sis_splice_time
 *****************************************************************************
 * decode splice time in 90kHz clock
 *****************************************************************************/
static void dvbpsi_sis_splice_time(uint8_t *p_data, dvbpsi_sis_splice_time_t *p_splice_time)
{
    p_splice_time->b_time_specified_flag = (p_data[0] & 0x80);
    if (p_splice_time->b_time_specified_flag) {
        p_splice_time->i_pts_time =
                (((uint64_t)(p_data[0] & 0x01) << 32) |
                 ((uint64_t)p_data[1] << 24) |
                 ((uint64_t)p_data[2] << 16) |
                 ((uint64_t)p_data[3] <<  8) |
                  (uint64_t)p_data[4]);
    }
}

/*****************************************************************************
 * dvbpsi_sis_break_duration
 *****************************************************************************
 * * decode break duration in 90kHz clock
 *****************************************************************************/
static void dvbpsi_sis_break_duration(uint8_t *p_data, dvbpsi_sis_break_duration_t *p_break_duration)
{
    p_break_duration->b_auto_return = (p_data[0] & 0x80);
    p_break_duration->i_duration =
            ((((uint64_t)p_data[0] & 0x01) << 32) |
              ((uint64_t)p_data[1] << 24) |
              ((uint64_t)p_data[2] << 16) |
              ((uint64_t)p_data[3] << 8)  |
               (uint64_t)p_data[4]);
}

/*****************************************************************************
 * cmd_splice_insert_cleanup
 *****************************************************************************
 * Free resource for splice_insert command structure.
 *****************************************************************************/
static void cmd_splice_insert_cleanup(dvbpsi_sis_cmd_splice_insert_t *p_cmd)
{
    if (p_cmd)
    {
        dvbpsi_sis_component_splice_time_t *p_splice_time = p_cmd->p_splice_time;
        while (p_splice_time != NULL)
        {
            dvbpsi_sis_component_splice_time_t *p_next = p_splice_time->p_next;
            free(p_splice_time);
            p_splice_time = p_next;
        }
        p_cmd->p_splice_time = NULL;
        free(p_cmd);
    }
}

/*****************************************************************************
 * dvbpsi_sis_cmd_splice_insert_decode
 *****************************************************************************
 * splice_schedule command decoder.
 *****************************************************************************/
static dvbpsi_sis_cmd_splice_insert_t *
    dvbpsi_sis_cmd_splice_insert_decode(uint8_t *p_data, uint16_t i_length)
{
    /* splice_insert() is at least 5 bytes */
    if (i_length < 5) return NULL;

    dvbpsi_sis_cmd_splice_insert_t *p_cmd = calloc(1, sizeof(dvbpsi_sis_cmd_splice_insert_t));
    if (!p_cmd) return NULL;

    p_cmd->i_splice_event_id = (((uint32_t)p_data[0] << 24) |
                                ((uint32_t)p_data[1] << 16) |
                                ((uint32_t)p_data[2] << 8)  |
                                 (uint32_t)p_data[3]);
    p_cmd->b_splice_event_cancel_indicator = (p_data[4] & 0x80);
    if (!p_cmd->b_splice_event_cancel_indicator) {
        if (i_length < 10) /* should be at least 10 bytes now */
            goto error;

        p_cmd->b_out_of_network_indicator = (p_data[5] & 0x80);
        p_cmd->b_program_splice_flag      = (p_data[5] & 0x40);
        p_cmd->b_duration_flag            = (p_data[5] & 0x20);
        p_cmd->b_splice_immediate_flag    = (p_data[5] & 0x10);

        uint16_t pos = 6;
        uint16_t i_needed = 0;

        if (p_cmd->b_program_splice_flag && !p_cmd->b_splice_immediate_flag)
            i_needed = 1;
        else if (p_cmd->b_duration_flag)
            i_needed = 5;
        if (pos + i_needed >= i_length)
            goto error;

        if (p_cmd->b_program_splice_flag && !p_cmd->b_splice_immediate_flag) {
            if (pos + 5 >= i_length)
                goto error;

            /* splice_time () */
            dvbpsi_sis_splice_time(&p_data[pos], &p_cmd->i_splice_time);
            if (p_cmd->i_splice_time.b_time_specified_flag)
                pos += 5;
            else
                pos++;
            p_cmd->i_splice_time.p_next = NULL;
        }

        if (!p_cmd->b_program_splice_flag) {
            i_needed = 1;
            if (!p_cmd->b_splice_immediate_flag)
               i_needed += p_data[pos] * 6;
            else
               i_needed += p_data[pos];
            if (i_needed + pos >= i_length)
                goto error;

            p_cmd->i_component_count = p_data[pos];

            dvbpsi_sis_component_splice_time_t *p_last = p_cmd->p_splice_time;
            for (uint8_t i = 0; i < p_cmd->i_component_count; i++) {
                dvbpsi_sis_component_splice_time_t *p_splice_time;
                p_splice_time = (dvbpsi_sis_component_splice_time_t *)
                        calloc(1, sizeof(dvbpsi_sis_component_splice_time_t));
                if (!p_splice_time) {
                    /* partially decoded */
                    p_cmd->i_component_count = (i > 0) ? i - 1 : 0;
                    break;
                }
                p_splice_time->i_component_tag = p_data[pos++];
                if (!p_cmd->b_splice_immediate_flag) {
                    /* splice_time */
                    dvbpsi_sis_splice_time(&p_data[pos], &p_splice_time->i_splice_time);
                    if (p_splice_time->i_splice_time.b_time_specified_flag)
                        pos += 5;
                    else
                        pos++;
                }
                if (!p_cmd->p_splice_time)
                    p_cmd->p_splice_time = p_last = p_splice_time;
                else {
                    p_last->p_next = p_splice_time;
                    p_last = p_last->p_next;
                }
                /* Check if we have an overflow */
                assert(pos < i_length);
            }
        }
        if (p_cmd->b_duration_flag) {
            /* break duration */
            dvbpsi_sis_break_duration(&p_data[pos], &p_cmd->i_break_duration);
            pos += 5;
        }
        p_cmd->i_unique_program_id = (((uint16_t)p_data[pos] << 8) |
                                       (uint16_t)p_data[pos+1]);
        pos += 2;
        p_cmd->i_avail_num = p_data[pos];
        p_cmd->i_avails_expected = p_data[pos+1];
    }
    return p_cmd;
error:
    free(p_cmd);
    return NULL;
}

/*****************************************************************************
 * cmd_splice_schedule_cleanup
 *****************************************************************************
 * Free resource for splice_schedule command structure.
 *****************************************************************************/
static void cmd_splice_schedule_cleanup(dvbpsi_sis_cmd_splice_schedule_t *p_cmd)
{
    if (p_cmd)
    {
        dvbpsi_sis_splice_event_t* p_event = p_cmd->p_splice_event;
        while (p_event)
        {
            dvbpsi_sis_splice_event_t* p_next = p_event->p_next;
            if (p_event->p_component)
            {
                dvbpsi_sis_component_t* p_time = p_event->p_component;
                while (p_time)
                {
                    dvbpsi_sis_component_t* p_tmp = p_time->p_next;
                    free(p_time);
                    p_time = p_tmp;
                }
            }
            free(p_event);
            p_event = p_next;
        }
        free(p_cmd);
    }
}

/*****************************************************************************
 * dvbpsi_sis_cmd_splice_schedule_decode
 *****************************************************************************
 * splice_schedule command decoder.
 *****************************************************************************/
static dvbpsi_sis_cmd_splice_schedule_t *
    dvbpsi_sis_cmd_splice_schedule_decode(uint8_t *p_data, uint16_t i_length)
{
    dvbpsi_sis_cmd_splice_schedule_t *p_cmd = calloc(1, sizeof(dvbpsi_sis_cmd_splice_schedule_t));
    if (!p_cmd) return NULL;

    uint32_t pos = 0;
    p_cmd->i_splice_count = p_data[pos++];

    dvbpsi_sis_splice_event_t *p_last = p_cmd->p_splice_event;
    for (uint8_t i = pos; i < p_cmd->i_splice_count; i++) {
        dvbpsi_sis_splice_event_t *p_event;
        p_event = (dvbpsi_sis_splice_event_t *)calloc(1, sizeof(dvbpsi_sis_splice_event_t));
        if (!p_event) {
            cmd_splice_schedule_cleanup(p_cmd);
            return NULL;
        }

        p_event->i_splice_event_id = p_data[pos];
        pos += 4;
        p_event->b_splice_event_cancel_indicator = (p_data[pos] & 0x80);
        /* 7 reserved bits */
        if (!p_event->b_splice_event_cancel_indicator) {
            p_event->b_out_of_network_indicator = (p_data[pos++] & 0x80);
            p_event->b_program_splice_flag = (p_data[pos++] & 0x40);
            p_event->b_duration_flag = (p_data[pos++] & 0x20);
            /* 5 reserved bits */
            if (p_event->b_program_splice_flag) {
                /* utc_splice_time */
                p_event->i_utc_splice_time = dvbpsi_sis_utc_splice_time(&p_data[pos]);
                pos += 4;
            }
            else { /* component */
                /* Check */
                p_event->i_component_count = p_data[pos++];
                assert(pos + p_event->i_component_count * 5 < i_length);
                if (pos + p_event->i_component_count * 5 >= i_length) {
                    cmd_splice_schedule_cleanup(p_cmd);
                    free(p_event);
                    return NULL;
                }

                dvbpsi_sis_component_t *p_list = p_event->p_component;
                for (uint8_t j = 0; j < p_event->i_component_count; j++) {
                    dvbpsi_sis_component_t *p_time;
                    p_time  = (dvbpsi_sis_component_t *) calloc(1, sizeof(dvbpsi_sis_component_t));
                    if (!p_time) {
                        cmd_splice_schedule_cleanup(p_cmd);
                        free(p_event);
                        return NULL;
                    }
                    p_time->i_tag = p_data[pos++];
                    /* GPS_UTC time */
                    p_time->i_utc_splice_time = dvbpsi_sis_utc_splice_time(&p_data[pos]);
                    pos += 4;
                    if (!p_event->p_component)
                        p_event->p_component = p_list = p_time;
                    else {
                        p_list->p_next = p_time;
                        p_list = p_list->p_next;
                    }
                }
            }
            if (p_event->b_duration_flag) {
                /* break duration */
                dvbpsi_sis_break_duration(&p_data[pos], &p_event->i_break_duration);
                pos += 5;
            }
            p_event->i_unique_program_id = p_data[pos];
            pos += 2;
            p_event->i_avail_num = p_data[pos++];
            p_event->i_avails_expected = p_data[pos++];
        }

        if (!p_cmd->p_splice_event)
            p_cmd->p_splice_event = p_last = p_event;
        else {
            p_last->p_next = p_event;
            p_last = p_last->p_next;
        }

        /* Check if we have an overflow */
        assert(pos < i_length);
    }
    return p_cmd;
}

/*****************************************************************************
 * cmd_time_signal_cleanup
 *****************************************************************************
 * Free resource for time_signal command structure.
 *****************************************************************************/
static void cmd_time_signal_cleanup(dvbpsi_sis_cmd_time_signal_t *p_cmd)
{
    if (p_cmd)
    {
        dvbpsi_sis_splice_time_t *p_splice_time = p_cmd->p_splice_time;
        while (p_splice_time != NULL)
        {
            dvbpsi_sis_splice_time_t *p_next = p_splice_time->p_next;
            free(p_splice_time);
            p_splice_time = p_next;
        }
        p_cmd->p_splice_time = NULL;
        free(p_cmd);
    }
}

/*****************************************************************************
 * dvbpsi_sis_cmd_time_signal_decode
 *****************************************************************************
 * time_signal command decoder.
 *****************************************************************************/
static dvbpsi_sis_cmd_time_signal_t *
    dvbpsi_sis_cmd_time_signal_decode(uint8_t *p_data, uint16_t i_length)
{
    dvbpsi_sis_cmd_time_signal_t *p_cmd = calloc(1, sizeof(dvbpsi_sis_cmd_time_signal_t));
    if (!p_cmd) return NULL;
    
    if (i_length < 1)
    {
        cmd_time_signal_cleanup(p_cmd);
        return NULL;
    }

    bool b_time_specified = false;
    uint64_t i_pts_time = 0;
    if ((p_data[0] & 0x80) == 0x80)
    {
        if (i_length < 5)
        {
            cmd_time_signal_cleanup(p_cmd);
            return NULL;
        }

        b_time_specified = true;
        i_pts_time = ((((uint64_t)p_data[0] & 0x01) << 32) |
                       ((uint64_t)p_data[1] << 24) |
                       ((uint64_t)p_data[2] << 16) |
                       ((uint64_t)p_data[3] << 8) |
                        (uint64_t)p_data[4]);
    }
    
    p_cmd->p_splice_time = calloc(1, sizeof(dvbpsi_sis_splice_time_t));
    if (p_cmd->p_splice_time == NULL)
    {
        cmd_time_signal_cleanup(p_cmd);
        return NULL;
    }

    p_cmd->p_splice_time->b_time_specified_flag = b_time_specified;
    p_cmd->p_splice_time->i_pts_time = i_pts_time;
    p_cmd->p_splice_time->p_next = NULL;

    return p_cmd;
}

/*****************************************************************************
 * dvbpsi_sis_sections_decode
 *****************************************************************************
 * SIS decoder.
 *****************************************************************************/
void dvbpsi_sis_sections_decode(dvbpsi_t* p_dvbpsi, dvbpsi_sis_t* p_sis,
                              dvbpsi_psi_section_t* p_section)
{
    uint8_t *p_byte, *p_end;

    while (p_section)
    {
        for (p_byte = p_section->p_payload_start;
             p_byte + 17 <= p_section->p_payload_end; )
        {
            p_sis->i_protocol_version = p_byte[0];
            p_sis->b_encrypted_packet = ((p_byte[1] & 0x80) == 0x80);
            /* NOTE: cannot handle encrypted packet */
            assert(!p_sis->b_encrypted_packet);
            if (p_sis->b_encrypted_packet) {
                dvbpsi_error(p_dvbpsi, "SIS decoder", "cannot handle encrypted packets");
                break;
            }
            p_sis->i_encryption_algorithm = ((p_byte[1] & 0x7E) >> 1);
            p_sis->i_pts_adjustment = ((((uint64_t)p_byte[1] & 0x01) << 32) |
                                        ((uint64_t)p_byte[2] << 24) |
                                        ((uint64_t)p_byte[3] << 16) |
                                        ((uint64_t)p_byte[4] << 8)  |
                                         (uint64_t)p_byte[5]);
            p_sis->cw_index = p_byte[6];
            p_sis->i_tier = (p_byte[7] << 4) | (p_byte[8] >> 4);
            p_sis->i_splice_command_length = ((p_byte[8] & 0x0F) << 8) | p_byte[9];
            p_sis->i_splice_command_type = p_byte[10];

            if ((p_byte + 11 + p_sis->i_splice_command_length) >= p_section->p_payload_end) {
                dvbpsi_error(p_dvbpsi, "SIS decoder", "corrupt section data");
                break;
            }

            /* FIXME: handle splice_command_sections */
            switch(p_sis->i_splice_command_type)
            {
                case 0x00: /* splice_null */
                    p_sis->p_splice_command = NULL;
                    assert(p_sis->i_splice_command_length == 0);
                    break;
                case 0x04: /* splice_schedule */
                    p_sis->p_splice_command =
                            dvbpsi_sis_cmd_splice_schedule_decode(&p_byte[11],
                                                p_sis->i_splice_command_length);
                    if (!p_sis->p_splice_command)
                        dvbpsi_error(p_dvbpsi, "SIS decoder",
                                     "splice schedule command is invalid");
                    break;
                case 0x05: /* splice_insert */
                    p_sis->p_splice_command =
                            dvbpsi_sis_cmd_splice_insert_decode(&p_byte[11],
                            p_sis->i_splice_command_length);
                    if (!p_sis->p_splice_command)
                        dvbpsi_error(p_dvbpsi, "SIS decoder",
                                    "splice insert command is invalid");
                    break;
                case 0x06: /* time_signal */
                    p_sis->p_splice_command =
                            dvbpsi_sis_cmd_time_signal_decode(&p_byte[11],
                            p_sis->i_splice_command_length);
                    if (!p_sis->p_splice_command)
                        dvbpsi_error(p_dvbpsi, "SIS decoder",
                                     "time_signal command is invalid");
                    break;
                case 0x07: /* bandwidth_reservation */
                    break;
                default:
                    dvbpsi_error(p_dvbpsi, "SIS decoder", "invalid SIS Command found");
                    break;
            }

            /* Service descriptors */
            uint8_t *p_desc = p_byte + 11 + p_sis->i_splice_command_length;
            /* check our boundaries */
            if (p_desc + 2 >= p_section->p_payload_end)
                break;
            p_sis->i_descriptors_length = (p_desc[0] << 8) | p_desc[1];

            p_desc += 2;
            p_end = p_desc + p_sis->i_descriptors_length;
            if (p_end > p_section->p_payload_end) break;

            while (p_desc + 2 < p_end)
            {
                uint8_t i_tag = p_desc[0];
                uint8_t i_length = p_desc[1];
                if ((i_length <= 254) &&
                    (i_length + 2 < p_end - p_desc))
                    dvbpsi_sis_descriptor_add(p_sis, i_tag, i_length, p_desc + 2);
                p_desc += 2 + i_length;
            }

            if (p_sis->b_encrypted_packet)
            {
                /* FIXME: Currently ignored */
                /* Calculate crc32 over decoded
                 * p_sis->i_splice_command_type till p_sis->i_ecrc,
                 * the result should be exactly p_sis->i_ecrc and indicates
                 * a successfull decryption.
                 */
                /* check our boundaries */
                if (p_desc + 4 >= p_section->p_payload_end)
                    break;
                p_desc += 4; /* E CRC 32 */
            }

            /* check our boundaries */
            if (p_desc + 4 >= p_section->p_payload_end)
                break;
            /* point to next section */
            p_byte = p_desc + 4 /* CRC 32 */;
        }

        p_section = p_section->p_next;
    }
}

/*****************************************************************************
 * Generate SIS tables
 *****************************************************************************/
static uint32_t dvbpsi_sis_generate_utc_splice_time(uint8_t *p_data, const uint32_t i_time)
{
    p_data[0] = (i_time >> 24);
    p_data[1] = (i_time >> 16);
    p_data[2] = (i_time >> 8);
    p_data[3] = (i_time & 0xff);
    return 4;
}

static uint32_t dvbpsi_sis_generate_splice_time(uint8_t *p_data, const dvbpsi_sis_splice_time_t *p_splice_time)
{
    uint32_t i_pos = 1;
    p_data[0] = p_splice_time->b_time_specified_flag ? 0x80 : 0x00;
    if (p_splice_time->b_time_specified_flag)
    {
        p_data[0] |= ((p_splice_time->i_pts_time >> 32) & 0x01);
        p_data[1] = (p_splice_time->i_pts_time >> 24);
        p_data[2] = (p_splice_time->i_pts_time >> 16);
        p_data[3] = (p_splice_time->i_pts_time >> 8);
        p_data[4] = (p_splice_time->i_pts_time & 0xff);
        i_pos += 4;
    }
    return i_pos;
}

static uint32_t dvbpsi_sis_generate_break_duration(uint8_t *p_data, const dvbpsi_sis_break_duration_t *p_break_duration)
{
    p_data[0] = p_break_duration->b_auto_return ? 0x80 : 0x00;
    p_data[0] |= ((p_break_duration->i_duration >> 32) & 0x01);
    p_data[1] = (p_break_duration->i_duration >> 24);
    p_data[2] = (p_break_duration->i_duration >> 16);
    p_data[3] = (p_break_duration->i_duration >> 8);
    p_data[4] = (p_break_duration->i_duration & 0xff);
    return 5;
}

static uint32_t dvbpsi_sis_generate_splice_cmd_schedule(uint8_t *p_data,
                                   const dvbpsi_sis_cmd_splice_schedule_t *p_schedule)
{
    uint32_t i_pos = 0, i_event = 0;
    p_data[i_pos] = p_schedule->i_splice_count;
    i_pos++;
    dvbpsi_sis_splice_event_t *p_event = p_schedule->p_splice_event;
    while (p_event)
    {
        p_data[i_pos + 0] = (p_event->i_splice_event_id >> 24);
        p_data[i_pos + 1] = (p_event->i_splice_event_id >> 16);
        p_data[i_pos + 2] = (p_event->i_splice_event_id >> 8);
        p_data[i_pos + 3] = (p_event->i_splice_event_id);
        p_data[i_pos + 4] = p_event->b_splice_event_cancel_indicator ? 0x80 : 0x00;
        i_pos += 5;

        if (!p_event->b_splice_event_cancel_indicator)
        {
            p_data[i_pos] = p_event->b_out_of_network_indicator ? 0x80 : 0x00;
            p_data[i_pos] |= p_event->b_program_splice_flag ? 0x40 : 0x00;
            p_data[i_pos] |= p_event->b_duration_flag ? 0x20 : 0x00;
            i_pos++;
            if (p_event->b_program_splice_flag)
                i_pos += dvbpsi_sis_generate_utc_splice_time(p_data + i_pos, p_event->i_utc_splice_time);
            else
            {
                /* component loop */
                p_data[i_pos] = p_event->i_component_count;
                i_pos++;

                int count = p_event->i_component_count;
                dvbpsi_sis_component_t *p_comp = p_event->p_component;
                while (p_comp && count > 0)
                {
                    p_data[i_pos] = p_comp->i_tag;
                    i_pos++;
                    i_pos += dvbpsi_sis_generate_utc_splice_time(p_data + i_pos, p_comp->i_utc_splice_time);
                    p_comp = p_comp->p_next;
                    count--;
                }
                assert(count == 0);
                assert(p_comp == NULL);
            }

            if (p_event->b_duration_flag)
                i_pos += dvbpsi_sis_generate_break_duration(p_data + i_pos, &p_event->i_break_duration);

            p_data[i_pos + 0] = (p_event->i_unique_program_id >> 8);
            p_data[i_pos + 1] = (p_event->i_unique_program_id & 0x00ff);
            p_data[i_pos + 2] = p_event->i_avail_num;
            p_data[i_pos + 3] = p_event->i_avails_expected;
            i_pos += 4;
        }
        p_event = p_event->p_next;
        i_event++;
    }
    assert(i_event == p_schedule->i_splice_count);
    return i_pos;
}

static uint32_t dvbpsi_sis_generate_splice_cmd_insert(uint8_t *p_data,
                                                      const dvbpsi_sis_cmd_splice_insert_t *p_insert)
{
    p_data[0] = (p_insert->i_splice_event_id >> 24);
    p_data[1] = (p_insert->i_splice_event_id >> 16);
    p_data[2] = (p_insert->i_splice_event_id >> 8);
    p_data[3] = (p_insert->i_splice_event_id & 0x000000ff);
    p_data[4] = p_insert->b_splice_event_cancel_indicator ? 0x01 : 0x00;
    if (p_insert->b_splice_event_cancel_indicator)
        return 5;

    p_data[5] = p_insert->b_out_of_network_indicator ? 0x80 : 0x00;
    p_data[5] |= p_insert->b_program_splice_flag ? 0x40 : 0x00;
    p_data[5] |= p_insert->b_duration_flag ? 0x20 : 0x00;
    p_data[5] |= p_insert->b_splice_immediate_flag ? 0x10 : 0x00;

    uint8_t i_pos = 6;
    if (p_insert->b_program_splice_flag && !p_insert->b_splice_immediate_flag)
    {
        i_pos += dvbpsi_sis_generate_splice_time(p_data + i_pos, &p_insert->i_splice_time);
    }
    if (!p_insert->b_program_splice_flag)
    {
        p_data[i_pos] = p_insert->i_component_count;
        i_pos++;

        int count = p_insert->i_component_count;
        dvbpsi_sis_component_splice_time_t *p_comp = p_insert->p_splice_time;
        while (p_comp && count > 0)
        {
            p_data[i_pos] = p_comp->i_component_tag;
            i_pos++;
            i_pos += dvbpsi_sis_generate_splice_time(p_data + i_pos, &p_comp->i_splice_time);
            p_comp = p_comp->p_next;
            count--;
        }
        assert(count == 0);
        assert(p_comp == NULL);
    }
    if (p_insert->b_duration_flag)
    {
        i_pos += dvbpsi_sis_generate_break_duration(p_data + i_pos, &p_insert->i_break_duration);
    }
    p_data[i_pos + 0] = (p_insert->i_unique_program_id >> 8);
    p_data[i_pos + 1] = (p_insert->i_unique_program_id & 0x00ff);
    p_data[i_pos + 2] = p_insert->i_avail_num;
    p_data[i_pos + 3] = p_insert->i_avails_expected;
    i_pos += 4;
    return i_pos;
}

static uint32_t dvbpsi_sis_generate_splice_cmd_time_signal(uint8_t *p_data,
                                                           const dvbpsi_sis_cmd_time_signal_t *p_signal)
{
    return dvbpsi_sis_generate_splice_time(p_data, p_signal->p_splice_time);
}

static uint32_t dvbpsi_sis_generate_splice_cmd_bandwidth_reservation(uint8_t *p_data,
                                   const dvbpsi_sis_cmd_bandwidth_reservation_t *p_bandwidth)
{
    return 0;
}

/*****************************************************************************
 * dvbpsi_sis_sections_generate
 *****************************************************************************
 * Generate SIS sections based on the dvbpsi_sis_t structure.
 *****************************************************************************/
dvbpsi_psi_section_t *dvbpsi_sis_sections_generate(dvbpsi_t *p_dvbpsi, dvbpsi_sis_t* p_sis)
{
    dvbpsi_psi_section_t * p_current = dvbpsi_NewPSISection(1024);
    if (!p_current)
        return NULL;

    p_current->i_table_id = 0xFC;
    p_current->b_syntax_indicator = false;
    p_current->b_private_indicator = false;
    p_current->i_length = 3;                     /* header + CRC_32 */

    /* FIXME: looks weird */
    p_current->p_payload_end += 2;               /* just after the header */
    p_current->p_payload_start = p_current->p_data + 3;

    p_current->p_data[3] = p_sis->i_protocol_version;
    p_current->p_data[4] = p_sis->b_encrypted_packet ? 0x80 : 0x0;
    /* NOTE: cannot handle encrypted packet */
    assert(p_sis->b_encrypted_packet);
    p_current->p_data[4] |= ((p_sis->i_encryption_algorithm << 1) & 0x7E);

    p_current->p_data[4] |= ((p_sis->i_pts_adjustment >> 32) & 0x01);
    p_current->p_data[5] = (p_sis->i_pts_adjustment >> 24);
    p_current->p_data[6] = (p_sis->i_pts_adjustment >> 16);
    p_current->p_data[7] = (p_sis->i_pts_adjustment >> 8);
    p_current->p_data[8] =  p_sis->i_pts_adjustment;

    p_current->p_data[9]  = p_sis->cw_index;
    p_current->p_data[11] = 0x00;
    p_current->p_data[11]|= ((p_sis->i_splice_command_length >> 8) & 0x0F);
    p_current->p_data[12] = (uint8_t) (p_sis->i_splice_command_length & 0xFF);
    p_current->p_data[13] = p_sis->i_splice_command_type;

    uint32_t i_desc_start = 13 + p_sis->i_splice_command_length;
    i_desc_start++; /* descriptor loop starts here */

    assert(p_sis->i_splice_command_length <= 0xfff);
    if (p_sis->i_splice_command_length > 0xfff)
        p_sis->i_splice_command_length = 0xfff; /* truncate */

    /* TODO: FIXME: Handle splice_command_sections */
    uint32_t i_cmd_start = 14;
    uint32_t i_pos = 0;
    switch(p_sis->i_splice_command_type)
    {
        case 0x00: /* splice_null */
            assert(p_sis->i_splice_command_length == 0);
            break;
        case 0x04: /* splice_schedule */
            i_pos = dvbpsi_sis_generate_splice_cmd_schedule(p_current->p_data + i_cmd_start,
                                        (dvbpsi_sis_cmd_splice_schedule_t *)p_sis->p_splice_command);
            break;
        case 0x05: /* splice_insert */
            i_pos = dvbpsi_sis_generate_splice_cmd_insert(p_current->p_data + i_cmd_start,
                                        (dvbpsi_sis_cmd_splice_insert_t *)p_sis->p_splice_command);
            break;
        case 0x06: /* time_signal */
            i_pos = dvbpsi_sis_generate_splice_cmd_time_signal(p_current->p_data + i_cmd_start,
                                        (dvbpsi_sis_cmd_time_signal_t *) p_sis->p_splice_command);
            break;
        case 0x07: /* bandwidth_reservation */
            i_pos = dvbpsi_sis_generate_splice_cmd_bandwidth_reservation(p_current->p_data + i_cmd_start,
                                        (dvbpsi_sis_cmd_bandwidth_reservation_t *) p_sis->p_splice_command);

            break;
        default:
            dvbpsi_error(p_dvbpsi, "SIS decoder", "invalid SIS Command found");
            break;
    }

    if (i_pos > 0)
        i_cmd_start += i_pos;

    /* The splice command may not overrun the start of the descriptor loop */
    assert(i_cmd_start < i_desc_start);

    /* Service descriptors */
    p_current->p_data[i_desc_start] = (p_sis->i_descriptors_length >> 8);
    p_current->p_data[i_desc_start+1] = (uint8_t)(p_sis->i_descriptors_length & 0xFF);

    p_current->p_payload_end += (i_desc_start + 1);

    uint32_t i_desc_length = 0;

    dvbpsi_descriptor_t * p_descriptor = p_sis->p_first_descriptor;
    while ((p_descriptor != NULL) && (p_current->i_length <= 1018))
    {
        i_desc_length += p_descriptor->i_length + 2;
        p_descriptor = p_descriptor->p_next;

        /* p_payload_end is where the descriptor begins */
        p_current->p_payload_end[0] = p_descriptor->i_tag;
        p_current->p_payload_end[1] = p_descriptor->i_length;
        memcpy(p_current->p_payload_end + 2, p_descriptor->p_data, p_descriptor->i_length);
        /* Increase length by descriptor_length + 2 */
        p_current->p_payload_end += p_descriptor->i_length + 2;
        p_current->i_length += p_descriptor->i_length + 2;

    }
    /* Coding error if this condition is not met */
    assert( i_desc_length == p_sis->i_descriptors_length);

    /* Finalization */
    dvbpsi_BuildPSISection(p_dvbpsi, p_current);
    return p_current;
}
