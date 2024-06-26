// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

/*
 * BufferImpl.cpp
 * Access to IDG's high level gridder routines
 */

#include "BufferImpl.h"
#include "BufferSetImpl.h"

#include <limits>

namespace {
constexpr double speed_of_light = 299792458.0;

constexpr float meters_to_pixels(float meters, float imagesize,
                                 float frequency) {
  return meters * imagesize * (frequency / speed_of_light);
}
}  // namespace

namespace idg {
namespace api {

// Constructors and destructor
BufferImpl::BufferImpl(const BufferSetImpl& bufferset, size_t bufferTimesteps)
    : m_bufferset(bufferset),
      m_bufferTimesteps(bufferTimesteps),
      m_timeStartThisBatch(0),
      m_timeStartNextBatch(bufferTimesteps),
      m_nrStations(0),
      m_nr_baselines(0),
      m_shift{0, 0},
      m_default_aterm_offsets(2),
      m_frequencies(aocommon::xt::CreateSpan<float, 1>(nullptr, {0})),
      m_bufferUVW(
          aocommon::xt::CreateSpan<idg::UVW<float>, 2>(nullptr, {0, 0})),
      m_bufferStationPairs(
          aocommon::xt::CreateSpan<std::pair<unsigned int, unsigned int>, 1>(
              nullptr, {0})),
      m_bufferVisibilities(aocommon::xt::CreateSpan<std::complex<float>, 4>(
          nullptr, {0, 0, 0, 0})) {
#if defined(DEBUG)
  cout << __func__ << endl;
#endif
  assert(bufferset.get_proxy().get_grid().shape(2) ==
         bufferset.get_proxy().get_grid().shape(3));

  assert(bufferset.get_shift().size() == 2);
  std::copy_n(bufferset.get_shift().data(), m_shift.size(), m_shift.data());

  m_default_aterm_offsets[0] = 0;
  m_default_aterm_offsets[1] = bufferTimesteps;
  m_aterm_offsets = m_default_aterm_offsets;
}

BufferImpl::~BufferImpl() {
#if defined(DEBUG)
  cout << __func__ << endl;
#endif
}

// Set/get all parameters

void BufferImpl::set_stations(const size_t nrStations) {
  m_nrStations = nrStations;
  m_nr_baselines = ((nrStations - 1) * nrStations) / 2;
}

size_t BufferImpl::get_stations() const { return m_nrStations; }

double BufferImpl::get_image_size() const {
  aocommon::xt::Span<std::complex<float>, 4> grid =
      m_bufferset.get_proxy().get_grid();
  const size_t grid_size = grid.shape(2);
  assert(grid.shape(3) == grid_size);
  return m_bufferset.get_cell_size() * grid_size;
}

void BufferImpl::set_frequencies(size_t nr_channels,
                                 const double* frequencyList) {
  m_nr_channels = nr_channels;
  m_frequencies =
      m_bufferset.get_proxy().allocate_span<float, 1>({m_nr_channels});
  for (int i = 0; i < m_nr_channels; i++) {
    m_frequencies(i) = frequencyList[i];
  }
}

void BufferImpl::set_frequencies(const std::vector<double>& frequency_list) {
  m_nr_channels = frequency_list.size();
  m_frequencies =
      m_bufferset.get_proxy().allocate_span<float, 1>({m_nr_channels});
  m_frequencies = xt::adapt(frequency_list);
}

double BufferImpl::get_frequency(const size_t channel) const {
  return m_frequencies(channel);
}

size_t BufferImpl::get_frequencies_size() const { return m_frequencies.size(); }

// Plan creation and helper functions

void BufferImpl::bake() {
#if defined(DEBUG)
  cout << __func__ << endl;
#endif

  // Setup buffers
  malloc_buffers();
  reset_buffers();  // optimization: only call "set_uvw_to_infinity()" here
}

void BufferImpl::malloc_buffers() {
  const size_t nr_correlations = m_bufferset.get_nr_correlations();
  proxy::Proxy& proxy = m_bufferset.get_proxy();
  m_bufferUVW =
      proxy.allocate_span<UVW<float>, 2>({m_nr_baselines, m_bufferTimesteps});
  m_bufferVisibilities = proxy.allocate_span<std::complex<float>, 4>(
      {m_nr_baselines, m_bufferTimesteps, m_nr_channels, nr_correlations});
  m_bufferStationPairs =
      proxy.allocate_span<std::pair<unsigned int, unsigned int>, 1>(
          {m_nr_baselines});
  m_bufferStationPairs.fill(
      std::pair<unsigned int, unsigned int>(m_nrStations, m_nrStations));
  // m_aterms is already allocated in BufferImpl::init_default_aterm
}

void BufferImpl::reset_buffers() {
  m_bufferVisibilities.fill(std::complex<float>(0, 0));
  set_uvw_to_infinity();
  init_default_aterm();
}

void BufferImpl::set_uvw_to_infinity() {
  m_bufferUVW.fill(UVW<float>{std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::infinity()});
}

void BufferImpl::init_default_aterm() {
  const size_t subgridsize = m_bufferset.get_subgridsize();
  m_default_aterms.resize(m_nrStations * subgridsize * subgridsize,
                          {{1}, {0}, {0}, {1}});
  m_aterms = m_default_aterms;
}

// Set the a-term that starts validity at timeIndex
void BufferImpl::set_aterm(size_t timeIndex,
                           const std::complex<float>* aterms) {
  const auto* const local_aterms =
      reinterpret_cast<decltype(m_aterms)::const_pointer>(aterms);
  const int local_time = timeIndex - m_timeStartThisBatch;

  const size_t subgridsize = m_bufferset.get_subgridsize();
  const size_t aterm_block_size = m_nrStations * subgridsize * subgridsize;

  assert(m_aterm_offsets.size() >= 2);
  const int last_offset = m_aterm_offsets[m_aterm_offsets.size() - 2];

  assert(local_time >= last_offset);
  if (local_time == last_offset) {
    // Overwrite last a-term.
    std::copy(local_aterms, local_aterms + aterm_block_size,
              m_aterms.data() + m_aterms.size() - aterm_block_size);
  } else {  // local_time > last_offset
    // Insert new timeIndex before the last element in m_aterm_offsets.
    assert(local_time <= m_bufferTimesteps);
    m_aterm_offsets.back() = local_time;
    m_aterm_offsets.push_back(m_bufferTimesteps);
    m_aterms.insert(m_aterms.end(), local_aterms,
                    local_aterms + aterm_block_size);
  }

  assert(m_aterms.size() == (m_aterm_offsets.size() - 1) * aterm_block_size);
}

}  // namespace api
}  // namespace idg

// C interface:
// Rationale: calling the code from C code and Fortran easier,
// and bases to create interface to scripting languages such as
// Python, Julia, Matlab, ...
extern "C" {

int Buffer_get_stations(idg::api::BufferImpl* p) { return p->get_stations(); }

void Buffer_set_stations(idg::api::BufferImpl* p, int n) { p->set_stations(n); }

void Buffer_set_frequencies(idg::api::BufferImpl* p, int nr_channels,
                            double* frequencies) {
  p->set_frequencies(nr_channels, frequencies);
}

double Buffer_get_frequency(idg::api::BufferImpl* p, int channel) {
  return p->get_frequency(channel);
}

int Buffer_get_frequencies_size(idg::api::BufferImpl* p) {
  return p->get_frequencies_size();
}

double Buffer_get_image_size(idg::api::BufferImpl* p) {
  return p->get_image_size();
}

void Buffer_bake(idg::api::BufferImpl* p) { p->bake(); }

void Buffer_flush(idg::api::BufferImpl* p) { p->flush(); }

}  // extern C
