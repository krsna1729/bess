// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_EVENT_H_
#define BESS_EVENT_H_

#include <map>
#include <set>

class Module;

namespace bess {

/*
 * An Event is a lightweight notification of some activity in the BESS core.
 * Currently these are only "sent" to Modules via `Module::OnEvent()`, but they
 * could easily be extended to other entities in the system. See below for a
 * description of
 *
 * PreResume
 * ---------
 * Modules will receive the PreResume event immediately before a call to
 * `resume_worker()` or `resume_all_workers()`. If a Module is attached to
 * multiple workers which are being resumed at the same time (e.g., via
 * WorkerPauser) it will recieve PreResume exactly once.
 */
enum class Event { PreResume };

extern std::map<Event, std::set<Module *>> event_modules;

}  // namespace bess

#endif  // BESS_EVENT_H_
