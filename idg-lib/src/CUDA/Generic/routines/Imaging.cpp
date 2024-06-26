#include "../Generic.h"
#include "InstanceCUDA.h"

using namespace idg::kernel::cuda;

namespace idg {
namespace proxy {
namespace cuda {

void Generic::run_imaging(
    const Plan& plan, const aocommon::xt::Span<float, 1>& frequencies,
    const aocommon::xt::Span<std::complex<float>, 4>& visibilities,
    const aocommon::xt::Span<UVW<float>, 2>& uvw,
    const aocommon::xt::Span<std::pair<unsigned int, unsigned int>, 1>&
        baselines,
    aocommon::xt::Span<std::complex<float>, 4>& grid,
    const aocommon::xt::Span<Matrix2x2<std::complex<float>>, 4>& aterms,
    const aocommon::xt::Span<unsigned int, 1>& aterm_offsets,
    const aocommon::xt::Span<float, 2>& taper, ImagingMode mode) {
  InstanceCUDA& device = get_device(0);
  const cu::Context& context = device.get_context();

  // Arguments
  const size_t nr_baselines = visibilities.shape(0);
  const size_t nr_timesteps = visibilities.shape(1);
  const size_t nr_channels = visibilities.shape(2);
  const size_t nr_correlations = visibilities.shape(3);
  const size_t nr_stations = aterms.shape(1);
  const size_t nr_polarizations = grid.shape(1);
  const size_t grid_size = grid.shape(2);
  assert(grid.shape(3) == grid_size);
  const float cell_size = plan.get_cell_size();
  const float image_size = cell_size * grid_size;
  const size_t subgrid_size = plan.get_subgrid_size();
  const float w_step = plan.get_w_step();
  const std::array<float, 2>& shift = plan.get_shift();

  WTileUpdateSet wtile_set;
  if (mode == ImagingMode::mode_gridding) {
    wtile_set = plan.get_wtile_flush_set();
  } else if (mode == ImagingMode::mode_degridding) {
    wtile_set = plan.get_wtile_initialize_set();
  }

  Tensor<float, 1> wavenumbers = compute_wavenumbers(frequencies);

  const size_t sizeof_wavenumbers =
      wavenumbers.Span().size() * sizeof(*wavenumbers.Span().data());
  const size_t sizeof_taper = taper.size() * sizeof(*taper.data());
  const size_t sizeof_aterms = aterms.size() * sizeof(*aterms.data());
  const size_t sizeof_aterm_indices =
      auxiliary::sizeof_aterm_indices(nr_baselines, nr_timesteps);
  const unsigned int* aterm_indices = plan.get_aterm_indices_ptr();

  // Configuration
  const unsigned nr_devices = get_num_devices();
  int device_id = 0;  // only one GPU is used

  // Performance measurements
  get_report()->initialize(nr_channels, subgrid_size, grid_size);
  device.set_report(get_report());
  std::vector<pmt::State> startStates(nr_devices + 1);
  std::vector<pmt::State> endStates(nr_devices + 1);

  // Load streams
  cu::Stream& executestream = device.get_execute_stream();
  cu::Stream& htodstream = device.get_htod_stream();
  cu::Stream& dtohstream = device.get_dtoh_stream();

  // Allocate device memory
  cu::DeviceMemory d_wavenumbers(context, sizeof_wavenumbers);
  cu::DeviceMemory d_taper(context, sizeof_taper);
  cu::DeviceMemory d_aterms(context, sizeof_aterms);
  cu::DeviceMemory d_aterm_indices(context, sizeof_aterm_indices);

  // Initialize device memory
  htodstream.memcpyHtoDAsync(d_wavenumbers, wavenumbers.Span().data(),
                             sizeof_wavenumbers);
  htodstream.memcpyHtoDAsync(d_taper, taper.data(), sizeof_taper);
  htodstream.memcpyHtoDAsync(d_aterms, aterms.data(), sizeof_aterms);
  htodstream.memcpyHtoDAsync(d_aterm_indices, aterm_indices,
                             sizeof_aterm_indices);

  // When degridding, d_avg_aterm is not used and remains a null pointer.
  // When gridding, d_avg_aterm always holds a cu::DeviceMemory object. When
  // average aterm correction is disabled, the cu::DeviceMemory object contains
  // a null pointer, such that the gridder kernel can detect that it should not
  // apply average aterm corrections.
  std::unique_ptr<cu::DeviceMemory> d_avg_aterm;
  if (mode == ImagingMode::mode_gridding) {
    size_t sizeof_avg_aterm_correction =
        m_avg_aterm_correction.size() * sizeof(std::complex<float>);
    d_avg_aterm.reset(
        new cu::DeviceMemory(context, sizeof_avg_aterm_correction));
    htodstream.memcpyHtoDAsync(*d_avg_aterm, m_avg_aterm_correction.data(),
                               sizeof_avg_aterm_correction);
  }

  // Plan subgrid fft
  device.plan_subgrid_fft(subgrid_size, nr_polarizations);

  // Get the available device memory, reserving some
  // space for the w-padded tile FFT plan which is allocated
  // in run_wtiles_to_grid and run_wtiles_from_grid.
  size_t bytes_free = device.get_free_memory();
  if (!m_disable_wtiling) {
    bytes_free -= bytes_required_wtiling(plan.get_wtile_initialize_set(),
                                         nr_polarizations, subgrid_size,
                                         image_size, w_step, shift, bytes_free);
  }

  // Initialize jobs
  std::vector<JobData> jobs;
  int jobsize =
      initialize_jobs(nr_baselines, nr_timesteps, nr_channels, subgrid_size,
                      bytes_free, plan, visibilities, uvw, jobs);

  // Allocate device memory for jobs
  int max_nr_subgrids = plan.get_max_nr_subgrids(jobsize);
  size_t sizeof_visibilities = auxiliary::sizeof_visibilities(
      jobsize, nr_timesteps, nr_channels, nr_correlations);
  size_t sizeof_uvw = auxiliary::sizeof_uvw(jobsize, nr_timesteps);
  size_t sizeof_subgrids = auxiliary::sizeof_subgrids(
      max_nr_subgrids, subgrid_size, nr_correlations);
  size_t sizeof_metadata = auxiliary::sizeof_metadata(max_nr_subgrids);

  std::array<cu::DeviceMemory, 2> d_visibilities_{
      {{context, sizeof_visibilities}, {context, sizeof_visibilities}}};
  std::array<cu::DeviceMemory, 2> d_uvw_{
      {{context, sizeof_uvw}, {context, sizeof_uvw}}};
  std::array<cu::DeviceMemory, 2> d_subgrids_{
      {{context, sizeof_subgrids}, {context, sizeof_subgrids}}};
  std::array<cu::DeviceMemory, 2> d_metadata_{
      {{context, sizeof_metadata}, {context, sizeof_metadata}}};

  // Page-locked host memory
  cu::RegisteredMemory h_metadata(context, (void*)plan.get_metadata_ptr(),
                                  plan.get_sizeof_metadata());

  // Events
  std::vector<cu::Event> inputCopied;
  std::vector<cu::Event> gpuFinished;
  std::vector<cu::Event> outputCopied;  // Only used when degridding.
  unsigned int nr_jobs = (nr_baselines + jobsize - 1) / jobsize;
  inputCopied.reserve(nr_jobs);
  gpuFinished.reserve(nr_jobs);
  outputCopied.reserve(nr_jobs);
  for (unsigned int i = 0; i < nr_jobs; i++) {
    inputCopied.emplace_back(context);
    gpuFinished.emplace_back(context);
    outputCopied.emplace_back(context);
  }

  // Start performance measurement
  startStates[device_id] = device.measure();
  startStates[nr_devices] = power_meter_->Read();

  // Iterate all jobs
  for (unsigned job_id = 0; job_id < jobs.size(); job_id++) {
    // Id for double-buffering
    unsigned local_id = job_id % 2;
    unsigned job_id_next = job_id + 1;
    unsigned local_id_next = (local_id + 1) % 2;

    // Get parameters for current job
    unsigned int time_offset_current = jobs[job_id].current_time_offset;
    unsigned int nr_baselines_current = jobs[job_id].current_nr_baselines;
    unsigned int nr_subgrids_current = jobs[job_id].current_nr_subgrids;
    auto metadata_ptr = jobs[job_id].metadata_ptr;
    auto uvw_ptr = jobs[job_id].uvw_ptr;
    auto visibilities_ptr = jobs[job_id].visibilities_ptr;

    // Load memory objects
    cu::DeviceMemory& d_visibilities = d_visibilities_[local_id];
    cu::DeviceMemory& d_uvw = d_uvw_[local_id];
    cu::DeviceMemory& d_subgrids = d_subgrids_[local_id];
    cu::DeviceMemory& d_metadata = d_metadata_[local_id];

    // Copy input data for first job to device
    if (job_id == 0) {
      if (mode == ImagingMode::mode_gridding) {
        htodstream.memcpyHtoDAsync(
            d_visibilities, visibilities_ptr,
            auxiliary::sizeof_visibilities(nr_baselines_current, nr_timesteps,
                                           nr_channels, nr_correlations));
      }
      htodstream.memcpyHtoDAsync(
          d_uvw, uvw_ptr,
          auxiliary::sizeof_uvw(nr_baselines_current, nr_timesteps));
      htodstream.memcpyHtoDAsync(
          d_metadata, metadata_ptr,
          auxiliary::sizeof_metadata(nr_subgrids_current));
      htodstream.record(inputCopied[job_id]);
    }

    // Copy input data for next job
    if (job_id_next < jobs.size()) {
      // Wait for previous job to finish before
      // overwriting its input buffers.
      if (job_id_next > 1) {
        htodstream.waitEvent(gpuFinished[job_id_next - 2]);
      }

      // Load memory objects
      cu::DeviceMemory& d_visibilities_next = d_visibilities_[local_id_next];
      cu::DeviceMemory& d_uvw_next = d_uvw_[local_id_next];
      cu::DeviceMemory& d_metadata_next = d_metadata_[local_id_next];

      // Get parameters for next job
      unsigned int nr_baselines_next = jobs[job_id_next].current_nr_baselines;
      unsigned int nr_subgrids_next = jobs[job_id_next].current_nr_subgrids;
      auto metadata_ptr_next = jobs[job_id_next].metadata_ptr;
      auto uvw_ptr_next = jobs[job_id_next].uvw_ptr;
      auto visibilities_ptr_next = jobs[job_id_next].visibilities_ptr;

      // Copy input data to device
      if (mode == ImagingMode::mode_gridding) {
        size_t sizeof_visibilities_next = auxiliary::sizeof_visibilities(
            nr_baselines_next, nr_timesteps, nr_channels, nr_correlations);
        htodstream.memcpyHtoDAsync(d_visibilities_next, visibilities_ptr_next,
                                   sizeof_visibilities_next);
      }
      htodstream.memcpyHtoDAsync(
          d_uvw_next, uvw_ptr_next,
          auxiliary::sizeof_uvw(nr_baselines_next, nr_timesteps));
      htodstream.memcpyHtoDAsync(d_metadata_next, metadata_ptr_next,
                                 auxiliary::sizeof_metadata(nr_subgrids_next));
      htodstream.record(inputCopied[job_id_next]);
    }

    // Initialize output buffer to zero
    if (mode == ImagingMode::mode_gridding) {
      d_subgrids.zero(executestream);
    } else if (mode == ImagingMode::mode_degridding) {
      if (job_id > 1) {
        executestream.waitEvent(outputCopied[job_id - 2]);
      }
      d_visibilities.zero(executestream);
    }

    // Wait for input to be copied
    executestream.waitEvent(inputCopied[job_id]);

    if (mode == ImagingMode::mode_gridding) {
      // Launch gridder kernel
      device.launch_gridder(
          time_offset_current, nr_subgrids_current, nr_polarizations, grid_size,
          subgrid_size, image_size, w_step, nr_channels, nr_stations, shift[0],
          shift[1], d_uvw, d_wavenumbers, d_visibilities, d_taper, d_aterms,
          d_aterm_indices, d_metadata, *d_avg_aterm, d_subgrids);

      // Launch FFT
      device.launch_subgrid_fft(d_subgrids, nr_subgrids_current,
                                nr_polarizations, FourierDomainToImageDomain);

      // Launch adder
      if (plan.get_use_wtiles()) {
        auto subgrid_offset = plan.get_subgrid_offset(jobs[job_id].first_bl);
        run_subgrids_to_wtiles(
            nr_polarizations, subgrid_offset, nr_subgrids_current, subgrid_size,
            image_size, w_step, shift, wtile_set, d_subgrids, d_metadata);
      } else {
        if (m_use_unified_memory) {
          device.launch_adder_unified(nr_subgrids_current, grid_size,
                                      subgrid_size, d_metadata, d_subgrids,
                                      get_unified_grid_data());
        } else {
          device.launch_adder(nr_subgrids_current, nr_polarizations, grid_size,
                              subgrid_size, d_metadata, d_subgrids, *d_grid_);
        }
      }
      executestream.record(gpuFinished[job_id]);

    } else if (mode == ImagingMode::mode_degridding) {
      // Launch splitter
      if (plan.get_use_wtiles()) {
        auto subgrid_offset = plan.get_subgrid_offset(jobs[job_id].first_bl);
        run_subgrids_from_wtiles(
            nr_polarizations, subgrid_offset, nr_subgrids_current, subgrid_size,
            image_size, w_step, shift, wtile_set, d_subgrids, d_metadata);
      } else {
        if (m_use_unified_memory) {
          device.launch_splitter_unified(nr_subgrids_current, grid_size,
                                         subgrid_size, d_metadata, d_subgrids,
                                         get_unified_grid_data());
        } else {
          device.launch_splitter(nr_subgrids_current, nr_polarizations,
                                 grid_size, subgrid_size, d_metadata,
                                 d_subgrids, *d_grid_);
        }
      }

      // Launch FFT
      device.launch_subgrid_fft(d_subgrids, nr_subgrids_current,
                                nr_polarizations, ImageDomainToFourierDomain);

      // Launch degridder kernel
      device.launch_degridder(
          time_offset_current, nr_subgrids_current, nr_polarizations, grid_size,
          subgrid_size, image_size, w_step, nr_channels, nr_stations, shift[0],
          shift[1], d_uvw, d_wavenumbers, d_visibilities, d_taper, d_aterms,
          d_aterm_indices, d_metadata, d_subgrids);
      executestream.record(gpuFinished[job_id]);

      // Copy visibilities to host
      dtohstream.waitEvent(gpuFinished[job_id]);
      auto sizeof_visibilities = auxiliary::sizeof_visibilities(
          nr_baselines_current, nr_timesteps, nr_channels, nr_correlations);
      dtohstream.memcpyDtoHAsync(visibilities_ptr, d_visibilities,
                                 sizeof_visibilities);
      dtohstream.record(outputCopied[job_id]);
    }

    // Report performance
    device.enqueue_report(executestream, nr_polarizations,
                          jobs[job_id].current_nr_timesteps,
                          jobs[job_id].current_nr_subgrids);
  }  // end for bl

  // Wait for all visibilities to be copied
  if (mode == ImagingMode::mode_degridding) {
    dtohstream.synchronize();
  }

  // Wait for all reports to be printed
  executestream.synchronize();

  // End performance measurement
  endStates[device_id] = device.measure();
  endStates[nr_devices] = power_meter_->Read();
  get_report()->update(Report::device, startStates[device_id],
                       endStates[device_id]);
  get_report()->update(Report::host, startStates[nr_devices],
                       endStates[nr_devices]);

  // Update report
  auto total_nr_subgrids = plan.get_nr_subgrids();
  auto total_nr_timesteps = plan.get_nr_timesteps();
  auto total_nr_visibilities = plan.get_nr_visibilities();
  get_report()->print_total(nr_correlations, total_nr_timesteps,
                            total_nr_subgrids);
  const std::string* name;
  if (mode == ImagingMode::mode_gridding) {
    name = &auxiliary::name_gridding;
  } else if (mode == ImagingMode::mode_degridding) {
    name = &auxiliary::name_degridding;
  }
  get_report()->print_visibilities(*name, total_nr_visibilities);

  // Cleanup
  device.free_subgrid_fft();
}

}  // end namespace cuda
}  // end namespace proxy
}  // end namespace idg
