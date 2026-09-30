// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DEBUG_H_
#define BESS_DEBUG_H_


namespace bess {
namespace debug {

void SetTrapHandler(void);
[[noreturn]] void GoPanic(void);
void DumpTypes(void);

}  // namespace debug
}  // namespace bess

#endif  // BESS_DEBUG_H_
