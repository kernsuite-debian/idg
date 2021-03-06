// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <complex>

#include <stdlib.h>
#include <stdint.h>

#include "Types.h"
#include "Index.h"

extern "C" {
void kernel_splitter(const long nr_subgrids, const long grid_size,
                     const int subgrid_size, const idg::Metadata* metadata,
                     idg::float2* subgrid, const idg::float2* grid) {
  // Precompute phaosr
  float phasor_real[subgrid_size][subgrid_size];
  float phasor_imag[subgrid_size][subgrid_size];

#pragma omp parallel for collapse(2)
  for (int y = 0; y < subgrid_size; y++) {
    for (int x = 0; x < subgrid_size; x++) {
      float phase = -M_PI * (x + y - subgrid_size) / subgrid_size;
      phasor_real[y][x] = cosf(phase);
      phasor_imag[y][x] = sinf(phase);
    }
  }

#pragma omp parallel for
  for (int s = 0; s < nr_subgrids; s++) {
    // Load subgrid coordinates
    int subgrid_x = metadata[s].coordinate.x;
    int subgrid_y = metadata[s].coordinate.y;
    int subgrid_w = metadata[s].coordinate.z;

    // Mirror grid for negative w-values
    bool negative_w = subgrid_w < 0;

    // Determine polarization index
    const int index_pol_default[NR_POLARIZATIONS] = {0, 1, 2, 3};
    const int index_pol_transposed[NR_POLARIZATIONS] = {0, 2, 1, 3};
    int* index_pol =
        (int*)(subgrid_w < 0 ? index_pol_default : index_pol_transposed);

    for (int y = 0; y < subgrid_size; y++) {
      for (int x = 0; x < subgrid_size; x++) {
        // Compute position in subgrid
        int x_dst = (x + (subgrid_size / 2)) % subgrid_size;
        int y_dst = (y + (subgrid_size / 2)) % subgrid_size;

        // Compute position in grid
        int x_src = negative_w ? grid_size - subgrid_x - x : subgrid_x + x;
        int y_src = negative_w ? grid_size - subgrid_y - y : subgrid_y + y;

        // Check whether subgrid fits in grid
        if (!(subgrid_x >= 0 && subgrid_x < grid_size - subgrid_size - 1 &&
              subgrid_y >= 0 && subgrid_y < grid_size - subgrid_size - 1))
          continue;

        // Load phasor
        idg::float2 phasor = {phasor_real[y][x], phasor_imag[y][x]};

        // Set grid value to subgrid
        for (int pol = 0; pol < NR_POLARIZATIONS; pol++) {
          int pol_src = index_pol[pol];
          long src_idx =
              index_grid(grid_size, subgrid_w, pol_src, y_src, x_src);
          long dst_idx = index_subgrid(subgrid_size, s, pol, y_dst, x_dst);
          idg::float2 value = grid[src_idx];
          value = negative_w ? conj(value) : value;
          subgrid[dst_idx] = phasor * value;
        }  // end for pol
      }    // end for x
    }      // end for y
  }        // end for s
}  // end kernel_splitter
}  // end extern "C"
