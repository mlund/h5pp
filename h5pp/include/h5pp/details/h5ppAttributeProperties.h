
#pragma once
#include "h5ppHid.h"
#include <hdf5.h>
#include <hdf5_hl.h>
#include <optional>
#include <string>
#include <vector>
namespace h5pp {
    class AttributeProperties {
        public:
        hid::h5a                            attributeId;
        hid::h5o                            linkObject;
        hid::h5t                            dataType;
        hid::h5s                            memSpace;
        hid::h5p                            plist_attr_create = H5P_DEFAULT;
        hid::h5p                            plist_attr_access = H5P_DEFAULT;
        std::optional<std::string>          attrName;
        std::optional<std::string>          linkName;
        std::optional<bool>                 attrExists;
        std::optional<bool>                 linkExists;
        std::optional<hsize_t>              size;
        std::optional<size_t>               bytes;
        std::optional<int>                  ndims;
        std::optional<std::vector<hsize_t>> dims;
    };
} // namespace h5pp
