// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <complex>

#include "common/Types.h"
#include "common/Index.h"

namespace idg {
namespace kernel {
namespace cpu {
namespace optimized {

void kernel_splitter_wstack(const int nr_subgrids, const int nr_polarizations,
                            const long grid_size, const int subgrid_size,
                            const idg::Metadata* metadata,
                            std::complex<float>* subgrid,
                            const std::complex<float>* grid) {
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
    // Load position in grid
    int subgrid_x = metadata[s].coordinate.x;
    int subgrid_y = metadata[s].coordinate.y;
    int subgrid_w = metadata[s].coordinate.z;

    // Mirror w-layer for negative w-values
    bool negative_w = subgrid_w < 0;
    int w_layer = negative_w ? -subgrid_w - 1 : subgrid_w;

    // Determine polarization index
    const int index_pol_default[nr_polarizations] = {0, 1, 2, 3};
    const int index_pol_transposed[nr_polarizations] = {0, 2, 1, 3};
    int* index_pol =
        (int*)(negative_w ? index_pol_default : index_pol_transposed);

    for (int y = 0; y < subgrid_size; y++) {
      for (int x = 0; x < subgrid_size; x++) {
        // Compute position in subgrid
        int x_dst = (x + (subgrid_size / 2)) % subgrid_size;
        int y_dst = (y + (subgrid_size / 2)) % subgrid_size;

        // Compute position in grid
        int x_src = negative_w ? grid_size - subgrid_x - x : subgrid_x + x;
        int y_src = negative_w ? grid_size - subgrid_y - y : subgrid_y + y;

        // Check whether subgrid fits in grid
        if (subgrid_x >= 1 && subgrid_x < grid_size - subgrid_size &&
            subgrid_y >= 1 && subgrid_y < grid_size - subgrid_size) {
          // Load phasor
          std::complex<float> phasor = {phasor_real[y][x], phasor_imag[y][x]};

          // Set grid value to subgrid
          for (int pol = 0; pol < nr_polarizations; pol++) {
            int pol_src = index_pol[pol];
            long src_idx = index_grid_4d(nr_polarizations, grid_size, w_layer,
                                         pol_src, y_src, x_src);
            long dst_idx = index_subgrid(nr_polarizations, subgrid_size, s, pol,
                                         y_dst, x_dst);
            std::complex<float> value = grid[src_idx];
            value = negative_w ? conj(value) : value;
            subgrid[dst_idx] = phasor * value;
          }  // end for pol
        }    // end if fit
      }      // end for x
    }        // end for y
  }          // end for s
}  // end kernel_splitter_wstack

}  // end namespace optimized
}  // end namespace cpu
}  // end namespace kernel
}  // end namespace idg