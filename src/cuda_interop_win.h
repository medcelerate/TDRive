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

// ABI mirrors of cudaChannelFormatDesc / cudaExtent (driver_types.h).
struct ChannelFormatDesc { int x, y, z, w; int f; };
struct Extent            { size_t width, height, depth; };

// Loads cudart (idempotent). Returns false if no cudart / no CUDA device.
bool Load();

// True when cudart is loaded, at least one CUDA device exists, AND at least
// one DXGI adapter maps to a CUDA device (guards hybrid-GPU laptops where the
// D3D11 default adapter isn't the NVIDIA GPU). Safe to call at DLL load time;
// this is what decides TOP_ExecuteMode.
bool AvailableForD3D11();

// True when TDRIVE_CUDA is set to something other than "0" / "false" / "off".
//
// CUDA execute mode is OPT-IN, and process-wide: it is decided at load time
// for every Rive TOP. At 60 fps TD reports ~5 ms gpuCookTime per node in this
// mode, but that timer spans an interop wait: our CUDA calls are ~0.5 ms, the
// Rive render ~0.15 ms, and uncapped with 1-8 nodes throughput matched or beat
// CPUMem with less CPU cook time (RTX 2070 SUPER, TD 2025.30280). A dedicated
// CUDA stream made no difference.
bool EnabledByEnv();

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
    cudaError_t (*memcpy2DArrayToArray)(
        cudaArray* dst, size_t wOffsetDst, size_t hOffsetDst,
        const cudaArray* src, size_t wOffsetSrc, size_t hOffsetSrc,
        size_t widthBytes, size_t height, int kind);
    // The real allocated extent of a cudaArray, in elements.
    cudaError_t (*arrayGetInfo)(ChannelFormatDesc* desc, Extent* extent,
                                unsigned int* flags, cudaArray* array);
    const char* (*getErrorString)(cudaError_t err);
};

// nullptr until Load() succeeds.
const Api* Get();

} // namespace tdrive::cuda

#endif // _WIN32
