// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_TABLE_POLICY_H_
#define BESS_DATAPLANE_TABLE_POLICY_H_

// What a packet-path table promises about its writers, readers and growth,
// as types (user decisions 1 and 3, 2026-10-04): a library takes its table as
// a type, and the module that owns the library picks it. Tags only; each
// table says which it is (`writers`, `readers`, `growth` member typedefs).

namespace bess::dataplane {

// Writers.
struct OwnerWrites {};   // one worker owns the table and is its only reader too
struct SingleWriter {};  // one thread at a time writes (the caller serialises);
                         // any thread may read, lock-free
struct MultiWriter {};   // any thread writes under the table's lock; any reads

// Readers.
struct OwnerReads {};
struct AnyReader {};

// Growth.
struct Fixed {};
struct Growable {};

// The writer guard of a table whose owner is its only writer: nothing to hold,
// so code written against a guard compiles to nothing for it.
struct NoLock {};

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_TABLE_POLICY_H_
