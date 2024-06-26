// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef IDG_POWER_RECORD_H_
#define IDG_POWER_RECORD_H_

#include <cstdio>

#include "idg-common.h"

#include "CU.h"

namespace idg {
namespace kernel {
namespace cuda {

class PowerRecord {
 public:
  PowerRecord(cu::Event& event, pmt::Pmt& sensor);

  void enqueue(cu::Stream& stream);
  static void getPower(CUstream, CUresult, void* userData);
  pmt::Pmt& sensor;
  pmt::State state;
  cu::Event& event;
};

}  // end namespace cuda
}  // end namespace kernel
}  // end namespace idg

#endif