// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_SNBUFLAYOUT_H_
#define BESS_SNBUFLAYOUT_H_

/* BESS packet-private and default payload sizes.
 *
 * SNBUF_DATA is the historical default payload capacity and remains the
 * limit for consumers with fixed application-level offset or MTU contracts.
 * PacketPool data rooms are configurable and may be larger; these values are
 * not a native mbuf layout contract.
 */
#define SNBUF_METADATA 128
#define SNBUF_SCRATCHPAD 64
#define SNBUF_DATA 2048

#endif  // BESS_SNBUFLAYOUT_H_
