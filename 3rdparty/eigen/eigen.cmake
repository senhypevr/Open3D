include(ExternalProject)

ExternalProject_Add(
    ext_eigen
    PREFIX eigen
    # Commit point: https://gitlab.com/libeigen/eigen/-/merge_requests/716
    URL https://drive.google.com/uc?id=14Q5RRHGoif99NeP2QQth6xGhBpfQ7VH0&export=download
    URL_HASH SHA256=0a558164b4ebb2fcd773758691ba73a610725335008a8f0e2b2cebfb41ea556c
    DOWNLOAD_DIR "${OPEN3D_THIRD_PARTY_DOWNLOAD_DIR}/eigen"
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
)

ExternalProject_Get_Property(ext_eigen SOURCE_DIR)
set(EIGEN_INCLUDE_DIRS ${SOURCE_DIR}/Eigen)
