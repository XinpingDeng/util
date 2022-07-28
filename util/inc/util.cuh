#ifndef _UTIL_UTIL_CUH
#define _UTIL_UTIL_CUH

#include <cuComplex.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cuda.h>

#include <cufft.h>
#include <curand.h>
#include <curand_kernel.h>
#include <helper_cuda.h>

#ifdef __cplusplus
extern "C" {
#endif
  /*! \brief A function to check CUDA global memory.
   *
   * This function prints out total and available CUDA global memory in MBytes. 
   * 
   */
  int print_cuda_memory_info();
  /*! A kernel to contraint random number from range (0.0 1.0] to range (exclude include] or [include exclude).
   *
   * \param[in, out] data    The input data in range (0.0 1.0] and new data in range (exclude include] or [include exclude) is also returned with it.
   * \param[in]      exclude The exclusive end of random numbers
   * \param[in]      range   The range of random numbers, it does not have to be positive, it is calculated with `include - exclude`
   * \param[in]      ndata   Number of data
   *
   */
  __global__ void cudautil_contraintor(float *data, float exclude, float range, int ndata);
  
#ifdef __cplusplus
}
#endif

#include <iostream>
static inline std::ostream& operator<<(std::ostream& os, const cuComplex& data){
  os << data.x << ' ' << data.y << ' ';
  return os;
}

__device__ __host__ static inline cuComplex operator*(cuComplex a, float b) { return make_cuComplex(a.x*b, a.y*b);}
__device__ __host__ static inline cuComplex operator*(float a, cuComplex b) { return make_cuComplex(b.x*a, b.y*a);}
__device__ __host__ static inline cuComplex operator/(cuComplex a, float b) { return make_cuComplex(a.x/b, a.y/b);}
__device__ __host__ static inline void operator/=(cuComplex &a, float b)     { a.x/=b;   a.y/=b;}
__device__ __host__ static inline void operator+=(cuComplex &a, cuComplex b) { a.x+=b.x; a.y+=b.y;}
__device__ __host__ static inline void operator-=(cuComplex &a, cuComplex b) { a.x-=b.x; a.y-=b.y;}

#include "reduction.cuh"
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

//// We need more type case overload functions here
//// The following convert float to other types
//template <typename T1, typename T2>
//__device__ static inline void scalar_typecast(const T1 a, T2 &b){b = a;}
//
//template <typename T>
//__device__ static inline void scalar_typecast(const float a, T &b) { b = CUDAUTIL_FLOAT2INT(a);}
//
//// The following are special cases
//__device__ static inline void scalar_typecast(const float a, half     &b) { b = CUDAUTIL_FLOAT2HALF(a);}
//__device__ static inline void scalar_typecast(const half a,  float    &b) { b = CUDAUTIL_HALF2FLOAT(a);}
//__device__ static inline void scalar_typecast(const float a, unsigned &b) { b = CUDAUTIL_FLOAT2UINT(a);}

template <typename TMIN, typename TSUB, typename TRES>
__device__ static inline TRES operator-(const TMIN minuend, const TSUB subtrahend) {
  TRES casted_minuend;
  TRES casted_subtrahend;
  
  scalar_typecast(minuend,    casted_minuend);
  scalar_typecast(subtrahend, casted_subtrahend);  

  TRES result = casted_minuend-casted_subtrahend;
  return result;
}

template <typename TREAL, typename TIMAG, typename TCMPX>
__device__ static inline void make_cuComplex(const TREAL x, const TIMAG y, TCMPX &z){
  scalar_typecast(x, z.x);
  scalar_typecast(y, z.y);
}

/*! \brief A class to generate uniform distributed \p ndata random float data on device 
 *
 * The clase is created to generate uniform distributed \p ndata random float data on device in the range (exclude include] or [include exclude)
 * It uses `curandGenerateUniform` [curand](https://docs.nvidia.com/cuda/curand/index.html) API to generate random on device directly and then constraint it to a given range
 *
 */

class RealDataGeneratorUniform{
public:
  float *d_data = NULL; ///< Uniform distributed random number in float on device

  //! Constructor of RealDataGeneratorUniform class.
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
  RealDataGeneratorUniform(curandGenerator_t gen, int ndata, float exclude, float include, int nthread)
    :gen(gen), ndata(ndata), exclude(exclude), include(include), nthread(nthread){

    range = include-exclude;
    
    checkCudaErrors(cudaMalloc(&d_data, ndata * sizeof(float)));
    
    checkCudaErrors(curandGenerateUniform(gen, d_data, ndata));
        
    nblock = ndata/nthread;
    nblock = (nblock>1)?nblock:1;

    cudautil_contraintor<<<nblock, nthread>>>(d_data, exclude, range, ndata);
  }
  
  //! Deconstructor of RealDataGeneratorUniform class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealDataGeneratorUniform(){
    checkCudaErrors(cudaFree(d_data));
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

class RealDataGeneratorNormal{
public:
  float *d_data = NULL; ///< Normal distributed random number in float on device

  //! Constructor of RealDataGeneratorNormal class.
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
  RealDataGeneratorNormal(curandGenerator_t gen, float mean, float stddev, int ndata)
    :gen(gen), mean(mean), stddev(stddev), ndata(ndata){
    checkCudaErrors(cudaMalloc(&d_data, ndata * sizeof(float)));
    
    checkCudaErrors(curandGenerateNormal(gen, d_data, ndata, mean, stddev));
  }
  
  //! Deconstructor of RealDataGeneratorNormal class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealDataGeneratorNormal(){
    checkCudaErrors(cudaFree(d_data));
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
class RealDataConvertor{
public:
  TOUT *d_converted_data = NULL; ///< Converted data on device as \p TOUT data type
  
  //! Constructor of RealDataConvertor class.
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
   * \param[in] d_data  Data to be converted with data type \p TIN on device
   * \param[in] ndata   Number of data points ton be converted
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_convert` kernel
   *
   */
  RealDataConvertor(TIN *d_data, int ndata, int nthread)
    :d_data(d_data), ndata(ndata), nthread(nthread){
    
    checkCudaErrors(cudaMalloc(&d_converted_data, ndata*sizeof(TOUT)));
    
    nblock = ndata/nthread;
    nblock = (nblock>1)?nblock:1;

    cudautil_convert<<<nblock, nthread>>>(d_data, d_converted_data, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_convert ]");
  }

  
  //! Deconstructor of RealDataGeneratorNormal class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealDataConvertor(){
  
    checkCudaErrors(cudaFree(d_converted_data));
  }
  
private:
  TIN *d_data = NULL; ///< An internal pointer to input data
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
class RealDataMeanStddevCalcultor {
  
public:
  float mean;   ///< Mean of the difference between input two vectors, always in float
  float stddev; ///< Standard deviation of the difference between input two vectors, always in float

  
  //! Constructor of class RealDataMeanStddevCalcultor
  /*!
   * - initialise the class
   * - create required device memory
   * - convert input data from \p T to float and calculate its power in a single CUDA kernel `cudautil_pow`
   * - reduce the float data and its power to get mean
   * - calculate standard deviation with the mean of float and power data
   *
   * \param[in] d_data  The input vector on device with data type \p T
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
  RealDataMeanStddevCalcultor(T *d_data, int ndata, int nthread, int method)
    :d_data(d_data), ndata(ndata), nthread(nthread), method(method){
    
    nblock = ndata/nthread;
    nblock = (nblock>1)?nblock:1;
    
    checkCudaErrors(cudaMalloc(&d_float, ndata*sizeof(float)));
    checkCudaErrors(cudaMalloc(&d_float2, ndata*sizeof(float)));

    checkCudaErrors(cudaMalloc(&d_reduction, nblock*sizeof(float)));
    checkCudaErrors(cudaMallocHost(&d_sum, sizeof(float)));
    checkCudaErrors(cudaMallocHost(&d_sum2, sizeof(float)));

    cudautil_pow<<<nblock, nthread>>>(d_data, d_float, d_float2, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_pow ]");
    
    // First reduce mean data
    reduce(ndata,  nthread, nblock, method, d_float, d_reduction);
    if(nblock > 1){
      reduce(nblock, nthread, 1, method, d_reduction, d_float);
      checkCudaErrors(cudaMemcpy(d_sum, d_float, sizeof(float), cudaMemcpyDeviceToHost));
    }else{
      checkCudaErrors(cudaMemcpy(d_sum, d_reduction, sizeof(float), cudaMemcpyDeviceToHost));
    }
    
    // Second reduce mean power 2 data
    reduce(ndata,  nthread, nblock, method, d_float2, d_reduction);
    if(nblock > 1){
      reduce(nblock, nthread, 1, method, d_reduction, d_float2);
      checkCudaErrors(cudaMemcpy(d_sum2, d_float2, sizeof(float), cudaMemcpyDeviceToHost));
    }else{
      checkCudaErrors(cudaMemcpy(d_sum2, d_reduction, sizeof(float), cudaMemcpyDeviceToHost));
    }
    
    mean  = d_sum[0]/(float)ndata;
    mean2 = d_sum2[0]/(float)ndata;
    
    stddev = sqrtf(mean2 - mean*mean);
  }
  
  //! Deconstructor of RealDataMeanStddevCalcultor class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealDataMeanStddevCalcultor(){
    checkCudaErrors(cudaFree(d_float));
    checkCudaErrors(cudaFree(d_float2));
    checkCudaErrors(cudaFree(d_reduction));
    
    checkCudaErrors(cudaFreeHost(d_sum));
    checkCudaErrors(cudaFreeHost(d_sum2));
  }
  
private:

  int ndata; ///< Number of input data
  int nthread; ///< Number of threads per CUDA block
  int nblock;  ///< Number of CUDA blocks
  int method; ///< data d_reduction method
  
  T *d_data = NULL;
  float *d_float = NULL;
  float *d_float2 = NULL;

  float *d_reduction; ///< it holds intermediate float data duration data d_reduction on device
  
  float *d_sum;  ///< d_sum of difference
  float *d_sum2; ///< d_sum of difference power of 2
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
    d_diff[idx] = d_data1[idx] - d_data2[idx];
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
class RealDataDifferentiator {

public:
  float *d_diff  = NULL;  ///< the difference between input \p d_data1 and \p d_data2
  
  //! Constructor of RealDataDifferentiator class.
  /*!
   * 
   * - initialise the class
   * - create device memory for the difference \p d_diff
   * - calculate the difference with a CUDA kernel `cudautil_subtract`
   *
   * \see cudautil_subtract, scalar_subtract
   * 
   * \param[in] d_data1 The first input real vector
   * \param[in] d_data2 The second input real vector
   * \param[in] ndata   Number of float random numbers to generate
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_convert`
   *
   */
  RealDataDifferentiator(T1 *d_data1, T2 *d_data2, int ndata, int nthread)
    :d_data1(d_data1), d_data2(d_data2), ndata(ndata), nthread(nthread){
    
    // Now get memory for \p diff 
    nblock = ndata/nthread;
    nblock = (nblock>1)?nblock:1;
    checkCudaErrors(cudaMalloc(&d_diff,  ndata*sizeof(float)));
    
    // Now get difference
    cudautil_subtract<<<nblock, nthread>>>(d_data1, d_data2, d_diff, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_subtract ]");
  }
  
  //! Deconstructor of RealDataDifferentiator class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~RealDataDifferentiator(){
    
    checkCudaErrors(cudaFree(d_diff));
  }
  
private:

  int ndata; ///< Number of input data
  int nthread; ///< Number of threads per CUDA block
  int nblock;  ///< Number of CUDA blocks

  T1 *d_data1 = NULL; ///< private variable to hold input vector pointer 1
  T2 *d_data2 = NULL; ///< private variable to hold input vector pointer 2
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
class ComplexDataBuilder {
public:
  TCMPX *d_cmpx = NULL; ///< Complex data on device
  
  //! Constructor of ComplexDataBuilder class.
  /*!
   * 
   * - initialise the class
   * - create device memory for \p ndata complex numbers
   * - build complex numbers with \p d_real and \p d_imag
   *
   * \see cudautil_complexbuilder, scalar_typecast
   *
   * \tparam TREAL Real part data type
   * \tparam TIMAG Imag part data type
   * \tparam TCMPX Complex data type
   * 
   * \param[in] d_real  Real part to build complex numbers
   * \param[in] d_imag  Imag part to build complex numbers
   * \param[in] ndata   Number of data points ton be converted
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_complexbuilder` kernel
   *
   */
  ComplexDataBuilder(TREAL *d_real, TIMAG *d_imag, int ndata, int nthread )
    :d_real(d_real), d_imag(d_imag), ndata(ndata), nthread(nthread){
    checkCudaErrors(cudaMalloc(&d_cmpx, ndata*sizeof(TCMPX)));

    nblock = ndata/nthread;
    nblock = (nblock>1)?nblock:1;
    
    cudautil_complexbuilder<<<nblock, nthread>>>(d_real, d_imag, d_cmpx, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_complexbuilder ]");
  }
  
  //! Deconstructor of ComplexDataBuilder class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~ComplexDataBuilder(){
    checkCudaErrors(cudaFree(d_cmpx));
  }
  
private:
  TREAL *d_real = NULL;
  TIMAG *d_imag = NULL;
  
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
class ComplexDataSplitter {
public:
  TREAL *d_real = NULL; ///< Real part on device
  TIMAG *d_imag = NULL; ///< Imag part on device
  
  //! Constructor of ComplexDataSplitter class.
  /*!
   * 
   * - initialise the class
   * - create device memory for \p ndata real and imag numbers
   * - split complex numbers to \p d_real and \p d_imag
   *
   * \see cudautil_complexbuilder, scalar_typecast
   *
   * \tparam TREAL Real part data type
   * \tparam TIMAG Imag part data type
   * \tparam TCMPX Complex data type
   * 
   * \param[in] d_cmpx  Complex numbers
   * \param[in] ndata   Number of data points ton be converted
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_complexbuilder` kernel
   *
   */
  
  ComplexDataSplitter(TCMPX *d_cmpx, int ndata, int nthread)
    :d_cmpx(d_cmpx), ndata(ndata), nthread(nthread){
    checkCudaErrors(cudaMalloc(&d_real, ndata*sizeof(TREAL)));
    checkCudaErrors(cudaMalloc(&d_imag, ndata*sizeof(TIMAG)));

    nblock = ndata/nthread;
    nblock = (nblock>1)?nblock:1;
    
    cudautil_complexsplitter<<<nblock, nthread>>>(d_cmpx, d_real, d_imag, ndata);
    getLastCudaError("Kernel execution failed [ cudautil_complexsplitter ]");
  }
  
  //! Deconstructor of ComplexDataSplitter class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~ComplexDataSplitter(){
    checkCudaErrors(cudaFree(d_real));
    checkCudaErrors(cudaFree(d_imag));
  }
  
private:
  TCMPX *d_cmpx = NULL;
  
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
  float *d_amp = NULL;///< Calculated amplitude on device
  float *d_pha = NULL;///< Calculated phase on device
  
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
   * \param[in] d_data  input Complex data
   * \param[in] ndata   Number of samples to be converted, the size of d_data is 2*ndata
   * \param[in] nthread Number of threads per CUDA block to run `cudautil_amplitude_phase` kernel
   *
   */
  AmplitudePhaseCalculator(T *d_data,
			   int ndata,
			   int nthread
			   )
    :d_data(d_data), ndata(ndata), nthread(nthread){
    // Get other buffers
    checkCudaErrors(cudaMalloc(&d_amp, ndata * sizeof(float)));
    checkCudaErrors(cudaMalloc(&d_pha, ndata * sizeof(float)));
  
    // Get amplitude and phase
    nblock = ndata/nthread;
    nblock = (nblock>1)?nblock:1;
    cudautil_amplitude_phase<<<nblock, nthread>>>(d_data, d_amp, d_pha, ndata);
  }
  
  //! Deconstructor of RealDataGeneratorNormal class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~AmplitudePhaseCalculator(){
    
    checkCudaErrors(cudaFree(d_amp));
    checkCudaErrors(cudaFree(d_pha));
  }

private:
  int ndata; ///< number of values as a private parameter
  int nblock; ///< Number of CUDA blocks
  int nthread; ///< number of threas per block
  
  T *d_data; ///< To get hold on the input data
};


/*! \brief A class to allocate memory on device 
 *
 * \tparam T data type of the device memory, which can be a complex data type
 * 
 */
template <typename T>
class DeviceMemoryAllocator {
public:
  T *d_data = NULL; ///< Device memory
  T *h_data = NULL; ///< Host memory
  
  /*! Constructor of class DeviceMemoryAllocator
   *
   * \param[in] ndata Number of data on host as type \p T
   * \param[in] host  Marker to see if we also need a copy on host
   *
   */
 DeviceMemoryAllocator(int ndata, int host=0)
   :ndata(ndata), host(host){
    checkCudaErrors(cudaMalloc(&d_data, ndata*sizeof(T)));
    if(host){
      checkCudaErrors(cudaMallocHost(&h_data, ndata*sizeof(T)));
    }
  }
  
  //! Deconstructor of DeviceMemoryAllocator class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~DeviceMemoryAllocator(){
    checkCudaErrors(cudaFree(d_data));
    if(host){
      checkCudaErrors(cudaFreeHost(h_data));
    }
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
  T *h_data = NULL; ///< Host buffer to hold data
  
  /*!
   * \param[in] d_data Device data 
   * \param[in] ndata Number of data on host as type \p T
   * async memcpy will not work here as we always get new copy of memory
   */
  DeviceDataExtractor(T *d_data, int ndata)
    :d_data(d_data), ndata(ndata){
   
    size = ndata*sizeof(T);
    checkCudaErrors(cudaMallocHost(&h_data, size));
    checkCudaErrors(cudaMemcpy(h_data, d_data, size, cudaMemcpyDeviceToHost));
  }
  
  //! Deconstructor of DeviceDataExtractor class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~DeviceDataExtractor(){
    checkCudaErrors(cudaFreeHost(h_data));
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
  T *d_data = NULL; ///< Device buffer to hold data

  /*! Constructor of class HostDataExtractor
   *
   * \param[in] h_data Host data 
   * \param[in] ndata Number of data on host as type \p T
   * async memcpy will not work here as we always get new copy of memory
   */
  HostDataExtractor(T *h_data, int ndata)
    :h_data(h_data), ndata(ndata){
    
    size = ndata*sizeof(T);
    checkCudaErrors(cudaMalloc(&d_data, size));
    checkCudaErrors(cudaMemcpy(d_data, h_data, size, cudaMemcpyHostToDevice));
  }
  
  //! Deconstructor of HostDataExtractor class.
  /*!
   * 
   * - free device memory at the class life end
   */
  ~HostDataExtractor(){
    checkCudaErrors(cudaFree(d_data));
  }
  
private:
  T *h_data = NULL;
  int ndata;
  int size;
};


/*! \brief A class to allocate memory on host 
 *
 * \tparam T data type of the host memory, which can be a complex data type
 * 
 */
template <typename T>
class HostMemoryAllocator {
public:
  T *h_data = NULL; ///< Host memory
  T *d_data = NULL; ///< Device memory
  
  /*! Constructor of class HostMemoryAllocator
   *
   * \param[in] ndata  Number of data on host as type \p T
   * \param[in] device Marker to tell if we also need a copy on device
   *
   */
 HostMemoryAllocator(int ndata, int device)
   :ndata(ndata), device(device){
    checkCudaErrors(cudaMallocHost(&h_data, ndata*sizeof(T)));
    if(device){
      checkCudaErrors(cudaMalloc(&d_data, ndata*sizeof(T)));
    }
  }
  
  //! Deconstructor of HostMemoryAllocator class.
  /*!
   * 
   * - free host memory at the class life end
   */
  ~HostMemoryAllocator(){
    checkCudaErrors(cudaFreeHost(h_data));
    if(device){
      checkCudaErrors(cudaFree(d_data));
    }
  }

private:
  int ndata; ///< Number of data points
  int device; ///< Do we need a copy on device?
};


#endif // _CUDAUTIL_H