// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_SYMMETRIC_INPUTS_H_
#define BESS_MODULES_SYMMETRIC_INPUTS_H_

#include <optional>
#include <string>

class Module;

namespace bess::modules {

// Whether every packet `m` receives comes from a port queue with a symmetric
// hash, laid out so both directions of a connection meet on one worker (TP8,
// table_policy.md 5.3): every task that reaches `m` (as last propagated) is a
// PortInc or QueueInc connected straight to `m` (no module in between that
// could rewrite or decapsulate), on a port whose symmetric_rss() holds; all
// those ports have the same number of receive queues and, with several, the
// same rss_signature(); and queue q of each port runs on the same worker. nullopt when it holds; otherwise why not. A module that keeps
// per-worker connection state checks this before resuming: a mis-steered
// reply would look like a new or untracked connection, a silent policy error.
std::optional<std::string> AsymmetricInputs(const Module &m);

}  // namespace bess::modules

#endif  // BESS_MODULES_SYMMETRIC_INPUTS_H_
