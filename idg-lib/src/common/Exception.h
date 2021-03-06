// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef IDG_EXCEPTION_H_
#define IDG_EXCEPTION_H_

namespace idg {
namespace exception {

class NotImplemented : public std::logic_error {
 public:
  NotImplemented() : std::logic_error("Not Implemented"){};
  explicit NotImplemented(const std::string& what_arg)
      : std::logic_error("Not Implemented: " + what_arg){};
};

}  // namespace exception
}  // namespace idg

#endif
