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
#include <algorithm>
#include <ralf/PackageMount.h>
#include <ralf/PackageMetaData.h>
#include <ralf/VersionNumber.h>
#include <fstream>
#include <sstream>

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

    // A (packageId, version) key is safe to use in filesystem paths if the id is a plain
    // path component (non-empty, not "." or "..", no separator) and the version parses as a
    // ralf version number - that grammar excludes separators and "..", so a valid version is
    // always a safe path component.
    static bool isPathSafePackageKey(const std::string &packageId, const std::string &version)
    {
        return !packageId.empty() && packageId != "." && packageId != ".." &&
               packageId.find('/') == std::string::npos &&
               static_cast<bool>(ralf::VersionNumber::fromString(version));
    }

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

    // Removes staging files left behind by installs that were terminated after staging but
    // before the atomic rename. Package discovery only recognizes the exact package.ralf
    // name, so without this sweep such leftovers would accumulate in persistent storage on
    // every crash. Callers serialize Install calls and Initialize runs before any Install of
    // this process, so every staging file found here belongs to a dead installer and can be
    // removed outright - no ownership check is needed.
    static void cleanupStaleStagingFiles()
    {
        const std::string stagingPrefix = std::string(RalfPackage) + ".tmp";
        std::error_code ec;
        const std::filesystem::recursive_directory_iterator end;
        for (std::filesystem::recursive_directory_iterator it(AppInstallationPath, ec); it != end; it.increment(ec))
        {
            if (!it->is_regular_file(ec))
            {
                continue;
            }
            const auto fileName = it->path().filename().string();
            if (fileName.find(stagingPrefix) != 0 ||
                (fileName.size() > stagingPrefix.size() && fileName[stagingPrefix.size()] != '.'))
            {
                continue;
            }
            if (std::filesystem::remove(it->path(), ec))
            {
                std::cout << "[libPackage] Removed stale staging file: " << it->path() << std::endl;
            }
            else if (ec)
            {
                std::cerr << "[libPackage] Failed to remove stale staging file: " << it->path() << ": " << ec.message() << std::endl;
                ec.clear();
            }
        }
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
        mIsInitialized = true;
        if (!std::filesystem::exists(AppInstallationPath))
        {
            std::cout << "[libPackage] App installation path does not exist. Creating: " << AppInstallationPath << std::endl;
            // Record which path components are missing before creating the root, so each new
            // directory entry can be fsynced from its parent. Install's durability chain stops
            // at AppInstallationPath, so without this a crash could lose the entire newly
            // created installation tree even though installs report success.
            std::vector<std::filesystem::path> createdDirs;
            for (auto dir = std::filesystem::path(AppInstallationPath); !dir.empty() && !std::filesystem::exists(dir); dir = dir.parent_path())
            {
                createdDirs.push_back(dir);
            }
            std::filesystem::create_directories(AppInstallationPath);
            for (const auto &createdDir : createdDirs)
            {
                if (!syncFile(createdDir.parent_path()))
                {
                    std::cerr << "[libPackage] Warning: failed to sync directory to disk: " << createdDir.parent_path()
                              << " (installation root may not survive a crash)" << std::endl;
                }
            }
            return Result::SUCCESS;
        }
        else
        {
            // Reclaim staging files left behind by interrupted installs before scanning.
            cleanupStaleStagingFiles();

            std::vector<std::string> installedPackages;
            // Let us get package metadata of all installed packages
            auto count = getInstalledPackages(installedPackages);
            std::cout << "[libPackage] Found " << count << " installed packages." << std::endl;

            for (auto packagePath : installedPackages)
            {
                std::string appId, appVersion;
                ConfigMetaData configMetadata;
                auto package = openPackage(packagePath);
                if (!package)
                {
                    std::cerr << "[libPackage] Failed to open package: " << packagePath << std::endl;
                    continue;
                }
                configMetadata.appPath = std::filesystem::path(packagePath);
                appId = package->id();

                if (!extractMetadataFromPackage(package.value(), configMetadata))
                {
                    std::cerr << "[libPackage] Warning!! Failed to extract metadata from package: " << packagePath << std::endl;
                    continue;
                }
                appVersion = package->version().toString();
                auto configKey = std::make_shared<ConfigMetadataKey>(std::make_pair(appId, appVersion));

                if (configMetadata.dial)
                {
                    mDialPackages.push_back(configKey);
                }
                mInstalledPackages.push_back(configKey);
                std::cout << "[libPackage] Found installed package: " << appId << ", version: " << appVersion << std::endl;

                ConfigMetadataKey appKey = {appId, appVersion};
                aConfigMetadata[appKey] = configMetadata;
            }
        }

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

    Result RalfPackageImpl::Install(const std::string &packageId, const std::string &version, const NameValues &additionalMetadata, const std::string &fileLocator, ConfigMetaData &configMetadata)
    {
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::Install called before initialization." << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] RalfPackageImpl::Install called with packageId: " << packageId << ", version: " << version << ", fileLocator: " << fileLocator << std::endl;
        // packageId/version are caller-supplied and used in filesystem paths below; reject
        // anything that is not a path-safe key before touching the filesystem.
        if (!isPathSafePackageKey(packageId, version))
        {
            std::cerr << "[libPackage] Invalid packageId/version: " << packageId << ", " << version << std::endl;
            return Result::FAILED;
        }
        auto package = openPackage(fileLocator, true);
        if (!package)
        {
            std::cerr << "[libPackage] Package verification failed for: " << fileLocator << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] Successfully opened package: " << fileLocator << std::endl;

        // Registration (here) and the boot-time scan (Initialize) must use the same canonical
        // id/version key, and the caller's arguments also name the on-disk directories.
        // Reject a mismatch with the embedded metadata so a package can never be registered
        // under one key and discovered under another.
        if (package->id() != packageId || package->version().toString() != version)
        {
            std::cerr << "[libPackage] Package metadata mismatch: requested " << packageId << ", " << version
                      << " but package contains " << package->id() << ", " << package->version().toString() << std::endl;
            return Result::FAILED;
        }

        if (enableDependencyCheck)
        {
            if (!checkPackageDependencies(package.value()))
                return Result::FAILED;
        }

        // Create the directory structure. Remember which path components do not exist yet:
        // syncing packagePath after the rename only persists the package file's entry, not the
        // newly created packageId/version directory entries - those live in their parents and
        // each affected parent must be fsynced separately for the new path to survive a crash.
        auto packagePath = std::filesystem::path(AppInstallationPath) / packageId / version;
        std::vector<std::filesystem::path> createdDirs;
        for (auto dir = packagePath; dir != AppInstallationPath && !std::filesystem::exists(dir); dir = dir.parent_path())
        {
            createdDirs.push_back(dir);
        }
        std::filesystem::create_directories(packagePath);

        // Install the package by staging it in a temporary file and atomically renaming it
        // into place. If the package is currently mounted (loop device + dm-verity), the loop
        // device keeps the old inode alive, so a running app keeps using the old image until it
        // is unmounted, while future mounts pick up the new file. Overwriting the file in place
        // (copy_file with overwrite_existing directly on the destination) would corrupt the
        // live mount.
        auto destRalfPackagePath = packagePath / RalfPackage;
        // Callers serialize Install calls, so a single deterministic staging name is safe:
        // no other install can touch it while it is being written, synced or renamed. A file
        // left here by a crashed install is removed by cleanupStaleStagingFiles() at startup.
        auto tempRalfPackagePath = packagePath / (std::string(RalfPackage) + ".tmp");
        try
        {
            // Remove any pre-existing staging file first: a symlink planted at the
            // deterministic staging path would otherwise be followed by copy_file, writing
            // the package bytes to a target outside the installation tree. The tree is
            // writable only by this service, so this is defense in depth.
            std::error_code ec;
            std::filesystem::remove(tempRalfPackagePath, ec);
            std::filesystem::copy_file(fileLocator, tempRalfPackagePath, std::filesystem::copy_options::overwrite_existing);
            if (std::filesystem::exists(destRalfPackagePath))
            {
                // Swapping an existing installation: keep the original file permissions
                std::filesystem::permissions(tempRalfPackagePath, std::filesystem::status(destRalfPackagePath).permissions());
            }
            // The source was verified before the copy, but copy_file re-read it by pathname
            // and it may have been replaced in between (TOCTOU). Verify the staged copy
            // itself, so only bytes verified as staged can be renamed into place.
            auto stagedPackage = openPackage(tempRalfPackagePath, true);
            if (!stagedPackage || stagedPackage->id() != packageId || stagedPackage->version().toString() != version)
            {
                std::cerr << "[libPackage] Staged package verification failed for: " << tempRalfPackagePath << std::endl;
                std::filesystem::remove(tempRalfPackagePath);
                return Result::FAILED;
            }
            if (!syncFile(tempRalfPackagePath))
            {
                std::cerr << "[libPackage] Failed to sync package file to disk: " << tempRalfPackagePath << std::endl;
                std::filesystem::remove(tempRalfPackagePath);
                return Result::FAILED;
            }
            std::filesystem::rename(tempRalfPackagePath, destRalfPackagePath);
            // The rename is the commit point: once it succeeds the new package is visible at
            // the destination, so the install is a fact and the registration below matches
            // the on-disk state. Crash analysis of this sequence (fsync data -> atomic rename
            // -> fsync directory):
            //   1. All fsyncs succeed: the new package is durable, period.
            //   2. Crash before the fsyncs: the rename may or may not have reached the disk,
            //      but since the file's data was fsynced BEFORE its name was moved into
            //      place and the rename is atomic, the post-crash state is always either the
            //      old package or the complete new package - never a torn package.ralf.
            //   3. A directory fsync fails below: case 2 with unknown durability. The running
            //      system still sees the new package, so in-memory registration matches disk;
            //      after a power loss the boot-time rescan rebuilds registration from
            //      whatever actually survived. Hence a failure here is logged as a warning,
            //      not returned as FAILED - FAILED would misreport an install that did
            //      happen and would itself create the "installed but unregistered" state.
            if (!syncFile(packagePath))
            {
                std::cerr << "[libPackage] Warning: failed to sync package directory to disk: " << packagePath
                          << " (package is installed but may not survive a crash)" << std::endl;
            }
            // Fsync the parents of any directories created above, so the new packageId/version
            // directory entries themselves are durable; same post-commit warning-only treatment.
            for (const auto &createdDir : createdDirs)
            {
                if (!syncFile(createdDir.parent_path()))
                {
                    std::cerr << "[libPackage] Warning: failed to sync directory to disk: " << createdDir.parent_path()
                              << " (package is installed but may not survive a crash)" << std::endl;
                }
            }

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
        auto configKey = std::make_shared<ConfigMetadataKey>(std::make_pair(packageId, version));

        if (extractMetadataFromPackage(package.value(), configMetadata))
        {
            if (configMetadata.dial)
            {   
                // a re-installed package may have been registered before
                // without its dial metadata
                const auto isAlreadyDialRegistered = std::any_of(mDialPackages.begin(), mDialPackages.end(),
                                                                 [&packageId, &version](const std::shared_ptr<ConfigMetadataKey> &entry)
                                                                 { return entry->first == packageId && entry->second == version; });
                if (!isAlreadyDialRegistered)
                {
                    mDialPackages.push_back(configKey);
                }
            }
            else
            {
                // a re-installed package may no longer carry dial metadata;
                // drop any stale dial registration for it
                mDialPackages.erase(std::remove_if(mDialPackages.begin(), mDialPackages.end(),
                                                   [&packageId, &version](const std::shared_ptr<ConfigMetadataKey> &entry)
                                                   { return entry->first == packageId && entry->second == version; }),
                                    mDialPackages.end());
            }
        }

        // On a re-install (file swap) of an already known package, do not register it twice
        const auto isAlreadyRegistered = std::any_of(mInstalledPackages.begin(), mInstalledPackages.end(),
                                                     [&packageId, &version](const std::shared_ptr<ConfigMetadataKey> &entry)
                                                     { return entry->first == packageId && entry->second == version; });
        if (!isAlreadyRegistered)
        {
            mInstalledPackages.push_back(configKey);
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
        // TODO we need to remove the entries from mInstalledPackages vector. Since currently no version info is passed, this is on hold.
        // For the time being, we will remove every instance of the package from the installed packages list.
        for (auto it = mInstalledPackages.begin(); it != mInstalledPackages.end();)
        {
            // the vector is a pair of packageId and version
            if ((*it)->first == packageId)
            {
                it = mInstalledPackages.erase(it);
            }
            else
            {
                ++it;
            }
        }
        // Remove the package from the dial packages list as well
        // TODO the same logic applies here as well.
        for (auto it = mDialPackages.begin(); it != mDialPackages.end();)
        {
            if ((*it)->first == packageId)
            {
                it = mDialPackages.erase(it);
            }
            else
            {
                ++it;
            }
        }
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
        // packageId/version are caller-supplied and used in filesystem paths below; reject
        // anything that is not a path-safe key before touching the filesystem.
        if (!isPathSafePackageKey(packageId, version))
        {
            std::cerr << "[libPackage] Invalid packageId/version: " << packageId << ", " << version << std::endl;
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
        // The mount table is keyed by the embedded metadata (package.id() + "_" + version),
        // while Unlock looks entries up by the caller's arguments. Reject a mismatch here so
        // both sides always use the same canonical key; otherwise the mount would be stored
        // under a key Unlock can never find, leaking it and its dependency counts.
        if (package->id() != packageId || package->version().toString() != version)
        {
            std::cerr << "[libPackage] Package metadata mismatch: requested " << packageId << ", " << version
                      << " but package contains " << package->id() << ", " << package->version().toString() << std::endl;
            return Result::FAILED;
        }

        std::vector<RalfPackageInfo> mountPkgList;
        auto status = lockPackage(package.value(), mountPkgList, configMetadata);
        if (status)
        {
            // We need to dump this to a temp file and add it as par of configMetadata.
            // packageId/version were already validated as a path-safe key at entry.
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
        auto key = std::make_pair(packageId, version);
        if (mMountedPackages.find(key) == mMountedPackages.end())
        {
            // After an upgrade swap the caller may present the version currently installed
            // rather than the version that was actually locked. If exactly one version of
            // this package is mounted, that entry is unambiguously the lock to release -
            // its recorded dependency tree is the one that was locked.
            const ConfigMetadataKey *mountedKey = nullptr;
            int matches = 0;
            for (const auto &entry : mMountedPackages)
            {
                if (entry.first.first == packageId)
                {
                    mountedKey = &entry.first;
                    ++matches;
                }
            }
            if (matches == 1)
            {
                std::cout << "[libPackage] Requested version " << version << " of " << packageId
                          << " is not mounted; unlocking the mounted version " << mountedKey->second << std::endl;
                key = *mountedKey;
            }
        }
        bool unmountResult = unlockPackage(key);

        // Clean up the temporary metadata file created during Lock, using the key that was
        // actually locked (may differ from the caller's version after an upgrade swap).
        // Only perform the cleanup for a path-safe key, so it can never remove a file
        // outside the temp directory.
        if (isPathSafePackageKey(key.first, key.second))
        {
            const auto tempFilePath = std::filesystem::temp_directory_path() / (key.first + "_" + key.second + "_metadata.json");
            try
            {
                if (std::filesystem::exists(tempFilePath))
                {
                    std::filesystem::remove(tempFilePath);
                    std::cout << "[libPackage] Removed temporary metadata file: " << tempFilePath << std::endl;
                }
            }
            catch (const std::filesystem::filesystem_error &e)
            {
                std::cerr << "[libPackage] Error removing temporary metadata file " << tempFilePath << ": " << e.what() << std::endl;
            }
        }

        return unmountResult ? Result::SUCCESS : Result::FAILED;
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

        packageId = package->id();
        version = package->version().toString();
        if (extractMetadataFromPackage(package.value(), configMetadata) == false)
        {
            std::cerr << "[libPackage] Failed to extract metadata from package: " << fileLocator << std::endl;
            return Result::FAILED;
        }
        auto packagePath = std::filesystem::path(fileLocator);
        configMetadata.appPath = packagePath.string();
        return Result::SUCCESS;
    }

    bool RalfPackageImpl::extractMetadataFromPackage(const ralf::Package &package, ConfigMetaData &configMetadata)
    {
        auto pkgMetadata = package.metaData();
        if (!pkgMetadata)
        {
            std::cerr << "[libPackage] Failed to read package metadata for extracting metadata: " << package.metaData().error().what() << std::endl;
            return false;
        }

        configMetadata.packageFormat = "ralf";
        configMetadata.userId = mUserId;   // Ralf user id.
        configMetadata.groupId = mGroupId; // Ralf user group.
        if (pkgMetadata)
        {
            auto pkgMetadataValue = pkgMetadata.value();
            configMetadata.mimeType = pkgMetadataValue.mimeType();
            if (pkgMetadataValue.applicationInfo())
            {
                auto appInfo = pkgMetadataValue.applicationInfo();
                auto appInfoValue = appInfo.value();
                addPackagePermissionsToConfigMetadata(appInfoValue, configMetadata);
                configMetadata.dial = appInfoValue.dialInfo().has_value();
            }
        }
        return true;
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

        const ConfigMetadataKey pkgVerKey = std::make_pair(packageId, version);
        // Directory name for the on-disk mount point only; the in-memory mount table uses
        // the structured key above. The flat "<id>_<version>" name is unambiguous because
        // version is guaranteed to contain no '_' (validated at Install via
        // ralf::VersionNumber::fromString), so the last '_' is always the separator -
        // the id may contain '_' freely.
        const std::string pkgVerDirName = packageId + "_" + version;

        auto pkgMetadata = package.metaData();
        if (!pkgMetadata)
        {
            std::cerr << "[libPackage] Failed to read package metadata for locking dependencies: " << pkgMetadata.error().what() << std::endl;
            return false;
        }

        status = true;
        // Keys of the dependent packages locked by this call. Used for rollback on failure and
        // stored in the mount table on success, so Unlock can release them without re-opening files.
        std::vector<ConfigMetadataKey> lockedDependencies;
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
            lockedDependencies.push_back(std::make_pair(depPackage->id(), depPackage->version().toString()));
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
        if (!extractMetadataFromPackage(package, configMetadata))
        {
            std::cerr << "[libPackage] Warning!! Failed to extract metadata from package: " << package.id() << std::endl;
        }

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

        auto mountPath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / pkgVerDirName / "rootfs";
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
        std::cout << "[libPackage] Mounted package: " << pkgVerDirName << " with " << lockedDependencies.size() << " resolved dependencies:" << std::endl;
        for (const auto &depKey : lockedDependencies)
        {
            std::cout << "[libPackage]   -> " << depKey.first << "_" << depKey.second << std::endl;
        }

        auto configPath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / pkgVerDirName / RDK_PACKAGE_CONFIG;
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

    bool RalfPackageImpl::unlockPackage(const ConfigMetadataKey &pkgVerKey)
    {
        auto it = mMountedPackages.find(pkgVerKey);
        if (it == mMountedPackages.end())
        {
            std::cerr << "[libPackage] Package not found in mounted packages: " << pkgVerKey.first << "_" << pkgVerKey.second << std::endl;
            return false;
        }

        bool status = true;
        for (const auto &depKey : it->second->dependencies)
        {
            if (!unlockPackage(depKey))
            {
                std::cerr << "[libPackage] Failed to unlock dependent package: " << depKey.first << "_" << depKey.second << std::endl;
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
            auto mountBasePath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / (pkgVerKey.first + "_" + pkgVerKey.second);
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
    void RalfPackageImpl::addPackagePermissionsToConfigMetadata(const ralf::ApplicationInfo &appInfo, ConfigMetaData &configMetadata)
    {
        // Permissions are present in applicationInfo section of metadata.
        auto permissions = appInfo.permissions();

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
    packagemanager::Result RalfPackageImpl::GetInstalledPackageMetadata(const std::string &packageId, const std::string &version, std::string &config)
    {
        // Open the installed package and return its config metadata (JSON) as a string.
        // Returns Result::FAILED if the package cannot be opened or the metadata cannot be read.
        std::cout << "[libPackage] RalfPackageImpl::GetInstalledPackageMetadata called with packageId: " << packageId << ", version: " << version << std::endl;

        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::GetInstalledPackageMetadata called before initialization." << std::endl;
            return Result::FAILED;
        }
        Json::Value configJson;

        if (getMetadataAsJson(packageId, version, configJson))
        {
            // Set indentation to ""
            Json::StreamWriterBuilder writerBuilder;
            writerBuilder["indentation"] = "";
            config = Json::writeString(writerBuilder, configJson);
            return Result::SUCCESS;
        }
        return Result::FAILED;
    }
    packagemanager::Result RalfPackageImpl::GetConfigListForInstalledPackages(const std::string &filter, std::string &config)
    {
        // This method expects a filter. Currently the only supported filter is dial.
        //  Open all installed packages , identify application package, check if the package supports dial, then add it to a json array
        if (!mIsInitialized)
        {
            std::cerr << "[libPackage] RalfPackageImpl::GetConfigListForInstalledPackages called before initialization." << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] RalfPackageImpl::GetConfigListForInstalledPackages called with filter: " << filter << std::endl;

        if (filter != "dial")
        {
            std::cerr << "[libPackage] Unsupported filter: " << filter << std::endl;
            return Result::FAILED;
        }

        Json::Value dialConfigArray(Json::arrayValue);
        for (const auto &pkgInfo : mDialPackages)
        {
            auto packageId = pkgInfo->first;
            auto version = pkgInfo->second;
            Json::Value parsedJson;
            if (getMetadataAsJson(packageId, version, parsedJson))
            {
                dialConfigArray.append(parsedJson);
            }
        }

        // Set indentation to ""
        Json::StreamWriterBuilder writerBuilder;
        writerBuilder["indentation"] = "";
        config = Json::writeString(writerBuilder, dialConfigArray);
        return Result::SUCCESS;
    }
    bool RalfPackageImpl::getMetadataAsJson(const std::string &appId, const std::string &version, Json::Value &metadata)
    {
        auto packagePath = std::filesystem::path(AppInstallationPath) / appId / version / RalfPackage;
        auto package = openPackage(packagePath);
        if (!package)
        {
            std::cerr << "[libPackage] Failed to open package for getting config list: " << packagePath.string() << std::endl;
            return false;
        }

        auto packagejson = package->auxMetaDataFile(RDK_PACKAGE_CONFIG_MIME_TYPE);
        if (!packagejson)
        {
            std::cerr << "[libPackage] Failed to get auxMetaDataFile: " << packagejson.error().what() << std::endl;
            return false;
        }

        const auto contents = packagejson->readAll();
        if (!contents)
        {
            std::cerr << "[libPackage] Failed to read contents of auxMetaDataFile: " << packagePath.string() << std::endl;
            return false;
        }
        std::string jsonContent(reinterpret_cast<const char *>(contents->data()), contents->size());

        // So we have a json string in string format. Let us convert that to a json object.
        Json::CharReaderBuilder readerBuilder;
        std::string parseErrors;
        std::istringstream jsonStream(jsonContent);

        if (!Json::parseFromStream(readerBuilder, jsonStream, &metadata, &parseErrors))
        {
            std::cerr << "[libPackage] Failed to parse config JSON for dial package: " << packagePath.string()
                      << ": " << parseErrors << std::endl;
            return false;
        }
        return true;
    }
} // namespace packagemanager
