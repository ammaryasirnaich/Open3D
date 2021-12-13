include(ExternalProject)

if(NOT DEFINED CURL_OPENSSL_ROOT_DIR)
    message(FATAL_ERROR "CURL_OPENSSL_ROOT_DIR not set. "
                        "Please include openssl.cmake before including this file.")
endif()

ExternalProject_Add(
    ext_curl
    PREFIX curl
    URL https://github.com/curl/curl/releases/download/curl-7_79_1/curl-7.79.1.tar.gz
    URL_HASH SHA256=370b11201349816287fb0ccc995e420277fbfcaf76206e309b3f60f0eda090c2
    DOWNLOAD_DIR "${OPEN3D_THIRD_PARTY_DOWNLOAD_DIR}/curl"
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_CURL_EXE=OFF
        -DBUILD_TESTING=OFF
        -DCURL_DISABLE_LDAP=ON
        -DCURL_DISABLE_LDAPS=ON
        -DOPENSSL_ROOT_DIR=${CURL_OPENSSL_ROOT_DIR}
        ${ExternalProject_CMAKE_ARGS_hidden}
    BUILD_BYPRODUCTS
        <INSTALL_DIR>/${Open3D_INSTALL_LIB_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}curl${CMAKE_STATIC_LIBRARY_SUFFIX}
)

add_dependencies(ext_curl ext_openssl)

ExternalProject_Get_Property(ext_curl INSTALL_DIR)
set(CURL_INCLUDE_DIRS ${INSTALL_DIR}/include/) # "/" is critical.
set(CURL_LIB_DIR ${INSTALL_DIR}/${Open3D_INSTALL_LIB_DIR})
if(MSVC)
    set(CURL_LIBRARIES curl$<$<CONFIG:Debug>:d>)
else()
    set(CURL_LIBRARIES curl)
endif()