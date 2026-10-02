// ApplicationsToRestart.h
//
// The core of GetApplicationsToRestart: a BFS over the in-memory mount graph
// (reverse dependency walk from the locked versions of the changed package up
// to the root applications). Kept free of ralf/libpackage types so it is
// directly compilable on a PC - the gtest suite in test/ builds this exact
// translation unit (../src/ApplicationsToRestart.cpp), no mirrored copy.

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace packagemanager
{
    // Structured (packageId, version) key - matches the ConfigMetadataKey
    // typedef in IPackageImpl.h, declared here to keep this file
    // dependency-free.
    using ConfigMetadataKey = std::pair<std::string, std::string>;

    // Fills applicationIds with the ids of currently locked applications that
    // must be restarted because they are the given package or depend on it,
    // directly or transitively.
    //
    // lockedDependencies: the mount graph as adjacency - for every locked
    // (mounted) package key, the keys of its direct dependencies as resolved
    // at lock time. Deeper levels live in the entries of the dependencies
    // themselves.
    //
    // The package is matched by id only, deliberately without a version:
    // running instances are locked on the version that was installed at Lock
    // time, which is typically older than the version just installed. Matching
    // on the new version would find nothing, since that version is not locked
    // by anyone yet.
    void findApplicationsToRestart(
        const std::string &packageId,
        const std::map<ConfigMetadataKey, std::vector<ConfigMetadataKey> > &lockedDependencies,
        std::vector<std::string> &applicationIds);

} // namespace packagemanager
