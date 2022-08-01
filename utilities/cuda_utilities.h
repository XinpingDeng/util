// cuda_utilities.h
//
// last-edit-by: <> 
//
// Description:
//
//////////////////////////////////////////////////////////////////////

#ifndef CUDA_UTILITIES_H
#define CUDA_UTILITIES_H 1

#pragma once

#include <cuComplex.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cuda.h>

#include <cufft.h>
#include <curand.h>
#include <curand_kernel.h>
#include <helper_cuda.h>

#define NUM_BINS 256
/* Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
  Parallel reduction kernels

  This file is https://github.com/NVIDIA/cuda-samples/blob/master/Samples/2_Concepts_and_Techniques/reduction/reduction_kernel.cu, as of 26.07.2022
*/


#define _CG_ABI_EXPERIMENTAL
#include <cooperative_groups.h>
#include <cooperative_groups/reduce.h>
#include <stdio.h>

namespace cg = cooperative_groups;

static inline bool isPow2(unsigned int x) { return ((x & (x - 1)) == 0); }

// Utility class used to avoid linker errors with extern
// unsized shared memory arrays with templated type
template <class T>
struct SharedMemory {
  __device__ inline operator T *() {
    extern __shared__ int __smem[];
    return (T *)__smem;
  }

  __device__ inline operator const T *() const {
    extern __shared__ int __smem[];
    return (T *)__smem;
  }
};

// specialize for double to avoid unaligned memory
// access compile errors
template <>
struct SharedMemory<double> {
  __device__ inline operator double *() {
    extern __shared__ double __smem_d[];
    return (double *)__smem_d;
  }

  __device__ inline operator const double *() const {
    extern __shared__ double __smem_d[];
    return (double *)__smem_d;
  }
};

template <class T>
__device__ __forceinline__ T warpReduceSum(unsigned int mask, T mySum) {
  for (int offset = warpSize / 2; offset > 0; offset /= 2) {
    mySum += __shfl_down_sync(mask, mySum, offset);
  }
  return mySum;
}

#if __CUDA_ARCH__ >= 800
// Specialize warpReduceFunc for int inputs to use __reduce_add_sync intrinsic
// when on SM 8.0 or higher
template <>
__device__ __forceinline__ int warpReduceSum<int>(unsigned int mask,
                                                  int mySum) {
  mySum = __reduce_add_sync(mask, mySum);
  return mySum;
}
#endif

/*
  Parallel sum reduction using shared memory
  - takes log(n) steps for n input elements
  - uses n threads
  - only works for power-of-2 arrays
*/

/* This reduction interleaves which threads are active by using the modulo
   operator.  This operator is very expensive on GPUs, and the interleaved
   inactivity means that no whole warps are active, which is also very
   inefficient */
template <class T>
__global__ void reduce0(T *g_idata, T *g_odata, unsigned int n) {
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  T *sdata = SharedMemory<T>();

  // load shared mem
  unsigned int tid = threadIdx.x;
  unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;

  sdata[tid] = (i < n) ? g_idata[i] : 0;

  cg::sync(cta);

  // do reduction in shared mem
  for (unsigned int s = 1; s < blockDim.x; s *= 2) {
    // modulo arithmetic is slow!
    if ((tid % (2 * s)) == 0) {
      sdata[tid] += sdata[tid + s];
    }

    cg::sync(cta);
  }

  // write result for this block to global mem
  if (tid == 0) g_odata[blockIdx.x] = sdata[0];
}

/* This version uses contiguous threads, but its interleaved
   addressing results in many shared memory bank conflicts.
*/
template <class T>
__global__ void reduce1(T *g_idata, T *g_odata, unsigned int n) {
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  T *sdata = SharedMemory<T>();

  // load shared mem
  unsigned int tid = threadIdx.x;
  unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;

  sdata[tid] = (i < n) ? g_idata[i] : 0;

  cg::sync(cta);

  // do reduction in shared mem
  for (unsigned int s = 1; s < blockDim.x; s *= 2) {
    int index = 2 * s * tid;

    if (index < blockDim.x) {
      sdata[index] += sdata[index + s];
    }

    cg::sync(cta);
  }

  // write result for this block to global mem
  if (tid == 0) g_odata[blockIdx.x] = sdata[0];
}

/*
  This version uses sequential addressing -- no divergence or bank conflicts.
*/
template <class T>
__global__ void reduce2(T *g_idata, T *g_odata, unsigned int n) {
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  T *sdata = SharedMemory<T>();

  // load shared mem
  unsigned int tid = threadIdx.x;
  unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;

  sdata[tid] = (i < n) ? g_idata[i] : 0;

  cg::sync(cta);

  // do reduction in shared mem
  for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
    if (tid < s) {
      sdata[tid] += sdata[tid + s];
    }

    cg::sync(cta);
  }

  // write result for this block to global mem
  if (tid == 0) g_odata[blockIdx.x] = sdata[0];
}

/*
  This version uses n/2 threads --
  it performs the first level of reduction when reading from global memory.
*/
template <class T>
__global__ void reduce3(T *g_idata, T *g_odata, unsigned int n) {
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  T *sdata = SharedMemory<T>();

  // perform first level of reduction,
  // reading from global memory, writing to shared memory
  unsigned int tid = threadIdx.x;
  unsigned int i = blockIdx.x * (blockDim.x * 2) + threadIdx.x;

  T mySum = (i < n) ? g_idata[i] : 0;

  if (i + blockDim.x < n) mySum += g_idata[i + blockDim.x];

  sdata[tid] = mySum;
  cg::sync(cta);

  // do reduction in shared mem
  for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
    if (tid < s) {
      sdata[tid] = mySum = mySum + sdata[tid + s];
    }

    cg::sync(cta);
  }

  // write result for this block to global mem
  if (tid == 0) g_odata[blockIdx.x] = mySum;
}

/*
  This version uses the warp shuffle operation if available to reduce
  warp synchronization. When shuffle is not available the final warp's
  worth of work is unrolled to reduce looping overhead.

  See
  http://devblogs.nvidia.com/parallelforall/faster-parallel-reductions-kepler/
  for additional information about using shuffle to perform a reduction
  within a warp.

  Note, this kernel needs a minimum of 64*sizeof(T) bytes of shared memory.
  In other words if blockSize <= 32, allocate 64*sizeof(T) bytes.
  If blockSize > 32, allocate blockSize*sizeof(T) bytes.
*/
template <class T, unsigned int blockSize>
__global__ void reduce4(T *g_idata, T *g_odata, unsigned int n) {
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  T *sdata = SharedMemory<T>();

  // perform first level of reduction,
  // reading from global memory, writing to shared memory
  unsigned int tid = threadIdx.x;
  unsigned int i = blockIdx.x * (blockDim.x * 2) + threadIdx.x;

  T mySum = (i < n) ? g_idata[i] : 0;

  if (i + blockSize < n) mySum += g_idata[i + blockSize];

  sdata[tid] = mySum;
  cg::sync(cta);

  // do reduction in shared mem
  for (unsigned int s = blockDim.x / 2; s > 32; s >>= 1) {
    if (tid < s) {
      sdata[tid] = mySum = mySum + sdata[tid + s];
    }

    cg::sync(cta);
  }

  cg::thread_block_tile<32> tile32 = cg::tiled_partition<32>(cta);

  if (cta.thread_rank() < 32) {
    // Fetch final intermediate sum from 2nd warp
    if (blockSize >= 64) mySum += sdata[tid + 32];
    // Reduce final warp using shuffle
    for (int offset = tile32.size() / 2; offset > 0; offset /= 2) {
      mySum += tile32.shfl_down(mySum, offset);
    }
  }

  // write result for this block to global mem
  if (cta.thread_rank() == 0) g_odata[blockIdx.x] = mySum;
}

/*
  This version is completely unrolled, unless warp shuffle is available, then
  shuffle is used within a loop.  It uses a template parameter to achieve
  optimal code for any (power of 2) number of threads.  This requires a switch
  statement in the host code to handle all the different thread block sizes at
  compile time. When shuffle is available, it is used to reduce warp
  synchronization.

  Note, this kernel needs a minimum of 64*sizeof(T) bytes of shared memory.
  In other words if blockSize <= 32, allocate 64*sizeof(T) bytes.
  If blockSize > 32, allocate blockSize*sizeof(T) bytes.
*/
template <class T, unsigned int blockSize>
__global__ void reduce5(T *g_idata, T *g_odata, unsigned int n) {
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  T *sdata = SharedMemory<T>();

  // perform first level of reduction,
  // reading from global memory, writing to shared memory
  unsigned int tid = threadIdx.x;
  unsigned int i = blockIdx.x * (blockSize * 2) + threadIdx.x;

  T mySum = (i < n) ? g_idata[i] : 0;

  if (i + blockSize < n) mySum += g_idata[i + blockSize];

  sdata[tid] = mySum;
  cg::sync(cta);

  // do reduction in shared mem
  if ((blockSize >= 512) && (tid < 256)) {
    sdata[tid] = mySum = mySum + sdata[tid + 256];
  }

  cg::sync(cta);

  if ((blockSize >= 256) && (tid < 128)) {
    sdata[tid] = mySum = mySum + sdata[tid + 128];
  }

  cg::sync(cta);

  if ((blockSize >= 128) && (tid < 64)) {
    sdata[tid] = mySum = mySum + sdata[tid + 64];
  }

  cg::sync(cta);

  cg::thread_block_tile<32> tile32 = cg::tiled_partition<32>(cta);

  if (cta.thread_rank() < 32) {
    // Fetch final intermediate sum from 2nd warp
    if (blockSize >= 64) mySum += sdata[tid + 32];
    // Reduce final warp using shuffle
    for (int offset = tile32.size() / 2; offset > 0; offset /= 2) {
      mySum += tile32.shfl_down(mySum, offset);
    }
  }

  // write result for this block to global mem
  if (cta.thread_rank() == 0) g_odata[blockIdx.x] = mySum;
}

/*
  This version adds multiple elements per thread sequentially.  This reduces
  the overall cost of the algorithm while keeping the work complexity O(n) and
  the step complexity O(log n). (Brent's Theorem optimization)

  Note, this kernel needs a minimum of 64*sizeof(T) bytes of shared memory.
  In other words if blockSize <= 32, allocate 64*sizeof(T) bytes.
  If blockSize > 32, allocate blockSize*sizeof(T) bytes.
*/
template <class T, unsigned int blockSize, bool nIsPow2>
__global__ void reduce6(T *g_idata, T *g_odata, unsigned int n) {
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  T *sdata = SharedMemory<T>();

  // perform first level of reduction,
  // reading from global memory, writing to shared memory
  unsigned int tid = threadIdx.x;
  unsigned int gridSize = blockSize * gridDim.x;

  T mySum = 0;

  // we reduce multiple elements per thread.  The number is determined by the
  // number of active thread blocks (via gridDim).  More blocks will result
  // in a larger gridSize and therefore fewer elements per thread
  if (nIsPow2) {
    unsigned int i = blockIdx.x * blockSize * 2 + threadIdx.x;
    gridSize = gridSize << 1;

    while (i < n) {
      mySum += g_idata[i];
      // ensure we don't read out of bounds -- this is optimized away for
      // powerOf2 sized arrays
      if ((i + blockSize) < n) {
        mySum += g_idata[i + blockSize];
      }
      i += gridSize;
    }
  } else {
    unsigned int i = blockIdx.x * blockSize + threadIdx.x;
    while (i < n) {
      mySum += g_idata[i];
      i += gridSize;
    }
  }

  // each thread puts its local sum into shared memory
  sdata[tid] = mySum;
  cg::sync(cta);

  // do reduction in shared mem
  if ((blockSize >= 512) && (tid < 256)) {
    sdata[tid] = mySum = mySum + sdata[tid + 256];
  }

  cg::sync(cta);

  if ((blockSize >= 256) && (tid < 128)) {
    sdata[tid] = mySum = mySum + sdata[tid + 128];
  }

  cg::sync(cta);

  if ((blockSize >= 128) && (tid < 64)) {
    sdata[tid] = mySum = mySum + sdata[tid + 64];
  }

  cg::sync(cta);

  cg::thread_block_tile<32> tile32 = cg::tiled_partition<32>(cta);

  if (cta.thread_rank() < 32) {
    // Fetch final intermediate sum from 2nd warp
    if (blockSize >= 64) mySum += sdata[tid + 32];
    // Reduce final warp using shuffle
    for (int offset = tile32.size() / 2; offset > 0; offset /= 2) {
      mySum += tile32.shfl_down(mySum, offset);
    }
  }

  // write result for this block to global mem
  if (cta.thread_rank() == 0) g_odata[blockIdx.x] = mySum;
}

template <typename T, unsigned int blockSize, bool nIsPow2>
__global__ void reduce7(const T *__restrict__ g_idata, T *__restrict__ g_odata,
                        unsigned int n) {
  T *sdata = SharedMemory<T>();

  // perform first level of reduction,
  // reading from global memory, writing to shared memory
  unsigned int tid = threadIdx.x;
  unsigned int gridSize = blockSize * gridDim.x;
  unsigned int maskLength = (blockSize & 31);  // 31 = warpSize-1
  maskLength = (maskLength > 0) ? (32 - maskLength) : maskLength;
  const unsigned int mask = (0xffffffff) >> maskLength;

  T mySum = 0;

  // we reduce multiple elements per thread.  The number is determined by the
  // number of active thread blocks (via gridDim).  More blocks will result
  // in a larger gridSize and therefore fewer elements per thread
  if (nIsPow2) {
    unsigned int i = blockIdx.x * blockSize * 2 + threadIdx.x;
    gridSize = gridSize << 1;

    while (i < n) {
      mySum += g_idata[i];
      // ensure we don't read out of bounds -- this is optimized away for
      // powerOf2 sized arrays
      if ((i + blockSize) < n) {
        mySum += g_idata[i + blockSize];
      }
      i += gridSize;
    }
  } else {
    unsigned int i = blockIdx.x * blockSize + threadIdx.x;
    while (i < n) {
      mySum += g_idata[i];
      i += gridSize;
    }
  }

  // Reduce within warp using shuffle or reduce_add if T==int & CUDA_ARCH ==
  // SM 8.0
  mySum = warpReduceSum<T>(mask, mySum);

  // each thread puts its local sum into shared memory
  if ((tid % warpSize) == 0) {
    sdata[tid / warpSize] = mySum;
  }

  __syncthreads();

  const unsigned int shmem_extent =
    (blockSize / warpSize) > 0 ? (blockSize / warpSize) : 1;
  const unsigned int ballot_result = __ballot_sync(mask, tid < shmem_extent);
  if (tid < shmem_extent) {
    mySum = sdata[tid];
    // Reduce final warp using shuffle or reduce_add if T==int & CUDA_ARCH ==
    // SM 8.0
    mySum = warpReduceSum<T>(ballot_result, mySum);
  }

  // write result for this block to global mem
  if (tid == 0) {
    g_odata[blockIdx.x] = mySum;
  }
}

// Performs a reduction step and updates numTotal with how many are remaining
template <typename T, typename Group>
__device__ T cg_reduce_n(T in, Group &threads) {
  return cg::reduce(threads, in, cg::plus<T>());
}

template <class T>
__global__ void cg_reduce(T *g_idata, T *g_odata, unsigned int n) {
  // Shared memory for intermediate steps
  T *sdata = SharedMemory<T>();
  // Handle to thread block group
  cg::thread_block cta = cg::this_thread_block();
  // Handle to tile in thread block
  cg::thread_block_tile<32> tile = cg::tiled_partition<32>(cta);

  unsigned int ctaSize = cta.size();
  unsigned int numCtas = gridDim.x;
  unsigned int threadRank = cta.thread_rank();
  unsigned int threadIndex = (blockIdx.x * ctaSize) + threadRank;

  T threadVal = 0;
  {
    unsigned int i = threadIndex;
    unsigned int indexStride = (numCtas * ctaSize);
    while (i < n) {
      threadVal += g_idata[i];
      i += indexStride;
    }
    sdata[threadRank] = threadVal;
  }

  // Wait for all tiles to finish and reduce within CTA
  {
    unsigned int ctaSteps = tile.meta_group_size();
    unsigned int ctaIndex = ctaSize >> 1;
    while (ctaIndex >= 32) {
      cta.sync();
      if (threadRank < ctaIndex) {
        threadVal += sdata[threadRank + ctaIndex];
        sdata[threadRank] = threadVal;
      }
      ctaSteps >>= 1;
      ctaIndex >>= 1;
    }
  }

  // Shuffle redux instead of smem redux
  {
    cta.sync();
    if (tile.meta_group_rank() == 0) {
      threadVal = cg_reduce_n(threadVal, tile);
    }
  }

  if (threadRank == 0) g_odata[blockIdx.x] = threadVal;
}

template <class T, size_t BlockSize, size_t MultiWarpGroupSize>
__global__ void multi_warp_cg_reduce(T *g_idata, T *g_odata, unsigned int n) {
  // Shared memory for intermediate steps
  T *sdata = SharedMemory<T>();
  __shared__ cg::experimental::block_tile_memory<sizeof(T), BlockSize> scratch;

  // Handle to thread block group
  auto cta = cg::experimental::this_thread_block(scratch);
  // Handle to multiWarpTile in thread block
  auto multiWarpTile = cg::experimental::tiled_partition<MultiWarpGroupSize>(cta);

  unsigned int gridSize = BlockSize * gridDim.x;
  T threadVal = 0;

  // we reduce multiple elements per thread.  The number is determined by the
  // number of active thread blocks (via gridDim).  More blocks will result
  // in a larger gridSize and therefore fewer elements per thread
  int nIsPow2 = !(n & n-1);
  if (nIsPow2) {
    unsigned int i = blockIdx.x * BlockSize * 2 + threadIdx.x;
    gridSize = gridSize << 1;

    while (i < n) {
      threadVal += g_idata[i];
      // ensure we don't read out of bounds -- this is optimized away for
      // powerOf2 sized arrays
      if ((i + BlockSize) < n) {
        threadVal += g_idata[i + blockDim.x];
      }
      i += gridSize;
    }
  } else {
    unsigned int i = blockIdx.x * BlockSize + threadIdx.x;
    while (i < n) {
      threadVal += g_idata[i];
      i += gridSize;
    }
  }

  threadVal = cg_reduce_n(threadVal, multiWarpTile);

  if (multiWarpTile.thread_rank() == 0) {
    sdata[multiWarpTile.meta_group_rank()] = threadVal;
  }
  cg::sync(cta);

  if (threadIdx.x == 0) {
    threadVal = 0;
    for (int i=0; i < multiWarpTile.meta_group_size(); i++) {
      threadVal += sdata[i];
    }
    g_odata[blockIdx.x] = threadVal;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Wrapper function for kernel launch
////////////////////////////////////////////////////////////////////////////////
template <class T>
void reduce(int size, int threads, int blocks, int whichKernel, T *d_idata,
            T *d_odata) {
  dim3 dimBlock(threads, 1, 1);
  dim3 dimGrid(blocks, 1, 1);

  // when there is only one warp per block, we need to allocate two warps
  // worth of shared memory so that we don't index shared memory out of bounds
  int smemSize =
    (threads <= 32) ? 2 * threads * sizeof(T) : threads * sizeof(T);

  // as kernel 9 - multi_warp_cg_reduce cannot work for more than 64 threads
  // we choose to set kernel 7 for this purpose.
  if (threads < 64 && whichKernel == 9)
    {
      whichKernel = 7;
    }

  // choose which of the optimized versions of reduction to launch
  switch (whichKernel) {
  case 0:
    reduce0<T><<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
    break;

  case 1:
    reduce1<T><<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
    break;

  case 2:
    reduce2<T><<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
    break;

  case 3:
    reduce3<T><<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
    break;

  case 4:
    switch (threads) {
    case 512:
      reduce4<T, 512>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 256:
      reduce4<T, 256>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 128:
      reduce4<T, 128>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 64:
      reduce4<T, 64>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 32:
      reduce4<T, 32>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 16:
      reduce4<T, 16>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 8:
      reduce4<T, 8>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 4:
      reduce4<T, 4>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 2:
      reduce4<T, 2>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 1:
      reduce4<T, 1>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;
    }

    break;

  case 5:
    switch (threads) {
    case 512:
      reduce5<T, 512>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 256:
      reduce5<T, 256>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 128:
      reduce5<T, 128>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 64:
      reduce5<T, 64>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 32:
      reduce5<T, 32>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 16:
      reduce5<T, 16>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 8:
      reduce5<T, 8>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 4:
      reduce5<T, 4>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 2:
      reduce5<T, 2>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 1:
      reduce5<T, 1>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;
    }

    break;

  case 6:
    if (isPow2(size)) {
      switch (threads) {
      case 512:
	reduce6<T, 512, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 256:
	reduce6<T, 256, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 128:
	reduce6<T, 128, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 64:
	reduce6<T, 64, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 32:
	reduce6<T, 32, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 16:
	reduce6<T, 16, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 8:
	reduce6<T, 8, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 4:
	reduce6<T, 4, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 2:
	reduce6<T, 2, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 1:
	reduce6<T, 1, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;
      }
    } else {
      switch (threads) {
      case 512:
	reduce6<T, 512, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 256:
	reduce6<T, 256, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 128:
	reduce6<T, 128, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 64:
	reduce6<T, 64, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 32:
	reduce6<T, 32, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 16:
	reduce6<T, 16, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 8:
	reduce6<T, 8, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 4:
	reduce6<T, 4, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 2:
	reduce6<T, 2, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 1:
	reduce6<T, 1, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;
      }
    }

    break;

  case 7:
    // For reduce7 kernel we require only blockSize/warpSize
    // number of elements in shared memory
    smemSize = ((threads / 32) + 1) * sizeof(T);
    if (isPow2(size)) {
      switch (threads) {
      case 1024:
	reduce7<T, 1024, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;
      case 512:
	reduce7<T, 512, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 256:
	reduce7<T, 256, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 128:
	reduce7<T, 128, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 64:
	reduce7<T, 64, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 32:
	reduce7<T, 32, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 16:
	reduce7<T, 16, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 8:
	reduce7<T, 8, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 4:
	reduce7<T, 4, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 2:
	reduce7<T, 2, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 1:
	reduce7<T, 1, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;
      }
    } else {
      switch (threads) {
      case 1024:
	reduce7<T, 1024, true>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;
      case 512:
	reduce7<T, 512, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 256:
	reduce7<T, 256, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 128:
	reduce7<T, 128, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 64:
	reduce7<T, 64, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 32:
	reduce7<T, 32, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 16:
	reduce7<T, 16, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 8:
	reduce7<T, 8, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 4:
	reduce7<T, 4, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 2:
	reduce7<T, 2, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;

      case 1:
	reduce7<T, 1, false>
	  <<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
	break;
      }
    }

    break;
  case 8:
    cg_reduce<T><<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
    break;
  case 9:
    constexpr int numOfMultiWarpGroups = 2;
    smemSize = numOfMultiWarpGroups * sizeof(T);
    switch (threads) {
    case 1024:
      multi_warp_cg_reduce<T, 1024, 1024/numOfMultiWarpGroups>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 512:
      multi_warp_cg_reduce<T, 512, 512/numOfMultiWarpGroups>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 256:
      multi_warp_cg_reduce<T, 256, 256/numOfMultiWarpGroups>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 128:
      multi_warp_cg_reduce<T, 128, 128/numOfMultiWarpGroups>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    case 64:
      multi_warp_cg_reduce<T, 64, 64/numOfMultiWarpGroups>
	<<<dimGrid, dimBlock, smemSize>>>(d_idata, d_odata, size);
      break;

    default:
      printf("thread block size of < 64 is not supported for this kernel\n");
      break;
    }
    break;
  }
}

#define CUDA_STARTTIME(x)  cudaEventRecord(x ## _start, 0);
#define CUDA_STOPTIME(x) {					\
    float dtime;						\
    cudaEventRecord(x ## _stop, 0);				\
    cudaEventSynchronize(x ## _stop);				\
    cudaEventElapsedTime(&dtime, x ## _start, x ## _stop);	\
    x ## time += dtime; }

inline int print_cuda_memory_info() {
  //cudaError_t status;
  size_t free, total;
  
  checkCudaErrors(cudaMemGetInfo(&free, &total));
  
  fprintf(stdout, "GPU free memory is %.1f, total is %.1f MBbytes\n",
	  free/1024.0/1024, total/1024.0/1024);
  
  if(free<=0){
    fprintf(stderr, "Use too much GPU memory.\n");
    exit(EXIT_FAILURE);
  }
  
  return EXIT_SUCCESS;
}

/*! Overload cout with cuda complex data type
 */
#include <iostream>
static inline std::ostream& operator<<(std::ostream& os, const cuComplex& data){
  os << data.x << ' ' << data.y << ' ';
  return os;
}

/*! Overload * operator to multiple a cuComplex with a float for device and host code
 *
 * \param[in] a Input cuComplex number
 * \param[in] b Input float number
 * \returns   \a a * \a b
 *
 */
__device__ __host__ static inline cuComplex operator*(cuComplex a, float b) { return make_cuComplex(a.x*b, a.y*b);}

/*! Overload * operator to multiple a float with a cuComplex for device and host code
 *
 * \param[in] a Input float number
 * \param[in] b Input cuComplex number
 * \returns   \a a * \a b
 *
 */
__device__ __host__ static inline cuComplex operator*(float a, cuComplex b) { return make_cuComplex(b.x*a, b.y*a);}

/*! Overload / operator to divide a cuComplex with a float for device and host code
 *
 * \param[in, out] a A cuComplex number which will be divided by float \a b 
 * \param[in]      b Input float number
 * \returns        \a a / \a b
 *
 */
__device__ __host__ static inline cuComplex operator/(cuComplex a, float b) { return make_cuComplex(a.x/b, a.y/b);}

/*! Overload /= operator to divide a cuComplex with a float before it is accumulated to itself for device and host code
 *
 * \param[in, out] a A cuComplex number which will be divided by float \a b and accumulated to
 * \param[in]      b Input float number
 *
 */
__device__ __host__ static inline void operator/=(cuComplex &a, float b)     { a.x/=b;   a.y/=b;}

/*! Overload /= operator to plus a cuComplex with a cuComplex before it is accumulated to itself for device and host code
 *
 * \param[in, out] a A cuComplex number which will be added by cuComplex \a b and accumulated to
 * \param[in]      b Input cuComplex number
 *
 */
__device__ __host__ static inline void operator+=(cuComplex &a, cuComplex b) { a.x+=b.x; a.y+=b.y;}

/*! Overload /= operator to minus a cuComplex with a cuComplex before it is accumulated to itself for device and host code
 *
 * \param[in, out] a A cuComplex number which will be minused by cuComplex \a b and accumulated to
 * \param[in]      b Input cuComplex number
 *
 */
__device__ __host__ static inline void operator-=(cuComplex &a, cuComplex b) { a.x-=b.x; a.y-=b.y;}


#define CUDAUTIL_FLOAT2HALF __float2half
#define CUDAUTIL_FLOAT2INT  __float2int_rz
#define CUDAUTIL_FLOAT2UINT __float2uint_rz
#define CUDAUTIL_HALF2FLOAT __half2float
#define CUDAUTIL_DOUBLE2INT __double2int_rz

// We need more type case overload functions here
// The following convert float to other types
__device__ static inline void scalar_typecast(const float a, double   &b) { b = a;}
__device__ static inline void scalar_typecast(const float a, float    &b) { b = a;}
__device__ static inline void scalar_typecast(const float a, half     &b) { b = CUDAUTIL_FLOAT2HALF(a);}
__device__ static inline void scalar_typecast(const float a, int      &b) { b = CUDAUTIL_FLOAT2INT(a);}
__device__ static inline void scalar_typecast(const float a, int16_t  &b) { b = CUDAUTIL_FLOAT2INT(a);}
__device__ static inline void scalar_typecast(const float a, int8_t   &b) { b = CUDAUTIL_FLOAT2INT(a);}
__device__ static inline void scalar_typecast(const float a, unsigned &b) { b = CUDAUTIL_FLOAT2UINT(a);}

// The following convert other types to float
__device__ static inline void scalar_typecast(const double a,   float &b) { b = a;}
__device__ static inline void scalar_typecast(const half a,     float &b) { b = CUDAUTIL_HALF2FLOAT(a);}
__device__ static inline void scalar_typecast(const int a,      float &b) { b = a;}
__device__ static inline void scalar_typecast(const int16_t a,  float &b) { b = a;}
__device__ static inline void scalar_typecast(const int8_t a,   float &b) { b = a;}
__device__ static inline void scalar_typecast(const unsigned a, float &b) { b = a;}

template <typename TMIN, typename TSUB, typename TRES>
__device__ static inline void scalar_subtract(const TMIN minuend, const TSUB subtrahend, TRES &result) {
  TRES casted_minuend;
  TRES casted_subtrahend;
  
  scalar_typecast(minuend,    casted_minuend);
  scalar_typecast(subtrahend, casted_subtrahend);
  
  result = casted_minuend - casted_subtrahend;
}

template <typename TREAL, typename TIMAG, typename TCMPX>
__device__ static inline void make_cuComplex(const TREAL x, const TIMAG y, TCMPX &z){
  scalar_typecast(x, z.x);
  scalar_typecast(y, z.y);
}

/*! A template function to get a buffer into device
  It check where is the input buffer, if the buffer is on device, it just pass the pointer
  otherwise it alloc new buffer on device and copy the data to it
*/
template <typename T>
T* copy2device(T *raw, int ndata, enum cudaMemoryType &type){
  T *data = NULL;
  
  cudaPointerAttributes attributes; ///< to hold memory attributes
  
  // cudaMemoryTypeUnregistered for unregistered host memory,
  // cudaMemoryTypeHost for registered host memory,
  // cudaMemoryTypeDevice for device memory or
  // cudaMemoryTypeManaged for managed memory.
  checkCudaErrors(cudaPointerGetAttributes(&attributes, raw));
  type = attributes.type;
  
  if(type == cudaMemoryTypeUnregistered || type == cudaMemoryTypeHost){
    int nbytes = ndata*sizeof(T);
    checkCudaErrors(cudaMallocManaged(&data, nbytes, cudaMemAttachGlobal));
    checkCudaErrors(cudaMemcpy(data, raw, nbytes, cudaMemcpyDefault));
  }
  else{
    data = raw;
  }
  
  return data;
}

/*! A function to free memory if it is a copy of a host memory
 */
template<typename T>
int remove_device_copy(enum cudaMemoryType type, T *data){
  
  if(type == cudaMemoryTypeUnregistered || type == cudaMemoryTypeHost){
    checkCudaErrors(cudaFree(data));
  }
  
  return EXIT_SUCCESS;
}

/*! A kernel to contraint random number from range (0.0 1.0] to range (exclude include] or [include exclude).
 *
 * \param[in, out] data    The input data in range (0.0 1.0] and new data in range (exclude include] or [include exclude) is also returned with it.
 * \param[in]      exclude The exclusive end of random numbers
 * \param[in]      range   The range of random numbers, it does not have to be positive, it is calculated with `include - exclude`
 * \param[in]      ndata   Number of data
 * \tparam         T       We do not really need it here, just to make the function templated so that I can put it into a header file without complain
 *
 */
template<typename T>
__global__ void cudautil_contraintor(T *data, T exclude, T range, int ndata){
  // Maximum x-dimension of a grid of thread blocks is 2^31-1
  // Maximum x- or y-dimension of a block is 1024
  // So here we can cover (2^31-1)*1024 random numbers, which are 2^41-1024
  // should be big enough

  int idx = blockDim.x*blockIdx.x + threadIdx.x;
  if(idx<ndata){
    // Just in case we have a very small ndata
    data[idx] = data[idx]*range+exclude;
  }
}
//__global__ void cudautil_contraintor(float *data, float exclude, float range, int ndata);

/*! \brief A class to generate uniform distributed \p ndata random float data on device 
 *
 * The clase is created to generate uniform distributed \p ndata random float data on device in the range (exclude include] or [include exclude)
 * It uses `curandGenerateUniform` [curand](https://docs.nvidia.com/cuda/curand/index.html) API to generate random on device directly and then constraint it to a given range
 *
 */
class RealGeneratorUniform{
public:
  float *data = NULL; ///< Unified Memory to hold generated uniform distributed random numbers in float
  
  //! Constructor of RealGeneratorUniform class.
  /*!
   * 
   * - initialise the class
   * - create device memory for \p ndata float random numbers
   * - generate uniform distributed random numbers with `curandGenerateUniform` [curand](https://docs.nvidia.com/cuda/curand/index.html) API. 
   * 
   * `curandGenerateUniform` generates uniform distributed random numbers in the range of (0.0 1.0], the class converts the random numbers into a range defined by `exclude` and `include`, 
   * where `include` is the inclusive limit and `exclude` is the exclusive limit. If `exclude` is larger than `include`, the final data is in range [include exclude), otherwise the it is in range (exclude include]. If `include` is equal to `exclude`, we will get a constant number series as `exclude`. 
   * 
   * \param[in] gen     curand generator, should not create the generator inside the class, 
   *                    otherwise it is very likely that the same random numbers will be generated with different class instantiations
   * \param[in] exclude The exclusive limit of uniform random numbers
   * \param[in] include The inclusive limit if uniform random numbers
   * \param[in] ndata   Number of float random numbers to generate
   */
  RealGeneratorUniform(curandGenerator_t gen, int ndata, float exclude, float include, int nthread)
    :gen(gen), ndata(ndata), exclude(exclude), include(include), nthread(nthread){

    // Figure out range
    range = include-exclude;

    // Create output buffer as managed
    checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(float), cudaMemAttachGlobal));

    // Generate data
    checkCudaErrors(curandGenerateUniform(gen, data, ndata));

    // Setup kernel size and run it to convert to a given range
    nblock = (int)(ndata/(float)nthread+0.5);
    cudautil_contraintor<float><<<nblock, nthread>>>(data, exclude, range, ndata);

    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of RealGeneratorUniform class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealGeneratorUniform(){
    checkCudaErrors(cudaFree(data));
    checkCudaErrors(cudaDeviceSynchronize());
  }
    
private:
  int ndata;   ///< Number of generated data
  float include;  ///< inclusive limit of random numbers
  float exclude;  ///< inclusive limit of random numbers
  float range;    ///< Range
  int nthread;    ///< Number of threads
  int nblock;     ///< Number of cuda blocks
  
  curandGenerator_t gen; ///< Generator to generate uniform distributed random numbers
};

/*! \brief A class to generate normal distributed \p ndata random float data on device with given \p mean and \p stddev
 *
 * The clase is created to generate normal distributed \p ndata random float data on device with given \p mean and \p stddev.
 * It uses `curandGenerateNormal` [curand](https://docs.nvidia.com/cuda/curand/index.html) API to generate random on device directly, no further process happens here. 
 *
 */

class RealGeneratorNormal{
public:
  float *data = NULL; ///< Unified memory to hold normal distributed random numbers
  
  //! Constructor of RealGeneratorNormal class.
  /*!
   * 
   * - initialise the class
   * - create device memory for \p ndata float random numbers
   * - generate normal distributed random numbers with `curandGenerateNormal` [curand](https://docs.nvidia.com/cuda/curand/index.html) API. 
   * 
   * \param[in] gen    curand generator, should not create the generator inside the class, 
   *                   otherwise it is very likely that the same random numbers will be generated with different class instantiations
   * \param[in] mean   Required mean for normal distributed random numbers
   * \param[in] stddev Required standard deviation for normal distributed random numbers
   * \param[in] ndata  Number of float random numbers to generate
   */
  RealGeneratorNormal(curandGenerator_t gen, float mean, float stddev, int ndata)
    :gen(gen), mean(mean), stddev(stddev), ndata(ndata){

    // Create output buffer
    checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(float), cudaMemAttachGlobal));

    // Generate normal data
    checkCudaErrors(curandGenerateNormal(gen, data, ndata, mean, stddev));
  }
  
  //! Deconstructor of RealGeneratorNormal class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealGeneratorNormal(){
    checkCudaErrors(cudaFree(data));
  }
    
private:
  float mean;  ///< Mean of generated data
  float stddev;///< Standard deviation of generated data
  int ndata;   ///< Number of generated data
  
  curandGenerator_t gen; ///< Generator to generate normal distributed random numbers
};


/*! \brief A function to convert real data from \p TIN to \p TOUT on GPU
 * 
 * It is a template function which converts data from \p TIN to \p TOUT, where
 * \tparam TIN  The input data type
 * \tparam TOUT The output data type
 *
 * The data type convertation is done with an overloadded function `scalar_typecast`
 * \see scalar_typecast
 *
 * The supported data convertation is shown in the following table (we can add more support here later)
 *
 * TIN    | TOUT
 * -------|----
 * float  | float
 * float  | double
 * float  | half 
 * float  | int
 * float  | int16_t
 * float  | int8_t
 * double | float
 * half   | float
 * int    | float
 * int16_t| float
 * int8_t | float
 *
 * \param[in]  input  Data to be converted
 * \param[in]  ndata  Number of data points to be converted
 * \param[out] output Converted data
 *
 */
template <typename TIN, typename TOUT>
__global__ void cudautil_convert(const TIN *input, TOUT *output, int ndata){
  // Maximum x-dimension of a grid of thread blocks is 2^31-1
  // Maximum x- or y-dimension of a block is 1024
  // So here we can cover (2^31-1)*1024 random numbers, which are 2^41-1024
  // should be big enough
  
  int idx = blockDim.x*blockIdx.x + threadIdx.x;
  if(idx<ndata){
    // Just in case we have a very small ndata
    scalar_typecast(input[idx], output[idx]);
  }
}

/*! \brief A class to convert real device data from one type \p TIN to another \p TOUT
 * 
 * It is a template class which converts data from \p TIN to \p TOUT, where
 * \tparam TIN  The input data type
 * \tparam TOUT The output data type
 *
 * The data type convertation is done with a template CUDA kernel function `cudautil_convert`
 * \see cudautil_convert
 *
 * The supported data convertation is shown in the following table (we can add more support here later)
 *
 * TIN    | TOUT
 * -------|----
 * float  | float
 * float  | double
 * float  | half 
 * float  | int
 * float  | int16_t
 * float  | int8_t
 * double | float
 * half   | float
 * int    | float
 * int16_t| float
 * int8_t | float
 *
 */
template <typename TIN, typename TOUT>
class RealConvertor{
public:
  TOUT *data = NULL; ///< Converted data on Unified memory
  
  //! Constructor of RealConvertor class.
  /*!
   * 
   * - initialise the class
   * - create device memory for \p ndata float random numbers
   * - convert input data from \p TIN to \p TOUT on GPU with CUDA Kernel `cudautil_convert`
   *
   * \see cudautil_convert
   *
   * \tparam TIN Input data type
   * 
   * \param[in] raw     Data to be converted with data type \p TIN on device or host
   * \param[in] ndata   Number of data points ton be converted
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_convert` kernel
   *
   */
  RealConvertor(TIN *raw, int ndata, int nthread)
    :ndata(ndata), nthread(nthread){

    input = copy2device(raw, ndata, type);
    
    // Create output buffer as managed
    checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(TOUT), cudaMemAttachGlobal));

    // Setup kernel size and run it to convert data
    nblock = (int)(ndata/(float)nthread+0.5);
    cudautil_convert<<<nblock, nthread>>>(input, data, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_convert ]");

    // Free intermediate memory
    remove_device_copy(type, input);
    
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of RealConvertor class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealConvertor(){  
    checkCudaErrors(cudaFree(data));
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
private:
  enum cudaMemoryType type; ///< memory type
  TIN *input = NULL; ///< An internal pointer to input data
  int ndata;   ///< Number of generated data
  int nthread; ///< Number of threads per CUDA block
  int nblock;  ///< Number of blocks to process \p ndata
};


/*! \brief A function to convert input data from \p T to float and calculate its power in parallel on GPU
 * 
 * It is a template function to convert input data from \p T to float and calculate its power in parallel on GPU
 * \tparam T  The input data type
 *
 * The data type convertation is done with an overloadded function `scalar_typecast`
 * \see scalar_typecast
 *
 * The supported data convertation is shown in the following table (we can add more support here later)
 *
 * |T      |
 * |-------|
 * |double |
 * |half   |
 * |int    |
 * |int16_t|
 * |int8_t |
 *
 * \param[in]  d_data   Input data
 * \param[in]  ndata    Number of data
 * \param[out] d_float  Converted data in float
 * \param[out] d_float2 Power of converted data 
 *
 */
template <typename T>
__global__ void cudautil_pow(const T *d_data, float *d_float, float *d_float2, int ndata){
  int idx = blockDim.x*blockIdx.x + threadIdx.x;
  
  if(idx < ndata){
    float f_data;

    scalar_typecast(d_data[idx], f_data);
    d_float[idx]  = f_data;
    d_float2[idx] = f_data*f_data;
  }
}

template <typename T>
class RealMeanStddevCalculator {
  
public:
  float mean;   ///< Mean of the difference between input two vectors, always in float
  float stddev; ///< Standard deviation of the difference between input two vectors, always in float

  
  //! Constructor of class RealMeanStddevCalculator
  /*!
   * - initialise the class
   * - create required device memory
   * - convert input data from \p T to float and calculate its power in a single CUDA kernel `cudautil_pow`
   * - reduce the float data and its power to get mean
   * - calculate standard deviation with the mean of float and power data
   *
   * \param[in] raw     The input vector on device/host with data type \p T
   * \param[in] ndata   Number of data
   * \param[in] nthread Number of threads per CUDA block to run kernel `cudautil_pow`
   * \param[in] method  Data reduction method, which can be from 0 to 7 inclusive
   *
   * As kernel `cudautil_pow` uses `scalar_typecast` to convert \p T to float, the support \p T can be
   *
   * |T |
   * |--|
   * |double | 
   * |half   |
   * |int    | 
   * |int16_t|
   * |int8_t |
   * 
   * \see cudautil_pow, reduce, scalar_typecast
   *
   */
  RealMeanStddevCalculator(T *raw, int ndata, int nthread, int method)
    :ndata(ndata), nthread(nthread), method(method){

    /* Sort out input buffers */
    data = copy2device(raw, ndata, type);
    
    // Now do calculation
    nblock = (int)(ndata/(float)nthread+0.5);
    
    checkCudaErrors(cudaMallocManaged(&d_float,  ndata*sizeof(float), cudaMemAttachGlobal));
    checkCudaErrors(cudaMallocManaged(&d_float2, ndata*sizeof(float), cudaMemAttachGlobal));
    
    checkCudaErrors(cudaMallocManaged(&d_reduction, nblock*sizeof(float), cudaMemAttachGlobal));
    
    cudautil_pow<<<nblock, nthread>>>(data, d_float, d_float2, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_pow ]");
    
    // First reduce mean data
    reduce(ndata,  nthread, nblock, method, d_float, d_reduction);
    checkCudaErrors(cudaDeviceSynchronize());
    if(nblock > 1){
      reduce(nblock, nthread, 1, method, d_reduction, d_float);
      checkCudaErrors(cudaDeviceSynchronize());
      mean = d_float[0]/(float)ndata;
    }else{
      mean = d_reduction[0]/(float)ndata;
    }
    
    // Second reduce mean power 2 data
    reduce(ndata,  nthread, nblock, method, d_float2, d_reduction);
    checkCudaErrors(cudaDeviceSynchronize());
    if(nblock > 1){
      reduce(nblock, nthread, 1, method, d_reduction, d_float2);
      checkCudaErrors(cudaDeviceSynchronize());
      mean2 = d_float2[0]/(float)ndata;
    }else{
      mean2 = d_reduction[0]/(float)ndata;
    }

    // Got final numbers
    stddev = sqrtf(mean2 - mean*mean);

    // As we only need stddev and mean
    // Probably better to free all memory here
    checkCudaErrors(cudaFree(d_float));
    checkCudaErrors(cudaFree(d_float2));
    checkCudaErrors(cudaFree(d_reduction));
    
    remove_device_copy(type, data);
    
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of RealMeanStddevCalculator class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealMeanStddevCalculator(){
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
private:
  enum cudaMemoryType type; ///< memory type

  int ndata; ///< Number of input data
  int nthread; ///< Number of threads per CUDA block
  int nblock;  ///< Number of CUDA blocks
  int method; ///< data d_reduction method
  
  T *data = NULL;
  float *d_float = NULL;
  float *d_float2 = NULL;

  float *d_reduction; ///< it holds intermediate float data duration data d_reduction on device
  
  float mean2; ///< mean of difference power of 2
};


/*! \brief Overloadded kernel to get d_difference between two real input vectors
 *
 * \tparam T1 Data type of the first input vector
 * \tparam T2 Data type of the second input vector
 * 
 * \param[in]  d_data1 The first input vector in \p T1
 * \param[in]  d_data2 The second input vector in \p T2
 * \param[in]  ndata   Number of data
 * \param[out] d_diff  The d_difference between these two vectors in float, it is always in float
 *
 * The kernel uses `scalar_subtract` to get difference (in float) between two numbers and currently it supports (we can add more support later).
 *
 * T1     | T2
 * -------|----
 * float  | float
 * float  | half
 * half   | float 
 * half   | half 
 * 
 * \see scalar_subtract
 * 
 */
template <typename T1, typename T2>
__global__ void cudautil_subtract(const T1 *d_data1, const T2 *d_data2, float *d_diff, int ndata){
  int idx = blockDim.x*blockIdx.x + threadIdx.x;

  if(idx < ndata){
    //d_diff[idx] = d_data1[idx] - d_data2[idx];

    scalar_subtract(d_data1[idx], d_data2[idx], d_diff[idx]);
  }
}

/*! \brief A class to get the difference between two real vectors
 *
 * \tparam T1 Typename of the data in one vector
 * \tparam T2 Typename of the data in the other vector
 *
 * 
 * Suggested combinations of T1 and T2 are (other combinations may not work, we can add more support later)
 * T1     | T2
 * -------|----
 * float  | float
 * float  | half
 * half   | float 
 * half   | half  
 * 
 * The class to get difference between two real vectors, it is allowed to have different types for these inputs and
 * the result will be in float.
 * 
 */
template <typename T1, typename T2>
class RealDifferentiator {

public:
  float *data  = NULL;  ///< the difference between input \p data1 and \p data2
  
  //! Constructor of RealDifferentiator class.
  /*!
   * 
   * - initialise the class
   * - create device memory for the difference \p diff
   * - calculate the difference with a CUDA kernel `cudautil_subtract`
   *
   * \see cudautil_subtract, scalar_subtract
   * 
   * \param[in] raw1 The first input real vector
   * \param[in] raw2 The second input real vector
   * \param[in] ndata   Number of data to subtract
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_subtract`
   *
   */
  RealDifferentiator(T1 *raw1, T2 *raw2, int ndata, int nthread)
    :ndata(ndata), nthread(nthread){

    // sort out input buffers
    data1 = copy2device(raw1, ndata, type1);
    data2 = copy2device(raw2, ndata, type2);

    // Create output buffer as managed
    checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(float), cudaMemAttachGlobal));
    
    // setup kernel size and run it to get difference
    nblock = (int)(ndata/(float)nthread+0.5);
    cudautil_subtract<<<nblock, nthread>>>(data1, data2, data, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_subtract ]");

    // Free intermediate memory
    remove_device_copy(type1, data1);
    remove_device_copy(type2, data2);
    
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of RealDifferentiator class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealDifferentiator(){
    checkCudaErrors(cudaFree(data));
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
private:
  enum cudaMemoryType type1; ///< memory type
  enum cudaMemoryType type2; ///< memory type

  int ndata; ///< Number of input data
  int nthread; ///< Number of threads per CUDA block
  int nblock;  ///< Number of CUDA blocks

  T1 *data1 = NULL; ///< private variable to hold input vector pointer 1
  T2 *data2 = NULL; ///< private variable to hold input vector pointer 2
};

//! A template kernel to build complex numbers with its real and imag part
/*!
 * 
 * \see scalar_typecast
 *
 * \tparam TREAL Real part data type
 * \tparam TIMAG Imag part data type
 * \tparam TCMPX Complex data type
 * 
 * \param[in]  d_real  Real part to build complex numbers
 * \param[in]  d_imag  Imag part to build complex numbers
 * \param[in]  ndata   Number of data points ton be built
 * \param[out] d_cmpx  Complex numbers
 *
 */
template <typename TREAL, typename TIMAG, typename TCMPX>
__global__ void cudautil_complexbuilder(const TREAL *d_real, const TIMAG *d_imag, TCMPX *d_cmpx, int ndata){
  // Maximum x-dimension of a grid of thread blocks is 2^31-1
  // Maximum x- or y-dimension of a block is 1024
  // So here we can cover (2^31-1)*1024 random numbers, which are 2^41-1024
  // should be big enough
  
  int idx = blockDim.x*blockIdx.x + threadIdx.x;
  if(idx<ndata){

    scalar_typecast(d_real[idx], d_cmpx[idx].x);
    scalar_typecast(d_imag[idx], d_cmpx[idx].y);
  }
}

/*! \brief A class to build a complex vector with two real vectors
 *
 * \tparam TREAL Typename of the real part data
 * \tparam TIMAG Typename of the imag part data
 * \tparam TCMPX Typename of the complex data
 *
 * The class use kernel `cudautil_complexbuilder` to convert data type and build complex numbers. `cudautil_complexbuilder` uses `scalar_typecast` to convert data type. 
 * As of that, the allowed data type here is limited to following table (more types can be added later) 
 * 
 * TREAL/TIMAG    | TCMPX
 * -------|----
 * float  | cuComplex
 * float  | cuDoubleComplex
 * float  | half2
 * float  | int2
 * float  | short2
 * float  | int8_t ???
 * double | cuComplex
 * half   | cuComplex
 * int    | cuComplex
 * int16_t| cuComplex
 * int8_t | cuComplex
 *
 */
template <typename TREAL, typename TIMAG, typename TCMPX>
class ComplexBuilder {
public:
  TCMPX *data = NULL; ///< Complex data on device
  
  //! Constructor of ComplexBuilder class.
  /*!
   * 
   * - initialise the class
   * - create device memory for \p ndata complex numbers
   * - build complex numbers with \p real and \p imag
   *
   * \see cudautil_complexbuilder, scalar_typecast
   *
   * \tparam TREAL Real part data type
   * \tparam TIMAG Imag part data type
   * \tparam TCMPX Complex data type
   * 
   * \param[in] real  Real part to build complex numbers
   * \param[in] imag  Imag part to build complex numbers
   * \param[in] ndata   Number of data points ton be converted
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_complexbuilder` kernel
   *
   */
  ComplexBuilder(TREAL *real, TIMAG *imag, int ndata, int nthread )
    :ndata(ndata), nthread(nthread){

    // Sort out input data
    data_real = copy2device(real, ndata, type_real);
    data_imag = copy2device(imag, ndata, type_imag);

    // Create output buffer
    checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(TCMPX), cudaMemAttachGlobal));

    // Setup kernel size and run it
    nblock = (int)(ndata/(float)nthread+0.5);
    cudautil_complexbuilder<<<nblock, nthread>>>(data_real, data_imag, data, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_complexbuilder ]");

    // Free intermediate memory
    remove_device_copy(type_real, data_real);
    remove_device_copy(type_imag, data_imag);

    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of ComplexBuilder class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~ComplexBuilder(){
    checkCudaErrors(cudaFree(data));
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
private:
  TREAL *data_real = NULL;
  TIMAG *data_imag = NULL;

  enum cudaMemoryType type_real; ///< memory type
  enum cudaMemoryType type_imag; ///< memory type
  
  int ndata;
  int nthread;
  int nblock;
};

//! A template kernel to split complex numbers into its real and imag part
/*!
 * 
 * \see scalar_typecast
 *
 * \tparam TREAL Real part data type
 * \tparam TIMAG Imag part data type
 * \tparam TCMPX Complex data type
 * 
 * \param[in]  d_cmpx  Complex numbers 
 * \param[out] d_real  Real part of complex numbers
 * \param[out] d_imag  Imag part of complex numbers
 * \param[in]  ndata   Number of data points to be splitted
 *
 */
template <typename TCMPX, typename TREAL, typename TIMAG>
__global__ void cudautil_complexsplitter(const TCMPX *d_cmpx, const TREAL *d_real, TIMAG *d_imag, int ndata){
  // Maximum x-dimension of a grid of thread blocks is 2^31-1
  // Maximum x- or y-dimension of a block is 1024
  // So here we can cover (2^31-1)*1024 random numbers, which are 2^41-1024
  // should be big enough
  
  int idx = blockDim.x*blockIdx.x + threadIdx.x;
  if(idx<ndata){

    scalar_typecast(d_cmpx[idx].x,   d_real[idx]);
    scalar_typecast(d_cmpx[idx].y, d_imag[idx]);
  }
}

/*! \brief A class to build a complex vector with two real vectors
 *
 * \tparam TREAL Typename of the real part data
 * \tparam TIMAG Typename of the imag part data
 * \tparam TCMPX Typename of the complex data
 *
 * The class use kernel `cudautil_complexbuilder` to convert data type and build complex numbers. `cudautil_complexbuilder` uses `scalar_typecast` to convert data type. 
 * As of that, the allowed data type here is limited to following table (more types can be added later) 
 * 
 * TREAL/TIMAG    | TCMPX
 * -------|----
 * float  | cuComplex
 * float  | cuDoubleComplex
 * float  | half2
 * float  | int2
 * float  | short2
 * float  | int8_t ???
 * double | cuComplex
 * half   | cuComplex
 * int    | cuComplex
 * int16_t| cuComplex
 * int8_t | cuComplex
 *
 */
template <typename TCMPX, typename TREAL, typename TIMAG>
class ComplexSplitter {
public:
  TREAL *real = NULL; ///< Real part on device
  TIMAG *imag = NULL; ///< Imag part on device
  
  //! Constructor of ComplexSplitter class.
  /*!
   * 
   * - initialise the class
   * - create device memory for \p ndata real and imag numbers
   * - split complex numbers to \p real and \p imag
   *
   * \see cudautil_complexbuilder, scalar_typecast
   *
   * \tparam TREAL Real part data type
   * \tparam TIMAG Imag part data type
   * \tparam TCMPX Complex data type
   * 
   * \param[in] cmpx  Complex numbers
   * \param[in] ndata   Number of data points ton be converted
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_complexbuilder` kernel
   *
   */
  
  ComplexSplitter(TCMPX *cmpx, int ndata, int nthread)
    :ndata(ndata), nthread(nthread){

    // Sort out input buffer
    data = copy2device(cmpx, ndata, type);
    
    // Create managed memory for output
    checkCudaErrors(cudaMallocManaged(&real, ndata*sizeof(TREAL), cudaMemAttachGlobal));
    checkCudaErrors(cudaMallocManaged(&imag, ndata*sizeof(TIMAG), cudaMemAttachGlobal));

    // Setup kernel and run it 
    nblock = (int)(ndata/(float)nthread+0.5);
    
    cudautil_complexsplitter<<<nblock, nthread>>>(data, real, imag, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_complexsplitter ]");

    // Free intermediate memory
    remove_device_copy(type, data);    
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of ComplexSplitter class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~ComplexSplitter(){
    checkCudaErrors(cudaFree(real));
    checkCudaErrors(cudaFree(imag));

    checkCudaErrors(cudaDeviceSynchronize());
  }
  
private:
  TCMPX *data = NULL;
  
  enum cudaMemoryType type; ///< memory type
    
  int ndata;
  int nthread;
  int nblock;
};

//! A template kernel to calculate phase and amplitude of input array 
/*!
 * 
 * \see scalar_typecast
 *
 * \tparam T Complex number component data type
 * 
 * \param[in]  v         input Complex data
 * \param[in]  ndata     Number of data samples to be calculated
 * \param[out] amplitude Calculated amplitude
 * \param[out] phase     Calculated amplitude
 *
 */
template <typename T>
__global__ void cudautil_amplitude_phase(const T *v, float *amplitude, float *phase, int ndata){
  int idx = blockDim.x*blockIdx.x + threadIdx.x;
  
  if(idx < ndata){
    // We always do calculation in float
    float v1;
    float v2;
    
    scalar_typecast(v[idx].x, v1);
    scalar_typecast(v[idx].y, v2);
    
    amplitude[idx] = sqrtf(v1*v1+v2*v2);
    phase[idx]     = atan2f(v2, v1); // in radians
  }
}

template <typename T>
class AmplitudePhaseCalculator{
public:
  float *amp = NULL;///< Calculated amplitude on device
  float *pha = NULL;///< Calculated phase on device
  
  //! Constructor of AmplitudePhaseCalculator class.
  /*!
   * 
   * - initialise the class
   * - create device memory for amplitude and phase
   * - calculate phase and amplitude with CUDA
   *
   * \see cudautil_amplitude_phase
   *
   * \tparam TIN Input data type
   * 
   * \param[in] raw  input Complex data
   * \param[in] ndata   Number of samples to be converted, the size of data is 2*ndata
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_amplitude_phase` kernel
   *
   */
  AmplitudePhaseCalculator(T *raw,
			   int ndata,
			   int nthread
			   )
    :ndata(ndata), nthread(nthread){

    // sourt out input data
    data = copy2device(raw, ndata, type);
    
    // Get output buffer as managed
    checkCudaErrors(cudaMallocManaged(&amp, ndata * sizeof(float), cudaMemAttachGlobal));
    checkCudaErrors(cudaMallocManaged(&pha, ndata * sizeof(float), cudaMemAttachGlobal));
  
    // Get amplitude and phase
    nblock = (int)(ndata/(float)nthread+0.5);
    cudautil_amplitude_phase<<<nblock, nthread>>>(data, amp, pha, ndata);

    remove_device_copy(type, data);
    
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of RealGeneratorNormal class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~AmplitudePhaseCalculator(){
    
    checkCudaErrors(cudaFree(amp));
    checkCudaErrors(cudaFree(pha));

    checkCudaErrors(cudaDeviceSynchronize());
  }

private:
  int ndata; ///< number of values as a private parameter
  int nblock; ///< Number of CUDA blocks
  int nthread; ///< number of threas per block
  
  enum cudaMemoryType type; ///< memory type
  
  T *data; ///< To get hold on the input data
};


/*! \brief A class to allocate memory on device 
 *
 * \tparam T data type of the device memory, which can be a complex data type
 * 
 */
template <typename T>
class DeviceMemoryAllocator {
public:
  T *data = NULL; ///< Device memory or managed memory
  
  /*! Constructor of class DeviceMemoryAllocator
   *
   * \param[in] ndata Number of data on host as type \p T
   * \param[in] host  Marker to see if we also need a copy on host
   *
   */
  DeviceMemoryAllocator(int ndata, int host=0)
    :ndata(ndata), host(host){
    if(host){
      checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(T), cudaMemAttachGlobal));
    }
    else{
      checkCudaErrors(cudaMalloc(&data, ndata*sizeof(T)));
    }
  }
  
  //! Deconstructor of DeviceMemoryAllocator class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~DeviceMemoryAllocator(){
    checkCudaErrors(cudaFree(data));
  }

private:
  int ndata; ///< Number of data points
  int host;  ///< Do we also need to copy on host?
};


/*! \brief A class to copy device data to host
 *
 * \tparam T data type, which can be a complex data type
 * 
 */
template <typename T>
class DeviceDataExtractor {
public:
  T *data = NULL; ///< Host buffer to hold data
  
  /*!
   * \param[in] d_data Device data 
   * \param[in] ndata Number of data on host as type \p T
   * async memcpy will not work here as we always get new copy of memory
   */
  DeviceDataExtractor(T *d_data, int ndata)
    :d_data(d_data), ndata(ndata){
   
    size = ndata*sizeof(T);
    checkCudaErrors(cudaMallocHost(&data, size));
    checkCudaErrors(cudaMemcpy(data, d_data, size, cudaMemcpyDefault));
  }
  
  //! Deconstructor of DeviceDataExtractor class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~DeviceDataExtractor(){
    checkCudaErrors(cudaFreeHost(data));
  }
  
private:
  T *d_data = NULL;
  int ndata;
  int size;
};


/*! \brief A class to copy data from host to device
 *
 * \tparam T data type, which can be a complex data type
 * 
 */
template <typename T>
class HostDataExtractor {
public:
  T *data = NULL; ///< Device buffer to hold data

  /*! Constructor of class HostDataExtractor
   *
   * \param[in] h_data Host data 
   * \param[in] ndata Number of data on host as type \p T
   * async memcpy will not work here as we always get new copy of memory
   */
  HostDataExtractor(T *h_data, int ndata)
    :h_data(h_data), ndata(ndata){
    
    size = ndata*sizeof(T);
    checkCudaErrors(cudaMalloc(&data, size));
    checkCudaErrors(cudaMemcpy(data, h_data, size, cudaMemcpyDefault));
  }
  
  //! Deconstructor of HostDataExtractor class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~HostDataExtractor(){
    checkCudaErrors(cudaFree(data));
  }
  
private:
  T *h_data = NULL;
  int ndata;
  int size;
};


/*! \brief A class to allocate memory as managed
 *
 * \tparam T data type of the host memory, which can be a complex data type
 * 
 */
template <typename T>
class ManagedMemoryAllocator {
public:
  T *data = NULL; ///< Managed memory 
  
  /*! Constructor of class ManagedMemoryAllocator
   *
   * \param[in] ndata  Number of data on host as type \p T
   *
   */
  ManagedMemoryAllocator(int ndata)
    :ndata(ndata){
    checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(T), cudaMemAttachGlobal));
  }
  
  //! Deconstructor of ManagedMemoryAllocator class.
  /*!
   * 
   * - free host memory at the class life end
   */
  ~ManagedMemoryAllocator(){
    checkCudaErrors(cudaFree(data));
  }
  
private:
  int ndata; ///< Number of data points
};


/*! \brief A class to allocate memory on host 
 *
 * \tparam T data type of the host memory, which can be a complex data type
 * 
 */
template <typename T>
class HostMemoryAllocator {
public:
  T *data = NULL; ///< Host memory or managed memory
  
  /*! Constructor of class HostMemoryAllocator
   *
   * \param[in] ndata  Number of data on host as type \p T
   * \param[in] device Marker to tell if we also need a copy on device
   *
   */
  HostMemoryAllocator(int ndata, int device=0)
    :ndata(ndata), device(device){
    if(device){
      checkCudaErrors(cudaMallocManaged(&data, ndata*sizeof(T), cudaMemAttachGlobal));
    }
    else{
      checkCudaErrors(cudaMallocHost(&data, ndata*sizeof(T)));
    }
  }
  
  //! Deconstructor of HostMemoryAllocator class.
  /*!
   * 
   * - free host memory at the class life end
   */
  ~HostMemoryAllocator(){
    checkCudaErrors(cudaFreeHost(data));
    if(device){
      checkCudaErrors(cudaFree(data));
    }
  }

private:
  int ndata; ///< Number of data points
  int device; ///< Do we need a copy on device?
};


/*! This kernel is not used for real input data, 
 *
 * \param[in] in  Data to be binned
 * \param[in] ndata Number of data points
 * \param[in] max   Maximum to check, not Maximum of given data
 * \param[in] max   Maximum to check, not Maximum of given data
 * \param[in] out   Binned data
 *
 * \tparam TIN  The input data type
 */
template <typename T>
__global__ void cudautil_histogram(const T *in, int ndata, float min, float max, unsigned int *out)
{
  // pixel coordinates
  int x = blockIdx.x * blockDim.x + threadIdx.x;

  // grid dimensions
  int nx = blockDim.x * gridDim.x; 

  // initialize temporary accumulation array in shared memory
  // has one extra element?
  // if blockDim.x is smaller than NUM_BINS, we have to process multiple bins per thread
  __shared__ unsigned int smem[NUM_BINS + 1];
  for (int i = threadIdx.x; i < NUM_BINS + 1; i += blockDim.x) smem[i] = 0;
  __syncthreads();

  // process pixels
  // updates our block's partial histogram in shared memory
  // if kernel size is smaller than data size, each thread may have to process multiple inputs
  for (int col = x; col < ndata; col += nx) {
    int r = ((in[col] - min)/(max-min))*NUM_BINS;
    if(r >= 0 && r < NUM_BINS){
      // ignore samples outside range
      atomicAdd(&smem[r], 1);
    }
  }
  __syncthreads();

  // write partial histogram into the global memory
  out += blockIdx.x * NUM_BINS;
  for (int i = threadIdx.x; i < NUM_BINS; i += blockDim.x) {
    out[i] = smem[i];
  }
}

/*! A kernel to finish binning process started by cudautil_histogram_smem_atomics
  
  the kernel size here should be the same as previous gridDim.x size or larger
  
  \tparam T It is not necessary here but I need it to make the function templated.
  \see cudautil_histogram
*/
template <typename T>
__global__ void cudautil_histogram_final(const T *in, int n, unsigned int *out)
{
  // gridDim.x and blockDim.x should be enough to cover al bin index
  int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < NUM_BINS) {
    T total = 0;
    for (int j = 0; j < n; j++) 
      total += in[i + NUM_BINS * j];
    out[i] = total;
  }
}

/*! \brief A class to get histogram of \p ndata input data with a data type \tparam T
 *
 */
template <typename T>
class RealHistogram{
public:
  unsigned *data = NULL; ///< Unified Memory to hold generated uniform distributed random numbers in float
  
  //! Constructor of RealHistogram class.
  /*!
   */
  RealHistogram(T *raw, int ndata, float min, float max, int nblock, int nthread)
    :ndata(ndata), min(min), max(max), nblock(nblock), nthread(nthread){

    // Sort out input data
    input = copy2device(raw, ndata, type);
    
    // Create output buffer as managed
    checkCudaErrors(cudaMallocManaged(&data, NUM_BINS*sizeof(unsigned), cudaMemAttachGlobal));

    // Setup kernel size and run it to get bin
    dim3 grid_smem(nblock);  // Do not want have too many blocks
    dim3 grid_final(nblock/nthread);
    
    checkCudaErrors(cudaMallocManaged(&result, nblock*NUM_BINS*sizeof(unsigned), cudaMemAttachGlobal));
    
    cudautil_histogram<<<grid_smem, nthread>>>(input, ndata, min, max, result);
    getLastCudaError("Kernel execution failed [ cudautil_histogram ]");
    cudautil_histogram_final<unsigned int ><<<grid_final, nthread>>>(result, nblock, data);
    getLastCudaError("Kernel execution failed [ cudautil_histogram_final ]");

    // free intermediate data
    remove_device_copy(type, input);
    checkCudaErrors(cudaDeviceSynchronize());
  }
  
  //! Deconstructor of RealHistogram class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealHistogram(){
    checkCudaErrors(cudaFree(data));
    checkCudaErrors(cudaDeviceSynchronize());
  }
    
private:
  enum cudaMemoryType type; ///< memory type

  T *input; ///< input buffer
  int ndata;   ///< Number of generated data
  float min;  ///< min to check
  float max;  ///< max to check
  int nthread;    ///< Number of threads
  int nblock;     ///< Number of cuda blocks

  unsigned *result; ///< it is not the final result
};

#endif // CUDA_UTILITIES_H
//////////////////////////////////////////////////////////////////////
// $Log:$
//