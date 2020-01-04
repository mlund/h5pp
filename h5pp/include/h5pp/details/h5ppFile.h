
#pragma once

#include "h5ppAttributeProperties.h"
#include "h5ppConstants.h"
#include "h5ppDatasetProperties.h"
#include "h5ppFileCounter.h"
#include "h5ppHdf5.h"
#include "h5ppLogger.h"
#include "h5ppTextra.h"
#include "h5ppType.h"
#include "h5ppTypeCheck.h"
#include "h5ppUtils.h"
#include <hdf5.h>
#include <hdf5_hl.h>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>

// Include filesystem or experimental/filesystem
#if __has_include(<filesystem>)
#include <filesystem>
namespace h5pp {
    namespace fs = std::filesystem;
}
#elif __has_include(<experimental/filesystem>)
#include <experimental/filesystem>
namespace h5pp {
    namespace fs = std::experimental::filesystem;
}
#else
#error Could not find <filesystem> or <experimental/filesystem>
#endif

// Include experimental/type_traits
#if __has_include(<experimental/type_traits>)
#include <experimental/type_traits>
#else
#error Could not find <experimental/type_traits>
#endif

// Include optional or experimental/optional
#if __has_include(<optional>)
#include <optional>

#elif __has_include(<experimental/optional>)
#include <experimental/optional>
namespace h5pp {
    constexpr const std::experimental::nullopt_t &nullopt = std::experimental::nullopt;
    template<typename T> using optional                   = std::experimental::optional<T>;
}
#else
#error Could not find <optional> or <experimental/optional>
#endif

namespace h5pp {

    namespace tc = h5pp::Type::Check;
    /*!
     \brief Writes and reads data to a binary hdf5-file.
    */
    enum class CreateMode { OPEN, TRUNCATE, RENAME };
    enum class AccessMode { READONLY, READWRITE };

    inline bool checkIfCompressionIsAvailable() {
        /*
         * Check if zlib compression is available and can be used for both
         * compression and decompression. We do not throw errors because this
         * filter is an optional part of the hdf5 library.
         */
        [[maybe_unused]] herr_t       status;
        [[maybe_unused]] htri_t       avail;
        [[maybe_unused]] H5Z_filter_t filter_type;
        [[maybe_unused]] unsigned int flags, filter_info;

        avail = H5Zfilter_avail(H5Z_FILTER_DEFLATE);
        if(not avail) { h5pp::Logger::log->debug("zlib filter not available"); }
        H5Zget_filter_info(H5Z_FILTER_DEFLATE, &filter_info);
        if(!(filter_info & H5Z_FILTER_CONFIG_ENCODE_ENABLED) || !(filter_info & H5Z_FILTER_CONFIG_DECODE_ENABLED)) {
            h5pp::Logger::log->debug("zlib filter not available for encoding or decoding");
        }
        return avail;
    }

    class File {
        private:
        mutable herr_t retval = 0;
        fs::path       FileName; /*!< Filename (possibly relative) and extension, e.g. ../files/output.h5 */
        fs::path       FilePath; /*!< Full path to the file */

        AccessMode accessMode = AccessMode::READWRITE;
        CreateMode createMode = CreateMode::RENAME;

        size_t logLevel          = 2;
        bool   defaultExtendable = false; /*!< New datasets with rank >= can be set to extendable by default. For small datasets, setting this true results in larger file size */
        size_t compressionLevel  = 0;

        // Mpi related constants
        hid_t plist_facc  = 0;
        hid_t plist_xfer  = 0;
        hid_t plist_lncr  = 0;
        hid_t plist_lapl  = 0;
        hid_t error_stack = 0;

        public:
        bool hasInitialized = false;
        File() { h5pp::Logger::setLogger("h5pp", logLevel, false); }

        File(const File &other) {
            h5pp::Logger::log->debug("Copy-constructing this file [{}] from given file: [{}]. Previously initialized (this): {}. Previously initialized (other): {}",
                                     FileName.string(),
                                     other.getFileName(),
                                     hasInitialized,
                                     other.hasInitialized);
            *this = other;
        }

        explicit File(const std::string &FileName_, AccessMode accessMode_ = AccessMode::READWRITE, CreateMode createMode_ = CreateMode::RENAME, size_t logLevel_ = 2)
            : FileName(FileName_), accessMode(accessMode_), createMode(createMode_), logLevel(logLevel_) {
            h5pp::Logger::setLogger("h5pp", logLevel, false);
            h5pp::Logger::log->debug("Constructing h5pp file. Given path: [{}]", FileName.string());

            if(accessMode_ == AccessMode::READONLY and createMode_ == CreateMode::TRUNCATE) {
                Logger::log->error("Options READONLY and TRUNCATE are incompatible.");
                return;
            }
            checkIfCompressionIsAvailable();
            initialize();
        }

        explicit File(const std::string &FileName_, CreateMode createMode_, size_t logLevel_ = 2) : File(FileName_, AccessMode::READWRITE, createMode_, logLevel_) {}

        ~File() {
            auto savedLog = h5pp::Logger::log->name();
            h5pp::Logger::setLogger("h5pp-exit", logLevel, false);
            try {
                if(h5pp::Counter::ActiveFileCounter::getCount() == 1) {
                    H5Pclose(plist_facc);
                    H5Pclose(plist_xfer);
                    H5Pclose(plist_lncr);
                    h5pp::Type::Complex::closeTypes();
                }
                h5pp::Counter::ActiveFileCounter::decrementCounter(FileName.string());
                if(h5pp::Counter::ActiveFileCounter::getCount() == 0) {
                    h5pp::Logger::log->debug(
                        "Closing file: {}.", FileName.string(), h5pp::Counter::ActiveFileCounter::getCount(), h5pp::Counter::ActiveFileCounter::OpenFileNames());

                } else {
                    h5pp::Logger::log->debug("Closing file: {}. There are still {} files open: {}",
                                             FileName.string(),
                                             h5pp::Counter::ActiveFileCounter::getCount(),
                                             h5pp::Counter::ActiveFileCounter::OpenFileNames());
                }
            } catch(...) { h5pp::Logger::log->warn("Failed to properly close file: ", getFilePath()); }
            h5pp::Logger::setLogger(savedLog, logLevel, false);
        }

        File &operator=(const File &other) {
            h5pp::Logger::log->debug("Assign-constructing this file [{}] from given file: [{}]. Previously initialized (this): {}. Previously initialized (other): {}",
                                     FileName.string(),
                                     other.getFileName(),
                                     hasInitialized,
                                     other.hasInitialized);
            if(&other != this) {
                if(hasInitialized) { h5pp::Counter::ActiveFileCounter::decrementCounter(FileName.string()); }
                if(other.hasInitialized) {
                    logLevel = other.logLevel;
                    h5pp::Logger::setLogger("h5pp", logLevel, false);
                    accessMode = other.getAccessMode();
                    createMode = CreateMode::OPEN;
                    FileName   = other.FileName;
                    FilePath   = other.FilePath;
                    checkIfCompressionIsAvailable();
                    initialize();
                }
            }
            return *this;
        }

        hid_t openFileHandle() const {
            try {
                if(hasInitialized) {
                    switch(accessMode) {
                        case(AccessMode::READONLY): {
                            h5pp::Logger::log->trace("Opening file handle in READONLY mode");
                            hid_t fileHandle = H5Fopen(FilePath.c_str(), H5F_ACC_RDONLY, plist_facc);
                            if(fileHandle < 0) {
                                H5Eprint(H5E_DEFAULT, stderr);
                                throw std::runtime_error("Failed to open file in read-only mode: " + FilePath.string());
                            } else {
                                return fileHandle;
                            }
                        }
                        case(AccessMode::READWRITE): {
                            h5pp::Logger::log->trace("Opening file handle in READWRITE mode");
                            hid_t fileHandle = H5Fopen(FilePath.c_str(), H5F_ACC_RDWR, plist_facc);
                            if(fileHandle < 0) {
                                H5Eprint(H5E_DEFAULT, stderr);
                                throw std::runtime_error("Failed to open file in read-write mode: " + FilePath.string());
                            } else {
                                return fileHandle;
                            }
                        }
                        default: throw std::runtime_error("Invalid access mode");
                    }
                } else {
                    throw std::runtime_error("File hasn't initialized");
                }

            } catch(std::exception &ex) { throw std::runtime_error("Could not open file handle: " + std::string(ex.what())); }
        }

        herr_t closeFileHandle(hid_t file) const {
            h5pp::Logger::log->trace("Closing file handle");
            herr_t fileClose = H5Fclose(file);
            if(fileClose < 0) {
                H5Eprint(H5E_DEFAULT, stderr);
                throw std::runtime_error("Failed to close file handle");
            } else {
                return fileClose;
            }
        }

        void setCreateMode(CreateMode createMode_) { createMode = createMode_; }
        void setAccessMode(AccessMode accessMode_) { accessMode = accessMode_; }
        void enableDefaultExtendable() { defaultExtendable = true; }
        void disableDefaultExtendable() { defaultExtendable = false; }

        // Functions for querying the file

        CreateMode getCreateMode() const { return createMode; }
        AccessMode getAccessMode() const { return accessMode; }

        std::string getFileName() const { return FileName.string(); }
        std::string getFilePath() const { return FilePath.string(); }

        // Functions for querying datasets
        std::vector<size_t> getDatasetDims(const std::string &datasetPath) {
            hid_t                file = openFileHandle();
            std::vector<hsize_t> dims;
            try {
                hid_t dataset  = h5pp::Hdf5::openLink(file, datasetPath);
                hid_t memspace = H5Dget_space(dataset);
                int   ndims    = H5Sget_simple_extent_ndims(memspace);
                dims.resize(ndims);
                H5Sget_simple_extent_dims(memspace, dims.data(), nullptr);
                h5pp::Hdf5::closeLink(dataset);
                closeFileHandle(file);
            } catch(std::exception &ex) {
                closeFileHandle(file);
                H5Eprint(H5E_DEFAULT, stderr);
                throw std::runtime_error("getDatasetDims failed. Dataset name [" + datasetPath + "]  | reason: " + std::string(ex.what()));
            }
            std::vector<size_t> retdims;
            std::copy(dims.begin(), dims.end(), back_inserter(retdims));
            return retdims;
        }

        void setLogLevel(size_t logLevelZeroToFive) {
            logLevel = logLevelZeroToFive;
            h5pp::Logger::setLogLevel(logLevelZeroToFive);
        }

        // Functions related to datasets

        void setCompressionLevel(size_t compressionLevelZeroToNine) {
            if(checkIfCompressionIsAvailable()) {
                if(compressionLevelZeroToNine < 10) {
                    h5pp::Logger::log->debug("Compression level set to {}", compressionLevelZeroToNine);
                    compressionLevel = compressionLevelZeroToNine;
                } else {
                    h5pp::Logger::log->warn("Given invalid compression level: {}. Expected value 0 to 9");
                }
            } else {
                h5pp::Logger::log->info("Compression not available");
            }
        }

        size_t getCompressionLevel() const { return compressionLevel; }

        template<typename DataType> void writeDataset(const DataType &data, const std::string &datasetPath, std::optional<bool> extendable = std::nullopt);

        template<typename DataType, typename T, size_t N>
        void writeDataset(const DataType &data, const T (&dims)[N], const std::string &datasetPath, std::optional<bool> extendable = std::nullopt);

        template<typename DataType> void writeDataset(const DataType &data, const DatasetProperties &props);

        template<typename DataType, typename T, size_t N>
        void writeDataset(const DataType *data, const T (&dims)[N], const std::string &datasetPath, std::optional<bool> extendable = std::nullopt);

        template<typename DataType, typename T, typename = std::enable_if_t<std::is_integral<T>::value>>
        void writeDataset(const DataType *data, T size, const std::string &datasetPath, std::optional<bool> extendable = std::nullopt) {
            return writeDataset(data, {size}, datasetPath, extendable);
        }

        template<typename DataType> void readDataset(DataType &data, const std::string &datasetPath) const;

        template<typename DataType> DataType readDataset(const std::string &datasetPath) const;

        template<typename DataType, typename T, size_t N> void readDataset(DataType *data, const T (&dims)[N], const std::string &datasetPath);

        template<typename DataType, typename T, typename = std::enable_if_t<std::is_integral<T>::value>> void readDataset(DataType *data, T size, const std::string &datasetPath) {
            return readDataset(data, {size}, datasetPath);
        }

        // Functions related to attributes
        template<typename DataType> void writeAttribute(const DataType &attribute, const AttributeProperties &aprops);

        template<typename DataType> void writeAttribute(const DataType &attribute, const std::string &attributeName, const std::string &linkPath);

        template<typename DataType> void writeAttributeToFile(const DataType &attribute, const std::string &attributeName);

        inline std::vector<std::string> getAttributeNames(const std::string &linkPath) const {
            hid_t                    file = openFileHandle();
            std::vector<std::string> attributeNames;
            if(h5pp::Hdf5::checkIfLinkExistsRecursively(file, linkPath)) { attributeNames = h5pp::Hdf5::getAttributeNames(file, linkPath); }
            closeFileHandle(file);
            return attributeNames;
        }

        template<typename DataType> void readAttribute(DataType &data, const std::string &attributeName, const std::string &linkPath) const;

        template<typename DataType> DataType readAttribute(const std::string &attributeName, const std::string &linkPath) const;

        bool linkExists(const std::string &link) {
            hid_t file = openFileHandle();
            h5pp::Logger::log->trace("Checking if link exists: [{}]", link);
            bool exists = h5pp::Hdf5::checkIfLinkExistsRecursively(file, link);
            closeFileHandle(file);
            return exists;
        }

        std::vector<std::string> getContentsOfGroup(const std::string &groupName) const {
            hid_t file = openFileHandle();
            h5pp::Logger::log->trace("Getting contents of group: [{}]", groupName);
            auto foundLinks = h5pp::Hdf5::getContentsOfGroup(file, groupName);
            closeFileHandle(file);
            return foundLinks;
        }

        bool fileIsValid() const { return fileIsValid(FilePath); }

        static bool fileIsValid(const fs::path &fileName) { return fs::exists(fileName) and H5Fis_hdf5(fileName.c_str()) > 0; }

        inline void createGroupLink(const std::string &group_relative_name) {
            hid_t file = openFileHandle();
            h5pp::Logger::log->trace("Creating group: [{}]", group_relative_name);
            h5pp::Hdf5::create_group_link(file, plist_lncr, group_relative_name);
            closeFileHandle(file);
        }

        inline void writeSymbolicLink(const std::string &src_path, const std::string &tgt_path) {
            hid_t file = openFileHandle();
            h5pp::Logger::log->trace("Creating symbolik link: [{}] --> [{}]", src_path, tgt_path);
            h5pp::Hdf5::write_symbolic_link(file, src_path, tgt_path);
            closeFileHandle(file);
        }

        private:
        fs::path getNewFileName(const fs::path &fileName) const {
            int      i           = 1;
            fs::path newFileName = fileName;
            while(fs::exists(newFileName)) { newFileName.replace_filename(fileName.stem().string() + "-" + std::to_string(i++) + fileName.extension().string()); }
            return newFileName;
        }

        void createDatasetLink(hid_t file, const DatasetProperties &props) {
            if(not props.linkExists) {
                h5pp::Logger::log->trace("Creating dataset link: [{}]", props.dsetName);
                h5pp::Hdf5::createDatasetLink(file, plist_lncr, props);
            }
        }

        void selectHyperslab(const hid_t &filespace, const hid_t &memspace) { h5pp::Hdf5::select_hyperslab(filespace, memspace); }

        template<typename DataType>
        bool determineIfExtendable(const DataType &data, const std::string &dsetName, std::optional<bool> userPrefersExtendable, std::optional<bool> dsetExists = std::nullopt);

        void initialize() {
            auto savedLog = h5pp::Logger::log->name();
            h5pp::Logger::setLogger("h5pp-init", logLevel, false);
            /* Turn off error handling permanently */
            error_stack                          = H5Eget_current_stack();
            herr_t turnOffAutomaticErrorPrinting = H5Eset_auto(error_stack, nullptr, nullptr);
            if(turnOffAutomaticErrorPrinting < 0) {
                H5Eprint(H5E_DEFAULT, stderr);
                throw std::runtime_error("Failed to turn off H5E error printing");
            }
            plist_facc = H5Pcreate(H5P_FILE_ACCESS);
            plist_lncr = H5Pcreate(H5P_LINK_CREATE); // Create missing intermediate group if they don't exist
            plist_xfer = H5Pcreate(H5P_DATASET_XFER);
            plist_lapl = H5Pcreate(H5P_LINK_ACCESS);
            H5Pset_create_intermediate_group(plist_lncr, 1);
            setOutputFilePath();
            h5pp::Type::Complex::initTypes();
            hasInitialized = true;
            //            fileCount++;
            h5pp::Counter::ActiveFileCounter::incrementCounter(FileName.string());
            h5pp::Logger::setLogger("h5pp|" + FileName.string(), logLevel, false);
            //            h5pp::Logger::setLogger(savedLog,logLevel,false);
        }

        void setOutputFilePath() {
            h5pp::Logger::log->trace("Attempting to set file name and path. File name [{}] path [{}]. Has initialized: {}", FileName.string(), FilePath.string(), hasInitialized);

            // There are different possibilities:
            // 1) File is being initialized from another h5pp File (e.g. by copy or assignment) In that case the following applies:
            //      a) FileName =  just a filename such as myfile.h5 without parent path.
            //      b) FilePath =  an absolute path to the file such as /home/yada/yada/myFile.h5
            // 2) File did not exist previously
            //      a) FileName = a filename possibly with parent path or not, such as ../myDir/myFile.h5 or just myFile
            //      b) FilePath = empty

            // Take case 2 first and make it into a case 1
            if(FilePath.empty()) {
                h5pp::Logger::log->trace("File path empty. Detecting path...");
                FilePath = fs::absolute(FileName);
                FileName = FilePath.filename();
            }

            // Now we expect case 1 to hold.

            fs::path currentDir = fs::current_path();
            h5pp::Logger::log->trace("Current path        : {}", fs::current_path().string());
            h5pp::Logger::log->debug("Detected file name  : {}", FileName.string());
            h5pp::Logger::log->debug("Detected file path  : {}", FilePath.string());

            try {
                if(fs::create_directories(FilePath.parent_path())) {
                    h5pp::Logger::log->trace("Created directory: {}", FilePath.parent_path().string());
                } else {
                    h5pp::Logger::log->trace("Directory already exists: {}", FilePath.parent_path().string());
                }
            } catch(std::exception &ex) { throw std::runtime_error("Failed to create directory: " + std::string(ex.what())); }

            switch(createMode) {
                case CreateMode::OPEN: {
                    h5pp::Logger::log->debug("File mode [OPEN]: Opening file [{}]", FilePath.string());
                    try {
                        if(fileIsValid(FilePath)) {
                            hid_t file;
                            switch(accessMode) {
                                case(AccessMode::READONLY): file = H5Fopen(FilePath.c_str(), H5F_ACC_RDONLY, plist_facc); break;
                                case(AccessMode::READWRITE): file = H5Fopen(FilePath.c_str(), H5F_ACC_RDWR, plist_facc); break;
                                default: throw std::runtime_error("Invalid access mode");
                            }
                            if(file < 0) {
                                H5Eprint(H5E_DEFAULT, stderr);
                                throw std::runtime_error("Failed to open file: [" + FilePath.string() + "]");
                            }

                            H5Fclose(file);
                            FilePath = fs::canonical(FilePath);
                        } else {
                            throw std::runtime_error("Invalid file: [" + FilePath.string() + "]");
                        }
                    } catch(std::exception &ex) { throw std::runtime_error("Failed to open hdf5 file: " + std::string(ex.what())); }
                    break;
                }
                case CreateMode::TRUNCATE: {
                    h5pp::Logger::log->debug("File mode [TRUNCATE]: Overwriting file if it exists: [{}]", FilePath.string());
                    try {
                        hid_t file = H5Fcreate(FilePath.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, plist_facc);
                        if(file < 0) {
                            H5Eprint(H5E_DEFAULT, stderr);
                            throw std::runtime_error("Failed to create file: [" + FilePath.string() + "]");
                        }
                        H5Fclose(file);
                        FilePath = fs::canonical(FilePath);
                    } catch(std::exception &ex) { throw std::runtime_error("Failed to create hdf5 file: " + std::string(ex.what())); }
                    break;
                }
                case CreateMode::RENAME: {
                    try {
                        h5pp::Logger::log->debug("File mode [RENAME]: Finding new file name if previous file exists: [{}]", FilePath.string());
                        if(fileIsValid(FilePath)) {
                            FilePath = getNewFileName(FilePath);
                            h5pp::Logger::log->info("Previous file exists. Choosing new file name: [{}] ---> [{}]", FileName.string(), FilePath.filename().string());
                            FileName = FilePath.filename();
                        }
                        hid_t file = H5Fcreate(FilePath.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, plist_facc);
                        if(file < 0) {
                            H5Eprint(H5E_DEFAULT, stderr);
                            throw std::runtime_error("Failed to create file: [" + FilePath.string() + "]");
                        }
                        H5Fclose(file);
                        FilePath = fs::canonical(FilePath);
                    } catch(std::exception &ex) { throw std::runtime_error("Failed to create renamed hdf5 file : " + std::string(ex.what())); }
                    break;
                }
                default: {
                    h5pp::Logger::log->error("File Mode not set. Choose  CreateMode::<OPEN|TRUNCATE|RENAME>");
                    throw std::runtime_error("File Mode not set. Choose  CreateMode::<OPEN|TRUNCATE|RENAME>");
                }
            }
        }
    };

}

template<typename DataType> void h5pp::File::writeDataset(const DataType &data, const DatasetProperties &props) {
    hid_t file = openFileHandle();
    createDatasetLink(file, props);
    h5pp::Logger::log->debug("Writing dataset: [{}] | size {} | rank {} | extent {}", props.dsetName, props.size, props.ndims, props.dims);
    if(props.extendable) { h5pp::Hdf5::setExtentDataset(file, props); }
    hid_t dataset   = h5pp::Hdf5::openLink(file, props.dsetName);
    hid_t filespace = H5Dget_space(dataset);
    selectHyperslab(filespace, props.memSpace);

    try {
        if(props.linkExists and not props.extendable) {
            hsize_t old_dsetSize = H5Dget_storage_size(dataset);
            hsize_t new_dsetSize = h5pp::Utils::getByteSize(data);
            if(old_dsetSize != new_dsetSize) {
                Logger::log->critical("The non-extendable dataset [{}] is being overwritten with a different size.\n\t Old size = {} bytes. New size = {} bytes",
                                      props.dsetName,
                                      old_dsetSize,
                                      new_dsetSize);
                throw std::runtime_error("Overwriting non-extendable dataset with different size");
            }
        }

        if constexpr(tc::hasMember_c_str<DataType>::value) {
            retval = H5Dwrite(dataset, props.dataType, props.memSpace, filespace, H5P_DEFAULT, data.c_str());
            if(retval < 0) {
                H5Eprint(H5E_DEFAULT, stderr);
                throw std::runtime_error("Failed to write text to file");
            }
        } else if constexpr(tc::hasMember_data<DataType>::value) {
            retval = H5Dwrite(dataset, props.dataType, props.memSpace, filespace, H5P_DEFAULT, data.data());
            if(retval < 0) {
                H5Eprint(H5E_DEFAULT, stderr);
                throw std::runtime_error("Failed to write data to file");
            }
        } else {
            retval = H5Dwrite(dataset, props.dataType, props.memSpace, filespace, H5P_DEFAULT, &data);
            if(retval < 0) {
                H5Eprint(H5E_DEFAULT, stderr);
                throw std::runtime_error("Failed to write number to file");
            }
        }
    } catch(std::exception &ex) {
        h5pp::Hdf5::closeLink(dataset);
        closeFileHandle(file);
        throw std::runtime_error("Write to file failed [" + props.dsetName + "]: " + std::string(ex.what()));
    }
    h5pp::Hdf5::closeLink(dataset);
    closeFileHandle(file);
}

template<typename DataType> void h5pp::File::writeDataset(const DataType &data, const std::string &datasetPath, std::optional<bool> extendable) {
    if(accessMode == AccessMode::READONLY) { throw std::runtime_error("Attempted to write to read-only file"); }
    DatasetProperties props;
    props.linkExists       = linkExists(datasetPath);
    props.extendable       = determineIfExtendable(data, datasetPath, extendable, props.linkExists);
    props.dataType         = h5pp::Type::getDataType<DataType>();
    props.size             = h5pp::Utils::getSize<DataType>(data);
    props.ndims            = h5pp::Utils::getRank<DataType>();
    props.dims             = h5pp::Utils::getDimensions<DataType>(data);
    props.chunkDims        = h5pp::Utils::getChunkDimensions<DataType>(data);
    props.dsetName         = datasetPath;
    props.memSpace         = h5pp::Utils::getMemSpace(props.ndims, props.dims);
    props.dataSpace        = h5pp::Utils::getDataSpace(props.ndims, props.dims, props.extendable);
    props.compressionLevel = compressionLevel;
    if constexpr(tc::is_eigen_type<DataType>::value and not tc::is_eigen_1d<DataType>::value) {
        h5pp::Logger::log->debug("Converting data to row-major storage order");
        const auto tempRowm = Textra::to_RowMajor(data); // Convert to Row Major first;
        writeDataset(tempRowm, props);
    } else {
        if(H5Tequal(props.dataType, H5T_C_S1)) {
            // Read more about this step here
            // http://www.astro.sunysb.edu/mzingale/io_tutorial/HDF5_simple/hdf5_simple.c
            props.size = h5pp::Utils::setStringSize(props.dataType, props.size);
            writeDataset(data, props);
        } else {
            writeDataset(data, props);
        }
    }
}

template<typename DataType, typename T, std::size_t N>
void h5pp::File::writeDataset(const DataType &data, const T (&dims)[N], const std::string &datasetPath, std::optional<bool> extendable) {
    if(accessMode == AccessMode::READONLY) { throw std::runtime_error("Attempted to write to read-only file"); }
    static_assert(std::is_integral_v<T>);
    static_assert(N > 0, "Dimensions of given data are too few, N == 0");
    DatasetProperties props;
    props.linkExists       = linkExists(datasetPath);
    props.extendable       = determineIfExtendable(data, datasetPath, extendable);
    props.dataType         = h5pp::Type::getDataType<DataType>();
    props.dims             = std::vector<hsize_t>(dims, dims + N);
    props.ndims            = (int) props.dims.size();
    props.chunkDims        = props.dims;
    props.dsetName         = datasetPath;
    props.size             = 1;
    props.compressionLevel = compressionLevel;
    for(const auto &dim : dims) props.size *= dim;
    props.memSpace  = h5pp::Utils::getMemSpace(props.ndims, props.dims);
    props.dataSpace = h5pp::Utils::getDataSpace(props.ndims, props.dims, props.extendable);

    if constexpr(tc::is_eigen_type<DataType>::value and not tc::is_eigen_1d<DataType>::value) {
        h5pp::Logger::log->debug("Converting data to row-major storage order");
        const auto temp_rowm = Textra::to_RowMajor(data); // Convert to Row Major first;
        writeDataset(temp_rowm, props);
    } else {
        if(H5Tequal(props.dataType, H5T_C_S1)) {
            // Read more about this step here
            // http://www.astro.sunysb.edu/mzingale/io_tutorial/HDF5_simple/hdf5_simple.c
            h5pp::Utils::setStringSize(props.dataType, props.size);
            writeDataset(data, props);
        } else {
            writeDataset(data, props);
        }
    }
}

template<typename DataType, typename T, size_t N>
void h5pp::File::writeDataset(const DataType *data, const T (&dims)[N], const std::string &datasetPath, std::optional<bool> extendable) {
    // This function takes a pointer and a specifiation of dimensions. Easiest thing to do
    // is to wrap this in an Eigen::Tensor and send to writeDataset
    // Note that C-style arrays are generally row-major order already
    Eigen::DSizes<long, N> dimsizes;
    std::copy_n(std::begin(dims), N, dimsizes.begin()); // Copy the dimensions (not the data)
    auto tensorWrap = Eigen::TensorMap<const Eigen::Tensor<const DataType, N, Eigen::RowMajor>>(data, dimsizes);
    writeDataset(tensorWrap, datasetPath, extendable);
}

template<typename DataType> void h5pp::File::readDataset(DataType &data, const std::string &datasetPath) const {
    hid_t file = openFileHandle();
    try {
        hid_t                dataset  = h5pp::Hdf5::openLink(file, datasetPath);
        hid_t                memspace = H5Dget_space(dataset);
        hid_t                datatype = H5Dget_type(dataset);
        int                  ndims    = H5Sget_simple_extent_ndims(memspace);
        std::vector<hsize_t> dims(ndims);
        H5Sget_simple_extent_dims(memspace, dims.data(), nullptr);
        hsize_t size = 1;
        for(const auto &dim : dims) size *= dim;
        h5pp::Logger::log->debug("Reading dataset: [{}] | size {} | rank {} | dim extents {}", datasetPath, size, ndims, dims);

        if constexpr(tc::is_eigen_core<DataType>::value) {
            if(data.IsRowMajor) {
                // Data is RowMajor in HDF5, user gave a RowMajor container so no need to swap layout.
                data.resize(dims[0], dims[1]);
                retval = H5LTread_dataset(file, datasetPath.c_str(), datatype, data.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Matrix rowmajor dataset"); }
            } else {
                // Data is RowMajor in HDF5, user gave a ColMajor container we need to swap layout.
                Eigen::Matrix<typename DataType::Scalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> matrixRowmajor;
                matrixRowmajor.resize(dims[0], dims[1]); // Data is transposed in HDF5!
                retval = H5LTread_dataset(file, datasetPath.c_str(), datatype, matrixRowmajor.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Matrix colmajor dataset"); }
                data = matrixRowmajor;
            }
        } else if constexpr(tc::is_eigen_tensor<DataType>()) {
            Eigen::DSizes<long, DataType::NumDimensions> eigenDims;
            std::copy(dims.begin(), dims.end(), eigenDims.begin());
            if constexpr(DataType::Options == Eigen::RowMajor) {
                // Data is RowMajor in HDF5, user gave a RowMajor container so no need to swap layout.
                data.resize(eigenDims);
                retval = H5LTread_dataset(file, datasetPath.c_str(), datatype, data.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Tensor rowmajor dataset"); }
            } else {
                // Data is RowMajor in HDF5, user gave a ColMajor container we need to swap layout.
                Eigen::Tensor<typename DataType::Scalar, DataType::NumIndices, Eigen::RowMajor> tensorRowmajor(eigenDims);
                H5LTread_dataset(file, datasetPath.c_str(), datatype, tensorRowmajor.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Tensor colmajor dataset"); }
                data = Textra::to_ColMajor(tensorRowmajor);
            }
        } else if constexpr(tc::is_std_vector<DataType>::value) {
            assert(ndims == 1 and "Vector cannot take 2D datasets");
            data.resize(dims[0]);
            H5LTread_dataset(file, datasetPath.c_str(), datatype, data.data());
            if(retval < 0) { throw std::runtime_error("Failed to read std::vector dataset"); }
        } else if constexpr(std::is_same<std::string, DataType>::value) {
            assert(ndims == 1 and "std string needs to have 1 dimension");
            hsize_t stringsize = H5Dget_storage_size(dataset);
            data.resize(stringsize);
            retval = H5LTread_dataset(file, datasetPath.c_str(), datatype, data.data());
            if(retval < 0) { throw std::runtime_error("Failed to read std::string dataset"); }

        } else if constexpr(std::is_arithmetic<DataType>::value) {
            retval = H5LTread_dataset(file, datasetPath.c_str(), datatype, &data);
            if(retval < 0) { throw std::runtime_error("Failed to read arithmetic type dataset"); }

        } else {
            Logger::log->error("Attempted to read dataset of unknown type. Name: [{}] | Type: [{}]", datasetPath, typeid(data).name());
            throw std::runtime_error("Attempted to read dataset of unknown type");
        }
        H5Tclose(datatype);
        h5pp::Hdf5::closeLink(dataset);

    } catch(std::exception &ex) {
        closeFileHandle(file);
        H5Eprint(H5E_DEFAULT, stderr);
        throw std::runtime_error(fmt::format("readDataset failed. Dataset name [{}] | Link [{}] | type [{}] | Reason: {}", datasetPath, typeid(data).name(), ex.what()));
    }

    closeFileHandle(file);
}

template<typename DataType, typename T, size_t N> void h5pp::File::readDataset(DataType *data, const T (&dims)[N], const std::string &datasetPath) {
    // This function takes a pointer and a specifiation of dimensions. Easiest thing to do
    // is to wrap this in an Eigen::Tensor and send to readDataset
    Eigen::DSizes<long, N> dimsizes;
    std::copy_n(std::begin(dims), N, dimsizes.begin());
    auto tensorWrap = Eigen::TensorMap<Eigen::Tensor<DataType, N>>(data, dimsizes);
    readDataset(tensorWrap, datasetPath);
}

template<typename DataType> DataType h5pp::File::readDataset(const std::string &datasetPath) const {
    DataType data;
    readDataset(data, datasetPath);
    return data;
}

template<typename DataType> void h5pp::File::writeAttributeToFile(const DataType &attribute, const std::string &attributeName) {
    hid_t file     = openFileHandle();
    hid_t datatype = h5pp::Type::getDataType<DataType>();
    auto  size     = h5pp::Utils::getSize(attribute);
    auto  ndims    = h5pp::Utils::getRank<DataType>();
    auto  dims     = h5pp::Utils::getDimensions(attribute);
    hid_t memspace = h5pp::Utils::getMemSpace(ndims, dims);
    h5pp::Logger::log->debug("Writing attribute to file: [{}] | size {} | rank {} | dim extents {}", attributeName, size, ndims, dims);

    if constexpr(tc::hasMember_c_str<DataType>::value or std::is_same<char *, typename std::decay<DataType>::type>::value) { h5pp::Utils::setStringSize(datatype, size); }

    hid_t attributeId = H5Acreate(file, attributeName.c_str(), datatype, memspace, H5P_DEFAULT, H5P_DEFAULT);
    if constexpr(tc::hasMember_c_str<DataType>::value) {
        retval = H5Awrite(attributeId, datatype, attribute.c_str());
    } else if constexpr(tc::hasMember_data<DataType>::value) {
        retval = H5Awrite(attributeId, datatype, attribute.data());
    } else {
        retval = H5Awrite(attributeId, datatype, &attribute);
    }
    if(retval < 0) {
        H5Eprint(H5E_DEFAULT, stderr);
        throw std::runtime_error("Failed to write attribute [ " + attributeName + " ] to file");
    }
    H5Tclose(datatype);
    H5Aclose(attributeId);
    closeFileHandle(file);
}

template<typename DataType> void h5pp::File::writeAttribute(const DataType &attribute, const AttributeProperties &aprops) {
    hid_t file = openFileHandle();
    if(h5pp::Hdf5::checkIfLinkExistsRecursively(file, aprops.linkPath)) {
        if(not h5pp::Hdf5::checkIfAttributeExists(file, aprops.linkPath, aprops.attrName)) {
            hid_t linkObject  = h5pp::Hdf5::openLink(file, aprops.linkPath);
            hid_t attributeId = H5Acreate(linkObject, aprops.attrName.c_str(), aprops.dataType, aprops.memSpace, H5P_DEFAULT, H5P_DEFAULT);
            h5pp::Logger::log->trace("Writing attribute: [{}] | size {} | rank {} | dim extents {}", aprops.attrName, aprops.size, aprops.ndims, aprops.dims);
            try {
                if constexpr(tc::hasMember_c_str<DataType>::value) {
                    retval = H5Awrite(attributeId, aprops.dataType, attribute.c_str());
                } else if constexpr(tc::hasMember_data<DataType>::value) {
                    retval = H5Awrite(attributeId, aprops.dataType, attribute.data());
                } else {
                    retval = H5Awrite(attributeId, aprops.dataType, &attribute);
                }
                if(retval < 0) { throw std::runtime_error("Failed to write attribute. Attribute name: [ " + aprops.attrName + " ]"); }
            } catch(std::exception &ex) {
                H5Aclose(attributeId);
                h5pp::Hdf5::closeLink(linkObject);
                closeFileHandle(file);
                H5Eprint(H5E_DEFAULT, stderr);
                throw std::runtime_error("Link [ " + aprops.linkPath + " ]: " + std::string(ex.what()));
            }

            H5Aclose(attributeId);
            h5pp::Hdf5::closeLink(linkObject);
            closeFileHandle(file);
        }
    } else {
        closeFileHandle(file);
        std::string error = "Link " + aprops.linkPath + " does not exist, yet attribute is being written.";
        h5pp::Logger::log->critical(error);
        throw(std::logic_error(error));
    }
}

template<typename DataType> void h5pp::File::writeAttribute(const DataType &attribute, const std::string &attributeName, const std::string &linkPath) {
    AttributeProperties aprops;
    aprops.dataType = h5pp::Type::getDataType<DataType>();
    aprops.size     = h5pp::Utils::getSize(attribute);
    aprops.ndims    = h5pp::Utils::getRank<DataType>();
    aprops.dims     = h5pp::Utils::getDimensions(attribute);
    aprops.attrName = attributeName;
    aprops.linkPath = linkPath;
    aprops.memSpace = h5pp::Utils::getMemSpace(aprops.ndims, aprops.dims);

    if constexpr(tc::is_eigen_type<DataType>::value and not tc::is_eigen_1d<DataType>::value) {
        h5pp::Logger::log->debug("Converting data to row-major storage order");
        const auto tempRowm = Textra::to_RowMajor(attribute); // Convert to Row Major first;
        writeAttribute(tempRowm, aprops);
    } else if constexpr(tc::hasMember_c_str<DataType>::value or std::is_same<char *, typename std::decay<DataType>::type>::value) {
        aprops.size = h5pp::Utils::setStringSize(aprops.dataType, aprops.size);
        writeAttribute(attribute, aprops);
    } else {
        writeAttribute(attribute, aprops);
    }
}

template<typename DataType> void h5pp::File::readAttribute(DataType &data, const std::string &attributeName, const std::string &linkPath) const {
    hid_t file = openFileHandle();
    try {
        hid_t link           = h5pp::Hdf5::openLink(file, linkPath);
        hid_t link_attribute = H5Aopen_name(link, attributeName.c_str());

        hid_t                memspace = H5Aget_space(link_attribute);
        hid_t                datatype = H5Aget_type(link_attribute);
        int                  ndims    = H5Sget_simple_extent_ndims(memspace);
        std::vector<hsize_t> dims(ndims);
        H5Sget_simple_extent_dims(memspace, dims.data(), nullptr);
        hsize_t size = 1;
        for(const auto &dim : dims) size *= dim;

        h5pp::Logger::log->debug(
            "Reading attribute: [{}] | link {} | size {} | rank {} | dim extents {} | type {}", attributeName, linkPath, size, ndims, dims, typeid(data).name());
        if(not h5pp::Utils::typeSizesMatch<DataType>(datatype)) throw std::runtime_error("DataTypes do not match");

        if constexpr(tc::is_eigen_core<DataType>::value) {
            if(data.IsRowMajor) {
                // Data is RowMajor in HDF5, user gave a RowMajor container so no need to swap layout.
                data.resize(dims[0], dims[1]);
                retval = H5Aread(link_attribute, datatype, data.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Matrix rowmajor dataset"); }
            } else {
                // Data is RowMajor in HDF5, user gave a ColMajor container so we need to swap the layout.
                Eigen::Matrix<typename DataType::Scalar, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> matrixRowmajor;
                matrixRowmajor.resize(dims[0], dims[1]); // Data is transposed in HDF5!
                retval = H5Aread(link_attribute, datatype, matrixRowmajor.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Matrix colmajor dataset"); }
                data = matrixRowmajor;
            }

        } else if constexpr(tc::is_eigen_tensor<DataType>()) {
            Eigen::DSizes<long, DataType::NumDimensions> eigenDims;
            std::copy(dims.begin(), dims.end(), eigenDims.begin());
            if constexpr(DataType::Options == Eigen::RowMajor) {
                // Data is RowMajor in HDF5, user gave a RowMajor container so no need to swap layout.
                data.resize(eigenDims);
                retval = H5Aread(link_attribute, datatype, data.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Tensor rowmajor dataset"); }
            } else {
                // Data is RowMajor in HDF5, user gave a ColMajor container so we need to swap the layout.
                Eigen::Tensor<typename DataType::Scalar, DataType::NumIndices, Eigen::RowMajor> tensorRowmajor(eigenDims);
                retval = H5Aread(link_attribute, datatype, tensorRowmajor.data());
                if(retval < 0) { throw std::runtime_error("Failed to read Eigen Tensor colmajor dataset"); }
                data = Textra::to_ColMajor(tensorRowmajor);
            }
        }

        else if constexpr(tc::is_std_vector<DataType>::value) {
            if(ndims != 1) throw std::runtime_error("Vector cannot take datatypes with dimension: " + std::to_string(ndims));
            data.resize(dims[0]);
            retval = H5Aread(link_attribute, datatype, data.data());
            if(retval < 0) { throw std::runtime_error("Failed to read std::vector dataset"); }
        } else if constexpr(tc::is_std_array<DataType>::value) {
            if(ndims != 1) throw std::runtime_error("Array cannot take datatypes with dimension: " + std::to_string(ndims));
            retval = H5Aread(link_attribute, datatype, data.data());
            if(retval < 0) { throw std::runtime_error("Failed to read std::vector dataset"); }
        } else if constexpr(std::is_same<std::string, DataType>::value) {
            if(ndims != 1) throw std::runtime_error("std::string expected dimension 1. Got: " + std::to_string(ndims));
            hsize_t stringsize = H5Tget_size(datatype);
            data.resize(stringsize);
            retval = H5Aread(link_attribute, datatype, data.data());
            if(retval < 0) { throw std::runtime_error("Failed to read std::string dataset"); }

        } else if constexpr(std::is_arithmetic<DataType>::value or tc::is_StdComplex<DataType>()) {
            retval = H5Aread(link_attribute, datatype, &data);
            if(retval < 0) { throw std::runtime_error("Failed to read arithmetic type dataset"); }
        } else {
            Logger::log->error("Attempted to read attribute of unknown type. Name: [{}] | Type: [{}]", attributeName, typeid(data).name());
            throw std::runtime_error("Attempted to read dataset of unknown type");
        }
        H5Tclose(datatype);
        H5Aclose(link_attribute);
        h5pp::Hdf5::closeLink(link);

    } catch(std::exception &ex) {
        closeFileHandle(file);
        H5Eprint(H5E_DEFAULT, stderr);
        throw std::runtime_error(
            fmt::format("readAttribute failed. Attribute name [{}] | Link [{}] | type [{}] | Reason: {}", attributeName, linkPath, typeid(data).name(), ex.what()));
    }

    closeFileHandle(file);
}

template<typename DataType> DataType h5pp::File::readAttribute(const std::string &attributeName, const std::string &linkPath) const {
    DataType data;
    readAttribute(data, attributeName, linkPath);
    return data;
}

template<typename DataType>
bool h5pp::File::determineIfExtendable(const DataType &data, const std::string &dsetName, std::optional<bool> userPrefersExtendable, std::optional<bool> dsetExists) {
    hsize_t size     = h5pp::Utils::getSize<DataType>(data);
    hsize_t rank     = h5pp::Utils::getRank<DataType>();
    hid_t   datatype = h5pp::Type::getDataType<DataType>();
    bool    isLarge  = size * H5Tget_size(datatype) >= h5pp::Constants::max_size_contiguous;
    if(not dsetExists.has_value()) dsetExists = linkExists(dsetName);
    bool isUnlimited = false;
    H5Tclose(datatype);

    if(dsetExists.value()) {
        hid_t                file      = openFileHandle();
        hid_t                dataSet   = h5pp::Hdf5::openLink(file, dsetName);
        hid_t                dataSpace = H5Dget_space(dataSet);
        hsize_t              ndims     = H5Sget_simple_extent_ndims(dataSpace);
        std::vector<hsize_t> old_dims(ndims);
        std::vector<hsize_t> max_dims(ndims);
        H5Sget_simple_extent_dims(dataSpace, old_dims.data(), max_dims.data());
        if(std::any_of(old_dims.begin(), old_dims.end(), [](int i) { return i < 0; })) { isUnlimited = true; }
        if(std::any_of(max_dims.begin(), max_dims.end(), [](int i) { return i < 0; })) { isUnlimited = true; }
        h5pp::Logger::log->trace("Checking existing if dataset is extendable: [{}] ... {}", dsetName, isUnlimited);
        H5Sclose(dataSpace);
        H5Dclose(dataSet);
        closeFileHandle(file);
    }

    if(userPrefersExtendable) {
        if(userPrefersExtendable.value() and dsetExists.value() and not isUnlimited) {
            Logger::log->warn("Asked for an extendable dataset, but a non-extendable dataset already exists: [{}]. Conversion is not supported!", dsetName);
        }
        if(not userPrefersExtendable.value() and dsetExists.value() and isUnlimited) {
            Logger::log->warn("Asked for a non-extendable dataset, but an extendable dataset already exists: [{}]. Conversion is not supported!", dsetName);
        }

        if(not dsetExists.value()) return userPrefersExtendable.value();
    }

    if(dsetExists.value() and isUnlimited) {
        h5pp::Logger::log->trace("Dataset [{}] is extendable: {}", dsetName, true);
        return true;
    }
    if(dsetExists.value() and not isUnlimited) {
        h5pp::Logger::log->trace("Dataset [{}] is extendable: {}", dsetName, false);
        return false;
    }
    if(not dsetExists.value() and defaultExtendable and rank >= 1) {
        h5pp::Logger::log->trace("Dataset [{}] is extendable: {}", dsetName, true);
        return true;
    }
    if(not dsetExists.value() and isLarge and rank >= 1) {
        h5pp::Logger::log->trace("Dataset [{}] is extendable: {}", dsetName, true);
        return true;
    }
    return false;
}