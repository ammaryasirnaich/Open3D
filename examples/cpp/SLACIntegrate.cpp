// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
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
#include "open3d/Open3D.h"
#include "open3d/t/pipelines/slac/ControlGrid.h"

using namespace open3d;
using namespace open3d::core;

void PrintHelp() {
    using namespace open3d;

    PrintOpen3DVersion();
    // clang-format off
    utility::LogInfo("Usage:");
    utility::LogInfo(">    SLAC [dataset_folder] [slac_folder] [options]");
    utility::LogInfo("     --color_subfolder [default: color]");
    utility::LogInfo("     --depth_subfolder [default: depth]");
    utility::LogInfo("     --device [default: CPU:0]");
    utility::LogInfo("     --voxel_size [=0.0058 (m)]");
    utility::LogInfo("     --intrinsic_path [camera_intrinsic]");
    utility::LogInfo("     --depth_scale [=1000.0]");
    utility::LogInfo("     --max_depth [=3.0]");
    utility::LogInfo("     --sdf_trunc [=0.04]");
    utility::LogInfo("     --device [CPU:0]");
    utility::LogInfo("     --mesh");
    utility::LogInfo("     --pointcloud");
    utility::LogInfo("--debug");
    // clang-format on
    utility::LogInfo("");
}

int main(int argc, char** argv) {
    if (argc == 1 || utility::ProgramOptionExists(argc, argv, "--help") ||
        argc < 2) {
        PrintHelp();
        return 1;
    }

    std::string color_subfolder = utility::GetProgramOptionAsString(
            argc, argv, "--color_subfolder", "color");
    std::string depth_subfolder = utility::GetProgramOptionAsString(
            argc, argv, "--depth_subfolder", "depth");

    // Color and depth
    std::string dataset_folder = std::string(argv[1]);
    std::string color_folder = dataset_folder + "/" + color_subfolder;
    std::string depth_folder = dataset_folder + "/" + depth_subfolder;
    std::string fragment_folder = dataset_folder + "/fragments";
    std::vector<std::string> color_filenames;
    utility::filesystem::ListFilesInDirectory(color_folder, color_filenames);
    std::sort(color_filenames.begin(), color_filenames.end());

    std::vector<std::string> depth_filenames;
    utility::filesystem::ListFilesInDirectory(depth_folder, depth_filenames);
    std::sort(depth_filenames.begin(), depth_filenames.end());

    // Optimized fragment pose graph
    std::string slac_folder = std::string(argv[2]);
    std::string posegraph_path =
            std::string(slac_folder + "/optimized_posegraph_slac.json");
    auto posegraph = *io::CreatePoseGraphFromFile(posegraph_path);

    // Intrinsics
    std::string intrinsic_path = utility::GetProgramOptionAsString(
            argc, argv, "--intrinsic_path", "");
    camera::PinholeCameraIntrinsic intrinsic = camera::PinholeCameraIntrinsic(
            camera::PinholeCameraIntrinsicParameters::PrimeSenseDefault);
    if (intrinsic_path.empty()) {
        utility::LogWarning("Using default Primesense intrinsics");
    } else if (!io::ReadIJsonConvertible(intrinsic_path, intrinsic)) {
        utility::LogError("Unable to convert json to intrinsics.");
    }

    auto focal_length = intrinsic.GetFocalLength();
    auto principal_point = intrinsic.GetPrincipalPoint();
    Tensor intrinsic_t = Tensor::Init<double>(
            {{focal_length.first, 0, principal_point.first},
             {0, focal_length.second, principal_point.second},
             {0, 0, 1}});

    std::string device_code = "CPU:0";
    if (utility::ProgramOptionExists(argc, argv, "--device")) {
        device_code = utility::GetProgramOptionAsString(argc, argv, "--device");
    }
    core::Device device(device_code);
    int block_count =
            utility::GetProgramOptionAsInt(argc, argv, "--block_count", 1000);
    float voxel_size = static_cast<float>(utility::GetProgramOptionAsDouble(
            argc, argv, "--voxel_size", 3.f / 512.f));
    float depth_scale = static_cast<float>(utility::GetProgramOptionAsDouble(
            argc, argv, "--depth_scale", 1000.f));
    float max_depth = static_cast<float>(
            utility::GetProgramOptionAsDouble(argc, argv, "--max_depth", 3.f));
    float sdf_trunc = static_cast<float>(utility::GetProgramOptionAsDouble(
            argc, argv, "--sdf_trunc", 0.04f));

    utility::LogInfo("Using device: {}", device.ToString());
    t::geometry::TSDFVoxelGrid voxel_grid({{"tsdf", core::Dtype::Float32},
                                           {"weight", core::Dtype::UInt16},
                                           {"color", core::Dtype::UInt16}},
                                          voxel_size, sdf_trunc, 16,
                                          block_count, device);

    core::Tensor ctr_grid_keys =
            core::Tensor::Load(slac_folder + "/ctr_grid_keys.npy");
    core::Tensor ctr_grid_values =
            core::Tensor::Load(slac_folder + "/ctr_grid_values.npy");

    utility::LogInfo("Control grid: {}", device.ToString());
    t::pipelines::slac::ControlGrid ctr_grid(3.0 / 8, ctr_grid_keys.To(device),
                                             ctr_grid_values.To(device),
                                             device);

    int k = 0;
    for (size_t i = 0; i < posegraph.nodes_.size(); ++i) {
        utility::LogInfo("Fragment: {}", i);
        auto fragment_pose_graph = *io::CreatePoseGraphFromFile(fmt::format(
                "{}/fragment_optimized_{:03d}.json", fragment_folder, i));
        for (auto node : fragment_pose_graph.nodes_) {
            Eigen::Matrix4d pose_local = node.pose_;
            Tensor extrinsic_local_t =
                    core::eigen_converter::EigenMatrixToTensor(
                            pose_local.inverse().eval());

            Eigen::Matrix4d pose = posegraph.nodes_[i].pose_ * node.pose_;
            Tensor extrinsic_t = core::eigen_converter::EigenMatrixToTensor(
                    pose.inverse().eval());

            std::shared_ptr<geometry::Image> depth_legacy =
                    io::CreateImageFromFile(depth_filenames[k]);
            std::shared_ptr<geometry::Image> color_legacy =
                    io::CreateImageFromFile(color_filenames[k]);

            t::geometry::Image depth =
                    t::geometry::Image::FromLegacyImage(*depth_legacy, device);
            t::geometry::Image color =
                    t::geometry::Image::FromLegacyImage(*color_legacy, device);

            utility::LogInfo("Reprojecting");
            t::geometry::Image depth_reproj, color_reproj;
            std::tie(depth_reproj, color_reproj) =
                    ctr_grid.Deform(depth, color, intrinsic_t,
                                    extrinsic_local_t, depth_scale, max_depth);
            utility::LogInfo("depth_reproj = {}", depth_reproj.ToString());
            if (false) {
                t::geometry::PointCloud pcd =
                        t::geometry::PointCloud::CreateFromRGBDImage(
                                t::geometry::RGBDImage(color, depth),
                                intrinsic_t, extrinsic_t, depth_scale,
                                max_depth);
                auto pcd_old_legacy =
                        std::make_shared<open3d::geometry::PointCloud>(
                                pcd.ToLegacyPointCloud());

                t::geometry::PointCloud pcd_reproj =
                        t::geometry::PointCloud::CreateFromRGBDImage(
                                t::geometry::RGBDImage(color_reproj,
                                                       depth_reproj),
                                intrinsic_t, extrinsic_t, depth_scale,
                                max_depth);
                auto pcd_legacy =
                        std::make_shared<open3d::geometry::PointCloud>(
                                pcd_reproj.ToLegacyPointCloud());
                visualization::DrawGeometries({pcd_old_legacy, pcd_legacy});
            }

            utility::Timer timer;
            timer.Start();
            voxel_grid.Integrate(depth_reproj, color_reproj, intrinsic_t,
                                 extrinsic_t, depth_scale, max_depth);
            timer.Stop();

            ++k;
            utility::LogInfo("{}: Integration takes {}", k,
                             timer.GetDuration());

            if (k % 10 == 0) {
#ifdef BUILD_CUDA_MODULE
                CUDACachedMemoryManager::ReleaseCache();
#endif
            }
        }
    }

    if (utility::ProgramOptionExists(argc, argv, "--mesh")) {
        auto mesh = voxel_grid.ExtractSurfaceMesh();
        auto mesh_legacy = std::make_shared<geometry::TriangleMesh>(
                mesh.ToLegacyTriangleMesh());
        open3d::io::WriteTriangleMesh("mesh_" + device.ToString() + ".ply",
                                      *mesh_legacy);
    }

    if (utility::ProgramOptionExists(argc, argv, "--pointcloud")) {
        auto pcd = voxel_grid.ExtractSurfacePoints();
        auto pcd_legacy = std::make_shared<open3d::geometry::PointCloud>(
                pcd.ToLegacyPointCloud());
        open3d::io::WritePointCloud("pcd_" + device.ToString() + ".ply",
                                    *pcd_legacy);
    }
}