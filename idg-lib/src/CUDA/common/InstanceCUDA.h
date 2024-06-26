// Copyright (C) 2023 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef IDG_INSTANCE_CUDA_H_
#define IDG_INSTANCE_CUDA_H_

#include <memory>

#include "CU.h"
#include "CUFFT.h"
#include "PowerRecord.h"

namespace idg::kernel::cuda {

class InstanceCUDA : public KernelsInstance {
 public:
  const size_t kTileSizeGrid = 128;
  const size_t kFftSubgridBatch = 1024;

  const std::string kNameGridder = "kernel_gridder";
  const std::string kNameDegridder = "kernel_degridder";
  const std::string kNameAdder = "kernel_adder";
  const std::string kNameSplitter = "kernel_splitter";
  const std::string kNameFft = "kernel_fft";
  const std::string kNameScaler = "kernel_scaler";
  const std::string kNnameCalibrateLMNP = "kernel_calibrate_lmnp";
  const std::string kNameCalibrateSums = "kernel_calibrate_sums";
  const std::string kNameCalibrateGradient = "kernel_calibrate_gradient";
  const std::string kNameCalibrateHessian = "kernel_calibrate_hessian";
  const std::string kNameAverageBeam = "kernel_average_beam";
  const std::string kNameFftShift = "kernel_fft_shift";
  const std::string kNameCopyTiles = "kernel_copy_tiles";
  const std::string kNameApplyPhasor = "kernel_apply_phasor";
  const std::string kNameSubgridsToWtiles = "kernel_subgrids_to_wtiles";
  const std::string kNameWtilesToGrid = "kernel_wtiles_to_grid";
  const std::string kNameSubgridsFromWtiles = "kernel_subgrids_from_wtiles";
  const std::string kNameWtilesFromGrid = "kernel_wtiles_from_grid";
  const std::string kNameWtilesToPatch = "kernel_wtiles_to_patch";
  const std::string kNameWtilesFromPatch = "kernel_wtiles_from_patch";

  InstanceCUDA(ProxyInfo& info, size_t device_id = 0);

  ~InstanceCUDA();

  cu::Context& get_context() const { return *context_; }
  cu::Device& get_device() const { return *device_; }
  cu::Stream& get_execute_stream() const { return *stream_execute_; };
  cu::Stream& get_htod_stream() const { return *stream_htod_; };
  cu::Stream& get_dtoh_stream() const { return *stream_dtoh_; };

  std::string get_compiler_flags();

  pmt::State measure();
  void measure(PowerRecord& record, cu::Stream& stream);

  void launch_gridder(
      int time_offset, int nr_subgrids, int nr_polarizations, int grid_size,
      int subgrid_size, float image_size, float w_step, int nr_channels,
      int nr_stations, float shift_l, float shift_m, cu::DeviceMemory& d_uvw,
      cu::DeviceMemory& d_wavenumbers, cu::DeviceMemory& d_visibilities,
      cu::DeviceMemory& d_taper, cu::DeviceMemory& d_aterms,
      cu::DeviceMemory& d_aterm_indices, cu::DeviceMemory& d_metadata,
      cu::DeviceMemory& d_avg_aterm, cu::DeviceMemory& d_subgrid);

  void launch_degridder(int time_offset, int nr_subgrids, int nr_polarizations,
                        int grid_size, int subgrid_size, float image_size,
                        float w_step, int nr_channels, int nr_stations,
                        float shift_l, float shift_m, cu::DeviceMemory& d_uvw,
                        cu::DeviceMemory& d_wavenumbers,
                        cu::DeviceMemory& d_visibilities,
                        cu::DeviceMemory& d_taper, cu::DeviceMemory& d_aterms,
                        cu::DeviceMemory& d_aterm_indices,
                        cu::DeviceMemory& d_metadata,
                        cu::DeviceMemory& d_subgrid);

  void launch_average_beam(int nr_baselines, int nr_antennas, int nr_timesteps,
                           int nr_channels, int nr_aterms, int subgrid_size,
                           cu::DeviceMemory& d_uvw,
                           cu::DeviceMemory& d_baselines,
                           cu::DeviceMemory& d_aterms,
                           cu::DeviceMemory& d_aterm_offsets,
                           cu::DeviceMemory& d_weights,
                           cu::DeviceMemory& d_average_beam);

  void launch_calibrate(
      int nr_subgrids, int grid_size, int subgrid_size, float image_size,
      float w_step, int total_nr_timesteps, int nr_channels, int nr_stations,
      int nr_terms, cu::DeviceMemory& d_uvw, cu::DeviceMemory& d_wavenumbers,
      cu::DeviceMemory& d_visibilities, cu::DeviceMemory& d_weights,
      cu::DeviceMemory& d_aterm, cu::DeviceMemory& d_aterm_derivatives,
      cu::DeviceMemory& d_aterm_indices, cu::DeviceMemory& d_metadata,
      cu::DeviceMemory& d_subgrid, cu::DeviceMemory& d_sums1,
      cu::DeviceMemory& d_sums2, cu::DeviceMemory& d_lmnp,
      cu::DeviceMemory& d_hessian, cu::DeviceMemory& d_gradient,
      cu::DeviceMemory& d_residual);

  void launch_grid_fft(cu::DeviceMemory& d_data, int batch, long size,
                       DomainAtoDomainB direction);

  void plan_subgrid_fft(size_t size, size_t nr_polarizations);

  void launch_subgrid_fft(cu::DeviceMemory& d_data, unsigned nr_subgrids,
                          unsigned nr_polarizations,
                          DomainAtoDomainB direction);

  void launch_fft_shift(cu::DeviceMemory& d_data, int batch, long size,
                        std::complex<float> scale = {1.0, 1.0});

  void launch_adder(int nr_subgrids, int nr_polarizations, long grid_size,
                    int subgrid_size, cu::DeviceMemory& d_metadata,
                    cu::DeviceMemory& d_subgrid, cu::DeviceMemory& d_grid);

  void launch_adder_unified(int nr_subgrids, long grid_size, int subgrid_size,
                            cu::DeviceMemory& d_metadata,
                            cu::DeviceMemory& d_subgrid, void* u_grid);

  void launch_splitter(int nr_subgrids, int nr_polarizations, long grid_size,
                       int subgrid_size, cu::DeviceMemory& d_metadata,
                       cu::DeviceMemory& d_subgrid, cu::DeviceMemory& d_grid);

  void launch_splitter_unified(int nr_subgrids, long grid_size,
                               int subgrid_size, cu::DeviceMemory& d_metadata,
                               cu::DeviceMemory& d_subgrid, void* u_grid);

  void launch_scaler(int nr_subgrids, int nr_polarizations, int subgrid_size,
                     cu::DeviceMemory& d_subgrid);

  void launch_scaler(int nr_subgrids, int subgrid_size, void* u_subgrid);

  void launch_copy_tiles(unsigned int nr_polarizations, unsigned int nr_tiles,
                         unsigned int src_tile_size, unsigned int dst_tile_size,
                         cu::DeviceMemory& d_src_tile_ids,
                         cu::DeviceMemory& d_dst_tile_ids,
                         cu::DeviceMemory& d_src_tiles,
                         cu::DeviceMemory& d_dst_tiles);

  void launch_apply_phasor_to_wtiles(unsigned int nr_polarizations,
                                     unsigned int nr_tiles, float image_size,
                                     float w_step, unsigned int tile_size,
                                     cu::DeviceMemory& d_tiles,
                                     cu::DeviceMemory& d_shift,
                                     cu::DeviceMemory& d_tile_coordinates,
                                     int sign = -1);

  void launch_adder_subgrids_to_wtiles(int nr_subgrids, int nr_polarizations,
                                       long grid_size, int subgrid_size,
                                       int tile_size, int subgrid_offset,
                                       cu::DeviceMemory& d_metadata,
                                       cu::DeviceMemory& d_subgrid,
                                       cu::DeviceMemory& d_tiles,
                                       std::complex<float> scale = {1.0, 1.0});

  void launch_adder_wtiles_to_grid(int nr_polarizations, int nr_tiles,
                                   long grid_size, int tile_size,
                                   int padded_tile_size,
                                   cu::DeviceMemory& d_tile_ids,
                                   cu::DeviceMemory& d_tile_coordinates,
                                   cu::DeviceMemory& d_tiles, void* u_grid);

  void launch_splitter_subgrids_from_wtiles(
      int nr_subgrids, int nr_polarizations, long grid_size, int subgrid_size,
      int tile_size, int subgrid_offset, cu::DeviceMemory& d_metadata,
      cu::DeviceMemory& d_subgrid, cu::DeviceMemory& d_tiles);

  void launch_splitter_wtiles_from_grid(int nr_polarizations, int nr_tiles,
                                        long grid_size, int tile_size,
                                        int padded_tile_size,
                                        cu::DeviceMemory& d_tile_ids,
                                        cu::DeviceMemory& d_tile_coordinates,
                                        cu::DeviceMemory& d_tiles,
                                        void* u_grid);

  void launch_adder_wtiles_to_patch(
      int nr_polarizations, int nr_tiles, long grid_size, int tile_size,
      int padded_tile_size, int patch_size, idg::Coordinate patch_coordinate,
      cu::DeviceMemory& d_tile_ids, cu::DeviceMemory& d_tile_coordinates,
      cu::DeviceMemory& d_tiles, cu::DeviceMemory& d_patch);

  void launch_splitter_wtiles_from_patch(
      int nr_polarizations, int nr_tiles, long grid_size, int tile_size,
      int padded_tile_size, int patch_size, idg::Coordinate patch_coordinate,
      cu::DeviceMemory& d_tile_ids, cu::DeviceMemory& d_tile_coordinates,
      cu::DeviceMemory& d_tiles, cu::DeviceMemory& d_patch);

  void free_subgrid_fft();
  int get_tile_size_grid() const { return kTileSizeGrid; };
  void free_events();

  void print_device_memory_info() const;
  size_t get_free_memory() const;
  size_t get_total_memory() const;
  template <CUdevice_attribute attribute>
  int get_attribute() const;

  void enqueue_report(cu::Stream& stream, int nr_polarizations,
                      int nr_timesteps, int nr_subgrids);

 private:
  void reset();

  // Since no CUDA calls are allowed from a callback, we have to
  // keep track of the cu::Events used in the UpdateData and
  // free them explicitely using the free_events() method.
  cu::Event& get_event();
  std::vector<std::unique_ptr<cu::Event>> events;

  // Runtime compilation
  cu::Module* compile_kernel(std::string& flags, std::string& src,
                             std::string& bin);
  void compile_kernels();
  void load_kernels();

  void set_parameters();
  void set_parameters_default();

  std::unique_ptr<cu::Context> context_;
  std::unique_ptr<cu::Device> device_;
  std::unique_ptr<cu::Stream>
      stream_execute_;  ///< Stream for kernel execution.
  std::unique_ptr<cu::Stream>
      stream_htod_;  ///< Stream for host to device memory copies.
  std::unique_ptr<cu::Stream>
      stream_dtoh_;  ///< Stream for device to host memory copies.
  std::unique_ptr<cu::Profiler> profiler_;
  std::unique_ptr<cu::Function> function_gridder_;
  std::unique_ptr<cu::Function> function_degridder_;
  std::unique_ptr<cu::Function> function_adder_;
  std::unique_ptr<cu::Function> function_splitter_;
  std::unique_ptr<cu::Function> function_scaler_;
  std::unique_ptr<cu::Function> function_average_beam_;
  std::unique_ptr<cu::Function> function_fft_shift_;
  std::vector<std::unique_ptr<cu::Function>> functions_calibrate_;
  std::vector<std::unique_ptr<cu::Function>> functions_wtiling_;
  std::vector<std::unique_ptr<cu::Module>> modules_;

  ProxyInfo& proxy_info_;

  // Subgrid FFT
  size_t fft_subgrid_batch_ = kFftSubgridBatch;
  size_t fft_subgrid_size_ = 0;
  std::unique_ptr<cufft::C2C_2D> fft_plan_subgrid_;
  std::unique_ptr<cu::DeviceMemory> device_subgrid_fft_;

  void start_measurement(void* data);
  void end_measurement(void* data);
};
std::ostream& operator<<(std::ostream& os, InstanceCUDA& d);

}  // end namespace idg::kernel::cuda

#endif
