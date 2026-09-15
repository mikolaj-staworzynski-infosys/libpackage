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

#pragma once
#include <map>
#include <vector>
#include <utility>
#include <memory>
#include <IPackageImpl.h>
#include <ralf/Package.h>
#include <ralf/VersionConstraint.h>

#include <sys/types.h> // For uid_t and gid_t

#ifndef DAC_APP_PATH
#define DAC_APP_PATH "/opt/media/apps/"
#endif

#ifndef RDK_PACKAGE_CERT_PATH
#define RDK_PACKAGE_CERT_PATH "/etc/rdk/certs"
#endif

#ifndef BUILD_REFERENCE
#define BUILD_REFERENCE "undefined"
#endif

#define RDK_PACKAGE_CONFIG_MIME_TYPE "application/vnd.rdk.package.config.v1+json"
#define RDK_PACKAGE_MOUNT_PATH "/tmp/mounts/"
#define RDK_PACKAGE_CONFIG "config.json"
namespace ralf = LIBRALF_NS;

namespace packagemanager
{
    struct MountedPackageInfo
    {
        // We need this to keep track of how many times a package is mounted. Otherwise we will unmount it too early
        int mountCount = 1;
        std::string pkgJsonPath;
        // Resolved "id_version" keys of the direct (one level down) dependencies locked together with
        // this package. Recorded at lock time so that unlock does not need to re-open the package file.
        // Deeper levels of the tree are stored in the entries of the dependencies themselves.
        std::vector<std::string> dependencies;
        std::unique_ptr<ralf::PackageMount> packageMount;
        void incMountCount() { mountCount++; }
        void decMountCount() { mountCount--; }
    };

    typedef struct _RalfPackageInfo
    {
        std::string pkgMountPath;
        std::string pkgMetaDataPath;
    } RalfPackageInfo;

    class RalfPackageImpl : public IPackageImpl
    {
    private:
        static int getInstalledPackages(std::vector<std::string> &pacakgeList);
        static void getPackageIdAndVersionFromRalfPackage(const std::string &packagePath, std::string &appId, std::string &appVersion);
        static bool enableDependencyCheck;

    public:
        ~RalfPackageImpl() override = default;
        RalfPackageImpl();

        Result Initialize(const std::string &configStr, ConfigMetadataArray &aConfigMetadata) override;

        /**
         * Installs the package from the given file. The package is fully verified
         * (signature and structure) before anything is written, staged under a temporary
         * name and atomically renamed into place as package.ralf.
         *
         * When fileLocator already points at the staged marker (package.ralf.install) in
         * the target <packageId>/<version> directory - the upgrade procedure's commit
         * path - the file is not copied at all, only renamed into place; a leftover
         * package.ralf.tmp staging file in that directory is removed.
         *
         * Recognized additionalMetadata keys:
         * - "COMMIT_INSTALL_SKIP_DEPENDENCY_CHECK" = "true": install without the dependency
         *   check. Control flag of the safe upgrade procedure's commit path - the commit
         *   applies staged markers in scan order, which cannot be dependency-ordered, and
         *   the package set was already validated when the commit plan was built. Not meant
         *   for regular installs.
         */
        Result Install(const std::string &packageId, const std::string &version, const NameValues &additionalMetadata, const std::string &fileLocator, ConfigMetaData &configMetadata) override;
        Result Uninstall(const std::string &packageId) override;

        Result Lock(const std::string &packageId, const std::string &version, std::string &unpackedPath, ConfigMetaData &configMetadata, NameValues &additionalLocks) override;
        Result Unlock(const std::string &packageId, const std::string &version) override;
        Result GetFileMetadata(const std::string &fileLocator, std::string &packageId, std::string &version, ConfigMetaData &configMetadata) override;
        Result VerifyPackage(const std::string &fileLocator) override;

        /**
         * Returns the ids of currently locked applications that must be restarted because they
         * are the given package or depend on it, directly or transitively.
         *
         * The package is matched by id only, deliberately without a version: running instances
         * are locked on the version that was installed at Lock time, which is typically older
         * than the version just installed. Matching on the new version would find nothing,
         * since that version is not mounted (locked) yet.
         */
        Result GetApplicationsToRestart(const std::string &packageId, std::vector<std::string> &applicationIds) override;
        Result Uninstall(const std::string &packageId, const std::string &version) override;

    private:
        // Flag to check initialisation status
        bool mIsInitialized = false;

        // Map to hold mountedPackages information.
        // Key is combination of package id and version.
        // Value is the MountedPackageInfo which has the mount point and other info.
        std::map<std::string, std::unique_ptr<MountedPackageInfo> > mMountedPackages;

        // RALF user id and group id. We will use these for setting the right permissions for the mounted package
        uid_t mUserId;
        gid_t mGroupId;

        // For package verification
        ralf::VerificationBundle mVerificationBundle;

        std::vector<std::unique_ptr<ConfigMetadataKey> > mInstalledPackages;

        /**
         * This function checks the dependencies of the given package and returns true if all dependencies are
         * satisfied; false otherwise. The dependency check is performed by reading the package metadata and
         * verifying that all required dependencies are installed and meet the version constraints specified
         * by the package. If any dependency is missing or does not satisfy the version constraint, the function
         * returns false.
         *
         * @param package The package whose dependencies are to be checked.
         * @return true if all dependencies are satisfied; false otherwise.
         */
        bool checkPackageDependencies(const ralf::Package &package);

        /**
         * Initializes the verification bundle by loading certificates from the specified directory.
         * @return true if at least one certificate was successfully loaded; false otherwise.
         */
        bool initializeVerificationBundle();

        /**
         * Rolls back the staged files of an interrupted prepare phase of the safe upgrade
         * procedure (see the marker constants in the source file). Called from Initialize,
         * before the installed packages are scanned. Runs only when STATE.prepare is present;
         * an interrupted commit (STATE.commit present) is left for the component above to
         * finish. Idempotent: a power outage during the rollback itself just means the next
         * boot repeats it with fewer files left.
         */
        void cleanupPreparedChanges();

        /**
         * Opens a package file and returns a Result containing the Package object.
         * Optionally performs full package verification.
         * @param packageFile The path to the package file.
         * @param performFullVerification If true, calls Package::verify() before returning success.
         * @return A Result containing the Package object if successful; an error otherwise.
         */
        ralf::Result<ralf::Package> openPackage(const std::string &packageFile, bool performFullVerification = false);

        /**
         * Locks the specified package for exclusive access. The package is verified, dependent packages are mounted,
         * and necessary resources are allocated.
         * @param package The package to be locked.
         * @param ralfMountInfo Output parameter to hold information about the mounted package.
         * @param configMetadata Metadata context for the lock operation, threaded through dependency locking and used by
         * the lock flow to populate capabilities.
         * @return true if the package is locked successfully; false otherwise.
         */
        bool lockPackage(const ralf::Package &package, std::vector<RalfPackageInfo> &ralfMountInfo, ConfigMetaData &configMetadata);

        /**
         * Releases a lock on the package identified by the given key. Decrements the mount count of the
         * package and, recursively, of the dependent packages recorded at lock time, unmounting any
         * package whose count reaches zero and removing its mount directory. Operates purely on the
         * in-memory mount table.
         * @param pkgVerKey The "id_version" key of the package to unlock.
         * @return true if the package and all its dependents were unlocked successfully; false otherwise.
         */
        bool unlockPackage(const std::string &pkgVerKey);

        /**
         * Core of GetApplicationsToRestart, operating on a given mount table instead of the
         * member one, so it can be exercised with synthetic dependency chains (self test).
         * Returns the ids of the locked applications (roots of the lock graph) that are the
         * given package or depend on it, directly or transitively.
         */
        static void findApplicationsToRestart(const std::string &packageId,
                                              const std::map<std::string, std::unique_ptr<MountedPackageInfo> > &mountedPackages,
                                              std::vector<std::string> &applicationIds);

#ifdef RALF_PACKAGE_SELF_TEST
        /**
         * Runs synthetic dependency-chain test cases through findApplicationsToRestart and
         * logs PASS/FAIL for each. Purely in-memory: no real packages or mounts are touched,
         * so it is safe to run on hardware.
         */
        static void runGetApplicationsToRestartSelfTest();
#endif

        /**
         * Identifies the installed version of a dependent package that satisfies the given version constraint.
         * @param depPackageId The ID of the dependent package.
         * @param depPackageVersion The version constraint for the dependent package.
         * @param depInstalledVersion Output parameter to hold the identified installed version.
         * @return true if a suitable installed version is found; false otherwise.
         */
        bool identifyDependencyVersion(const std::string &depPackageId, const ralf::VersionConstraint &depPackageVersion, std::string &depInstalledVersion);

        /**
         * Dumps the package information to a specified configuration path.
         * @param package The package whose information is to be dumped.
         * @param configPath The path where the package information will be dumped.
         * @return true if the package information is dumped successfully; false otherwise.
         */
        bool dumpPackageInfo(const ralf::Package &package, const std::filesystem::path &configPath);

        /**
         * Serializes the list of mounted packages to a JSON file at the specified output path.
         * @param mountPkgList The list of mounted packages to be serialized.
         * @param outputPath The path where the JSON file will be created.
         * @return true if serialization is successful; false otherwise.
         */
        bool serializeToJson(const std::vector<RalfPackageInfo> &mountPkgList, const std::filesystem::path &outputPath) const;

        /**
         * Returns the group id and user id of the ralf user. This is needed for setting the right permissions for the mounted package
         * @param userId  Out parameter holding user id
         * @param groupId Out parameter holding group id
         * @return true if user id and group id are successfully retrieved; false otherwise.
         */
        static bool getRalfUserInfo(uid_t &userId, gid_t &groupId);

        /**
         * Adds the permissions from the package metadata to the configuration metadata.
         * @param pkgMetadata The package metadata whose permissions are to be added.
         * @param configMetadata The configuration metadata to which the permissions will be added.
         */
        void addPackagePermissionsToConfigMetadata(const ralf::PackageMetaData &pkgMetadata, ConfigMetaData &configMetadata);
    };
}
