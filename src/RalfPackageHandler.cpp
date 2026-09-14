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

#include "RalfPackageImpl.h"
#include <iostream>
#include <filesystem>
#include <set>
#include <ralf/PackageMount.h>
#include <ralf/PackageMetaData.h>
#include <ralf/VersionNumber.h>
#include <json/json.h>
#include <fstream>

#include <algorithm>
#include <cstdint>

#include <fcntl.h>  // For open()
#include <unistd.h> // For fsync()/close()
#include <pwd.h> //For getting user id and group id of ralf user

namespace
{
    static constexpr const char *RALF_USER_NAME = "ralf";
    static constexpr const char *AppInstallationPath = DAC_APP_PATH;
    static constexpr const char *RalfPackage = "package.ralf";
    static constexpr const char *pkgCertDirPath = RDK_PACKAGE_CERT_PATH;
    static constexpr const char *BuildReference = BUILD_REFERENCE;

    // Safe upgrade procedure markers (created/removed by the component above, e.g. the
    // PackageManager plugin or CatalogInstaller). Layout, relative to AppInstallationPath:
    //   ./<packageId>/<version>/package.ralf.install - staged package, invisible until renamed to package.ralf
    //   ./<packageId>/<version>/package.ralf.remove  - empty marker: remove this version on commit
    //   ./STATE.prepare                              - prepare phase ongoing; present at boot = roll back
    //   ./STATE.commit                               - commit pending; present at boot = finish it
    static constexpr const char *RalfPackageInstallMarker = "package.ralf.install";
    static constexpr const char *RalfPackageRemoveMarker = "package.ralf.remove";
    static constexpr const char *StatePrepareFile = "STATE.prepare";
    static constexpr const char *StateCommitFile = "STATE.commit";

    // Flushes a file's (or directory's) data and metadata to disk. Used to make the
    // staged package file durable before atomically renaming it into place.
    static bool syncFile(const std::filesystem::path &path)
    {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0)
        {
            return false;
        }
        bool ok = (::fsync(fd) == 0);
        ::close(fd);
        return ok;
    }
}
namespace packagemanager
{
#ifdef DISABLE_DEPENDENCY_CHECK
    bool RalfPackageImpl::enableDependencyCheck = false;
#else
    bool RalfPackageImpl::enableDependencyCheck = true;
#endif

    RalfPackageImpl::RalfPackageImpl()
    {
        std::cout << "[libPackage] Code revision : " << BuildReference << std::endl;
#ifdef RALF_PACKAGE_SELF_TEST
        runGetApplicationsToRestartSelfTest();
#endif
    }
    int RalfPackageImpl::getInstalledPackages(std::vector<std::string> &pacakgeList)
    {
        std::cout << "[libPackage] Looking for installed packages in  " << AppInstallationPath << std::endl;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(AppInstallationPath))
        {
            if (std::filesystem::is_regular_file(entry.path()) && entry.path().filename() == RalfPackage)
            {
                pacakgeList.push_back(entry.path().string());
            }
        }
        return pacakgeList.size();
    }

    bool RalfPackageImpl::identifyDependencyVersion(const std::string &depPackageId, const ralf::VersionConstraint &depPackageVersion, std::string &depInstalledVersion)
    {
        for (const auto &pkgInfo : mInstalledPackages)
        {
            if (pkgInfo->first == depPackageId)
            {
                // See of the package version associated works
                const auto &pkgversion = pkgInfo->second;
                auto result = ralf::VersionNumber::fromString(pkgversion);
                if (!result)
                {
                    std::cerr << "[libPackage] Failed to parse version: " << pkgversion << std::endl;
                    continue;
                }
                ralf::VersionNumber versionNumber = result.value();
                if (depPackageVersion.isSatisfiedBy(versionNumber))
                {
                    depInstalledVersion = pkgversion;
                    return true;
                }
            }
        }
        return false;
    }

    void RalfPackageImpl::getPackageIdAndVersionFromRalfPackage(const std::string &packagePath, std::string &appId, std::string &appVersion)
    {
        std::filesystem::path p(packagePath);
        auto parentPath = p.parent_path();
        appVersion = parentPath.filename().string();
        auto grandParentPath = parentPath.parent_path();
        appId = grandParentPath.filename().string();
    }

    std::shared_ptr<IPackageImpl> IPackageImpl::instance()
    {
        std::shared_ptr<IPackageImpl> packageImpl = std::make_shared<RalfPackageImpl>();
        return packageImpl;
    }

    /**
     * The following activities are performed.
     * 1. Make sure the app installation path exists
     * 2. If path already exists, read the metadata of all installed packages and populate in configMetadata
     */
    Result RalfPackageImpl::Initialize(const std::string &configStr, ConfigMetadataArray &aConfigMetadata)
    {
        std::cout << "[libPackage] RalfPackageImpl::Initialize called with config: " << configStr << std::endl;
        if (getRalfUserInfo(mUserId, mGroupId))
        {
            std::cout << "[libPackage] Ralf user id and group id : " << mUserId << ", " << mGroupId << std::endl;
        }
        else
        {
            std::cerr << "[libPackage] Failed to get Ralf user info. Initialization failed." << std::endl;
            return Result::FAILED;
        }
        // Check if AppInstallationPath exists
        if (!initializeVerificationBundle())
        {
            std::cerr << "[libPackage] Failed to initialize verification bundle. No certificates loaded from: " << pkgCertDirPath << std::endl;
            return Result::FAILED;
        }
        if (!std::filesystem::exists(AppInstallationPath))
        {
            std::cout << "[libPackage] App installation path does not exist. Creating: " << AppInstallationPath << std::endl;
            std::filesystem::create_directories(AppInstallationPath);
        }
        else
        {
            // Recover from an interrupted prepare phase of the upgrade procedure before
            // scanning, so the scan below sees the final package set. An interrupted commit
            // (STATE.commit present) is intentionally left untouched here - it is finished
            // by the component above; the staged markers are invisible to the scan anyway
            // (only package.ralf is picked up).
            cleanupPreparedChanges();

            std::vector<std::string> installedPackages;
            // Let us get package metadata of all installed packages
            auto count = getInstalledPackages(installedPackages);
            std::cout << "[libPackage] Found " << count << " installed packages." << std::endl;

            for (auto packagePath : installedPackages)
            {
                std::string appId, appVersion;
                getPackageIdAndVersionFromRalfPackage(packagePath, appId, appVersion);
                mInstalledPackages.push_back(std::make_unique<ConfigMetadataKey>(std::make_pair(appId, appVersion)));
                std::cout << "[libPackage] Found installed package: " << appId << ", version: " << appVersion << std::endl;
                ConfigMetaData configMetadata;
                configMetadata.appPath = std::filesystem::path(packagePath);
                configMetadata.userId = mUserId;   // Ralf user id.
                configMetadata.groupId = mGroupId; // Ralf user group.

                ConfigMetadataKey appKey = {appId, appVersion};
                aConfigMetadata[appKey] = configMetadata;
            }
        }
        mIsInitialized = true;
        return Result::SUCCESS;
    }
    bool RalfPackageImpl::initializeVerificationBundle()
    {
        bool certLoaded = false;
        std::cout << "[libPackage] Initializing verification bundle from certificates in: " << pkgCertDirPath << std::endl;
        // Check whether directory exists
        if (!std::filesystem::exists(pkgCertDirPath))
        {
            std::cerr << "[libPackage] Certificate directory does not exist: " << pkgCertDirPath << std::endl;
            return false;
        }
        // Load the certificate from pkgCertDirPath
        // Iterate through all the certificates in the directory
        std::filesystem::directory_options options = std::filesystem::directory_options::skip_permission_denied;
        std::error_code ec;
        std::filesystem::directory_iterator dirIter(pkgCertDirPath, options, ec);
        if (ec)
        {
            std::cerr << "[libPackage] Error accessing certificate directory: " << ec.message() << std::endl;
            return false;
        }
        for (auto const &dirEntry : dirIter)
        {
            if (dirEntry.is_regular_file())
            {
                std::ifstream certFile(dirEntry.path());
                auto result = ralf::Certificate::loadFromFile(dirEntry.path().string());
                if (!result)
                {
                    std::cerr << "[libPackage] Failed to load certificate from file: " << dirEntry.path() << " Error: " << result.error().what() << std::endl;
                    continue;
                }
                mVerificationBundle.addCertificate(result.value());
                certLoaded = true;

                std::cout << "[libPackage] Successfully added certificate from: " << dirEntry.path() << " to verification bundle." << std::endl;
            }
        }
        return certLoaded;
    }

    void RalfPackageImpl::cleanupPreparedChanges()
    {
        // Rollback sequence of the safe upgrade procedure, run at startup before the
        // installed packages are scanned. Condition: STATE.prepare exists, meaning the
        // prepare phase did not finish and everything staged in the background has to be
        // removed from the flash filesystem:
        //   (1) remove all package.ralf.remove markers found in all subdirectories
        //   (2) remove all package.ralf.install files and the (now empty) id/version dirs
        //   (3) remove STATE.prepare itself
        // STATE.prepare is removed only as the last step, so if a power outage interrupts
        // this sequence, the next boot simply repeats it with fewer files left.
        namespace fs = std::filesystem;
        const fs::path installPath(AppInstallationPath);
        std::error_code errorCode;

        const fs::path prepareMarker = installPath / StatePrepareFile;
        if (!fs::exists(prepareMarker, errorCode))
        {
            if (fs::exists(installPath / StateCommitFile, errorCode))
            {
                // Not ours to finish here: committing means real installs/removals with
                // dependency ordering and bookkeeping by the component above. The staged
                // markers are invisible to the package scan (only package.ralf is picked
                // up), so leaving them in place is harmless.
                std::cout << "[libPackage] Found " << StateCommitFile
                          << " - interrupted commit phase, leaving staged changes to be applied" << std::endl;
            }
            return;
        }

        std::cout << "[libPackage] Found " << StatePrepareFile
                  << " - interrupted prepare phase, rolling back staged changes" << std::endl;

        // Collect all staged markers first, so that removals never disturb the iteration
        std::vector<fs::path> installMarkers, removeMarkers;
        fs::recursive_directory_iterator it(installPath, fs::directory_options::skip_permission_denied, errorCode);
        fs::recursive_directory_iterator end;
        for (; !errorCode && it != end; it.increment(errorCode))
        {
            if (!it->is_regular_file(errorCode))
            {
                continue;
            }
            const auto filename = it->path().filename();
            if (filename == RalfPackageInstallMarker)
            {
                installMarkers.push_back(it->path());
            }
            else if (filename == RalfPackageRemoveMarker)
            {
                removeMarkers.push_back(it->path());
            }
        }

        // (1) + (2). Every directory that lost an entry has to be fsynced, otherwise
        // a power outage can resurrect an already "removed" marker while STATE.prepare
        // is already gone - and the rollback would never run again.
        bool removalFailed = false;
        std::set<fs::path> dirsToSync;
        for (const auto &marker : removeMarkers)
        {
            std::cout << "[libPackage] Rolling back staged remove marker: " << marker << std::endl;
            fs::remove(marker, errorCode);
            if (errorCode)
            {
                std::cerr << "[libPackage] Failed to remove " << marker << ": " << errorCode.message() << std::endl;
                removalFailed = true;
                errorCode.clear();
            }
            else
            {
                dirsToSync.insert(marker.parent_path());
            }
        }
        for (const auto &marker : installMarkers)
        {
            std::cout << "[libPackage] Rolling back staged package file: " << marker << std::endl;
            fs::remove(marker, errorCode);
            if (errorCode)
            {
                std::cerr << "[libPackage] Failed to remove " << marker << ": " << errorCode.message() << std::endl;
                removalFailed = true;
                errorCode.clear();
            }
            else
            {
                dirsToSync.insert(marker.parent_path());
            }
        }
        // Make the marker removals durable (bottom-up: version dirs first)
        for (const auto &dir : dirsToSync)
        {
            syncFile(dir);
        }

        // Remove the (now empty) <id>/<version> directories created during the prepare
        // phase, fsyncing each parent after its child is gone
        for (const auto &idEntry : fs::directory_iterator(installPath, fs::directory_options::skip_permission_denied, errorCode))
        {
            if (!idEntry.is_directory())
            {
                continue;
            }
            bool versionDirRemoved = false;
            for (const auto &verEntry : fs::directory_iterator(idEntry.path(), fs::directory_options::skip_permission_denied, errorCode))
            {
                if (verEntry.is_directory() && fs::is_empty(verEntry.path(), errorCode))
                {
                    fs::remove(verEntry.path(), errorCode);
                    if (errorCode)
                    {
                        std::cerr << "[libPackage] Failed to remove " << verEntry.path() << ": " << errorCode.message() << std::endl;
                        removalFailed = true;
                        errorCode.clear();
                    }
                    else
                    {
                        versionDirRemoved = true;
                    }
                }
            }
            if (versionDirRemoved)
            {
                syncFile(idEntry.path());
            }
            if (fs::is_empty(idEntry.path(), errorCode))
            {
                fs::remove(idEntry.path(), errorCode);
                if (errorCode)
                {
                    std::cerr << "[libPackage] Failed to remove " << idEntry.path() << ": " << errorCode.message() << std::endl;
                    removalFailed = true;
                    errorCode.clear();
                }
                else
                {
                    syncFile(installPath);
                }
            }
        }

        // (3) STATE.prepare is removed only when everything staged is really gone - it is
        // the trigger for this rollback, so a failure above must leave it in place for the
        // next boot to retry
        if (!removalFailed)
        {
            fs::remove(prepareMarker, errorCode);
            if (errorCode)
            {
                std::cerr << "[libPackage] Failed to remove " << prepareMarker << ": " << errorCode.message() << std::endl;
                errorCode.clear();
            }
        }
        syncFile(installPath);
    }

    Result RalfPackageImpl::Install(const std::string &packageId, const std::string &version, const NameValues &additionalMetadata, const std::string &fileLocator, ConfigMetaData &configMetadata)
    {
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::Install called before initialization." << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] RalfPackageImpl::Install called with packageId: " << packageId << ", version: " << version << ", fileLocator: " << fileLocator << std::endl;
        auto package = openPackage(fileLocator, true);
        if (!package)
        {
            std::cerr << "[libPackage] Package verification failed for: " << fileLocator << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] Successfully opened package: " << fileLocator << std::endl;

        // Control flag for the commit path of the safe upgrade procedure: the commit applies
        // staged markers in scan order, which cannot be dependency-ordered, and the set was
        // already validated when the commit plan was built - so the per-package dependency
        // check is skipped for those installs.
        bool skipDependencyCheck = std::any_of(additionalMetadata.begin(), additionalMetadata.end(),
                                               [](const NameValue &nv)
                                               { return nv.first == "COMMIT_INSTALL_SKIP_DEPENDENCY_CHECK" && nv.second == "true"; });
        if (enableDependencyCheck && !skipDependencyCheck)
        {
            if (!checkPackageDependencies(package.value()))
                return Result::FAILED;
        }

        // Create the directory structure
        auto packagePath = std::filesystem::path(AppInstallationPath) / packageId / version;
        std::filesystem::create_directories(packagePath);

        // Install the package by staging it in a temporary file and atomically renaming it
        // into place. If the package is currently mounted (loop device + dm-verity), the loop
        // device keeps the old inode alive, so a running app keeps using the old image until it
        // is unmounted, while future mounts pick up the new file. Overwriting the file in place
        // (copy_file with overwrite_existing directly on the destination) would corrupt the
        // live mount.
        auto destRalfPackagePath = packagePath / RalfPackage;
        auto tempRalfPackagePath = packagePath / (std::string(RalfPackage) + ".tmp");
        try
        {
            std::filesystem::copy_file(fileLocator, tempRalfPackagePath, std::filesystem::copy_options::overwrite_existing);
            if (std::filesystem::exists(destRalfPackagePath))
            {
                // Swapping an existing installation: keep the original file permissions
                std::filesystem::permissions(tempRalfPackagePath, std::filesystem::status(destRalfPackagePath).permissions());
            }
            if (!syncFile(tempRalfPackagePath))
            {
                std::cerr << "[libPackage] Failed to sync package file to disk: " << tempRalfPackagePath << std::endl;
                std::filesystem::remove(tempRalfPackagePath);
                return Result::FAILED;
            }
            std::filesystem::rename(tempRalfPackagePath, destRalfPackagePath);
            syncFile(packagePath); // Flush the directory entry so the rename is durable

            auto appPath = destRalfPackagePath.string();
            configMetadata.appPath = std::move(appPath);
            configMetadata.userId = mUserId;   // Ralf user id.
            configMetadata.groupId = mGroupId; // Ralf user group.
            std::cout << "[libPackage] Installed package to: " << configMetadata.appPath << std::endl;
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            // Log error
            std::cerr
                << "[libPackage] Error installing package: " << e.what() << std::endl;
            std::error_code ec;
            std::filesystem::remove(tempRalfPackagePath, ec);
            return Result::FAILED;
        }
        // On a re-install (file swap) of an already known package, do not register it twice
        auto isAlreadyRegistered = std::any_of(mInstalledPackages.begin(), mInstalledPackages.end(),
                                               [&packageId, &version](const std::unique_ptr<ConfigMetadataKey> &entry)
                                               { return entry->first == packageId && entry->second == version; });
        if (!isAlreadyRegistered)
        {
            mInstalledPackages.push_back(std::make_unique<ConfigMetadataKey>(std::make_pair(packageId, version)));
        }
        return Result::SUCCESS;
    }
    bool RalfPackageImpl::checkPackageDependencies(const ralf::Package &package)
    {
        bool status = true;

        std::cout << "[libPackage] [DEPENDENCY_CHECK] Dependency check is enabled." << std::endl;
        auto pkgMetadata = package.metaData();
        if (pkgMetadata)
        {
            auto dependencies = pkgMetadata->dependencies();
            for (const auto &dependency : dependencies)
            {
                // Identify the dependency
                auto depPackageId = dependency.first;
                auto depPkgVersion = dependency.second;
                std::string depInstalledVersion;

                if (!identifyDependencyVersion(depPackageId, depPkgVersion, depInstalledVersion))
                {
                    std::cerr << "[libPackage] [DEPENDENCY_CHECK] Failed to identify dependency version for package: " << depPackageId << std::endl;
                    status = false;
                    break;
                }
            }
        }
        else
        {
            // Log error
            std::cerr
                << "[libPackage] [DEPENDENCY_CHECK] Failed to read package metadata: " << pkgMetadata.error().what() << std::endl;
            status = false;
        }
        std::cout << "[libPackage] Successfully identified dependencies for package: " << package.id() << std::endl;
        return status;
    }

    Result RalfPackageImpl::Uninstall(const std::string &packageId)
    {
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::Uninstall called before initialization." << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] RalfPackageImpl::Uninstall called with packageId: " << packageId << std::endl;
        // For the time being, we have to remove all the files in the package installation path, until we get a version with specific version to uninstall
        auto packagePath = std::filesystem::path(AppInstallationPath) / packageId;
        try
        {
            std::filesystem::remove_all(packagePath);
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            // Log error
            std::cerr
                << "[libPackage] Error uninstalling package: " << e.what() << std::endl;
            return Result::FAILED;
        }
        // Drop the package (all versions) from the in-memory registry, so that dependency
        // resolution no longer resolves to files that do not exist anymore
        mInstalledPackages.erase(
            std::remove_if(mInstalledPackages.begin(), mInstalledPackages.end(),
                           [&packageId](const std::unique_ptr<ConfigMetadataKey> &entry)
                           { return entry->first == packageId; }),
            mInstalledPackages.end());
        return Result::SUCCESS;
    }

    /**
     * The following steps are performed
     * 1. Get dependency list first.
     * For each dependency
     *  2. See if the packageInformation file is present. If so this package is already mounted once. No need to go any deeper.
     *  3. If not present, open the package, read the metadata and identify dependencies and dump the dependency data in packageInformation file.
     *  4. Check if the package is already mounted. IF so we need to increase the mount count of each dependency package.
     * 5. If not mounted, mount the package and all its dependencies and set mount count to 1.
     * 6. Return the mount point of the main package.
     */

    Result RalfPackageImpl::Lock(const std::string &packageId, const std::string &version, std::string &unpackedPath, ConfigMetaData &configMetadata, NameValues &additionalLocks)
    {
        std::cout << "[libPackage] RalfPackageImpl::Lock called with packageId: " << packageId << ", version: " << version << std::endl;

        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::Lock called before initialization." << std::endl;
            return Result::FAILED;
        }
        // Step 1: Determine the package path
        auto packagePath = std::filesystem::path(AppInstallationPath) / packageId / version / RalfPackage;
        auto package = openPackage(packagePath);
        if (!package)
        {
            std::cerr << "[libPackage] Failed to open package for locking: " << packagePath.string() << std::endl;
            return Result::FAILED;
        }

        std::vector<RalfPackageInfo> mountPkgList;
        auto status = lockPackage(package.value(), mountPkgList, configMetadata);
        if (status)
        {
            // We need to dump this to a temp file and add it as par of configMetadata
            auto tempFilePath = std::filesystem::temp_directory_path() / (packageId + "_" + version + "_metadata.json");
            if (serializeToJson(mountPkgList, tempFilePath))
            {
                std::cout << "[libPackage] Successfully serialized mount package list to: " << tempFilePath << std::endl;
                configMetadata.ralfPkgPath = tempFilePath.string();
                unpackedPath = packagePath.parent_path().string();
                return Result::SUCCESS;
            }
        }
        return Result::FAILED;
    }

    Result RalfPackageImpl::Unlock(const std::string &packageId, const std::string &version)
    {
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::Unlock called before initialization." << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] RalfPackageImpl::Unlock called with packageId: " << packageId << ", version: " << version << std::endl;

        // The dependency tree resolved at Lock time is stored in mMountedPackages, so there is
        // no need to re-open (and re-verify) the package file to release the lock.
        bool unmountResult = unlockPackage(packageId + "_" + version);

        return unmountResult ? Result::SUCCESS : Result::FAILED;
    }

    Result RalfPackageImpl::GetApplicationsToRestart(const std::string &packageId, std::vector<std::string> &applicationIds)
    {
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::GetApplicationsToRestart called before initialization." << std::endl;
            return Result::FAILED;
        }
        findApplicationsToRestart(packageId, mMountedPackages, applicationIds);
        std::cout << "[libPackage] GetApplicationsToRestart for packageId: " << packageId
                  << " -> " << applicationIds.size() << " application(s) to restart" << std::endl;
        return Result::SUCCESS;
    }

    void RalfPackageImpl::findApplicationsToRestart(const std::string &packageId,
                                                    const std::map<std::string, std::unique_ptr<MountedPackageInfo> > &mountedPackages,
                                                    std::vector<std::string> &applicationIds)
    {

        // Example trace. Mounted lock chains (keys are "id_version"):
        //   Application1_1.0      => com.rdkcentral.wpe_3.0 => com.rdkcentral.base_1.0
        //   Application2_2.0      => com.rdkcentral.wpe_3.0 => com.rdkcentral.base_1.0
        // A new version of com.rdkcentral.wpe was just installed, so the service calls
        // GetApplicationsToRestart("com.rdkcentral.wpe").
        //
        // 1) After building the reverse lock graph (dependency -> dependents):
        //      dependents = {
        //          "com.rdkcentral.wpe_3.0":  [ "Application1_1.0", "Application2_2.0" ],
        //          "com.rdkcentral.base_1.0": [ "com.rdkcentral.wpe_3.0" ]
        //      }
        //
        // 2) After seeding with the mounted versions of the package (note: the MOUNTED
        //    version 3.0, not the just-installed one, which is not locked by anyone yet):
        //      pending = [ "com.rdkcentral.wpe_3.0" ]
        //
        // 3) After the BFS upwards through all dependents:
        //      affected = { "com.rdkcentral.wpe_3.0", "Application1_1.0", "Application2_2.0" }
        //    (com.rdkcentral.base is NOT affected: it does not depend on wpe, wpe depends on it)
        //
        // 4) After filtering to roots (affected packages that nothing else depends on)
        //    and stripping the versions:
        //      "com.rdkcentral.wpe_3.0" has dependents        -> skipped (it is a library;
        //                                                        restarting the apps below
        //                                                        refreshes it anyway)
        //      "Application1_1.0", "Application2_2.0"         -> roots
        //      applicationIds = [ "Application1", "Application2" ]
        std::map<std::string, std::vector<std::string> > dependents;
        for (const auto &entry : mountedPackages)
        {
            for (const auto &dep : entry.second->dependencies)
            {
                dependents[dep].push_back(entry.first);
            }
        }

        // Walk upwards from every mounted version of the package (running instances are
        // typically on an older version than the one just installed) and collect the
        // whole affected subgraph
        std::set<std::string> affected;
        std::vector<std::string> pending;
        for (const auto &entry : mountedPackages)
        {
            if (entry.first.compare(0, packageId.size() + 1, packageId + "_") == 0)
            {
                pending.push_back(entry.first);
            }
        }
        while (!pending.empty())
        {
            const std::string key = pending.back();
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

        // Return the roots of the affected subgraph: locked packages that nothing else
        // depends on. Those are the applications; restarting them refreshes the whole
        // dependency chain, so intermediate libraries are not reported.
        std::set<std::string> appIds;
        for (const auto &key : affected)
        {
            if (dependents.find(key) == dependents.end())
            {
                appIds.insert(key.substr(0, key.rfind('_')));
            }
        }
        applicationIds.assign(appIds.begin(), appIds.end());
    }

    Result RalfPackageImpl::GetFileMetadata(const std::string &fileLocator, std::string &packageId, std::string &version, ConfigMetaData &configMetadata)
    {
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::GetFileMetadata called before initialization." << std::endl;
            return Result::FAILED;
        }
        auto package = openPackage(fileLocator);
        if (!package)
        {
            std::cerr << "[libPackage] Failed to open package for getting file metadata: " << fileLocator << std::endl;
            return Result::FAILED;
        }

        auto packagePath = std::filesystem::path(fileLocator);
        packageId = package->id();
        version = package->version().toString();
        configMetadata.appPath = packagePath.string();
        configMetadata.userId = mUserId;   // Ralf user id.
        configMetadata.groupId = mGroupId; // Ralf user group.
        auto pkgMetadata = package->metaData();
        if (pkgMetadata)
            addPackagePermissionsToConfigMetadata(pkgMetadata.value(), configMetadata);
        return Result::SUCCESS;
    }

    Result RalfPackageImpl::VerifyPackage(const std::string &fileLocator)
    {
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::VerifyPackage called before initialization." << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] RalfPackageImpl::VerifyPackage called with fileLocator: " << fileLocator << std::endl;

        auto package = openPackage(fileLocator, true);
        if (!package)
        {
            std::cerr << "[libPackage] Package verification failed: " << fileLocator << std::endl;
            return Result::VERIFICATION_FAILURE;
        }
        return Result::SUCCESS;
    }

    bool RalfPackageImpl::lockPackage(const ralf::Package &package, std::vector<RalfPackageInfo> &ralfMountInfo, ConfigMetaData &configMetadata)
    {
        bool status = false;
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::lockPackage called before initialization." << std::endl;
            return false;
        }
        auto packageId = package.id();
        auto version = package.version().toString();
        std::cout << "[libPackage] Locking packages." << packageId << ", version " << version << std::endl;

        std::string pkgVerKey = packageId + "_" + version;

        auto pkgMetadata = package.metaData();
        if (!pkgMetadata)
        {
            std::cerr << "[libPackage] Failed to read package metadata for locking dependencies: " << pkgMetadata.error().what() << std::endl;
            return false;
        }

        status = true;
        // Keys of the dependent packages locked by this call. Used for rollback on failure and
        // stored in the mount table on success, so Unlock can release them without re-opening files.
        std::vector<std::string> lockedDependencies;
        // Let us process dependencies first
        auto dependencies = pkgMetadata->dependencies();
        for (const auto &dependency : dependencies)
        {
            std::string depPackageId = dependency.first;

            auto depPkgVersion = dependency.second;
            std::string depInstalledVersion;
            if (!identifyDependencyVersion(depPackageId, depPkgVersion, depInstalledVersion))
            {
                std::cerr << "[libPackage] Failed to identify dependency version for package: " << depPackageId << std::endl;
                status = false;
                break;
            }

            auto fileLocator = std::filesystem::path(AppInstallationPath) / depPackageId / depInstalledVersion / RalfPackage;
            auto depPackage = openPackage(fileLocator);
            if (!depPackage)
            {
                std::cerr << "[libPackage] Failed to open package for locking: " << fileLocator.string() << std::endl;
                status = false;
                break;
            }

            if (!lockPackage(depPackage.value(), ralfMountInfo, configMetadata))
            {
                std::cerr << "[libPackage] Failed to lock dependent package: " << depPackageId << std::endl;
                status = false;
                break;
            }
            lockedDependencies.push_back(depPackageId + "_" + depInstalledVersion);
        }
        if (!status)
        {
            for (const auto &depKey : lockedDependencies)
            {
                unlockPackage(depKey);
            }
            return false;
        }
        // Let us get permissions. from package.
        addPackagePermissionsToConfigMetadata(pkgMetadata.value(), configMetadata);

        // At this point all dependencies are already mounted. if the packages are already mounted, we have metadata, so return.
        if (mMountedPackages.find(pkgVerKey) != mMountedPackages.end())
        {
            // Increase mount count
            mMountedPackages[pkgVerKey]->incMountCount();

            RalfPackageInfo ralfPkgInfo;

            ralfPkgInfo.pkgMountPath = mMountedPackages[pkgVerKey]->packageMount->mountPoint();
            ralfPkgInfo.pkgMetaDataPath = mMountedPackages[pkgVerKey]->pkgJsonPath;
            ralfMountInfo.push_back(ralfPkgInfo);

            return true;
        }

        // Verify and mount the package
        auto verifyResult = package.verify();
        if (!verifyResult)
        {
            std::cerr << "[libPackage] Failed to verify package: " << package.id() << " Error: " << verifyResult.error().what() << std::endl;
            for (const auto &depKey : lockedDependencies)
            {
                unlockPackage(depKey);
            }
            return false;
        }

        auto mountPath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / pkgVerKey / "rootfs";
        std::filesystem::create_directories(mountPath);
        std::cout << "[libPackage] Creating mount directory: " << mountPath << std::endl;

        auto mountResult = package.mount(mountPath);
        if (!mountResult)
        {
            std::cerr << "[libPackage][RALFMOUNT] Failed to mount dependent package: " << packageId << mountResult.error().what() << std::endl;
            for (const auto &depKey : lockedDependencies)
            {
                unlockPackage(depKey);
            }
            return false;
        }

        std::unique_ptr<MountedPackageInfo> mountInfo = std::make_unique<MountedPackageInfo>();
        mountInfo->dependencies = lockedDependencies;

        // Note: only the direct dependencies of this package are stored/printed here.
        // Each dependency's own entry in the mount table holds its own direct dependencies,
        // so the full tree is covered when walking recursively (e.g. in unlockPackage).
        std::cout << "[libPackage] Mounted package: " << pkgVerKey << " with " << lockedDependencies.size() << " resolved dependencies:" << std::endl;
        for (const auto &depKey : lockedDependencies)
        {
            std::cout << "[libPackage]   -> " << depKey << std::endl;
        }

        auto configPath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / pkgVerKey / RDK_PACKAGE_CONFIG;
        if (dumpPackageInfo(package, configPath))
        {
            mountInfo->pkgJsonPath = configPath.string();
        }

        mountInfo->packageMount = std::make_unique<ralf::PackageMount>(std::move(mountResult.value()));

        mMountedPackages[pkgVerKey] = std::move(mountInfo);

        RalfPackageInfo ralfPkgInfo;
        ralfPkgInfo.pkgMountPath = mountPath.string();
        ralfPkgInfo.pkgMetaDataPath = configPath.string();
        ralfMountInfo.push_back(ralfPkgInfo);

        return true;
    }
    ralf::Result<ralf::Package> RalfPackageImpl::openPackage(const std::string &fileLocator, bool performFullVerification)
    {
        const std::filesystem::path packagePath(fileLocator);
        if (!std::filesystem::exists(packagePath))
        {
            std::cerr << "[libPackage] Error: Package file does not exist: " << fileLocator << std::endl;
            return ralf::Error::format(ralf::make_error_code(ralf::ErrorCode::FileNotFound),
                                       "Package file does not exist: %s", fileLocator.c_str());
        }

        if (!std::filesystem::is_regular_file(packagePath))
        {
            std::cerr << "[libPackage] Error: Package path is not a regular file: " << fileLocator << std::endl;
            return ralf::Error::format(ralf::make_error_code(ralf::ErrorCode::InvalidArgument),
                                       "Package path is not a regular file: %s", fileLocator.c_str());
        }

        auto openFlags = ralf::Package::OpenFlags::CheckCertificateExpiry;
        auto package = ralf::Package::open(fileLocator, mVerificationBundle, openFlags);
        if (!package)
        {
            std::cerr << "[libPackage] Error: Failed to open package: " << fileLocator << " - " << package.error().what() << std::endl;
            return package;
        }

        if (!package->isValid())
        {
            std::cerr << "[libPackage] Error: Package is not valid: " << fileLocator << std::endl;
            return ralf::Error::format(ralf::make_error_code(ralf::ErrorCode::InvalidPackage),
                                       "Package is not valid: %s", fileLocator.c_str());
        }

        if (performFullVerification)
        {
            auto verifyResult = package->verify();
            if (!verifyResult)
            {
                std::cerr << "[libPackage] Error: Failed to fully verify package: " << fileLocator
                          << " - " << verifyResult.error().what() << std::endl;
                return verifyResult.error();
            }
        }

        return package;
    }

    bool RalfPackageImpl::unlockPackage(const std::string &pkgVerKey)
    {
        auto it = mMountedPackages.find(pkgVerKey);
        if (it == mMountedPackages.end())
        {
            std::cerr << "[libPackage] Package not found in mounted packages: " << pkgVerKey << std::endl;
            return false;
        }

        bool status = true;
        for (const auto &depKey : it->second->dependencies)
        {
            if (!unlockPackage(depKey))
            {
                std::cerr << "[libPackage] Failed to unlock dependent package: " << depKey << std::endl;
                // Keep unlocking the remaining dependencies even if one fails
                status = false;
            }
        }

        it->second->decMountCount();
        if (it->second->mountCount == 0)
        {
            // Need to unmount the package
            it->second->packageMount->unmount();

            // Clean up mount directories
            auto mountBasePath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / pkgVerKey;
            try
            {
                if (std::filesystem::exists(mountBasePath))
                {
                    std::filesystem::remove_all(mountBasePath);
                    std::cout << "[libPackage] Removed mount directory: " << mountBasePath << std::endl;
                }
            }
            catch (const std::filesystem::filesystem_error &e)
            {
                std::cerr << "[libPackage] Error removing mount directory " << mountBasePath << ": " << e.what() << std::endl;
            }

            mMountedPackages.erase(it);
        }

        return status;
    }

    bool RalfPackageImpl::dumpPackageInfo(const ralf::Package &package, const std::filesystem::path &configPath)
    {
        // Case 1. It was once mounted, so no need to generate new one
        if (std::filesystem::exists(configPath))
        {
            return true;
        }
        // Case 2. Generate new one.
        auto packagejson = package.auxMetaDataFile(RDK_PACKAGE_CONFIG_MIME_TYPE);
        if (packagejson)
        {
            const auto contents = packagejson->readAll();
            if (contents)
            {
                std::ofstream outFile(configPath);
                if (!outFile.is_open())
                {
                    std::cerr << "[libPackage] Failed to open config file for writing: " << configPath << std::endl;
                    return false;
                }
                outFile.write(reinterpret_cast<const char *>(contents->data()), contents->size());
                outFile.close();
                return true;
            }
        }
        return false;
    }
    bool RalfPackageImpl::serializeToJson(const std::vector<RalfPackageInfo> &mountPkgList, const std::filesystem::path &outputPath) const
    {
        /*
        The structure expected is as follows
        {
            "packages": [
                    {
                    "packagePath":"absolute path to package contents(mount point)",
                    "metadataPath":"absolute package config path (in json format"
                    },
                    {
                    "packagePath":"absolute path to package contents(mount point)",
                    "metadataPath":"absolute package config path (in json format"      },
                    {
                    "packagePath":"absolute path to package contents(mount point)",
                    "metadataPath":"absolute package config path (in json format"
                    }
            ]
        }
        */

        Json::Value packages(Json::arrayValue);
        for (const auto &pkgInfo : mountPkgList)
        {
            Json::Value pkgJson;
            pkgJson["pkgMountPath"] = pkgInfo.pkgMountPath;
            pkgJson["pkgMetaDataPath"] = pkgInfo.pkgMetaDataPath;
            packages.append(pkgJson);
        }
        Json::Value root;
        root["packages"] = packages;

        std::ofstream outputFile(outputPath);
        if (!outputFile.is_open())
        {
            std::cerr << "[libPackage] Failed to open output file: " << outputPath << std::endl;
            return false;
        }
        outputFile << root.toStyledString();
        outputFile.close();
        return true;
    }
    bool RalfPackageImpl::getRalfUserInfo(uid_t &userId, gid_t &groupId)
    {
        struct passwd *pwd = getpwnam(RALF_USER_NAME);
        if (pwd == nullptr)
        {
            std::cerr << "[libPackage] Failed to get user info for user: " << RALF_USER_NAME << std::endl;
            return false;
        }
        userId = pwd->pw_uid;
        groupId = pwd->pw_gid;
        return true;
    }
    void RalfPackageImpl::addPackagePermissionsToConfigMetadata(const ralf::PackageMetaData &pkgMetadata, ConfigMetaData &configMetadata)
    {
        // Check if the type is application, otherwise we should not be looking for permissions.
        if (pkgMetadata.type() != ralf::PackageType::Application)
        {
            std::cout << "[libPackage] Package type is not application. Skipping permissions extraction." << std::endl;
            return;
        }
        // Permissions are present in applicationInfo section of metadata.
        auto appInfo = pkgMetadata.applicationInfo();
        if (appInfo)
        {
            auto permissions = appInfo->permissions();

            auto perms = permissions.all();
            std::string permissionsStr;
            for (const auto &perm : perms)
            {
                permissionsStr += perm + ",";
            }
            if (!permissionsStr.empty())
            {
                // Remove the trailing comma
                permissionsStr.pop_back();
                configMetadata.capabilities = permissionsStr;
                std::cout << "[libPackage] Added package permissions to config metadata: " << permissionsStr << std::endl;
            }
        }
        else
        {
            std::cerr << "[libPackage] No application info found in package metadata." << std::endl;
        }
    }
} // namespace packagemanager
