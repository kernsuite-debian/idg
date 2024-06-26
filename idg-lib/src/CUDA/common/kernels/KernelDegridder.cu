// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Types.h"
#include "math.cu"
#include "KernelDegridder.cuh"

#define ALIGN(N,A) (((N)+(A)-1)/(A)*(A))


/**
 * Tuning parameters:
 *  - BLOCK_SIZE_X
 *  - NUM_BLOCKS
 *
 * This kernel is tuned for these
 *  architectures (__CUDA_ARCH__):
 *  - Ampere (800, 860)
 *  - Turing (750)
 *  - Volta (700)
 *  - Pascal (610)
 *  - Maxwell (520)
 *  - Kepler (350)
 *
 *  Derived parameters:
 *  - NUM_THREADS = BLOCK_SIZE_X
 *    -> the thread block is 1D
 *  - BATCH_SIZE = NUM_THREADS
 *    -> setting BATCH_SIZE as a multiple of
 *       NUM_THREADS does not improve performance
**/
#ifndef BLOCK_SIZE_X
#define BLOCK_SIZE_X KernelDegridder::block_size_x
#endif
#define NUM_THREADS BLOCK_SIZE_X

#ifndef NUM_BLOCKS
#if __CUDA_ARCH__ == 800
#define NUM_BLOCKS 5
#elif __CUDA_ARCH__ == 520 || \
      __CUDA_ARCH__ == 750 || \
      __CUDA_ARCH__ == 700
#define NUM_BLOCKS 6
#else // 350, 610, 800
#define NUM_BLOCKS 0
#endif
#endif

#ifndef BATCH_SIZE
#define BATCH_SIZE NUM_THREADS
#endif


/*
    Shared memory
*/
__shared__ float4 shared[3][BATCH_SIZE];

/*
    Kernels
*/
__device__ void prepare_shared(
    const int                  current_nr_pixels,
    const int                  pixel_offset,
    const int                  nr_polarizations,
    const int                  grid_size,
    const int                  subgrid_size,
    const float                image_size,
    const float                w_step,
    const float                shift_l,
    const float                shift_m,
    const int                  nr_stations,
    const int                  aterm_idx,
    const Metadata&            metadata,
    const float*  taper,
    const float2* aterms,
    const float2* subgrid)
{
    int s           = blockIdx.x;
    int num_threads = blockDim.x;
    int tid         = threadIdx.x;

    // Load metadata for current subgrid
    const int x_coordinate = metadata.coordinate.x;
    const int y_coordinate = metadata.coordinate.y;
    const int station1 = metadata.baseline.station1;
    const int station2 = metadata.baseline.station2;

    // Compute u,v,w offset in wavelenghts
    const float u_offset = (x_coordinate + subgrid_size/2 - grid_size/2) / image_size * 2 * M_PI;
    const float v_offset = (y_coordinate + subgrid_size/2 - grid_size/2) / image_size * 2 * M_PI;
    const float w_offset = w_step * ((float) metadata.coordinate.z + 0.5) * 2 * M_PI;

    for (int j = tid; j < current_nr_pixels; j += num_threads) {
        int y = (pixel_offset + j) / subgrid_size;
        int x = (pixel_offset + j) % subgrid_size;

        // Compute shifted position in subgrid
        int x_src = (x + (subgrid_size/2)) % subgrid_size;
        int y_src = (y + (subgrid_size/2)) % subgrid_size;

        // Load taper
        const float taper_ = taper[y * subgrid_size + x];

        // Load pixels
        float2 pixel[4];
        if (nr_polarizations == 4) {
            for (unsigned pol = 0; pol < nr_polarizations; pol++) {
                unsigned int pixel_idx = index_subgrid(nr_polarizations, subgrid_size, s, pol, y_src, x_src);
                pixel[pol] = subgrid[pixel_idx] * taper_;
            }
        } else if (nr_polarizations == 1) {
            unsigned int pixel_idx = index_subgrid(nr_polarizations, subgrid_size, s, 0, y_src, x_src);
            pixel[0] = subgrid[pixel_idx] * taper_;
            pixel[1] = make_float2(0, 0);
            pixel[2] = make_float2(0, 0);
            pixel[3] = subgrid[pixel_idx] * taper_;
        }

        // Apply aterm
        int station1_idx = index_aterm(subgrid_size, 4, nr_stations, aterm_idx, station1, y, x, 0);
        int station2_idx = index_aterm(subgrid_size, 4, nr_stations, aterm_idx, station2, y, x, 0);
        float2 *aterm1 = (float2 *) &aterms[station1_idx];
        float2 *aterm2 = (float2 *) &aterms[station2_idx];
        apply_aterm_degridder(pixel, aterm1, aterm2);

        // Store pixels in shared memory
        shared[0][j] = make_float4(pixel[0].x, pixel[0].y, pixel[1].x, pixel[1].y);
        shared[1][j] = make_float4(pixel[2].x, pixel[2].y, pixel[3].x, pixel[3].y);

        // Compute l,m,n for phase offset and phase index
        const float l_offset = compute_l(x, subgrid_size, image_size);
        const float m_offset = compute_m(y, subgrid_size, image_size);
        const float l_index = l_offset + shift_l;
        const float m_index = m_offset - shift_m;
        const float n = compute_n(l_index, m_index);
        const float phase_offset = -(u_offset*l_offset + v_offset*m_offset + w_offset*n);

        // Store l_index,m_index,n and phase offset in shared memory
        shared[2][j] = make_float4(l_index, m_index, n, phase_offset);
    } // end for j (pixels)
}

template<int unroll_channels>
__device__ void compute_visibility(
    const int     nr_polarizations,
    const int     current_nr_pixels,
    const int     channel_offset,
    const float   u,
    const float   v,
    const float   w,
    const float*  wavenumbers,
          float2* visibility)
{
    for (int i = 0; i < current_nr_pixels; i++) {
        const int unroll_pixels = 1;

        // Compute phase_offset and phase index
        float phase_offset[unroll_pixels];
        float phase_index[unroll_pixels];

        for (int j = 0; j < unroll_pixels; j++) {
            const float l_index = shared[2][i].x;
            const float m_index = shared[2][i].y;
            const float n = shared[2][i].z;
            phase_offset[j] = shared[2][i].w;
            phase_index[j] = u * l_index + v * m_index + w * n;
        }

        // Compute visibility
        #if USE_EXTRAPOLATE
        compute_reduction_extrapolate<unroll_pixels>(
            unroll_channels, nr_polarizations,
            wavenumbers + channel_offset, phase_index, phase_offset,
            &shared[0][i], &shared[1][i], visibility, 0, 0);
        #else
        compute_reduction(
            unroll_channels, unroll_pixels, nr_polarizations,
            wavenumbers + channel_offset, phase_index, phase_offset,
            &shared[0][i], &shared[1][i], visibility, 0, 0);
        #endif
    } // end for k (batch)
}

template<int unroll_channels>
__device__ void store_visibility(
    const int nr_polarizations,
    const int subgrid_size,
    const int nr_channels,
    const int channel_offset,
    const int time,
    const float2  visibility[unroll_channels][4],
          float2* visibilities)
{
    for (int chan = 0; chan < unroll_channels; chan++) {
        // Store visibility
        const float scale = 1.0f / (subgrid_size * subgrid_size);
        int idx_time = time;
        int idx_chan = channel_offset + chan;
        if (nr_polarizations == 4) {
            size_t idx_vis = index_visibility(4, nr_channels, idx_time, idx_chan, 0);
            float4 visA = make_float4(visibility[chan][0].x, visibility[chan][0].y, visibility[chan][1].x, visibility[chan][1].y);
            float4 visB = make_float4(visibility[chan][2].x, visibility[chan][2].y, visibility[chan][3].x, visibility[chan][3].y);
            float4 *vis_ptr = (float4 *) &visibilities[idx_vis];
            vis_ptr[0] = visA * scale;
            vis_ptr[1] = visB * scale;
        } else if (nr_polarizations == 1) {
            size_t idx_vis = index_visibility(2, nr_channels, idx_time, idx_chan, 0);
            float4 vis = make_float4(visibility[chan][0].x, visibility[chan][0].y, visibility[chan][3].x, visibility[chan][3].y);
            float4 *vis_ptr = (float4 *) &visibilities[idx_vis];
            *vis_ptr = vis * scale;
        }
    } // end for chan
}

template<int unroll_channels>
__device__ void update_visibility(
    const int nr_polarizations,
    const int subgrid_size,
    const int nr_channels,
    const int channel_offset,
    const int time,
    const float2  visibility[unroll_channels][4],
          float2* visibilities)
{
    for (int chan = 0; chan < unroll_channels; chan++) {
        // Store visibility
        const float scale = 1.0f / (subgrid_size * subgrid_size);
        int idx_time = time;
        int idx_chan = channel_offset + chan;
        if (nr_polarizations == 4) {
            int idx_vis = index_visibility(4, nr_channels, idx_time, idx_chan, 0);
            float4 visA = make_float4(visibility[chan][0].x, visibility[chan][0].y, visibility[chan][1].x, visibility[chan][1].y);
            float4 visB = make_float4(visibility[chan][2].x, visibility[chan][2].y, visibility[chan][3].x, visibility[chan][3].y);
            float4 *vis_ptr = (float4 *) &visibilities[idx_vis];
            vis_ptr[0] += visA * scale;
            vis_ptr[1] += visB * scale;
        } else if (nr_polarizations == 1) {
            int idx_vis = index_visibility(2, nr_channels, idx_time, idx_chan, 0);
            float4 vis = make_float4(visibility[chan][0].x, visibility[chan][0].y, visibility[chan][3].x, visibility[chan][3].y);
            float4 *vis_ptr = (float4 *) &visibilities[idx_vis];
            *vis_ptr += vis * scale;
        }
    } // end for chan
}

template<int unroll_channels>
__device__ void kernel_degridder_tp(
    const int                         time_offset_job,
    const int                         nr_polarizations,
    const int                         grid_size,
    const int                         subgrid_size,
    const float                       image_size,
    const float                       w_step,
    const float                       shift_l,
    const float                       shift_m,
    const int                         nr_channels,
    const int                         current_nr_channels,
    const int                         channel_offset,
    const int                         nr_stations,
    const UVW<float>*    __restrict__ uvw,
    const float*         __restrict__ wavenumbers,
          float2*        __restrict__ visibilities,
    const float*         __restrict__ taper,
    const float2*        __restrict__ aterms,
    const unsigned int*  __restrict__ aterm_indices,
    const Metadata*      __restrict__ metadata,
          float2*        __restrict__ subgrid)
{
    int s           = blockIdx.x;
    int num_threads = blockDim.x;
    int tid         = threadIdx.x;

    // Load metadata for current subgrid
    const Metadata &m = metadata[s];
    const int time_offset_global = m.time_index - time_offset_job;
    const int nr_timesteps = m.nr_timesteps;

    // Determine the number of visibilities are computed in parallel in the frequency dimension
    int nr_channels_parallel = current_nr_channels / unroll_channels;

    // Iterate timesteps
    int current_nr_timesteps = 0;
    for (int time_offset_local = 0; time_offset_local < nr_timesteps; time_offset_local += current_nr_timesteps) {
        const unsigned int aterm_idx = aterm_indices[time_offset_global + time_offset_local];

        // Determine number of timesteps to process
        current_nr_timesteps = 0;
        for (int time = time_offset_local; time < nr_timesteps; time++) {
            if (aterm_indices[time_offset_global + time] == aterm_idx) {
                current_nr_timesteps++;
            } else {
                break;
            }
        }

        // Determine the first and last timestep to process
        int time_start = time_offset_global + time_offset_local;
        int time_end = time_start + current_nr_timesteps;

        for (int i = tid; i < ALIGN(current_nr_timesteps * nr_channels_parallel, num_threads); i += num_threads) {
            int time = time_start + (i / nr_channels_parallel);
            int channel_offset_local = (i % nr_channels_parallel) * unroll_channels;
            int channel = channel_offset + channel_offset_local;

            float2 visibility[unroll_channels][4];

            for (int chan = 0; chan < unroll_channels; chan++) {
                for (int pol = 0; pol < 4; pol++) {
                    visibility[chan][pol] = make_float2(0, 0);
                }
            }

            float u, v, w;

            if (time < time_end) {
                u = uvw[time].u;
                v = uvw[time].v;
                w = uvw[time].w;
            }

            __syncthreads();

            // Iterate pixels
            const int nr_pixels = subgrid_size * subgrid_size;
            int current_nr_pixels = BATCH_SIZE;
            for (int pixel_offset = 0; pixel_offset < nr_pixels; pixel_offset += current_nr_pixels) {
                current_nr_pixels = nr_pixels - pixel_offset < min(num_threads, BATCH_SIZE) ?
                                    nr_pixels - pixel_offset : min(num_threads, BATCH_SIZE);

                __syncthreads();

                // Prepare data
                prepare_shared(
                    current_nr_pixels, pixel_offset, nr_polarizations, grid_size,
                    subgrid_size, image_size, w_step, shift_l, shift_m, nr_stations,
                    aterm_idx, m, taper, aterms, subgrid);

                __syncthreads();

                // Compute visibility
                compute_visibility<unroll_channels>(
                    nr_polarizations, current_nr_pixels, channel,
                    u, v, w, wavenumbers, reinterpret_cast<float2*>(visibility));
            } // end for pixel_offset

            if (time < time_end) {
                update_visibility<unroll_channels>(
                    nr_polarizations, subgrid_size, nr_channels,
                    channel, time, visibility, visibilities);
            }
        } // end for time
    } // end for time_offset_local
} // end kernel_degridder_tp

template<int unroll_channels>
__device__ void kernel_degridder_pt(
    const int           time_offset_job,
    const int           nr_polarizations,
    const int           grid_size,
    const int           subgrid_size,
    const float         image_size,
    const float         w_step,
    const float         shift_l,
    const float         shift_m,
    const int           nr_channels,
    const int           current_nr_channels,
    const int           channel_offset,
    const int           nr_stations,
    const UVW<float>*   uvw,
    const float*        wavenumbers,
          float2*       visibilities,
    const float*        taper,
    const float2*       aterms,
    const unsigned int* aterm_indices,
    const Metadata*     metadata,
          float2*       subgrid)
{
    int s           = blockIdx.x;
    int num_threads = blockDim.x;
    int tid         = threadIdx.x;

    // Load metadata for current subgrid
    const Metadata &m = metadata[s];
    const int time_offset_global = m.time_index - time_offset_job;
    const int nr_timesteps = m.nr_timesteps;

    // Determine the number of visibilities are computed in parallel in the frequency dimension
    int nr_channels_parallel = current_nr_channels / unroll_channels;

    // Iterate pixels
    const int nr_pixels = subgrid_size * subgrid_size;
    int current_nr_pixels = BATCH_SIZE;
    for (int pixel_offset = 0; pixel_offset < nr_pixels; pixel_offset += current_nr_pixels) {
        current_nr_pixels = nr_pixels - pixel_offset < min(num_threads, BATCH_SIZE) ?
                            nr_pixels - pixel_offset : min(num_threads, BATCH_SIZE);

        // Iterate timesteps
        int current_nr_timesteps = 0;
        for (int time_offset_local = 0; time_offset_local < nr_timesteps; time_offset_local += current_nr_timesteps) {
            const unsigned int aterm_idx = aterm_indices[time_offset_global + time_offset_local];

            // Determine number of timesteps to process
            current_nr_timesteps = 0;
            for (int time = time_offset_local; time < nr_timesteps; time++) {
                if (aterm_indices[time_offset_global + time] == aterm_idx) {
                    current_nr_timesteps++;
                } else {
                    break;
                }
            }

            __syncthreads();

            // Prepare data
            prepare_shared(
                current_nr_pixels, pixel_offset, nr_polarizations, grid_size,
                subgrid_size, image_size, w_step, shift_l, shift_m, nr_stations,
                aterm_idx, m, taper, aterms, subgrid);

            __syncthreads();

            // Determine the first and last timestep to process
            int time_start = time_offset_global + time_offset_local;
            int time_end = time_start + current_nr_timesteps;

            for (int i = tid; i < ALIGN(current_nr_timesteps * nr_channels_parallel, num_threads); i += num_threads) {
                int time = time_start + (i / nr_channels_parallel);
                int channel_offset_local = (i % nr_channels_parallel) * unroll_channels;
                int channel = channel_offset + channel_offset_local;

                float2 visibility[unroll_channels][4];

                for (int pol = 0; pol < 4; pol++) {
                    for (int chan = 0; chan < unroll_channels; chan++) {
                        visibility[chan][pol] = make_float2(0, 0);
                    }
                }

                float u, v, w;

                if (time < time_end) {
                    u = uvw[time].u;
                    v = uvw[time].v;
                    w = uvw[time].w;
                }

                // Compute visibility
                compute_visibility<unroll_channels>(
                    nr_polarizations, current_nr_pixels, channel,
                    u, v, w, wavenumbers, reinterpret_cast<float2*>(visibility));

                // Update visibility
                if (time < time_end) {
                    update_visibility<unroll_channels>(
                        nr_polarizations, subgrid_size, nr_channels,
                        channel, time, visibility, visibilities);
                }
            } // end for time
        } // end for time_offset_local
    } // end for pixel_offset
} // end kernel_degridder_pt

#define LOAD_METADATA \
    int s                   = blockIdx.x; \
    const Metadata &m       = metadata[s]; \
    const int nr_timesteps  = m.nr_timesteps; \
    const int channel_begin = m.channel_begin; \
    const int channel_end   = m.channel_end; \
    const int nr_aterms     = m.nr_aterms;

#define KERNEL_DEGRIDDER(current_nr_channels) \
    if ((nr_timesteps / nr_aterms) >= num_threads) { \
        for (; (channel_offset + current_nr_channels) <= channel_end; channel_offset += current_nr_channels) { \
            kernel_degridder_tp<current_nr_channels>( \
                time_offset, nr_polarizations, grid_size, subgrid_size, image_size, w_step, \
                shift_l, shift_m, nr_channels, current_nr_channels, channel_offset, nr_stations, \
                uvw, wavenumbers, visibilities, taper, aterms, aterm_indices, metadata, subgrid); \
        } \
    } else { \
        for (; (channel_offset + current_nr_channels) <= channel_end; channel_offset += current_nr_channels) { \
            kernel_degridder_pt<1>( \
                time_offset, nr_polarizations, grid_size, subgrid_size, image_size, w_step, \
                shift_l, shift_m, nr_channels, current_nr_channels, channel_offset, nr_stations, \
                uvw, wavenumbers, visibilities, taper, aterms, aterm_indices, metadata, subgrid); \
        } \
    }

extern "C" {

__global__
#if NUM_BLOCKS > 0
__launch_bounds__(NUM_THREADS, NUM_BLOCKS)
#endif
void kernel_degridder(
    const int                        time_offset,
    const int                        nr_polarizations,
    const int                        grid_size,
    const int                        subgrid_size,
    const float                      image_size,
    const float                      w_step,
    const float                      shift_l,
    const float                      shift_m,
    const int                        nr_channels,
    const int                        nr_stations,
    const UVW<float>*   __restrict__ uvw,
    const float*        __restrict__ wavenumbers,
          float2*       __restrict__ visibilities,
    const float*        __restrict__ taper,
    const float2*       __restrict__ aterms,
    const unsigned int* __restrict__ aterm_indices,
    const Metadata*     __restrict__ metadata,
          float2*       __restrict__ subgrid)
{
    int s                   = blockIdx.x;
    int num_threads         = blockDim.x;
    const Metadata &m       = metadata[s];
    const int channel_begin = m.channel_begin;
    const int channel_end   = m.channel_end;
    const int nr_timesteps  = m.nr_timesteps;
    const int nr_aterms     = m.nr_aterms;

    int channel_offset = channel_begin;
    KERNEL_DEGRIDDER(8)
    KERNEL_DEGRIDDER(7)
    KERNEL_DEGRIDDER(6)
    KERNEL_DEGRIDDER(5)
    KERNEL_DEGRIDDER(4)
    KERNEL_DEGRIDDER(3)
    KERNEL_DEGRIDDER(2)
    KERNEL_DEGRIDDER(1)
}
} // end extern "C"
