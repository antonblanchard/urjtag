/*
 * $Id$
 *
 * Copyright (C) 2002 ETC s.r.o.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by Marcel Telka <marcel@telka.sk>, 2002.
 *
 */

#include <sysdep.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#include <urjtag/cmd.h>

#include <urjtag/bsdl.h>

#include <urjtag/tap_register.h>
#include <urjtag/tap.h>
#include <urjtag/cable.h>
#include <urjtag/part.h>
#include <urjtag/chain.h>
#include <urjtag/part.h>
#include <urjtag/bus.h>
#include <urjtag/data_register.h>
#include <urjtag/parse.h>
#include <urjtag/jtag.h>

struct id_record
{
    char name[20];
    char fullname[100];
};

static int
find_record (char *filename, urj_tap_register_t *key, struct id_record *idr)
{
    FILE *file;
    urj_tap_register_t *tr;
    int r = 0;

    file = fopen (filename, "r");
    if (!file)
    {
        urj_error_IO_set ("Cannot open '%s'", filename);
        return 0;
    }

    tr = urj_tap_register_alloc (key->len);

    for (;;)
    {
        char *p;
        char *s;
        char line[1024];

        if (fgets (line, 1024, file) == NULL)
            break;

        /* remove comment and nl from the line */
        p = strpbrk (line, "#\n");
        if (p)
            *p = '\0';

        p = line;

        /* skip whitespace */
        while (*p && isspace (*p))
            p++;

        /* remove ending whitespace */
        s = strchr (p, '\0');
        while (s != p)
        {
            if (!isspace (*--s))
                break;
            *s = '\0';
        }

        /* line is empty? */
        if (!*p)
            continue;

        /* find end of field */
        s = p;
        while (*s && !isspace (*s))
            s++;
        if (*s)
            *s++ = '\0';

        /* test field length */
        if (strlen (p) != key->len)
            continue;

        /* match */
        urj_tap_register_init (tr, p);
        if (urj_tap_register_compare (tr, key))
            continue;

        /* next field */
        p = s;

        /* skip whitespace */
        while (*p && isspace (*p))
            p++;

        /* line is empty? */
        if (!*p)
            continue;

        /* find end of field */
        s = p;
        while (*s && !isspace (*s))
            s++;
        if (*s)
            *s++ = '\0';

        /* test field length */
        if (strlen (p) >= sizeof idr->name)
            continue;

        /* copy name */
        strcpy (idr->name, p);

        /* next field */
        p = s;

        /* skip whitespace */
        while (*p && isspace (*p))
            p++;

        /* line is empty? */
        if (!*p)
            continue;

        /* test field length */
        if (strlen (p) >= sizeof idr->fullname)
            continue;

        /* copy fullname */
        strcpy (idr->fullname, p);

        r = 1;
        break;
    }

    fclose (file);

    urj_tap_register_free (tr);

    return r;
}

int
urj_tap_detect_parts (urj_chain_t *chain, const char *db_path)
{
    int irlen;
    urj_tap_register_t *ir;
    int chlen;
    urj_tap_register_t *one;
    urj_tap_register_t *ones;
    urj_tap_register_t *br;
    urj_tap_register_t *id;
    urj_parts_t *ps;
    int i;

    char data_path[1024];
    char manufacturer[URJ_PART_MANUFACTURER_MAXLEN + 1];
    char partname[URJ_PART_PART_MAXLEN + 1];
    char stepping[URJ_PART_STEPPING_MAXLEN + 1];

    /* Detect IR length */
    urj_tap_reset (chain);
    urj_tap_capture_ir (chain);
    irlen = urj_tap_detect_register_size (chain);
    if (irlen < 1)
        // retain error state
        return -1;

    urj_log (URJ_LOG_LEVEL_NORMAL, _("IR length: %d\n"), irlen);
    chain->total_instr_len = irlen;

    /* Allocate IR */
    ir = urj_tap_register_fill (urj_tap_register_alloc (irlen), 1);
    if (ir == NULL)
        return -1;

    urj_tap_shift_register (chain, ir, NULL, URJ_CHAIN_EXITMODE_IDLE);
    urj_tap_register_free (ir);

    /* Detect chain length */
    urj_tap_capture_dr (chain);
    chlen = urj_tap_detect_register_size (chain);
    if (chlen < 1)
    {
        // retain error state
        urj_log (URJ_LOG_LEVEL_NORMAL,
                 _("Unable to detect JTAG chain length\n"));
        return -1;
    }
    urj_log (URJ_LOG_LEVEL_NORMAL, _("Chain length: %d\n"), chlen);

    /* Allocate registers and parts */
    one = urj_tap_register_fill (urj_tap_register_alloc (1), 1);
    ones = urj_tap_register_fill (urj_tap_register_alloc (31), 1);
    br = urj_tap_register_alloc (1);
    id = urj_tap_register_alloc (32);
    ps = urj_part_parts_alloc ();
    if (!one || !ones || !br || !id || !ps)
    {
        urj_tap_register_free (one);
        urj_tap_register_free (ones);
        urj_tap_register_free (br);
        urj_tap_register_free (id);
        urj_part_parts_free (ps);
        // retain error state
        return -1;
    }
    chain->parts = ps;
    chain->active_part = 0;

    /* Detect parts */
    urj_tap_reset (chain);
    urj_tap_capture_dr (chain);

    for (i = 0; i < chlen; i++)
    {
        urj_part_t *part;
        urj_tap_register_t *did = br;   /* detected id (length is 1 or 32) */
        urj_tap_register_t *key;
        struct id_record idr;
        char *p;

        urj_tap_shift_register (chain, one, br, URJ_CHAIN_EXITMODE_SHIFT);
        if (urj_tap_register_compare (one, br) == 0)
        {
            /* part with id */
            urj_tap_shift_register (chain, ones, id,
                                    URJ_CHAIN_EXITMODE_SHIFT);
            urj_tap_register_shift_left (id, 1);
            id->data[0] = 1;
            did = id;

            urj_log (URJ_LOG_LEVEL_NORMAL, _("Device Id: %s (0x%016" PRIX64 ")\n"),
                     urj_tap_register_get_string (did), urj_tap_register_get_value (did));
        } else
            urj_log (URJ_LOG_LEVEL_NORMAL, _("Device Id: unknown as bit 0 was not a 1\n"));

        part = urj_part_alloc (did);
        if (part == NULL)
            // @@@@ RFHH what about this error? Shouldn't we bail out?
            break;
        urj_part_parts_add_part (ps, part);

        if (did == br)
            continue;

        chain->active_part = ps->len - 1;

#ifdef ENABLE_BSDL
        if (urj_bsdl_scan_files (chain, urj_tap_register_get_string (did),
                                 URJ_BSDL_MODE_DETECT) <= 0)
        {
#endif

            /* find JTAG declarations for a part with id */

            data_path[0] = '\0';
            strncat (data_path, db_path, sizeof data_path);        /* FIXME: Buffer overrun */

            /* manufacturers */
            strncat (data_path, "/MANUFACTURERS", sizeof data_path);

            key = urj_tap_register_alloc (11);
            memcpy (key->data, &id->data[1], key->len);
            if (!find_record (data_path, key, &idr))
            {
                urj_log (URJ_LOG_LEVEL_NORMAL, _("  Unknown manufacturer!\n"));
                urj_tap_register_free (key);
                continue;
            }
            urj_tap_register_free (key);

            urj_log (URJ_LOG_LEVEL_NORMAL, _("  Manufacturer: %s\n"),
                     idr.fullname);
            if (strlen (idr.fullname) > URJ_PART_MANUFACTURER_MAXLEN)
                urj_warning (_("Manufacturer too long\n"));
            manufacturer[0] = '\0';
            strncat (manufacturer, idr.fullname, sizeof manufacturer);

            /* parts */
            p = strrchr (data_path, '/');
            if (p)
                p[1] = '\0';
            else
                data_path[0] = '\0';
            strncat (data_path, idr.name, sizeof data_path);
            strncat (data_path, "/PARTS", sizeof data_path);

            key = urj_tap_register_alloc (16);
            memcpy (key->data, &id->data[12], key->len);
            if (!find_record (data_path, key, &idr))
            {
                urj_log (URJ_LOG_LEVEL_NORMAL, _("  Unknown part!\n"));
                urj_tap_register_free (key);
                continue;
            }
            urj_tap_register_free (key);

            urj_log (URJ_LOG_LEVEL_NORMAL, _("  Part(%d):         %s\n"),
                     chain->active_part, idr.fullname);
            if (strlen (idr.fullname) > URJ_PART_PART_MAXLEN)
                urj_warning (_("Part too long\n"));
            partname[0] ='\0';
            strncat (partname, idr.fullname, sizeof partname);

            /* steppings */
            p = strrchr (data_path, '/');
            if (p)
                p[1] = '\0';
            else
                data_path[0] = '\0';
            strncat (data_path, idr.name, sizeof data_path);
            strncat (data_path, "/STEPPINGS", sizeof data_path);

            key = urj_tap_register_alloc (4);
            memcpy (key->data, &id->data[28], key->len);
            if (!find_record (data_path, key, &idr))
            {
                urj_log (URJ_LOG_LEVEL_NORMAL, _("  Unknown stepping!\n"));
                urj_tap_register_free (key);
                continue;
            }
            urj_tap_register_free (key);

            urj_log (URJ_LOG_LEVEL_NORMAL, _("  Stepping:     %s\n"),
                     idr.fullname);
            if (strlen (idr.fullname) > URJ_PART_STEPPING_MAXLEN)
                urj_warning (_("Stepping too long\n"));
            stepping[0] = '\0';
            strncat (stepping, idr.fullname, sizeof stepping);

            /* part definition file */
            p = strrchr (data_path, '/');
            if (p)
                p[1] = '\0';
            else
                data_path[0] = '\0';
            strncat (data_path, idr.name, sizeof data_path);

            urj_log (URJ_LOG_LEVEL_NORMAL, _("  Filename:     %s\n"),
                     data_path);

            /* run JTAG declarations */
            strcpy (part->manufacturer, manufacturer);
            strcpy (part->part, partname);
            strcpy (part->stepping, stepping);
            if (urj_parse_include (chain, data_path, 0) == URJ_STATUS_FAIL)
            {
                urj_log (URJ_LOG_LEVEL_NORMAL, "Error: %s\n",
                         urj_error_describe());
                urj_error_reset();
            }
#ifdef ENABLE_BSDL
        }
#endif

        if (part->active_instruction == NULL)
            part->active_instruction = urj_part_find_instruction (part,
                                                                  "IDCODE");
    }

    for (i = 0; i < 32; i++)
    {
        urj_tap_shift_register (chain, one, br, URJ_CHAIN_EXITMODE_SHIFT);
        if (urj_tap_register_compare (one, br) != 0)
        {
            urj_log (URJ_LOG_LEVEL_NORMAL,
                     _("Error: Unable to detect JTAG chain end!\n"));
            break;
        }
    }
    urj_tap_shift_register (chain, one, NULL, URJ_CHAIN_EXITMODE_IDLE);

    urj_tap_register_free (one);
    urj_tap_register_free (ones);
    urj_tap_register_free (br);
    urj_tap_register_free (id);

    return ps->len;
}


/* In case we do not want to detect, we can add parts manually */

int
urj_tap_manual_add (urj_chain_t *chain, int instr_len)
{
    urj_tap_register_t *id;
    urj_part_t *part;
    char *str;
    urj_part_instruction_t *bypass;

    id = urj_tap_register_alloc (1);
    if (id == NULL)
        return -1;

    /* if there are no parts, create the parts list */
    if (chain->parts == NULL)
    {
        chain->parts = urj_part_parts_alloc ();
        if (chain->parts == NULL)
            return -1;
    }

    part = urj_part_alloc (id);
    if (part == NULL)
        return -1;

    strncpy (part->part, "unknown", URJ_PART_PART_MAXLEN);
    part->instruction_length = instr_len;

    urj_part_parts_add_part (chain->parts, part);
    chain->active_part = chain->parts->len - 1;

    /* make the BR register available */
    if (urj_part_data_register_define (part, "BR", 1) != URJ_STATUS_OK)
    {
        urj_log (URJ_LOG_LEVEL_NORMAL, _("Error: could not set BR register"));
        return -1;
    }

    /* create a string of 1's for BYPASS instruction */
    str = calloc (instr_len + 1, sizeof (char));
    if (str == NULL)
    {
        urj_error_set (URJ_ERROR_OUT_OF_MEMORY, "calloc(%zd,%zd) fails",
                       (size_t) instr_len + 1, sizeof (char));
        return -1;
    }

    memset (str, '1', instr_len);
    str[instr_len] = '\0';
    bypass = urj_part_instruction_define (part, "BYPASS", str, "BR");
    free (str);

    if (bypass == NULL)
    {
        urj_log (URJ_LOG_LEVEL_NORMAL,
                 _("Error: could not set BYPASS instruction"));
        // retain error state
        return -1;
    }

    /* update total instruction register length of chain */
    chain->total_instr_len += instr_len;

    return chain->parts->len;
}

int
urj_tap_detect (urj_chain_t *chain)
{
    int i;
    urj_bus_t *abus;

    urj_bus_buses_free ();
    urj_part_parts_free (chain->parts);
    chain->parts = NULL;
    if (urj_tap_detect_parts (chain, urj_get_data_dir ()) == -1)
        // retain error state
        return URJ_STATUS_FAIL;
    if (!chain->parts)
    {
        urj_error_set (URJ_ERROR_INVALID, "chain has no parts");
        return URJ_STATUS_FAIL;
    }
    if (!chain->parts->len)
    {
        urj_part_parts_free (chain->parts);
        chain->parts = NULL;
        urj_error_set (URJ_ERROR_INVALID, "chain has empty parts list");
        return URJ_STATUS_FAIL;
    }

    urj_part_parts_set_instruction (chain->parts, "SAMPLE/PRELOAD");
    urj_tap_chain_shift_instructions (chain);
    urj_tap_chain_shift_data_registers (chain, 1);
    urj_part_parts_set_instruction (chain->parts, "BYPASS");
    urj_tap_chain_shift_instructions (chain);

    // Initialize all the buses
    for (i = 0; i < urj_buses.len; i++)
    {
        abus = urj_buses.buses[i];
        if (abus->driver->init)
        {
            if (abus->driver->init (abus) != URJ_STATUS_OK)
                // retain error state
                return URJ_STATUS_FAIL;
        }
    }

    return URJ_STATUS_OK;
}
