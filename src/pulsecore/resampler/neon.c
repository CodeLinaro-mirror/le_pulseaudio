/***
  Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; version 2.1.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>

#include <pulse/timeval.h>
#include <pulsecore/resampler.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/ltdl-helper.h>

#define NEON_RESAMPLER_LIB "libneon_resampler.so"

static unsigned neon_prop_resample(pa_resampler *r, const pa_memchunk *input,
        unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames);
static void neon_prop_update_rates(pa_resampler *r);
static void neon_prop_reset(pa_resampler *r);
static void neon_prop_free(pa_resampler *r);
static int create_neon_src_instance(pa_resampler *r);
static int neon_pre_init_check(const uint32_t rate_a, const uint32_t rate_b);
static int neon_prop_dl_load();

size_t (*memalloc_wrapper)(int, int, int32_t, int32_t);
int (*neon_init_wrapper)(int16_t *, int32_t, int32_t, int32_t, int32_t, int32_t,int32_t);
void (*resample90dB_wrapper)(int16_t *, void *, void *, size_t, size_t);
size_t (*get_num_in_samp_wrapper)(int16_t *, size_t);
void (*reinit_dynamic_resamp_wrapper)(int16_t *, int32_t, int32_t);
static lt_dlhandle dl;

struct neon_src_data {
    int16_t *state;
    pa_sample_spec w_i_ss;
    pa_sample_spec w_o_ss;
    pa_usec_t duration;
    size_t duration_bytes;
    void *deinterleave_stream;
};

static unsigned neon_prop_resample(pa_resampler *r, const pa_memchunk *input,
        unsigned in_n_frames, pa_memchunk *output, unsigned *out_n_frames) {
    void *in, *out;
    struct neon_src_data *src_data;
    size_t processed_in_frames, actual_out_frames;
    size_t pending_bytes = 0;
    size_t in_offset = 0;
    size_t out_offset = 0;
    pa_usec_t pending_duration, exec_time;
    struct timeval now;
    bool done = false;

    pa_assert(r);
    pa_assert(input);
    pa_assert(output);
    pa_assert(out_n_frames);

    pa_gettimeofday(&now);

    src_data = (struct neon_src_data *) r->impl.data;

    pa_assert(src_data->state);
    pa_assert(src_data->deinterleave_stream);

    in = pa_memblock_acquire_chunk(input);
    out = pa_memblock_acquire_chunk(output);
    pending_bytes = in_n_frames * r->w_fz;
    processed_in_frames = 0;
    actual_out_frames = 0;
    while (!done) {
        size_t src_in_frames = 0;
        size_t src_out_frames = 0;

        if (pending_bytes > src_data->duration_bytes) {
            pending_duration = src_data->duration;
        } else {
            pending_duration = pa_bytes_to_usec(pending_bytes, &src_data->w_i_ss);
            done = true;
        }

        src_out_frames = pa_usec_to_bytes(pending_duration, &src_data->w_o_ss) / r->w_fz;
        src_in_frames = (*get_num_in_samp_wrapper)(src_data->state, src_out_frames);

        if (!src_in_frames)
            break;

        pa_deinterleave_stream((uint8_t *)in + in_offset,
                               src_data->deinterleave_stream,
                               r->i_ss.channels,
                               pa_sample_size_of_format(r->work_format),
                               src_in_frames);
        (*resample90dB_wrapper)(src_data->state, src_data->deinterleave_stream,
                              (uint8_t *)out + out_offset, src_in_frames, src_out_frames);

        in_offset += (src_in_frames * r->w_fz);
        out_offset += (src_out_frames * r->w_fz);

        pending_bytes -= (src_in_frames * r->w_fz);
        processed_in_frames += src_in_frames;
        actual_out_frames += src_out_frames;
    }

    pa_memblock_release(input->memblock);
    pa_memblock_release(output->memblock);

#if 0
    pa_log("a_i_f: %u p_i_f: %u e_o_f: %u a_o_f: %u left: %u p_b : %u",
            in_n_frames, processed_in_frames, *out_n_frames, actual_out_frames,
            in_n_frames - processed_in_frames, pending_bytes);
#endif
    exec_time = pa_timeval_age(&now);
#if 0
    pa_log("Execution time : %llu", exec_time);
#endif

    *out_n_frames = actual_out_frames;
    return in_n_frames - processed_in_frames;
}

static void neon_prop_update_rates(pa_resampler *r) {
    pa_log("NEON SRC: neon_prop_update_rates");
    // No update rates API - so need to recreate the src instance
    neon_prop_reset(r);
}

static void neon_prop_reset(pa_resampler *r) {
    pa_log("NEON SRC: neon_prop_reset");
    neon_prop_free(r);
    create_neon_src_instance(r);
}

static void neon_prop_free(pa_resampler *r) {
    struct neon_src_data *src_data;
    pa_log("NEON SRC: neon_prop_free");
    pa_assert(r);

    if (!r->impl.data)
        return;

    src_data = (struct neon_src_data *) r->impl.data;
    pa_xfree(src_data->deinterleave_stream);
    pa_xfree(src_data->state);
    pa_xfree(src_data);

    r->impl.data = NULL;
}

static int create_neon_src_instance(pa_resampler *r) {
    int in_bit_depth;
    size_t state_size;
    struct neon_src_data *src_data;
    int ret = -1;

    pa_assert(r);

    pa_log("NEON SRC: create_neon_src_instance");
    in_bit_depth = (int)pa_sample_size(&r->i_ss) * 8;
    state_size = (*memalloc_wrapper)(in_bit_depth, r->i_ss.channels, r->i_ss.rate, r->o_ss.rate);

    src_data = pa_xnew0(struct neon_src_data, 1);

    // SRC state
    src_data->state = pa_xnew0(int16_t, state_size);

    // SRC in and out sample spec
    src_data->w_i_ss.format = r->work_format;
    src_data->w_i_ss.rate = r->i_ss.rate;
    src_data->w_i_ss.channels = r->i_ss.channels;
    src_data->w_o_ss.format = r->work_format;
    src_data->w_o_ss.rate = r->o_ss.rate;
    src_data->w_o_ss.channels = r->work_channels;

    // SRC each resample duration
    src_data->duration = PA_USEC_PER_MSEC;
    src_data->duration_bytes = pa_usec_to_bytes(PA_USEC_PER_MSEC, &src_data->w_i_ss);
    /*
     Note: allocate one extra frame bytes
     to avoid buffer overflow in case of src_in_frames round off to higher limit
    */
    src_data->deinterleave_stream = pa_xmalloc0(src_data->duration_bytes + pa_frame_size(&src_data->w_i_ss));

    r->impl.resample = neon_prop_resample;
    r->impl.update_rates = neon_prop_update_rates;
    r->impl.reset = neon_prop_reset;
    r->impl.free = neon_prop_free;
    r->impl.data = src_data;

    ret = (*neon_init_wrapper)(src_data->state, r->i_ss.channels, r->i_ss.rate, r->o_ss.rate, 0, 0, 0);
    return ret;
}

int pa_resampler_neon_prop_init(pa_resampler *r) {
    int ret = -1;
    pa_log("NEON SRC: pa_resampler_neon_prop_init");
    ret = create_neon_src_instance(r);
    return ret-1;
}

static int neon_prop_dl_load() {
    int ret = 0;
    dl = lt_dlopenext(NEON_RESAMPLER_LIB);
    if(dl == NULL) {
        pa_log_error("Failed to open Neon resampler library");
        ret = -1;
        goto finish;
    }

    if (!(memalloc_wrapper = (size_t(*)(int , int , int32_t , int32_t ))
                            pa_load_sym(dl, NULL, "MemAllocWrapper"))) {
        pa_log_error("Failed to load symbol 'MemAllocWrapper'");
        ret = -2;
        goto finish;
    }

    if (!(neon_init_wrapper = (int32_t(*)(int16_t * , int32_t , int32_t , int32_t , int32_t , int32_t , int32_t ))
                            pa_load_sym(dl, NULL, "InitWrapper"))) {
        pa_log_error("Failed to load symbol 'InitWrapper'");
        ret = -2;
        goto finish;
    }

    if (!(resample90dB_wrapper = (void (*)(int16_t * , void * , void * , size_t , size_t ))
                            pa_load_sym(dl, NULL, "Resample90dBWrapper"))) {
        pa_log_error("Failed to load symbol 'Resample90dBWrapper'");
        ret = -2;
        goto finish;
    }

    if (!(get_num_in_samp_wrapper = (size_t(*)(int16_t *, size_t))
                            pa_load_sym(dl, NULL, "GetNumInSampWrapper"))) {
        pa_log_error("Failed to load symbol 'GetNumInSampWrapper'");
        ret = -2;
        goto finish;
    }
    finish:
    if(ret < 0 && dl != NULL)
        lt_dlclose(dl);
    return ret;
}

static int neon_pre_init_check(const uint32_t rate_a, const uint32_t rate_b) {
    enum {
      INVALID = -5,
      INTERM_RATE_INVALID = -2,
      DOWNRATE_UNSUPPORTED = -1,
      UPSAMPLER_FAIL = 0
    };
    int ret = INVALID;
    size_t state_size = 0;
    uint8_t dummy_bit_depth = 8;
    uint8_t dummy_channels = 2;
    int16_t *state = NULL;

    state_size = (*memalloc_wrapper)(dummy_bit_depth, dummy_channels, rate_a, rate_b);
    state = pa_xnew0(int16_t, state_size);
    /*
    Init returns below values
    -2         : Intermediate sampling rate is greater than MAX_FREQ
    -1         : If inFreq > MAX_FREQ or inFreq < outFreq or the down-
                sampling rate is not suppoted
    0          : Fail to initialize up-sampler.
    1          : outFreq == inFreq;
    +ve int    : intermediate sample rate (if sampling rates are supported)
    */
   if(state) {
       ret = (*neon_init_wrapper)(state, dummy_channels, rate_a, rate_b, 0, 0, 0);
       pa_xfree(state);
       switch(ret) {
        case INTERM_RATE_INVALID:
            pa_log_error("Intermediate sampling rate is greater than MAX_FREQ");
            break;
        case DOWNRATE_UNSUPPORTED:
            pa_log_error("Downsampling rate not supported");
            break;
        case UPSAMPLER_FAIL:
            pa_log_error("Fail to initialize up-sampler");
            break;
        default:
            pa_log_info("Init returned %d",ret);
            break;
       }
    }
    return ret-1;
}

bool pa_neon_prop_supported(
        pa_resample_flags_t flags,
        const uint32_t rate_a,
        const uint32_t rate_b) {

    // TODO - check with system team if it support variable rate streams or not
    if (flags & PA_RESAMPLER_VARIABLE_RATE) {
        pa_log_info("Neon resampler cannot do variable rate");
        return false;
    }

    if (neon_prop_dl_load() < 0) {
        pa_log_warn("Problem loading Neon shared library file "NEON_RESAMPLER_LIB);
        return false;
    }

    if(neon_pre_init_check(rate_a, rate_b) < 0) {
        pa_log_warn("Neon resampler pre init check failed");
        return false;
    }

    return true;
}
