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

#include "open3d/tgeometry/VoxelGrid.h"

#include <Eigen/Dense>

#include "open3d/core/EigenAdaptor.h"
#include "open3d/core/SparseTensorList.h"
#include "open3d/core/TensorList.h"
#include "open3d/core/kernel/SpecialOp.h"
#include "open3d/utility/Console.h"
namespace open3d {
namespace tgeometry {
using namespace core;

VoxelGrid::VoxelGrid(float voxel_size,
                     float sdf_trunc,
                     int64_t resolution,
                     int64_t capacity,
                     const Device &device)
    : Geometry3D(Geometry::GeometryType::VoxelGrid),
      voxel_size_(voxel_size),
      sdf_trunc_(sdf_trunc),
      resolution_(resolution),
      capacity_(capacity),
      device_(device) {
    hashmap_ = CreateDefaultHashmap(
            capacity, 3 * sizeof(int64_t),
            2 * resolution * resolution * resolution * sizeof(float), device);
}

void VoxelGrid::Integrate(const tgeometry::Image &depth,
                          const Tensor &intrinsic,
                          const Tensor &extrinsic) {
    /// Unproject
    Tensor vertex_map = depth.Unproject(intrinsic);
    Tensor pcd_map = vertex_map.View({3, 480 * 640});

    /// Inverse is currently not available...
    Eigen::Matrix4f pose_ = ToEigen<float>(extrinsic).inverse();
    Tensor pose = FromEigen(pose_).Copy(device_);
    tgeometry::PointCloud pcd(pcd_map.T());
    pcd.Transform(pose);

    tgeometry::PointCloud pcd_down =
            pcd.VoxelDownSample(voxel_size_ * resolution_);

    Tensor coords = pcd_down.point_dict_["points"].AsTensor().To(Dtype::Int64);
    SizeVector coords_shape = coords.GetShape();
    int64_t N = coords_shape[0];

    auto iterators = MemoryManager::Malloc(sizeof(iterator_t) * N, device_);
    auto masks = MemoryManager::Malloc(sizeof(bool) * N, device_);

    hashmap_->Activate(static_cast<void *>(coords.GetBlob()->GetDataPtr()),
                       static_cast<iterator_t *>(iterators),
                       static_cast<bool *>(masks), N);

    hashmap_->Find(static_cast<void *>(coords.GetBlob()->GetDataPtr()),
                   static_cast<iterator_t *>(iterators),
                   static_cast<bool *>(masks), N);

    utility::LogInfo("Active entries = {}", N);

    SizeVector shape = SizeVector{resolution_, resolution_, resolution_};
    SparseTensorList sparse_tl(static_cast<void **>(iterators), N, true,
                               {shape, shape}, {Dtype::Float32, Dtype::Float32},
                               device_);
    Tensor voxel_size(std::vector<float>{voxel_size_}, {1}, Dtype::Float32);
    Tensor sdf_trunc(std::vector<float>{sdf_trunc_}, {1}, Dtype::Float32);

    kernel::SpecialOpEW(
            {depth.data_, intrinsic, extrinsic, voxel_size, sdf_trunc}, {},
            sparse_tl, kernel::SpecialOpCode::Integrate);
    utility::LogInfo("[VoxelGrid] Kernel launch finished");

    // Then manipulate iterators to integrate!
    MemoryManager::Free(iterators, coords.GetDevice());
    utility::LogInfo("[VoxelGrid] iterators freed");
    MemoryManager::Free(masks, coords.GetDevice());
    utility::LogInfo("[VoxelGrid] masks freed");

    utility::LogInfo("Hashmap size = {}", hashmap_->size());
}

}  // namespace tgeometry
}  // namespace open3d
