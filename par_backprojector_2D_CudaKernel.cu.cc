#if GOOGLE_CUDA
#define EIGEN_USE_GPU
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "helper_headers/helper_grid.h"
#include "helper_headers/helper_math.h"

texture<float, cudaTextureType2D, cudaReadModeElementType> sinogram_as_texture;
#define CUDART_INF_F __int_as_float(0x7f800000)

__global__ void backproject_2Dpar_beam_kernel(float *pVolume, const float2 *d_rays, const int number_of_projections,
                                              const int2 volume_size, const float2 volume_spacing, const float2 volume_origin,
                                              const int detector_size, const float detector_spacing, const float detector_origin)
{
    const float pi = 3.14159265359f;
    unsigned int volume_x = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned int volume_y = blockIdx.y * blockDim.y + threadIdx.y;
    if (volume_x >= volume_size.x || volume_y >= volume_size.y)
    {
        return;
    }
    //Preparations:
    const float2 pixel_coordinate = index_to_physical(make_float2(volume_x, volume_y), volume_origin, volume_spacing);
    float pixel_value = 0.0f;

    for (int n = 0; n < number_of_projections; n++)
    {
        float2 detector_normal = d_rays[n];
        float2 detector_vec = make_float2(-detector_normal.y, detector_normal.x);

        float s = dot(pixel_coordinate, detector_vec);
        unsigned int s_idx = physical_to_index(s, detector_origin, detector_spacing);

        pixel_value += tex2D(sinogram_as_texture, s_idx + 0.5f, n + 0.5f);
    }

    const unsigned volume_linearized_idx = volume_y * volume_size.x + volume_x;
    pVolume[volume_linearized_idx] = 2 * pi * pixel_value / number_of_projections;

    return;
}
/*************** WARNING ******************./
    * 
    *   Tensorflow is allocating the whole GPU memory for itself and just leave a small slack memory
    *   using cudaMalloc and cudaMalloc3D will allocate memory in this small slack memory !
    *   Therefore, currently only small volumes can be used (they have to fit into the slack memory which TF does not allocae !)
    * 
    *   This is the kernel based on texture interpolation, thus, the allocations are not within the Tensorflow managed memory.
    *   If memory errors occure:
    *    1. start Tensorflow with less gpu memory and allow growth
    *    2. TODO: no software interpolation based 2D verions are available yet
    * 
    *   TODO: use context->allocate_tmp and context->allocate_persistent instead of cudaMalloc for the ray_vectors array
    *       : https://stackoverflow.com/questions/48580580/tensorflow-new-op-cuda-kernel-memory-managment
    * 
    */
void Parallel_Backprojection2D_Kernel_Launcher(const float *sinogram_ptr, float *out, const float *ray_vectors, const int number_of_projections,
                                               const int volume_width, const int volume_height, const float volume_spacing_x, const float volume_spacing_y,
                                               const float volume_origin_x, const float volume_origin_y,
                                               const int detector_size, const float detector_spacing, const float detector_origin)
{
    cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<float>();
    sinogram_as_texture.addressMode[0] = cudaAddressModeBorder;
    sinogram_as_texture.addressMode[1] = cudaAddressModeBorder;
    sinogram_as_texture.filterMode = cudaFilterModeLinear;
    sinogram_as_texture.normalized = false;

    cudaArray *sinogram_array;
    cudaMallocArray(&sinogram_array, &channelDesc, detector_size, number_of_projections);
    cudaMemcpyToArray(sinogram_array, 0, 0, sinogram_ptr, detector_size * number_of_projections * sizeof(float), cudaMemcpyHostToDevice);
    cudaBindTextureToArray(sinogram_as_texture, sinogram_array, channelDesc);

    auto ray_size_b = number_of_projections * sizeof(float2);
    float2 *d_rays;
    cudaMalloc(&d_rays, ray_size_b);
    cudaMemcpy(d_rays, ray_vectors, ray_size_b, cudaMemcpyHostToDevice);

    int2 volume_size = make_int2(volume_width, volume_height);
    float2 volume_spacing = make_float2(volume_spacing_x, volume_spacing_y);
    float2 volume_origin = make_float2(volume_origin_x, volume_origin_y);

    const unsigned block_size = 16;
    const dim3 threads_per_block = dim3(block_size, block_size);
    const dim3 num_blocks = dim3(volume_width / threads_per_block.x + 1, volume_height / threads_per_block.y + 1);

    backproject_2Dpar_beam_kernel<<<num_blocks, threads_per_block>>>(out, d_rays, number_of_projections,
                                                                     volume_size, volume_spacing, volume_origin,
                                                                     detector_size, detector_spacing, detector_origin);

    cudaUnbindTexture(sinogram_as_texture);
    cudaFreeArray(sinogram_array);
    cudaFree(d_rays);
}

#endif


/*
 * Voxel-driven parllel-beam back-projector CUDA kernel
 * Implementation partially adapted from CONRAD
 * PyRo-ML is developed as an Open Source project under the GNU General Public License (GPL).
 * Copyright (C) 2019  Christopher Syben
*/