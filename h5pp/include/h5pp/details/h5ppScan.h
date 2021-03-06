#include "h5ppAttributeProperties.h"
#include "h5ppConstants.h"
#include "h5ppHdf5.h"
#include "h5ppUtils.h"
namespace h5pp::scan {

    inline h5pp::DatasetProperties getDatasetProperties_read(const hid::h5f &          file,
                                                             std::string_view          dsetName,
                                                             const std::optional<bool> dsetExists = std::nullopt,
                                                             const PropertyLists &     plists     = PropertyLists()) {
        // Use this function to get info from existing datasets on file.
        h5pp::logger::log->trace("Reading properties of dataset: [{}] from file", dsetName);
        h5pp::DatasetProperties dsetProps;
        dsetProps.dsetName   = dsetName;
        dsetProps.dsetExists = h5pp::hdf5::checkIfDatasetExists(file, dsetName, dsetExists, plists);
        if(dsetProps.dsetExists.value()) {
            dsetProps.dataSet   = h5pp::hdf5::openObject<hid::h5d>(file, dsetProps.dsetName.value(), dsetProps.dsetExists, dsetProps.plist_dset_access);
            dsetProps.dataType  = H5Dget_type(dsetProps.dataSet);
            dsetProps.dataSpace = H5Dget_space(dsetProps.dataSet);
            dsetProps.memSpace  = H5Dget_space(dsetProps.dataSet);
            dsetProps.ndims     = std::max(1, H5Sget_simple_extent_ndims(dsetProps.dataSpace));
            dsetProps.dims      = std::vector<hsize_t>(dsetProps.ndims.value(), 0);
            H5Sget_simple_extent_dims(dsetProps.dataSpace, dsetProps.dims.value().data(), nullptr);
            dsetProps.size  = H5Sget_simple_extent_npoints(dsetProps.dataSpace);
            dsetProps.bytes = H5Dget_storage_size(dsetProps.dataSet);

            // We read the layout from file. Note that it is not possible to change the layout on existing datasets! Read more here
            // https://support.hdfgroup.org/HDF5/Tutor/layout.html
            dsetProps.plist_dset_create = H5Dget_create_plist(dsetProps.dataSet);
            dsetProps.layout            = H5Pget_layout(dsetProps.plist_dset_create);
            dsetProps.chunkDims         = dsetProps.dims.value(); // Can get modified below
            if(dsetProps.layout.value() == H5D_CHUNKED)
                H5Pget_chunk(dsetProps.plist_dset_create, dsetProps.ndims.value(), dsetProps.chunkDims.value().data()); // Discard returned chunk rank, it's the same as ndims
        } else {
            h5pp::logger::log->info("Given dataset name does not point to a dataset: [{}]", dsetName);
            //            throw std::runtime_error("Given path does not point to a dataset: [{" + dsetProps.dsetName.value() + "]");
        }
        return dsetProps;
    }

    template<typename DataType>
    h5pp::DatasetProperties getDatasetProperties_bootstrap(hid::h5f &                                file,
                                                           std::string_view                          dsetName,
                                                           const DataType &                          data,
                                                           const std::optional<bool>                 dsetExists              = std::nullopt,
                                                           const std::optional<H5D_layout_t>         desiredLayout           = std::nullopt,
                                                           const std::optional<std::vector<hsize_t>> desiredChunkDims        = std::nullopt,
                                                           const std::optional<unsigned int>         desiredCompressionLevel = std::nullopt,
                                                           const PropertyLists &                     plists                  = PropertyLists()) {
        h5pp::logger::log->trace("Inferring properties for future dataset: [{}] from type", dsetName);

        // Use this function to detect info from the given DataType, to later create a dataset from scratch.
        h5pp::DatasetProperties dataProps;
        dataProps.dsetName   = dsetName;
        dataProps.dsetExists = h5pp::hdf5::checkIfLinkExists(file, dsetName, dsetExists, plists);
        // Infer properties from the given datatype
        dataProps.ndims     = h5pp::utils::getRank<DataType>();
        dataProps.dims      = h5pp::utils::getDimensions(data);
        dataProps.size      = h5pp::utils::getSize(data);
        dataProps.bytes     = h5pp::utils::getBytesTotal(data);
        dataProps.dataType  = h5pp::utils::getH5Type<DataType>();                   // We use our own data-type matching to avoid any confusion
        dataProps.size      = h5pp::utils::setStringSize(data, dataProps.dataType); // This only affects strings
        dataProps.layout    = h5pp::utils::decideLayout(dataProps.bytes.value(), desiredLayout);
        dataProps.chunkDims = h5pp::utils::getDefaultChunkDimensions(dataProps.size.value(), dataProps.dims.value(), desiredChunkDims);
        dataProps.memSpace  = h5pp::utils::getMemSpace(dataProps.size.value(), dataProps.ndims.value(), dataProps.dims.value());
        dataProps.dataSpace = h5pp::utils::getDataSpace(dataProps.size.value(), dataProps.ndims.value(), dataProps.dims.value(), dataProps.layout.value());

        if(desiredCompressionLevel) {
            dataProps.compressionLevel = std::min((unsigned int) 9, desiredCompressionLevel.value());
        } else {
            dataProps.compressionLevel = 0;
        }
        dataProps.plist_dset_create = H5Pcreate(H5P_DATASET_CREATE);
        h5pp::hdf5::setDatasetCreationPropertyLayout(dataProps);
        h5pp::hdf5::setDatasetCreationPropertyCompression(dataProps);
        h5pp::hdf5::setDataSpaceExtent(dataProps);
        return dataProps;
    }

    template<typename DataType>
    h5pp::DatasetProperties getDatasetProperties_write(hid::h5f &                                file,
                                                       std::string_view                          dsetName,
                                                       const DataType &                          data,
                                                       std::optional<bool>                       dsetExists              = std::nullopt,
                                                       const std::optional<H5D_layout_t>         desiredLayout           = std::nullopt,
                                                       const std::optional<std::vector<hsize_t>> desiredChunkDims        = std::nullopt,
                                                       const std::optional<unsigned int>         desiredCompressionLevel = std::nullopt,
                                                       const PropertyLists &                     plists                  = PropertyLists()) {
        h5pp::logger::log->trace("Reading properties for writing into dataset: [{}]", dsetName);

        if(not dsetExists) dsetExists = h5pp::hdf5::checkIfLinkExists(file, dsetName, dsetExists, plists);

        if(dsetExists.value()) {
            // We enter overwrite-mode
            // Here we get dataset properties which with which we can update datasets
            auto dsetProps = getDatasetProperties_read(file, dsetName, dsetExists, plists);

            if(dsetProps.ndims != h5pp::utils::getRank<DataType>())
                throw std::runtime_error("Number of dimensions in existing dataset (" + std::to_string(dsetProps.ndims.value()) + ") differ from dimensions in given data (" +
                                         std::to_string(h5pp::utils::getRank<DataType>()) + ")");

            if(not h5pp::hdf5::checkEqualTypesRecursive(dsetProps.dataType, h5pp::utils::getH5Type<DataType>()))
                throw std::runtime_error("Given datatype does not match the type of an existing dataset: " + dsetProps.dsetName.value());

            h5pp::DatasetProperties dataProps;
            // Start by copying properties that are immutable on overwrites
            dataProps.dataSet           = dsetProps.dataSet;
            dataProps.dsetName          = dsetProps.dsetName;
            dataProps.dsetExists        = dsetProps.dsetExists;
            dataProps.layout            = dsetProps.layout;
            dataProps.dataType          = h5pp::utils::getH5Type<DataType>();
            dataProps.ndims             = h5pp::utils::getRank<DataType>();
            dataProps.chunkDims         = dsetProps.chunkDims;
            dataProps.plist_dset_access = dsetProps.plist_dset_create;

            // The rest we can inferr directly from the data
            dataProps.dims  = h5pp::utils::getDimensions(data);
            dataProps.size  = h5pp::utils::getSize(data);
            dataProps.size  = h5pp::utils::setStringSize(data, dataProps.dataType); // This only affects strings
            dataProps.bytes = h5pp::utils::getBytesTotal(data);

            dataProps.memSpace         = h5pp::utils::getMemSpace(dataProps.size.value(), dataProps.ndims.value(), dataProps.dims.value());
            dataProps.dataSpace        = h5pp::utils::getDataSpace(dataProps.size.value(), dataProps.ndims.value(), dataProps.dims.value(), dataProps.layout.value());
            dataProps.compressionLevel = 0; // Not used when overwriting
            h5pp::hdf5::setDataSpaceExtent(dataProps);

            // Make some sanity checks on sizes
            auto dsetMaxDims = h5pp::hdf5::getMaxDimensions(dsetProps.dataSet);
            for(int idx = 0; idx < dsetProps.ndims.value(); idx++) {
                if(dsetMaxDims[idx] != H5S_UNLIMITED and dataProps.layout.value() == H5D_CHUNKED and dataProps.dims.value()[idx] > dsetMaxDims[idx])
                    throw std::runtime_error("Dimension too large. Existing dataset [" + dsetProps.dsetName.value() + "] has a maximum size [" + std::to_string(dsetMaxDims[idx]) +
                                             "] in dimension [" + std::to_string(idx) + "], but the given data has size [" + std::to_string(dataProps.dims.value()[idx]) +
                                             "] in the same dimension. The dataset has layout H5D_CHUNKED but the dimension is not tagged with H5S_UNLIMITED");
                if(dsetMaxDims[idx] != H5S_UNLIMITED and dataProps.layout.value() != H5D_CHUNKED and dataProps.dims.value()[idx] != dsetMaxDims[idx])
                    throw std::runtime_error("Dimensions not equal. Existing dataset [" + dsetProps.dsetName.value() + "] has a maximum size [" + std::to_string(dsetMaxDims[idx]) +
                                             "] in dimension [" + std::to_string(idx) + "], but the given data has size [" + std::to_string(dataProps.dims.value()[idx]) +
                                             "] in the same dimension. Consider using H5D_CHUNKED layout for resizeable datasets");
            }
            return dataProps;
        } else {
            // We enter write-from-scratch mode
            // Use this function to detect info from the given DataType, to later create a dataset from scratch.
            return getDatasetProperties_bootstrap(file, dsetName, data, dsetExists, desiredLayout, desiredChunkDims, desiredCompressionLevel, plists);
        }
    }

    inline h5pp::AttributeProperties getAttributeProperties_read(const hid::h5f &          file,
                                                                 std::string_view          attrName,
                                                                 std::string_view          linkName,
                                                                 const std::optional<bool> attrExists = std::nullopt,
                                                                 const std::optional<bool> linkExists = std::nullopt,
                                                                 const PropertyLists &     plists     = PropertyLists()) {
        // Use this function to get info from existing attributes on file.
        h5pp::logger::log->trace("Reading properties of attribute: [{}] in link [{}] from file", attrName, linkName);
        h5pp::AttributeProperties attrProps;
        attrProps.linkExists = h5pp::hdf5::checkIfLinkExists(file, linkName, linkExists, plists);
        attrProps.attrExists = h5pp::hdf5::checkIfAttributeExists(file, linkName, attrName, attrProps.linkExists, attrExists, plists);
        attrProps.linkName   = linkName;
        attrProps.attrName   = attrName;

        if(attrProps.attrExists.value()) {
            attrProps.linkObject  = h5pp::hdf5::openObject<hid::h5o>(file, attrProps.linkName.value(), attrProps.linkExists, attrProps.plist_attr_access);
            attrProps.attributeId = H5Aopen_name(attrProps.linkObject, std::string(attrProps.attrName.value()).c_str());
            H5I_type_t linkType   = H5Iget_type(attrProps.attributeId.value());
            if(linkType != H5I_ATTR) { throw std::runtime_error("Given attribute name does not point to an attribute: [{" + attrProps.attrName.value() + "]"); }

            attrProps.dataType = H5Aget_type(attrProps.attributeId);
            attrProps.memSpace = H5Aget_space(attrProps.attributeId);

            attrProps.ndims = std::max(1, H5Sget_simple_extent_ndims(attrProps.memSpace));
            attrProps.dims  = std::vector<hsize_t>(attrProps.ndims.value(), 0);
            H5Sget_simple_extent_dims(attrProps.memSpace, attrProps.dims.value().data(), nullptr);
            attrProps.size  = H5Sget_simple_extent_npoints(attrProps.memSpace);
            attrProps.bytes = H5Aget_storage_size(attrProps.attributeId);
        }
        return attrProps;
    }

    template<typename DataType>
    inline h5pp::AttributeProperties getAttributeProperties_bootstrap(const hid::h5f &     file,
                                                                      const DataType &     data,
                                                                      std::string_view     attrName,
                                                                      std::string_view     linkName,
                                                                      std::optional<bool>  attrExists = std::nullopt,
                                                                      std::optional<bool>  linkExists = std::nullopt,
                                                                      const PropertyLists &plists     = PropertyLists()) {
        // Use this function to get info from existing datasets on file.
        h5pp::logger::log->trace("Bootstrapping properties for writing attribute [{}] into link [{}]", attrName, linkName);
        h5pp::AttributeProperties dataProps;
        dataProps.linkExists = h5pp::hdf5::checkIfLinkExists(file, linkName, linkExists, plists);
        dataProps.attrExists = h5pp::hdf5::checkIfAttributeExists(file, linkName, attrName, dataProps.linkExists, attrExists, plists);
        dataProps.linkName   = linkName;
        dataProps.attrName   = attrName;
        if(dataProps.linkExists and dataProps.linkExists.value()) dataProps.linkObject = h5pp::hdf5::openLink(file, dataProps.linkName.value(), dataProps.linkExists, plists);
        if(dataProps.attrExists and dataProps.attrExists.value()) dataProps.attributeId = H5Aopen_name(dataProps.linkObject, std::string(dataProps.attrName.value()).c_str());

        dataProps.dataType = h5pp::utils::getH5Type<DataType>();
        dataProps.size     = h5pp::utils::getSize(data);
        dataProps.size     = h5pp::utils::setStringSize(data, dataProps.dataType); // This only affects strings
        dataProps.bytes    = h5pp::utils::getBytesTotal<DataType>(data);
        dataProps.ndims    = h5pp::utils::getRank<DataType>();
        dataProps.dims     = h5pp::utils::getDimensions(data);
        dataProps.memSpace = h5pp::utils::getMemSpace(dataProps.size.value(), dataProps.ndims.value(), dataProps.dims.value());
        return dataProps;
    }

    template<typename DataType>
    inline h5pp::AttributeProperties getAttributeProperties_write(const hid::h5f &     file,
                                                                  const DataType &     data,
                                                                  std::string_view     attrName,
                                                                  std::string_view     linkName,
                                                                  std::optional<bool>  attrExists = std::nullopt,
                                                                  std::optional<bool>  linkExists = std::nullopt,
                                                                  const PropertyLists &plists     = PropertyLists()) {
        // Use this function to get info from existing datasets on file.
        h5pp::logger::log->trace("Reading properties for writing into attribute: [{}] on link [{}]", attrName, linkName);

        linkExists = h5pp::hdf5::checkIfLinkExists(file, linkName, linkExists, plists);
        attrExists = h5pp::hdf5::checkIfAttributeExists(file, linkName, attrName, linkExists, std::nullopt, plists);

        if(linkExists.value() and attrExists.value()) {
            // We enter overwrite mode
            h5pp::logger::log->trace("Attribute [{}] exists in link [{}]", attrName, linkName);
            auto attrProps = getAttributeProperties_read(file, attrName, linkName, attrExists, linkExists, plists);
            // Sanity check
            if(attrProps.ndims != h5pp::utils::getRank<DataType>())
                throw std::runtime_error("Number of dimensions in existing dataset (" + std::to_string(attrProps.ndims.value()) + ") differ from dimensions in given data (" +
                                         std::to_string(h5pp::utils::getRank<DataType>()) + ")");

            if(not h5pp::hdf5::checkEqualTypesRecursive(attrProps.dataType, h5pp::utils::getH5Type<DataType>()))
                throw std::runtime_error("Given datatype does not match the type of an existing dataset: " + attrProps.linkName.value());

            h5pp::AttributeProperties dataProps;
            // Start by copying properties that are immutable on overwrites
            dataProps.attributeId       = attrProps.attributeId;
            dataProps.linkObject        = attrProps.linkObject;
            dataProps.attrName          = attrProps.attrName;
            dataProps.linkExists        = attrProps.linkExists;
            dataProps.attrExists        = attrProps.attrExists;
            dataProps.dataType          = h5pp::utils::getH5Type<DataType>();
            dataProps.ndims             = h5pp::utils::getRank<DataType>();
            dataProps.plist_attr_access = attrProps.plist_attr_create;
            // The rest we can inferr directly from the data
            dataProps.dataType = h5pp::utils::getH5Type<DataType>();
            dataProps.size     = h5pp::utils::getSize(data);
            dataProps.size     = h5pp::utils::setStringSize(data, dataProps.dataType); // This only affects strings
            dataProps.bytes    = h5pp::utils::getBytesTotal<DataType>(data);
            dataProps.ndims    = h5pp::utils::getRank<DataType>();
            dataProps.dims     = h5pp::utils::getDimensions(data);
            dataProps.memSpace = h5pp::utils::getMemSpace(dataProps.size.value(), dataProps.ndims.value(), dataProps.dims.value());
            return dataProps;

        } else {
            h5pp::logger::log->trace("Attribute [{}] does not exists in link [{}]", attrName, linkName);
            return getAttributeProperties_bootstrap(file, data, attrName, linkName, attrExists, linkExists, plists);
        }
    }

}
