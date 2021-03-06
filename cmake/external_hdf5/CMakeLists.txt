cmake_minimum_required(VERSION 3.10)
project(external-eigen3)
include(../GetNumThreads.cmake)
get_num_threads(NUM_THREADS)
set(ENV{CMAKE_BUILD_PARALLEL_LEVEL} ${NUM_THREADS})
include(ExternalProject)
option(HDF5_ENABLE_PARALLEL "" OFF)
option(HDF5_EXTERNALLY_CONFIGURED "" ON)


# The following check is needed because HDF5 will blindly use
# find_package(ZLIB), which finds the shared library
# (even when a static is present) and use it to link to static
# hdf5 libraries, causing a build error. Here we circumvent that
# by specifying the static zlib library explicitly, which
# works for both static and shared linking.

set(HDF5_ENABLE_Z_LIB_SUPPORT ON)
set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
find_package(ZLIB)
if(ZLIB_LIBRARY)
    get_filename_component(ZLIB_EXT ${ZLIB_LIBRARY} EXT)
    if(NOT BUILD_SHARED_LIBS AND ZLIB_EXT MATCHES "${CMAKE_SHARED_LIBRARY_SUFFIX}")
        message(STATUS "Could not find static ZLIB: disabling ZLIB support for hdf5")
        set(HDF5_ENABLE_Z_LIB_SUPPORT OFF)
    endif()
else()
    unset(ZLIB_LIBRARY)
    message(STATUS "Could not find ZLIB: disabling ZLIB support for hdf5")
    set(HDF5_ENABLE_Z_LIB_SUPPORT OFF)
endif()

# The following check is needed because HDF5 will blindly use
# find_package(SZIP), which finds the shared library
# (even when a static is present) and use it to link to static
# hdf5 libraries, causing a build error. Here we circumvent that
# by specifying the static SZIP library explicitly, which
# works for both static and shared linking.
# In addition, SZIP sometimes dependes on libaec.

#set(HDF5_ENABLE_SZIP_SUPPORT ON)
#set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
#find_library(SZIP_LIBRARY NAMES sz szip szip-static libsz libszip libszip-static) # No built in findSZIP.cmake
#if(SZIP_LIBRARY)
#    message(STATUS "Found SZIP: ${SZIP_LIBRARY}")
#    get_filename_component(SZIP_EXT ${SZIP_LIBRARY} EXT)
#    if(NOT BUILD_SHARED_LIBS AND SZIP_EXT MATCHES "${CMAKE_SHARED_LIBRARY_SUFFIX}")
#        message(STATUS "Could not find static SZIP: disabling SZIP support for hdf5")
#        set(HDF5_ENABLE_SZIP_SUPPORT OFF)
#    else()
#        find_library(AEC_LIBRARY NAMES aec)
#        if(AEC_LIBRARY)
#            message(STATUS "Found AEC: ${AEC_LIBRARY}")
#            list(APPEND CMAKE_SHARED_LINKER_FLAGS -laec)
#            list(APPEND SZIP_LIBRARY ${AEC_LIBRARY})
#        endif()
#    endif()
#
#else()
#    unset(SZIP_LIBRARY)
#    message(STATUS "Could not find SZIP: disabling SZIP support for hdf5")
#    set(HDF5_ENABLE_SZIP_SUPPORT OFF)
#endif()






ExternalProject_Add(external_hdf5
        URL         https://support.hdfgroup.org/ftp/HDF5/releases/hdf5-1.10/hdf5-1.10.6/src/hdf5-1.10.6.tar.gz
        URL_MD5     37f3089e7487daf0890baf3d3328e54a
        PREFIX      ${CMAKE_BINARY_DIR}
        INSTALL_DIR ${CMAKE_INSTALL_PREFIX}
        BUILD_ALWAYS OFF
#        CONFIGURE_COMMAND sh <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --enable-cxx --enable-hl --enable-silent-rules
#        BUILD_COMMAND ${CMAKE_MAKE_PROGRAM} -j ${NUM_THREADS}
#        INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install

        CMAKE_ARGS
        -DBUILD_SHARED_LIBS:BOOL=${BUILD_SHARED_LIBS}
        -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_GENERATOR=${CMAKE_GENERATOR}
        -DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DHDF5_ENABLE_PARALLEL:BOOL=${HDF5_ENABLE_PARALLEL}
        -DBUILD_TESTING:BOOL=OFF
        -DHDF5_ENABLE_Z_LIB_SUPPORT:BOOL=${HDF5_ENABLE_Z_LIB_SUPPORT}
        -DZLIB_LIBRARY:PATH=${ZLIB_LIBRARY}
        -DHDF5_ENABLE_SZIP_SUPPORT:BOOL=OFF # Complains about undefined reference to functions in libaec
        -DHDF5_BUILD_TOOLS:BOOL=ON
        -DHDF5_BUILD_FORTRAN:BOOL=OFF
        -DHDF5_BUILD_EXAMPLES:BOOL=OFF
        -DHDF5_BUILD_JAVA:BOOL=OFF
        -DCMAKE_BUILD_WITH_INSTALL_RPATH:BOOL=ON
        -DALLOW_UNSUPPORTED=ON
        -DCMAKE_INSTALL_MESSAGE=ALWAYS #Avoid unnecessary output to console

#        -DHDF5_ALLOW_EXTERNAL_SUPPORT:STRING=GIT
#        -DZLIB_URL:STRING=https://github.com/madler/zlib.git
#        -DZLIB_BRANCH:STRING=develop
#        -DSZIP_LIBRARY:PATH=${SZIP_LIBRARY}
#        -DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}
#        -DHDF5_EXTERNALLY_CONFIGURED=1
#        -DHDF5_USE_PREGEN:BOOL=ON
#        -DHDF5_BATCH_H5DETECT:BOOL=ON
        )
