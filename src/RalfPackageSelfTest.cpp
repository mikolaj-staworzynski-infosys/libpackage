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

// Self test cases for RalfPackageImpl::findApplicationsToRestart. Built and run only
// when RALF_PACKAGE_SELF_TEST is enabled (see src/CMakeLists.txt). All cases operate
// on synthetic, purely in-memory mount tables: no real packages are opened or mounted,
// so this is safe to run on hardware.

#ifdef RALF_PACKAGE_SELF_TEST

#include "RalfPackageImpl.h"
#include <algorithm>
#include <initializer_list>
#include <iostream>

namespace packagemanager
{
    void RalfPackageImpl::runGetApplicationsToRestartSelfTest()
    {
        auto makeEntry = [](std::initializer_list<std::string> deps)
        {
            auto info = std::make_unique<MountedPackageInfo>();
            info->dependencies.assign(deps.begin(), deps.end());
            return info;
        };
        auto runCase = [](const std::string &name,
                          const std::map<std::string, std::unique_ptr<MountedPackageInfo> > &mounted,
                          const std::string &packageId,
                          std::vector<std::string> expected)
        {
            std::vector<std::string> result;
            findApplicationsToRestart(packageId, mounted, result);
            std::sort(expected.begin(), expected.end()); // the result is set-backed, hence sorted
            if (result == expected)
            {
                std::cout << "[libPackage][SELFTEST] PASS: " << name << std::endl;
            }
            else
            {
                std::cerr << "[libPackage][SELFTEST] FAIL: " << name << " for package '" << packageId << "' expected [";
                for (const auto &e : expected) std::cerr << e << " ";
                std::cerr << "] got [";
                for (const auto &r : result) std::cerr << r << " ";
                std::cerr << "]" << std::endl;
            }
        };

        // Chains:
        //   Application1_1.0 => com.rdkcentral.wpe_3.0 => com.rdkcentral.base_1.0
        //   Application2_2.0 => com.rdkcentral.wpe_3.0 => com.rdkcentral.base_1.0
        {
            std::map<std::string, std::unique_ptr<MountedPackageInfo> > mounted;
            mounted["Application1_1.0"] = makeEntry({"com.rdkcentral.wpe_3.0"});
            mounted["Application2_2.0"] = makeEntry({"com.rdkcentral.wpe_3.0"});
            mounted["com.rdkcentral.wpe_3.0"] = makeEntry({"com.rdkcentral.base_1.0"});
            mounted["com.rdkcentral.base_1.0"] = makeEntry({});

            runCase("new wpe version: both applications restart",
                    mounted, "com.rdkcentral.wpe", {"Application1", "Application2"});
            runCase("new base version: both applications restart",
                    mounted, "com.rdkcentral.base", {"Application1", "Application2"});
            runCase("new Application1 version: only Application1 restarts",
                    mounted, "Application1", {"Application1"});
            runCase("package that is not mounted: nothing to restart",
                    mounted, "com.rdkcentral.other", {});
            runCase("id prefix must not match partially (wp vs wpe)",
                    mounted, "com.rdkcentral.wp", {});
        }

        // Two applications running two different mounted versions of the same library:
        //   AppOne_1.0 => com.rdkcentral.wpe_3.0 => com.rdkcentral.base_1.0
        //   AppTwo_1.0 => com.rdkcentral.wpe_4.0 => com.rdkcentral.base_1.0
        {
            std::map<std::string, std::unique_ptr<MountedPackageInfo> > mounted;
            mounted["AppOne_1.0"] = makeEntry({"com.rdkcentral.wpe_3.0"});
            mounted["AppTwo_1.0"] = makeEntry({"com.rdkcentral.wpe_4.0"});
            mounted["com.rdkcentral.wpe_3.0"] = makeEntry({"com.rdkcentral.base_1.0"});
            mounted["com.rdkcentral.wpe_4.0"] = makeEntry({"com.rdkcentral.base_1.0"});
            mounted["com.rdkcentral.base_1.0"] = makeEntry({});

            runCase("any mounted wpe version matches: both applications restart",
                    mounted, "com.rdkcentral.wpe", {"AppOne", "AppTwo"});
        }

        // Diamond dependency:
        //   App_1.0 => {A_1.0, B_1.0}, A_1.0 => C_1.0, B_1.0 => C_1.0
        {
            std::map<std::string, std::unique_ptr<MountedPackageInfo> > mounted;
            mounted["App_1.0"] = makeEntry({"A_1.0", "B_1.0"});
            mounted["A_1.0"] = makeEntry({"C_1.0"});
            mounted["B_1.0"] = makeEntry({"C_1.0"});
            mounted["C_1.0"] = makeEntry({});

            runCase("diamond: new C version restarts the application once",
                    mounted, "C", {"App"});
            runCase("diamond: new A version restarts the application",
                    mounted, "A", {"App"});
        }

        // Library that is also locked directly (not only as a dependency):
        //   Player_1.0 => codec_2.0, codec_2.0 locked standalone as well
        {
            std::map<std::string, std::unique_ptr<MountedPackageInfo> > mounted;
            mounted["Player_1.0"] = makeEntry({"codec_2.0"});
            mounted["codec_2.0"] = makeEntry({});

            runCase("directly locked library used by an app: only the app is returned",
                    mounted, "codec", {"Player"});
        }

        // Realistic mixed setup, all sharing com.rdkcentral.base:
        //   com.rdkcentral.Netflix_1.0 => com.rdkcentral.base_1.0
        //   AppF1_1.0 => com.rdkcentral.flutter_2.0 => com.rdkcentral.base_1.0
        //   AppF2_1.0 => com.rdkcentral.flutter_2.0 => com.rdkcentral.base_1.0
        {
            std::map<std::string, std::unique_ptr<MountedPackageInfo> > mounted;
            mounted["com.rdkcentral.Netflix_1.0"] = makeEntry({"com.rdkcentral.base_1.0"});
            mounted["AppF1_1.0"] = makeEntry({"com.rdkcentral.flutter_2.0"});
            mounted["AppF2_1.0"] = makeEntry({"com.rdkcentral.flutter_2.0"});
            mounted["com.rdkcentral.flutter_2.0"] = makeEntry({"com.rdkcentral.base_1.0"});
            mounted["com.rdkcentral.base_1.0"] = makeEntry({});

            runCase("new base version: Netflix and both flutter apps restart",
                    mounted, "com.rdkcentral.base", {"com.rdkcentral.Netflix", "AppF1", "AppF2"});
            runCase("new flutter version: both flutter apps restart, Netflix does not",
                    mounted, "com.rdkcentral.flutter", {"AppF1", "AppF2"});
            runCase("new version of the application itself: only Netflix restarts",
                    mounted, "com.rdkcentral.Netflix", {"com.rdkcentral.Netflix"});
            runCase("new version of the application itself: only AppF2 restarts",
                    mounted, "AppF2", {"AppF2"});
        }

        std::cout << "[libPackage][SELFTEST] GetApplicationsToRestart self test finished" << std::endl;
    }
} // namespace packagemanager

#endif // RALF_PACKAGE_SELF_TEST
