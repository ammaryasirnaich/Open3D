// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018-2021 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/data/Download.h"

// clang-format off
// Must include openssl before curl to build on Windows.
#include <openssl/sha.h>

// https://stackoverflow.com/a/41873190/1255535
#ifdef WINDOWS
#pragma comment(lib, "wldap32.lib" )
#pragma comment(lib, "crypt32.lib" )
#pragma comment(lib, "Ws2_32.lib")
#define USE_SSLEAY
#define USE_OPENSSL
#endif

#define CURL_STATICLIB

#include <curl/curl.h>
#include <curl/easy.h>
// clang-format on

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "open3d/data/Dataset.h"
#include "open3d/utility/FileSystem.h"
#include "open3d/utility/Logging.h"

namespace open3d {
namespace data {

std::string GetSHA256(const std::string& file_path) {
    // https://gist.github.com/arrieta/7d2e196c40514d8b5e9031f2535064fc
    // author    J. Arrieta <Juan.Arrieta@nablazerolabs.com>
    // copyright (c) 2017 Nabla Zero Labs
    // license   MIT License
    std::ifstream fp(file_path.c_str(), std::ios::in | std::ios::binary);

    if (!fp.good()) {
        std::ostringstream os;
        utility::LogError("Cannot open {}", file_path);
    }

    constexpr const std::size_t buffer_size{1 << 12};
    char buffer[buffer_size];

    unsigned char hash[SHA256_DIGEST_LENGTH] = {0};

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    while (fp.good()) {
        fp.read(buffer, buffer_size);
        SHA256_Update(&ctx, buffer, fp.gcount());
    }

    SHA256_Final(hash, &ctx);
    fp.close();

    std::ostringstream os;
    os << std::hex << std::setfill('0');

    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        os << std::setw(2) << static_cast<unsigned int>(hash[i]);
    }

    return os.str();
}

static size_t WriteDataCb(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written;
    written = fwrite(ptr, size, nmemb, stream);
    return written;
}

static std::string ResolveFilePath(const std::string& url,
                                   const std::string& data_root,
                                   const std::string& prefix) {
    std::string file_name =
            utility::filesystem::GetFileNameWithoutDirectory(url);

    std::string file_dir = data_root.empty() ? LocateDataRoot() : data_root;
    if (!prefix.empty()) {
        file_dir += "/" + prefix;
    }

    return file_dir + "/" + file_name;
}

bool DownloadFromURL(const std::string& url,
                     const std::string& sha256,
                     const std::string& data_root,
                     const std::string& prefix,
                     const bool always_download,
                     const bool print_progress) {
    const std::string file_path = ResolveFilePath(url, data_root, prefix);
    const std::string file_dir =
            utility::filesystem::GetFileParentDirectory(file_path);
    if (!utility::filesystem::DirectoryExists(file_dir)) {
        utility::filesystem::MakeDirectoryHierarchy(file_dir);
    }

    utility::LogInfo("file_path: {}", file_path);

    // Check and skip download if required.
    if (!always_download && utility::filesystem::FileExists(file_path)) {
        if (!sha256.empty()) {
            const std::string actual_hash = GetSHA256(file_path.c_str());
            if (sha256 == actual_hash) {
                utility::LogDebug(
                        "Downloading Skipped. File already present with "
                        "expected SHA256 hash.");
                return true;
            }
        } else {
            utility::LogError(
                    "Setting always_download to false, requires SHA256 "
                    "value, to verify the existing file.");
        }
    }

    // Download mechanism.
    CURL* curl;
    FILE* fp;
    CURLcode res;

    curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
    utility::LogDebug("libcurl ssl_version: {}", ver->ssl_version);

    // Initialize Curl.
    curl = curl_easy_init();

    if (curl) {
        fp = fopen(file_path.c_str(), "wb");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

        // Follow redirection in link. `-L option in curl`.
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        // TODO: Check if it is safe to skip this verification.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, false);

        // Write function callback.
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDataCb);
        // Pass file-handler to which the data will be written.
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

        // Progress bar options.
        if (print_progress) {
            // TODO: Add Open3D progress-bar option.
            // curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
            //                  download_progress_callback);
            // curl_easy_setopt(curl, CURLOPT_XFERINFODATA,
            //                  static_cast<void*>(&progress_bar));
            // curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
        }

        // Perform a file transfer synchronously.
        res = curl_easy_perform(curl);
        // Cleanup.
        curl_easy_cleanup(curl);
        // Close file.
        fclose(fp);

        // File downloaded without error.
        if (res == CURLE_OK) {
            // Verify SHA256 value.
            if (!sha256.empty()) {
                const std::string actual_hash = GetSHA256(file_path.c_str());
                if (sha256 == actual_hash) {
                    utility::LogDebug(
                            "Downloaded file {} with expected SHA256 hash.",
                            file_path);
                    return true;
                } else {
                    utility::LogWarning(
                            "SHA256 hash mismatch for file {}.\n Expected: "
                            "{}.\n Actual: {}.",
                            file_path, sha256, actual_hash);
                    return false;
                }
            }

            utility::LogDebug("Downloaded file {}.", file_path);
            return true;
        } else {
            utility::LogWarning("Download failed with error code: {}.",
                                curl_easy_strerror(res));

            return false;
        }

    } else {
        utility::LogWarning("Failed to initialize CURL.");
        return false;
    }
}

}  // namespace data
}  // namespace open3d
