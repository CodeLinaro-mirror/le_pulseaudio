/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <pulsecore/g711.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>

#include <pulsecore/sconv-s32le.h>

#include "sconv2.h"

static void u8_to_s32ne(unsigned n, const uint8_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--, a++, b++)
        *b = (((int32_t)*a) - 128) << 24;
}

static void u8_from_s32ne(unsigned n, const int32_t *a, uint8_t *b) {

    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--, a++, b++)
        *b = (uint8_t) ((uint32_t) *a >> 24) + (uint8_t) 0x80U;
}

static void ulaw_to_s32ne(unsigned n, const uint8_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--, a++, b++)
        *b = st_ulaw2linear16(*a) << 16;
}

static void ulaw_from_s32ne(unsigned n, const int32_t *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--, a++, b++)
        *b = st_14linear2ulaw((int16_t)(*a >> (2+16)));
}

static void alaw_to_s32ne(unsigned n, const int8_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--, a++, b++)
        *b = st_alaw2linear16((uint8_t) *a) << 16;
}

static void alaw_from_s32ne(unsigned n, const int32_t *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--, a++, b++)
        *b = st_13linear2alaw((int16_t)(*a >> (3+16)));
}

static pa_convert_func_t to_s32ne_table[] = {
    [PA_SAMPLE_U8]        = (pa_convert_func_t) u8_to_s32ne,
    [PA_SAMPLE_S16NE]     = (pa_convert_func_t) pa_sconv_s16le_to_s32ne,
    [PA_SAMPLE_S16RE]     = (pa_convert_func_t) pa_sconv_s16be_to_s32ne,
    [PA_SAMPLE_FLOAT32BE] = (pa_convert_func_t) pa_sconv_float32be_to_s32ne,
    [PA_SAMPLE_FLOAT32LE] = (pa_convert_func_t) pa_sconv_float32le_to_s32ne,
    [PA_SAMPLE_S32BE]     = (pa_convert_func_t) pa_sconv_s32be_to_s32ne,
    [PA_SAMPLE_S32LE]     = (pa_convert_func_t) pa_sconv_s32le_to_s32ne,
    [PA_SAMPLE_S24BE]     = (pa_convert_func_t) pa_sconv_s24be_to_s32ne,
    [PA_SAMPLE_S24LE]     = (pa_convert_func_t) pa_sconv_s24le_to_s32ne,
    [PA_SAMPLE_S24_32BE]  = (pa_convert_func_t) pa_sconv_s24_32be_to_s32ne,
    [PA_SAMPLE_S24_32LE]  = (pa_convert_func_t) pa_sconv_s24_32le_to_s32ne,
    [PA_SAMPLE_ALAW]      = (pa_convert_func_t) alaw_to_s32ne,
    [PA_SAMPLE_ULAW]      = (pa_convert_func_t) ulaw_to_s32ne
};

pa_convert_func_t pa_get_convert_to_s32ne_function(pa_sample_format_t f) {
	pa_assert(pa_sample_format_valid(f));

    return to_s32ne_table[f];
}

void pa_set_convert_to_s32ne_function(pa_sample_format_t f, pa_convert_func_t func) {
    pa_assert(pa_sample_format_valid(f));

	to_s32ne_table[f] = func;
}

static pa_convert_func_t from_s32ne_table[] = {
    [PA_SAMPLE_U8]        = (pa_convert_func_t) u8_from_s32ne,
    [PA_SAMPLE_S16NE]     = (pa_convert_func_t) pa_sconv_s16le_from_s32ne,
    [PA_SAMPLE_S16RE]     = (pa_convert_func_t) pa_sconv_s16be_from_s32ne,
    [PA_SAMPLE_FLOAT32BE] = (pa_convert_func_t) pa_sconv_float32be_from_s32ne,
    [PA_SAMPLE_FLOAT32LE] = (pa_convert_func_t) pa_sconv_float32le_from_s32ne,
    [PA_SAMPLE_S32BE]     = (pa_convert_func_t) pa_sconv_s32be_from_s32ne,
    [PA_SAMPLE_S32LE]     = (pa_convert_func_t) pa_sconv_s32le_from_s32ne,
    [PA_SAMPLE_S24BE]     = (pa_convert_func_t) pa_sconv_s24be_from_s32ne,
    [PA_SAMPLE_S24LE]     = (pa_convert_func_t) pa_sconv_s24le_from_s32ne,
    [PA_SAMPLE_S24_32BE]  = (pa_convert_func_t) pa_sconv_s24_32be_from_s32ne,
    [PA_SAMPLE_S24_32LE]  = (pa_convert_func_t) pa_sconv_s24_32le_from_s32ne,
    [PA_SAMPLE_ALAW]      = (pa_convert_func_t) alaw_from_s32ne,
    [PA_SAMPLE_ULAW]      = (pa_convert_func_t) ulaw_from_s32ne,
};

pa_convert_func_t pa_get_convert_from_s32ne_function(pa_sample_format_t f) {
    pa_assert(pa_sample_format_valid(f));

    return from_s32ne_table[f];
}

void pa_set_convert_from_s32ne_function(pa_sample_format_t f, pa_convert_func_t func) {
	pa_assert(pa_sample_format_valid(f));

    from_s32ne_table[f] = func;
}
