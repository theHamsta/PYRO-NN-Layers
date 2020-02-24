/*
 * Copyright [2019] [Christopher Syben]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Voxel-driven parllel-beam projector CUDA kernel
 * Implementation partially adapted from CONRAD
 * PYRO-NN is developed as an Open Source project under the Apache License,
 * Version 2.0.
 */
#if GOOGLE_CUDA
#define EIGEN_USE_GPU
#include "helper_headers/helper_grid.h"
#include "helper_headers/helper_math.h"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"

texture<float, cudaTextureType2D, cudaReadModeElementType> volume_as_texture;
#define CUDART_INF_F __int_as_float(0x7f800000)

inline __device__ float
kernel_project2D(const float2 source_point, const float2 ray_vector,
                 const float step_size, const int2 volume_size,
                 const float2 volume_origin, const float2 volume_spacing) {

  unsigned int detector_idx = blockIdx.x * blockDim.x + threadIdx.x;
  int projection_idx = blockIdx.y;

  float pixel = 0.0f;
  // Step 1: compute alpha value at entry and exit point of the volume
  float min_alpha, max_alpha;
  min_alpha = 0;
  max_alpha = CUDART_INF_F;
  if (0.0f != ray_vector.x) {
    float volume_min_edge_point =
        index_to_physical(0, volume_origin.x, volume_spacing.x) - 0.5f;
    float volume_max_edge_point =
        index_to_physical(volume_size.x, volume_origin.x, volume_spacing.x) -
        0.5f;

    float reci = 1.0f / ray_vector.x;
    float alpha0 = (volume_min_edge_point - source_point.x) * reci;
    float alpha1 = (volume_max_edge_point - source_point.x) * reci;
    min_alpha = fmin(alpha0, alpha1);
    max_alpha = fmax(alpha0, alpha1);
  }

  if (0.0f != ray_vector.y) {
    float volume_min_edge_point =
        index_to_physical(0, volume_origin.y, volume_spacing.y) - 0.5f;
    float volume_max_edge_point =
        index_to_physical(volume_size.y, volume_origin.y, volume_spacing.y) -
        0.5f;

    float reci = 1.0f / ray_vector.y;
    float alpha0 = (volume_min_edge_point - source_point.y) * reci;
    float alpha1 = (volume_max_edge_point - source_point.y) * reci;
    min_alpha = fmax(min_alpha, fmin(alpha0, alpha1));
    max_alpha = fmin(max_alpha, fmax(alpha0, alpha1));
  }

  float px, py;
  // pixel = source_point.x + min_alpha * ray_vector.x;
  // Entrance boundary
  // In CUDA, voxel centers are located at (xx.5, xx.5, xx.5),
  //  whereas, SwVolume has voxel centers at integers.
  // For the initial interpolated value, only a half stepsize is
  //  considered in the computation.
  if (min_alpha < max_alpha) {
    px = source_point.x + min_alpha * ray_vector.x;
    py = source_point.y + min_alpha * ray_vector.y;

    pixel +=
        0.5f *
        tex2D(volume_as_texture,
              physical_to_index(px, volume_origin.x, volume_spacing.x) + 0.5f,
              physical_to_index(py, volume_origin.y, volume_spacing.y) + 0.5f);
    min_alpha += step_size;
  }
  // Mid segments
  while (min_alpha < max_alpha) {
    px = source_point.x + min_alpha * ray_vector.x;
    py = source_point.y + min_alpha * ray_vector.y;
    float2 interp_point =
        physical_to_index(make_float2(px, py), volume_origin, volume_spacing);
    pixel +=
        tex2D(volume_as_texture,
              physical_to_index(px, volume_origin.x, volume_spacing.x) + 0.5f,
              physical_to_index(py, volume_origin.y, volume_spacing.y) + 0.5f);
    min_alpha += step_size;
  }
  // Scaling by stepsize;
  pixel *= step_size;

  // Last segment of the line
  if (pixel > 0.0f) {
    pixel -=
        0.5f * step_size *
        tex2D(volume_as_texture,
              physical_to_index(px, volume_origin.x, volume_spacing.x) + 0.5f,
              physical_to_index(py, volume_origin.y, volume_spacing.y) + 0.5f);
    min_alpha -= step_size;
    float last_step_size = max_alpha - min_alpha;
    pixel +=
        0.5f * last_step_size *
        tex2D(volume_as_texture,
              physical_to_index(px, volume_origin.x, volume_spacing.x) + 0.5f,
              physical_to_index(py, volume_origin.y, volume_spacing.y) + 0.5f);

    px = source_point.x + max_alpha * ray_vector.x;
    py = source_point.y + max_alpha * ray_vector.y;
    // The last segment of the line integral takes care of the
    // varying length.
    pixel +=
        0.5f * last_step_size *
        tex2D(volume_as_texture,
              physical_to_index(px, volume_origin.x, volume_spacing.x) + 0.5f,
              physical_to_index(py, volume_origin.y, volume_spacing.y) + 0.5f);
  }
  return pixel;
}

__global__ void project_2Dpar_beam_kernel(
    float *pSinogram, const float2 *d_rays, const int number_of_projections,
    const float sampling_step_size, const int2 volume_size,
    const float2 volume_spacing, const float2 volume_origin,
    const int detector_size, const float detector_spacing,
    const float detector_origin) {
  unsigned int detector_idx = blockIdx.x * blockDim.x + threadIdx.x;

  if (detector_idx >= detector_size) {
    return;
  }
  // Preparations:

  // Assume a source isocenter distance to compute the start of the ray,
  // although sid is not neseccary for a par beam geometry
  float sid = sqrt((float)(volume_size.x * volume_spacing.x * volume_size.x *
                           volume_spacing.x) +
                   (volume_size.y * volume_spacing.y * volume_size.y *
                    volume_spacing.y)) *
              1.2f;

  int projection_idx = blockIdx.y;
  float2 ray_vector = d_rays[projection_idx];

  // create detector coordinate system (u,v) w.r.t the ray
  float2 u_vec = make_float2(-ray_vector.y, ray_vector.x);
  // calculate physical coordinate of detector pixel
  float u = index_to_physical(detector_idx, detector_origin, detector_spacing);

  // Calculate "source"-Point (start point for the parallel ray), so we can use
  // the projection kernel Assume a source isocenter distance to compute the
  // start of the ray, although sid is not neseccary for a par beam geometry
  float2 virtual_source_point = ray_vector * (-sid) + u_vec * u;

  float pixel = kernel_project2D(
      virtual_source_point, ray_vector,
      sampling_step_size, // * fmin(volume_spacing.x, volume_spacing.y),
      volume_size, volume_origin, volume_spacing);

  pixel *= sqrt(
      (ray_vector.x * volume_spacing.x) * (ray_vector.x * volume_spacing.x) +
      (ray_vector.y * volume_spacing.y) * (ray_vector.y * volume_spacing.y));

  unsigned sinogram_idx = projection_idx * detector_size + detector_idx;
  pSinogram[sinogram_idx] = pixel;

  return;
}

void Parallel_Projection2D_Kernel_Launcher(
    const float *volume_ptr, float *out, const float *ray_vectors,
    const int number_of_projections, const int volume_width,
    const int volume_height, const float volume_spacing_x,
    const float volume_spacing_y, const float volume_origin_x,
    const float volume_origin_y, const int detector_size,
    const float detector_spacing, const float detector_origin) {
  cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<float>();
  volume_as_texture.addressMode[0] = cudaAddressModeBorder;
  volume_as_texture.addressMode[1] = cudaAddressModeBorder;
  volume_as_texture.filterMode = cudaFilterModeLinear;
  volume_as_texture.normalized = false;

  cudaArray *volume_array;
  cudaMallocArray(&volume_array, &channelDesc, volume_width, volume_height);
  cudaMemcpyToArray(volume_array, 0, 0, volume_ptr,
                    volume_width * volume_height * sizeof(float),
                    cudaMemcpyHostToDevice);
  cudaBindTextureToArray(volume_as_texture, volume_array, channelDesc);

  auto ray_size_b = number_of_projections * sizeof(float2);
  float2 *d_rays;
  cudaMalloc(&d_rays, ray_size_b);
  cudaMemcpy(d_rays, ray_vectors, ray_size_b, cudaMemcpyHostToDevice);

  float sampling_step_size = 0.2;

  int2 volume_size = make_int2(volume_width, volume_height);
  float2 volume_spacing = make_float2(volume_spacing_x, volume_spacing_y);
  float2 volume_origin = make_float2(volume_origin_x, volume_origin_y);

  const unsigned blocksize = 256;
  const dim3 gridsize =
      dim3((detector_size / blocksize) + 1, number_of_projections);
  project_2Dpar_beam_kernel<<<gridsize, blocksize>>>(
      out, d_rays, number_of_projections, sampling_step_size, volume_size,
      volume_spacing, volume_origin, detector_size, detector_spacing,
      detector_origin);

  // cleanup
  cudaUnbindTexture(volume_as_texture);
  cudaFreeArray(volume_array);
  cudaFree(d_rays);
}

#endif
