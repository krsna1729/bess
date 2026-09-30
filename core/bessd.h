// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

// Utility routines for the main bess daemon.

#ifndef BESS_BESSD_H_
#define BESS_BESSD_H_

#include <string>
#include <tuple>
#include <vector>

namespace bess {
namespace bessd {

// When Modules extend other Modules, they may reference a shared object
// that has not yet been loaded by the BESS daemon. kInheritanceLimit is
// the number of passes that will be made while loading Module shared objects,
// and thus the maximum inheritance depth of any Module.
const int kInheritanceLimit = 10;
// Process command line arguments from gflags.
void ProcessCommandLineArgs();

// Checks that we are running as superuser.
void CheckRunningAsRoot();

// Write the pid value to the given file fd.  Overwrites anything present at
// that fd.  Dies if unable to overwrite the file.
void WritePidfile(int fd, pid_t pid);

// Read the pid value from the given file fd.  Returns true and the read pid
// value upon success.  Returns false upon failure.
std::tuple<bool, pid_t> ReadPidfile(int fd);

// Tries to acquire the daemon pidfile lock for the file open at the given fd.
// Dies if an error occurs when trying to acquire the lock.  Returns a pair
// <lockheld, pid> where lockheld is a bool indicating if the lock is held and
// pid is a pid_t that is non-zero if lockheld is true indicating the process
// holding the lock.
std::tuple<bool, pid_t> TryAcquirePidfileLock(int fd);

// Ensures that we are a unique instance.
// Returns the (locked) file descriptor of pidfile_path.
int CheckUniqueInstance(const std::string &pidfile_path);

// Returns the default pidfile for the default RPC address and a stable,
// endpoint-specific sibling path for any other address.
std::string PidfilePathForRpcAddress(const std::string &default_path,
                                     const std::string &address);

// Starts BESS as a daemon running in the background.
int Daemonize();

// Sets BESS's resource limit.  Returns true upon success.
bool SetResourceLimit();

// Load an indiviual plugin specified by path. Return true upon success.
bool LoadPlugin(const std::string &path);

// Unload a loaded plugin specified by path. Return true upon success.
bool UnloadPlugin(const std::string &path);

// Load all the .so files in the specified directory. Return true upon success.
bool LoadPlugins(const std::string &directory);

// List all imported .so files.
std::vector<std::string> ListPlugins();


}  // namespace bessd
}  // namespace bess

#endif  // BESS_BESSD_H_
