/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2025 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ApplicationsToRestart.h"

#include <set>

namespace packagemanager
{
    // Example trace. Locked chains:
    //   ("Application1","1.0") -> ("com.rdkcentral.wpe","3.0") -> ("com.rdkcentral.base","1.0")
    //   ("Application2","2.0") -> ("com.rdkcentral.wpe","3.0") -> ("com.rdkcentral.base","1.0")
    // A new version of com.rdkcentral.wpe was just installed, so the caller
    // asks for "com.rdkcentral.wpe":
    //   1) reverse graph (dependency -> dependents):
    //        wpe  <- Application1, Application2
    //        base <- wpe
    //   2) seeded with the MOUNTED versions of the package: pending = [wpe 3.0]
    //      (not the just-installed one - nobody locks it yet)
    //   3) BFS upwards: affected = {wpe, Application1, Application2}
    //      (base is NOT affected: it does not depend on wpe, wpe depends on it)
    //   4) roots only (affected packages nothing depends on): wpe has
    //      dependents -> skipped (restarting the apps refreshes it anyway);
    //      result = ["Application1", "Application2"]
    void findApplicationsToRestart(
        const std::string &packageId,
        const std::map<ConfigMetadataKey, std::vector<ConfigMetadataKey> > &lockedDependencies,
        std::vector<std::string> &applicationIds)
    {
        // Reverse graph: dependency -> everything that directly depends on it.
        std::map<ConfigMetadataKey, std::vector<ConfigMetadataKey> > dependents;
        for (const auto &entry : lockedDependencies)
        {
            for (const auto &dep : entry.second)
            {
                dependents[dep].push_back(entry.first);
            }
        }

        // BFS upwards from every mounted version of the package, collecting
        // the whole affected subgraph.
        std::set<ConfigMetadataKey> affected;
        std::vector<ConfigMetadataKey> pending;
        for (const auto &entry : lockedDependencies)
        {
            if (entry.first.first == packageId)
            {
                pending.push_back(entry.first);
            }
        }
        while (!pending.empty())
        {
            const ConfigMetadataKey key = pending.back();
            pending.pop_back();
            if (!affected.insert(key).second)
            {
                continue;
            }
            const auto it = dependents.find(key);
            if (it != dependents.end())
            {
                pending.insert(pending.end(), it->second.begin(), it->second.end());
            }
        }

        // Roots of the affected subgraph: locked packages that nothing else
        // depends on. Those are the applications; restarting them refreshes
        // the whole chain, so intermediate libraries are not reported.
        std::set<std::string> appIds;
        for (const auto &key : affected)
        {
            if (dependents.find(key) == dependents.end())
            {
                appIds.insert(key.first);
            }
        }
        applicationIds.assign(appIds.begin(), appIds.end());
    }

} // namespace packagemanager
