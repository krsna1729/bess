// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MESSAGE_H_
#define BESS_MESSAGE_H_

#include <cstdarg>
#include <functional>

#include "pb/bess_msg.pb.h"
#include "pb/error.pb.h"
#include "pb/module_msg.pb.h"

template <typename T>
static inline bool UnpackTypedArgument(const google::protobuf::Any &input,
                                       T *output) {
  if (input.type_url().empty() && input.value().empty()) {
    output->Clear();
    return true;
  }
  return input.UnpackTo(output);
}
typedef bess::pb::Error pb_error_t;

using CommandResponse = bess::pb::CommandResponse;

CommandResponse CommandSuccess();
CommandResponse CommandSuccess(const google::protobuf::Message &return_data);

CommandResponse CommandFailure(int code);
[[gnu::format(printf, 2, 3)]] CommandResponse CommandFailure(int code,
                                                             const char *fmt,
                                                             ...);

template <typename T, typename M, typename A>
using pb_func_t = std::function<T(M *, const A &)>;

[[gnu::format(printf, 2, 3)]] pb_error_t pb_error(int code, const char *fmt,
                                                  ...);

static inline pb_error_t pb_errno(int code) {
  return pb_error(code, "%s", strerror(code));
}

#endif  // BESS_MESSAGE_H_
