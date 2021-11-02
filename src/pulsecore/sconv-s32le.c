/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

/* Despite the name of this file we implement S32 and S24 handling here, too. */

#include <inttypes.h>
#include <stdio.h>
#include <math.h>

#include <pulsecore/sconv.h>
#include <pulsecore/macro.h>
#include <pulsecore/endianmacros.h>

#include "sconv-s32le.h"

#ifndef INT16_FROM
#define INT16_FROM PA_INT16_FROM_LE
#endif
#ifndef UINT16_FROM
#define UINT16_FROM PA_UINT16_FROM_LE
#endif

#ifndef INT16_TO
#define INT16_TO PA_INT16_TO_LE
#endif
#ifndef UINT16_TO
#define UINT16_TO PA_UINT16_TO_LE
#endif

#ifndef INT32_FROM
#define INT32_FROM PA_INT32_FROM_LE
#endif
#ifndef UINT32_FROM
#define UINT32_FROM PA_UINT32_FROM_LE
#endif

#ifndef INT32_TO
#define INT32_TO PA_INT32_TO_LE
#endif
#ifndef UINT32_TO
#define UINT32_TO PA_UINT32_TO_LE
#endif

#ifndef READ24
#define READ24 PA_READ24LE
#endif
#ifndef WRITE24
#define WRITE24 PA_WRITE24LE
#endif

#ifndef SWAP_WORDS
#ifdef WORDS_BIGENDIAN
#define SWAP_WORDS 1
#else
#define SWAP_WORDS 0
#endif
#endif

void pa_sconv_s16le_from_s32ne(unsigned n, const int32_t *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = INT16_TO(*a >> 16);
        a++;
        b++;
    }
}

void pa_sconv_s16be_from_s32ne(unsigned n, const int32_t *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = PA_INT16_TO_BE(*a >> 16);
        a++;
        b++;
    }
}

void pa_sconv_s24le_from_s32ne(unsigned n, const int32_t *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        WRITE24(b, ((uint32_t) *a) >> 8);
        a++;
        b += 3;
    }
}

void pa_sconv_s24be_from_s32ne(unsigned n, const int32_t *a, uint8_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        PA_WRITE24BE(b, ((uint32_t) *a) >> 8);
        a++;
        b += 3;
    }
}

void pa_sconv_s24_32le_from_s32ne(unsigned n, const int32_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = INT32_TO(((uint32_t) *a) >> 8);
        a++;
        b++;
    }
}

void pa_sconv_s24_32be_from_s32ne(unsigned n, const int32_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = PA_INT32_TO_BE(((uint32_t) *a) << 8);
        a++;
        b++;
    }
}

void pa_sconv_s32le_from_s32ne(unsigned n, const int32_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = INT32_TO(*a);
        a++;
        b++;
    }
}

void pa_sconv_s32be_from_s32ne(unsigned n, const int32_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = PA_INT32_TO_BE(*a);
        a++;
        b++;
    }
}

void pa_sconv_s16le_to_s32ne(unsigned n, const int16_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = UINT32_TO((uint32_t) ((int32_t) *a << 16));
        a++;
        b++;
    }
}

void pa_sconv_s16be_to_s32ne(unsigned n, const int16_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = PA_UINT32_TO_BE((uint32_t) *a);
        a++;
        b++;
    }
}

void pa_sconv_s24le_to_s32ne(unsigned n, const uint8_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = (int32_t) (READ24(a) << 8);
        a += 3;
        b++;
    }
}

void pa_sconv_s24be_to_s32ne(unsigned n, const uint8_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = (int32_t) (PA_READ24BE(a) << 8);
        a += 3;
        b++;
    }
}

void pa_sconv_s24_32le_to_s32ne(unsigned n, const int32_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = UINT32_TO((uint32_t) ((int32_t) *a << 8));
        a++;
        b++;
    }
}

void pa_sconv_s24_32be_to_s32ne(unsigned n, const int32_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = PA_UINT32_TO_BE((uint32_t) (*a >> 8));
        a++;
        b++;
    }
}

void pa_sconv_s32le_to_s32ne(unsigned n, const int32_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = UINT32_TO((uint32_t) (*a));
        a++;
        b++;
    }
}

void pa_sconv_s32be_to_s32ne(unsigned n, const int32_t *a, uint32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = PA_UINT32_TO_BE((uint32_t) (*a));
        a++;
        b++;
    }
}
