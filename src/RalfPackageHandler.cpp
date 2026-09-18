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
#include <ralf/PackageMount.h>
#include <ralf/PackageMetaData.h>
#include <ralf/VersionNumber.h>
#include <fstream>
#include <sstream>

#include <cstdint>

#include <pwd.h> //For getting user id and group id of ralf user

namespace
{
    static constexpr const char *RALF_USER_NAME = "ralf";
    static constexpr const char *AppInstallationPath = DAC_APP_PATH;
    static constexpr const char *RalfPackage = "package.ralf";
    static constexpr const char *pkgCertDirPath = RDK_PACKAGE_CERT_PATH;
    static constexpr const char *BuildReference = BUILD_REFERENCE;
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
            std::filesystem::create_directories(AppInstallationPath);
            return Result::SUCCESS;
        }
        else
        {
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
        auto package = openPackage(fileLocator, true);
        if (!package)
        {
            std::cerr << "[libPackage] Package verification failed for: " << fileLocator << std::endl;
            return Result::FAILED;
        }
        std::cout << "[libPackage] Successfully opened package: " << fileLocator << std::endl;

        if (enableDependencyCheck)
        {
            if (!checkPackageDependencies(package.value()))
                return Result::FAILED;
        }

        // Create the directory structure
        auto packagePath = std::filesystem::path(AppInstallationPath) / packageId / version;
        std::filesystem::create_directories(packagePath);

        //  Copy the package to the installation directory
        auto destRalfPackagePath = packagePath / RalfPackage;
        try
        {
            std::filesystem::copy_file(fileLocator, destRalfPackagePath, std::filesystem::copy_options::overwrite_existing);
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
            return Result::FAILED;
        }
        auto configKey = std::make_shared<ConfigMetadataKey>(std::make_pair(packageId, version));

        if (extractMetadataFromPackage(package.value(), configMetadata))
        {

            if (configMetadata.dial)
            {
                mDialPackages.push_back(configKey);
            }
        }
        mInstalledPackages.push_back(configKey);

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
        auto packagePath = std::filesystem::path(AppInstallationPath) / packageId / version / RalfPackage;
        auto package = openPackage(packagePath);
        if (!package)
        {
            std::cerr << "[libPackage] Failed to open package for unlocking: " << packagePath.string() << std::endl;
            return Result::FAILED;
        }

        bool unmountResult = unmountDependentPackages(package.value());

        // Clean up temporary metadata file created during Lock
        auto tempFilePath = std::filesystem::temp_directory_path() / (packageId + "_" + version + "_metadata.json");
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

        std::string pkgVerKey = packageId + "_" + version;

        auto pkgMetadata = package.metaData();
        if (!pkgMetadata)
        {
            std::cerr << "[libPackage] Failed to read package metadata for locking dependencies: " << pkgMetadata.error().what() << std::endl;
            return false;
        }

        status = true;
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
        }
        if (!status)
        {
            unmountDependentPackages(package);
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
            unmountDependentPackages(package);
            return false;
        }

        auto mountPath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / pkgVerKey / "rootfs";
        std::filesystem::create_directories(mountPath);
        std::cout << "[libPackage] Creating mount directory: " << mountPath << std::endl;

        auto mountResult = package.mount(mountPath);
        if (!mountResult)
        {
            std::cerr << "[libPackage][RALFMOUNT] Failed to mount dependent package: " << packageId << mountResult.error().what() << std::endl;
            unmountDependentPackages(package);
            return false;
        }

        std::unique_ptr<MountedPackageInfo> mountInfo = std::make_unique<MountedPackageInfo>();

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

    bool RalfPackageImpl::unmountDependentPackages(const ralf::Package &package)
    {
        auto pkgMetadata = package.metaData();
        if (!pkgMetadata)
        {
            std::cerr << "[libPackage] Failed to read package metadata for unlocking dependencies: " << pkgMetadata.error().what() << std::endl;
            return false;
        }

        auto dependencies = pkgMetadata->dependencies();
        for (const auto &dependency : dependencies)
        {
            std::string depPackageId = dependency.first;
            ralf::VersionConstraint depPkgVersion = dependency.second;
            std::string depInstalledVersion;

            if (identifyDependencyVersion(depPackageId, depPkgVersion, depInstalledVersion))
            {
                auto fileLocator = std::filesystem::path(AppInstallationPath) / depPackageId / depInstalledVersion / RalfPackage;
                auto depPackage = openPackage(fileLocator);
                if (depPackage)
                {
                    if (!unmountDependentPackages(depPackage.value()))
                    {
                        std::cerr << "[libPackage] Failed to unmount dependent packages for package: " << depPackageId << ", version " << depInstalledVersion << std::endl;
                        // TODO revisit this logic
                        // return false;
                    }
                }
            }
            else
            {
                std::cerr << "[libPackage] Failed to idenitfy the version of dependency " << depPackageId << ", version " << depPkgVersion.toString() << std::endl;
            }
        }
        std::string depPackageKey = package.id() + "_" + package.version().toString();

        auto it = mMountedPackages.find(depPackageKey);
        if (it == mMountedPackages.end())
        {
            std::cerr << "[libPackage] Package not found in mounted packages: " << depPackageKey << std::endl;
            return false;
        }

        if (it->second->packageMount->isMounted() == false)
        {
            std::cerr << "[libPackage] Package is not mounted: " << depPackageKey << std::endl;
            return false;
        }
        it->second->decMountCount();
        if (it->second->mountCount == 0)
        {
            // Need to unmount the package
            it->second->packageMount->unmount();

            // Clean up mount directories
            auto mountBasePath = std::filesystem::path(RDK_PACKAGE_MOUNT_PATH) / depPackageKey;
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

        return true;
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
