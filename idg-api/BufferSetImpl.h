// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef IDG_API_BUFFERSETIMPL_H_
#define IDG_API_BUFFERSETIMPL_H_

#include <array>
#include <memory>
#include <vector>

#include "idg-common.h"
#include "idg-external.h"

#include "BufferSet.h"

namespace idg {
namespace api {

class GridderBufferImpl;

class BufferSetImpl : public virtual BufferSet {
 public:
  enum class Watch { kAvgBeam, kPlan, kGridding, kDegridding };

  BufferSetImpl(Type architecture);

  ~BufferSetImpl();

  const BulkDegridder* get_bulk_degridder(int i) final override;
  DegridderBuffer* get_degridder(int i) final override;
  GridderBuffer* get_gridder(int i) final override;

  virtual void init(size_t width, float cellsize, float max_w, float shiftl,
                    float shiftm, options_type& options) final override;

  virtual void init_buffers(size_t bufferTimesteps,
                            std::vector<std::vector<double>> bands,
                            int nr_stations, float max_baseline,
                            options_type& options,
                            BufferSetType buffer_set_type) final override;

  virtual void set_image(const double* image, bool do_scale) final override;
  virtual void get_image(double* image) final override;
  virtual void finished() final override;

  virtual size_t get_subgridsize() const final override {
    return m_subgridsize;
  }
  virtual float get_subgrid_pixelsize() const final override {
    return m_image_size / m_subgridsize;
  }
  virtual void set_apply_aterm(bool do_apply) final override {
    m_apply_aterm = do_apply;
  }
  virtual void init_compute_avg_beam(compute_flags flag) final override;
  virtual void finalize_compute_avg_beam() final override;
  virtual std::shared_ptr<std::vector<float>> get_scalar_beam()
      const final override {
    return m_scalar_beam;
  }
  virtual std::shared_ptr<std::vector<std::complex<float>>>
  get_matrix_inverse_beam() const final override {
    return m_matrix_inverse_beam;
  }
  virtual void set_scalar_beam(
      std::shared_ptr<std::vector<float>> scalar_beam) final override {
    m_scalar_beam = scalar_beam;
  }
  virtual void set_matrix_inverse_beam(
      std::shared_ptr<std::vector<std::complex<float>>> matrix_inverse_beam)
      final override;
  virtual void unset_matrix_inverse_beam() final override;

  void report_runtime();

  int get_nr_correlations() const { return m_nr_correlations; }
  int get_nr_polarizations() const { return m_nr_polarizations; }

  float get_cell_size() const { return m_cell_size; }
  float get_w_step() const { return m_w_step; }
  const std::array<float, 2>& get_shift() const { return m_shift; }
  float get_kernel_size() const { return m_kernel_size; }
  const aocommon::xt::Span<float, 2>& get_taper() const { return m_taper; }

  Stopwatch& get_watch(Watch watch) const;

  bool get_do_gridding() const { return m_do_gridding; }
  bool get_apply_aterm() const { return m_apply_aterm; }

  proxy::Proxy& get_proxy() const { return *m_proxy; }

 private:
  std::unique_ptr<proxy::Proxy> create_proxy(Type architecture);

  aocommon::xt::Span<std::complex<float>, 4> allocate_grid();
  void free_grid();

  std::unique_ptr<proxy::Proxy> m_proxy;
  BufferSetType m_buffer_set_type;
  std::vector<std::unique_ptr<GridderBufferImpl>> m_gridderbuffers;
  std::vector<std::unique_ptr<DegridderBuffer>> m_degridderbuffers;
  std::vector<std::unique_ptr<BulkDegridder>> m_bulkdegridders;
  std::vector<float> m_taper_subgrid;
  std::vector<float> m_taper_grid;
  std::vector<float> m_inv_taper;
  aocommon::xt::Span<float, 2> m_taper;
  std::vector<std::complex<float>> m_average_beam;
  std::shared_ptr<std::vector<float>> m_scalar_beam;
  std::shared_ptr<std::vector<std::complex<float>>> m_matrix_inverse_beam;
  bool m_stokes_I_only;
  int m_nr_correlations;
  int m_nr_polarizations;
  size_t m_subgridsize;
  float m_image_size;
  float m_cell_size;
  float m_w_step;
  int m_nr_w_layers;
  std::array<float, 2> m_shift;
  size_t m_size;
  size_t m_padded_size;
  float m_kernel_size;
  bool m_apply_aterm = false;
  bool m_do_gridding = true;
  bool m_apply_wstack_correction = false;

  // timing
  std::unique_ptr<Stopwatch> m_get_image_watch;
  std::unique_ptr<Stopwatch> m_set_image_watch;
  std::unique_ptr<Stopwatch> m_avg_beam_watch;
  std::unique_ptr<Stopwatch> m_plan_watch;
  std::unique_ptr<Stopwatch> m_gridding_watch;
  std::unique_ptr<Stopwatch> m_degridding_watch;

  // debug
  static void write_grid(
      const aocommon::xt::Span<std::complex<float>, 4>& grid);
};

}  // namespace api
}  // namespace idg

#endif
