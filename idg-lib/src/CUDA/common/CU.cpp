// Copyright (C) 2020 ASTRON (Netherlands Institute for Radio Astronomy)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CU.h"

#include <sstream>
#include <cstring>
#include <stdexcept>
#include <cassert>

#include <vector_types.h>

#define assertCudaCall(val) __assertCudaCall(val, #val, __FILE__, __LINE__)
#define checkCudaCall(val) __checkCudaCall(val, #val, __FILE__, __LINE__)

namespace cu {

/*
    Error checking
*/
inline void __assertCudaCall(CUresult result, char const *const func,
                             const char *const file, int const line) {
  if (result != CUDA_SUCCESS) {
    const char *msg;
    cuGetErrorString(result, &msg);
    std::cerr << "CUDA Error at " << file;
    std::cerr << ":" << line;
    std::cerr << " in function " << func;
    std::cerr << ": " << msg;
    std::cerr << std::endl;
    throw Error<CUresult>(result);
  }
}

inline void __checkCudaCall(CUresult result, char const *const func,
                            const char *const file, int const line) {
  try {
    __assertCudaCall(result, func, file, line);
  } catch (Error<CUresult> &error) {
    // pass
  }
}

/*
    Init
*/
void init(unsigned flags) { assertCudaCall(cuInit(flags)); }

/*
    Class Device
*/
int Device::getCount() {
  int nrDevices;
  assertCudaCall(cuDeviceGetCount(&nrDevices));
  return nrDevices;
}

Device::Device(int ordinal) { assertCudaCall(cuDeviceGet(&_device, ordinal)); }

std::string Device::get_name() const {
  char name[64];
  assertCudaCall(cuDeviceGetName(name, sizeof(name), _device));
  return std::string(name);
}

int Device::get_capability() const {
  int capability =
      10 * get_attribute<CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR>() +
      get_attribute<CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR>();
  return capability;
}

Device::operator CUdevice() { return _device; }

size_t Device::get_free_memory() const {
  size_t free;
  size_t total;
  cuMemGetInfo(&free, &total);
  return free;
}

size_t Device::get_total_memory() const {
  size_t free;
  size_t total;
  cuMemGetInfo(&free, &total);
  return total;
}

/*
    Class Context
*/
Context::Context() { _context = NULL; }

Context::Context(Device &device, int flags) {
  _device = device;
  assertCudaCall(cuCtxCreate(&_context, flags, device));
}

Context::~Context() {
  std::cout << "Destroying context" << std::endl;
  assertCudaCall(cuCtxDestroy(_context));
}

void Context::setCurrent() const { assertCudaCall(cuCtxSetCurrent(_context)); }

void Context::setCacheConfig(CUfunc_cache config) {
  assertCudaCall(cuCtxSetCacheConfig(config));
}

void Context::setSharedMemConfig(CUsharedconfig config) {
  assertCudaCall(cuCtxSetSharedMemConfig(config));
}

void Context::synchronize() { assertCudaCall(cuCtxSynchronize()); }

void Context::reset() { assertCudaCall(cuDevicePrimaryCtxReset(_device)); }

Context::operator CUcontext() { return _context; }

/*
    HostMemory
*/
HostMemory::HostMemory(size_t size, int flags) {
  m_capacity = size;
  m_bytes = size;
  _flags = flags;
  if (size != 0) {
    void *ptr;
    assertCudaCall(cuMemHostAlloc(&ptr, size, _flags));
    set(ptr);
  }
}

HostMemory::~HostMemory() { release(); }

void HostMemory::resize(size_t size) {
  assert(size > 0);
  if (size < m_capacity) {
    m_bytes = size;
  } else if (size > m_capacity) {
    release();
    void *ptr;
    assertCudaCall(cuMemHostAlloc(&ptr, size, _flags));
    m_bytes = size;
    m_capacity = size;
    set(ptr);
  }
}

void HostMemory::release() {
  void *ptr = get();
  if (ptr) {
    assertCudaCall(cuMemFreeHost(ptr));
    set(nullptr);
  }
}

void HostMemory::zero() { memset(get(), 0, m_bytes); }

/*
    RegisteredMemory
*/
RegisteredMemory::RegisteredMemory(void *ptr, size_t size, int flags) {
  m_bytes = size;
  _flags = flags;
  assert(ptr != NULL);
  set(ptr);
  checkCudaCall(cuMemHostRegister(ptr, size, _flags));
}

RegisteredMemory::~RegisteredMemory() { release(); }

void RegisteredMemory::resize(size_t size) {
  throw std::runtime_error("RegisteredMemory can not be resized!");
}

void RegisteredMemory::release() { checkCudaCall(cuMemHostUnregister(get())); }

void RegisteredMemory::zero() { memset(get(), 0, m_bytes); }

/*
    DeviceMemory
*/

// Ensure the static const member _nullptr is not only declared but also defined
// otherwise it has no address

const CUdeviceptr DeviceMemory::_nullptr;

DeviceMemory::DeviceMemory(size_t size) {
  _capacity = size;
  _size = size;
  if (size) {
    assertCudaCall(cuMemAlloc(&_ptr, size));
  }
}

DeviceMemory::~DeviceMemory() {
  if (_capacity) assertCudaCall(cuMemFree(_ptr));
}

size_t DeviceMemory::capacity() { return _capacity; }

size_t DeviceMemory::size() { return _size; }

void DeviceMemory::resize(size_t size) {
  _size = size;
  if (size > _capacity) {
    if (_capacity) assertCudaCall(cuMemFree(_ptr));
    assertCudaCall(cuMemAlloc(&_ptr, size));
    _capacity = size;
  }
}

void DeviceMemory::zero(CUstream stream) {
  if (_size) {
    if (stream != NULL) {
      cuMemsetD8Async(_ptr, 0, _size, stream);
    } else {
      cuMemsetD8(_ptr, 0, _size);
    }
  }
}

/*
    UnifiedMemory
 */
UnifiedMemory::UnifiedMemory(void *ptr, size_t size) {
  _ptr = (CUdeviceptr)ptr;
  _size = size;
}

UnifiedMemory::UnifiedMemory(size_t size, unsigned flags) {
  _size = size;
  free = true;
  assertCudaCall(cuMemAllocManaged(&_ptr, _size, flags));
}

UnifiedMemory::~UnifiedMemory() {
  if (free) {
    assertCudaCall(cuMemFree(_ptr));
  }
}

void UnifiedMemory::set_advice(CUmem_advise advice) {
  assertCudaCall(cuMemAdvise(_ptr, _size, advice, CU_DEVICE_CPU));
}

void UnifiedMemory::set_advice(CUmem_advise advice, Device &device) {
  assertCudaCall(cuMemAdvise(_ptr, _size, advice, device));
}

/*
    Source
*/
Source::Source(const char *input_file_name)
    : input_file_name(input_file_name) {}

void Source::compile(const char *output_file_name,
                     const char *compiler_options) {
  std::stringstream command_line;
  command_line << NVCC;
  command_line << " -cubin ";
  command_line << compiler_options;
  command_line << " -o ";
  command_line << output_file_name;
  command_line << ' ' << input_file_name;

#if defined(DEBUG)
#pragma omp critical(cout)
  std::clog << "Compiling " << output_file_name << std::endl;
  std::clog << command_line.str() << std::endl;
#endif
  int retval = system(command_line.str().c_str());

  if (WEXITSTATUS(retval) != 0) {
    throw cu::Error<CUresult>(CUDA_ERROR_INVALID_SOURCE);
  }
}

/*
   Module
*/
Module::Module(const char *file_name) {
  assertCudaCall(cuModuleLoad(&_module, file_name));
}

Module::Module(const void *data) {
  assertCudaCall(cuModuleLoadData(&_module, data));
}

Module::~Module() { assertCudaCall(cuModuleUnload(_module)); }

Module::operator CUmodule() { return _module; }

/*
    Function
*/
Function::Function(Module &module, const char *name) {
  assertCudaCall(cuModuleGetFunction(&_function, module, name));
}

Function::Function(CUfunction function) { _function = function; }

int Function::get_attribute(CUfunction_attribute attribute) {
  int value;
  assertCudaCall(cuFuncGetAttribute(&value, attribute, _function));
  return value;
}

void Function::setCacheConfig(CUfunc_cache config) {
  assertCudaCall(cuFuncSetCacheConfig(_function, config));
}

Function::operator CUfunction() { return _function; }

/*
    Event
*/
Event::Event(int flags) { assertCudaCall(cuEventCreate(&_event, flags)); }

Event::~Event() { assertCudaCall(cuEventDestroy(_event)); }

void Event::synchronize() { assertCudaCall(cuEventSynchronize(_event)); }

float Event::elapsedTime(Event &second) {
  float ms;
  assertCudaCall(cuEventElapsedTime(&ms, second, _event));
  return ms;
}

Event::operator CUevent() { return _event; }

/*
    Stream
*/
Stream::Stream(int flags) { assertCudaCall(cuStreamCreate(&_stream, flags)); }

Stream::~Stream() { assertCudaCall(cuStreamDestroy(_stream)); }

void Stream::memcpyHtoDAsync(CUdeviceptr devPtr, const void *hostPtr,
                             size_t size) {
  assertCudaCall(cuMemcpyHtoDAsync(devPtr, hostPtr, size, _stream));
}

void Stream::memcpyDtoHAsync(void *hostPtr, CUdeviceptr devPtr, size_t size) {
  assertCudaCall(cuMemcpyDtoHAsync(hostPtr, devPtr, size, _stream));
}

void Stream::memcpyDtoDAsync(CUdeviceptr dstPtr, CUdeviceptr srcPtr,
                             size_t size) {
  assertCudaCall(cuMemcpyDtoDAsync(dstPtr, srcPtr, size, _stream));
}

void Stream::launchKernel(Function &function, unsigned gridX, unsigned gridY,
                          unsigned gridZ, unsigned blockX, unsigned blockY,
                          unsigned blockZ, unsigned sharedMemBytes,
                          const void **parameters) {
  assertCudaCall(cuLaunchKernel(function, gridX, gridY, gridZ, blockX, blockY,
                                blockZ, sharedMemBytes, _stream,
                                const_cast<void **>(parameters), 0));
}

void Stream::launchKernel(Function &function, dim3 grid, dim3 block,
                          unsigned sharedMemBytes, const void **parameters) {
  assertCudaCall(cuLaunchKernel(function, grid.x, grid.y, grid.z, block.x,
                                block.y, block.z, sharedMemBytes, _stream,
                                const_cast<void **>(parameters), 0));
}

void Stream::query() { assertCudaCall(cuStreamQuery(_stream)); }

void Stream::synchronize() { assertCudaCall(cuStreamSynchronize(_stream)); }

void Stream::waitEvent(Event &event) {
  assertCudaCall(cuStreamWaitEvent(_stream, event, 0));
}

void Stream::addCallback(CUstreamCallback callback, void *userData, int flags) {
  assertCudaCall(cuStreamAddCallback(_stream, callback, userData, flags));
}

void Stream::record(Event &event) {
  assertCudaCall(cuEventRecord(event, _stream));
}

Stream::operator CUstream() { return _stream; }

Marker::Marker(const char *message, unsigned color) {
  _attributes = {0};
  _attributes.version = NVTX_VERSION;
  _attributes.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
  _attributes.colorType = NVTX_COLOR_ARGB;
  _attributes.color = color;
  _attributes.messageType = NVTX_MESSAGE_TYPE_ASCII;
  _attributes.message.ascii = message;
}

Marker::Marker(const char *message, Color color)
    : Marker(message, convert(color)) {}

void Marker::start() { _id = nvtxRangeStartEx(&_attributes); }

void Marker::end() { nvtxRangeEnd(_id); }

unsigned int Marker::convert(Color color) {
  switch (color) {
    case red:
      return 0xffff0000;
    case green:
      return 0xff00ff00;
    case blue:
      return 0xff0000ff;
    case yellow:
      return 0xffffff00;
    case black:
      return 0xff000000;
    default:
      return 0xff00ff00;
  }
}

}  // end namespace cu
