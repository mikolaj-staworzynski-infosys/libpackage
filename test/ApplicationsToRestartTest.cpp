// ApplicationsToRestartTest.cpp - PC gtest suite for findApplicationsToRestart
// (see ApplicationsToRestart.h). Synthetic in-memory mount graphs only; no real
// packages or mounts are touched. Mirrors the cases of the libpackage self
// test, adapted to structured (packageId, version) keys.

#include "ApplicationsToRestart.h"

#include <gtest/gtest.h>

namespace
{
    using packagemanager::ConfigMetadataKey;
    using packagemanager::findApplicationsToRestart;    using Graph = std::map<ConfigMetadataKey, std::vector<ConfigMetadataKey> >;

    std::vector<std::string> restartFor(const Graph &graph, const std::string &packageId)
    {
        std::vector<std::string> result;
        findApplicationsToRestart(packageId, graph, result);
        return result; // set-backed, hence sorted
    }

    // Chains:
    //   Application1 1.0 -> wpe 3.0 -> base 1.0
    //   Application2 2.0 -> wpe 3.0 -> base 1.0
    TEST(FindApplicationsToRestart, SharedLibraryChain)
    {
        const Graph graph = {
            {{"Application1", "1.0"}, {{"com.rdkcentral.wpe", "3.0"}}},
            {{"Application2", "2.0"}, {{"com.rdkcentral.wpe", "3.0"}}},
            {{"com.rdkcentral.wpe", "3.0"}, {{"com.rdkcentral.base", "1.0"}}},
            {{"com.rdkcentral.base", "1.0"}, {}},
        };

        EXPECT_EQ(restartFor(graph, "com.rdkcentral.wpe"),
                  (std::vector<std::string>{"Application1", "Application2"}));
        EXPECT_EQ(restartFor(graph, "com.rdkcentral.base"),
                  (std::vector<std::string>{"Application1", "Application2"}));
        EXPECT_EQ(restartFor(graph, "Application1"),
                  (std::vector<std::string>{"Application1"}));
        EXPECT_TRUE(restartFor(graph, "com.rdkcentral.other").empty());
        // Structured keys: a different id sharing a prefix can never match -
        // no "id_" separator tricks needed ("wp" vs "wpe").
        EXPECT_TRUE(restartFor(graph, "com.rdkcentral.wp").empty());
    }

    // Two applications running two DIFFERENT mounted versions of the same
    // library: AppOne -> wpe 3.0, AppTwo -> wpe 4.0. The id-only match must
    // seed the BFS from both mounted versions.
    TEST(FindApplicationsToRestart, MultipleMountedVersionsOfOnePackage)
    {
        const Graph graph = {
            {{"AppOne", "1.0"}, {{"com.rdkcentral.wpe", "3.0"}}},
            {{"AppTwo", "1.0"}, {{"com.rdkcentral.wpe", "4.0"}}},
            {{"com.rdkcentral.wpe", "3.0"}, {{"com.rdkcentral.base", "1.0"}}},
            {{"com.rdkcentral.wpe", "4.0"}, {{"com.rdkcentral.base", "1.0"}}},
            {{"com.rdkcentral.base", "1.0"}, {}},
        };

        EXPECT_EQ(restartFor(graph, "com.rdkcentral.wpe"),
                  (std::vector<std::string>{"AppOne", "AppTwo"}));
    }

    // Diamond: App -> {A, B}, A -> C, B -> C. A change deep in the diamond
    // reaches the application exactly once.
    TEST(FindApplicationsToRestart, Diamond)
    {
        const Graph graph = {
            {{"App", "1.0"}, {{"A", "1.0"}, {"B", "1.0"}}},
            {{"A", "1.0"}, {{"C", "1.0"}}},
            {{"B", "1.0"}, {{"C", "1.0"}}},
            {{"C", "1.0"}, {}},
        };

        EXPECT_EQ(restartFor(graph, "C"), (std::vector<std::string>{"App"}));
        EXPECT_EQ(restartFor(graph, "A"), (std::vector<std::string>{"App"}));
    }

    // A library that is ALSO locked directly (not only as a dependency):
    // Player -> codec, codec locked standalone as well. Only the application
    // (the root) is reported - the library itself is not a restart candidate.
    TEST(FindApplicationsToRestart, DirectlyLockedLibraryIsNotARoot)
    {
        const Graph graph = {
            {{"Player", "1.0"}, {{"codec", "2.0"}}},
            {{"codec", "2.0"}, {}},
        };

        EXPECT_EQ(restartFor(graph, "codec"), (std::vector<std::string>{"Player"}));
    }

    // Realistic mixed setup, all sharing base:
    //   Netflix 1.0 -> base 1.0
    //   AppF1 1.0 -> flutter 2.0 -> base 1.0
    //   AppF2 1.0 -> flutter 2.0 -> base 1.0
    TEST(FindApplicationsToRestart, RealisticMixedSetup)
    {
        const Graph graph = {
            {{"com.rdkcentral.Netflix", "1.0"}, {{"com.rdkcentral.base", "1.0"}}},
            {{"AppF1", "1.0"}, {{"com.rdkcentral.flutter", "2.0"}}},
            {{"AppF2", "1.0"}, {{"com.rdkcentral.flutter", "2.0"}}},
            {{"com.rdkcentral.flutter", "2.0"}, {{"com.rdkcentral.base", "1.0"}}},
            {{"com.rdkcentral.base", "1.0"}, {}},
        };

        EXPECT_EQ(restartFor(graph, "com.rdkcentral.base"),
                  (std::vector<std::string>{"AppF1", "AppF2", "com.rdkcentral.Netflix"}));
        EXPECT_EQ(restartFor(graph, "com.rdkcentral.flutter"),
                  (std::vector<std::string>{"AppF1", "AppF2"}));
        EXPECT_EQ(restartFor(graph, "com.rdkcentral.Netflix"),
                  (std::vector<std::string>{"com.rdkcentral.Netflix"}));
        EXPECT_EQ(restartFor(graph, "AppF2"), (std::vector<std::string>{"AppF2"}));
    }
}
