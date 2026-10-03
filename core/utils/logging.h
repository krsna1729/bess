// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_LOGGING_H_
#define BESS_UTILS_LOGGING_H_

// The one way BESS code includes glog. Include this, never <glog/logging.h>
// directly (tools/check_includes.py enforces it).
//
// glog and protobuf's absl logging (absl/log/log.h, which protobuf's own
// headers include) both define LOG, LOG_IF, VLOG and their variants, and
// glog's CHECK expands through LOG_IF. Each defines them unconditionally, so
// whichever of the two headers is included second wins; later includes are
// no-ops behind include guards. bessd initialises only glog
// (google::InitGoogleLogging), so a translation unit in which absl came
// second logged around glog's files and --v.
//
// Including absl's header first here makes glog second in every translation
// unit, whatever order other headers arrive in: if protobuf was included
// earlier, absl/log/log.h is already defined and glog still comes after it;
// if protobuf comes later, its absl/log/log.h include is a no-op.
//
// absl/log exists only from Abseil LTS 20230125, used by protobuf 22 and
// later. Older stacks (Ubuntu 24.04: protobuf 3.21, Abseil 20220623) have no
// absl/log/log.h, protobuf cannot include it there, and there is no collision;
// __has_include searches the same paths protobuf's own include would.
#if __has_include(<absl/log/log.h>)
#include <absl/log/log.h>  // IWYU pragma: keep (must precede glog)
#endif
#include <glog/logging.h>  // IWYU pragma: export

#endif  // BESS_UTILS_LOGGING_H_
