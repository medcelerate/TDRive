// cuda_interop_win.h
//
// Minimal, dynamically-loaded CUDA runtime bindings for D3D11 interop.
// We deliberately do NOT link against (or include headers from) the CUDA
// toolkit: the declarations below are the stable C ABI of cudart64_*.dll,
// resolved at runtime with GetProcAddress. This keeps the plugin loadable
// on machines with no NVIDIA hardware (where we fall back to CPUMem mode)
// and keeps CI free of a CUDA toolkit install.
//
// TouchDesigner itself loads a cudart64_*.dll into the process (its CUDA
// TOPs use it), so lookup order is: already-loaded module first, then
// LoadLibrary over known cudart names.

#pragma once

#if defined(_WIN32)

#include <cstddef>
#include <cstdint>

struct ID3D11Resource;
struct IDXGIAdapter;

// Opaque CUDA types (matches the forward declarations in the TD SDK).
struct cudaArray;
typedef struct CUstream_st*             cudaStream_t;
typedef struct cudaGraphicsResource*    cudaGraphicsResource_t;
typedef int                             cudaError_t;   // 0 == cudaSuccess

namespace tdrive::cuda {

constexpr cudaError_t  kSuccess                  = 0;
constexpr unsigned int kGraphicsRegisterFlagsNone = 0;
constexpr int          kMemcpyDeviceToDevice      = 3;

constexpr unsigned int kStreamNonBlocking         = 0x01;

// ABI mirrors of the driver_types.h structs we pass by pointer.
struct ChannelFormatDesc { int x, y, z, w; int f; };
struct Extent            { size_t width, height, depth; };
struct Pos               { size_t x, y, z; };
struct PitchedPtr        { void* ptr; size_t pitch, xsize, ysize; };
struct Memcpy3DParms {
    cudaArray*  srcArray; Pos srcPos; PitchedPtr srcPtr;
    cudaArray*  dstArray; Pos dstPos; PitchedPtr dstPtr;
    Extent      extent;   // in elements when either side is an array
    int         kind;
};

// Loads cudart (idempotent). Returns false if no cudart / no CUDA device.
bool Load();

// True when cudart is loaded, at least one CUDA device exists, AND at least
// one DXGI adapter maps to a CUDA device (guards hybrid-GPU laptops where the
// D3D11 default adapter isn't the NVIDIA GPU). Safe to call at DLL load time;
// this is what decides TOP_ExecuteMode.
bool AvailableForD3D11();

// False only when TDRIVE_CUDA is "0" / "false" / "off" - the opt-out.
//
// CUDA execute mode is the default wherever it is available, and process-wide:
// it is decided at load time for every Rive TOP. At 60 fps TD reports ~4-5 ms
// gpuCookTime per node in this mode, but a bare CUDA-mode TOP that writes
// nothing reports ~4.2 ms too - it is TD's CUDA bracket timing a wait. Our CUDA
// calls are ~0.5 ms, and uncapped with 1-8 nodes throughput matched or beat
// CPUMem with 25-30% less CPU cook time (RTX 2070 SUPER, TD 2025.30280).
bool AllowedByEnv();

// Picks the DXGI adapter (by EnumAdapters ordinal) that maps to a CUDA
// device. Returns -1 if none.
int FindCUDAAdapterOrdinal();

// Resolved entry points - valid after Load() returns true.
struct Api {
    cudaError_t (*getDeviceCount)(int* count);
    cudaError_t (*d3d11GetDevice)(int* device, IDXGIAdapter* adapter);
    cudaError_t (*graphicsD3D11RegisterResource)(
        cudaGraphicsResource_t* resource, ID3D11Resource* d3dResource,
        unsigned int flags);
    cudaError_t (*graphicsUnregisterResource)(cudaGraphicsResource_t resource);
    cudaError_t (*graphicsMapResources)(int count,
                                        cudaGraphicsResource_t* resources,
                                        cudaStream_t stream);
    cudaError_t (*graphicsUnmapResources)(int count,
                                          cudaGraphicsResource_t* resources,
                                          cudaStream_t stream);
    cudaError_t (*graphicsSubResourceGetMappedArray)(
        cudaArray** array, cudaGraphicsResource_t resource,
        unsigned int arrayIndex, unsigned int mipLevel);
    // cudaMemcpy2DArrayToArray has no stream variant; this is the one
    // array-to-array copy that takes a stream.
    cudaError_t (*memcpy3DAsync)(const Memcpy3DParms* p, cudaStream_t stream);
    cudaError_t (*streamCreateWithFlags)(cudaStream_t* stream,
                                         unsigned int flags);
    cudaError_t (*streamDestroy)(cudaStream_t stream);
    // The real allocated extent of a cudaArray, in elements.
    cudaError_t (*arrayGetInfo)(ChannelFormatDesc* desc, Extent* extent,
                                unsigned int* flags, cudaArray* array);
    const char* (*getErrorString)(cudaError_t err);
};

// nullptr until Load() succeeds.
const Api* Get();

} // namespace tdrive::cuda

#endif // _WIN32
