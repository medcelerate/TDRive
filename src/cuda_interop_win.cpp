// cuda_interop_win.cpp
//
// Runtime loader for the small slice of the CUDA runtime API we need for
// D3D11 interop. See cuda_interop_win.h for why we don't link the toolkit.

#if defined(_WIN32)

#include "cuda_interop_win.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstring>

namespace tdrive::cuda {

namespace {

Api  gApi{};
bool gLoaded = false;
bool gTried  = false;

// TouchDesigner ships/loads a cudart64_*.dll; prefer whatever is already in
// the process so we match TD's CUDA runtime version exactly.
HMODULE FindLoadedCudart()
{
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return nullptr;
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count && i < 1024; ++i) {
        char name[MAX_PATH] = {};
        if (GetModuleBaseNameA(GetCurrentProcess(), mods[i], name,
                               sizeof(name)) == 0) continue;
        if (_strnicmp(name, "cudart64", 8) == 0) return mods[i];
    }
    return nullptr;
}

HMODULE LoadCudart()
{
    if (HMODULE m = FindLoadedCudart()) return m;
    // Fall back to well-known cudart names, newest first.
    static const char* kNames[] = {
        "cudart64_13.dll", "cudart64_12.dll", "cudart64_110.dll",
        "cudart64_102.dll", "cudart64_101.dll", "cudart64_100.dll",
    };
    for (const char* n : kNames) {
        if (HMODULE m = LoadLibraryA(n)) return m;
    }
    return nullptr;
}

template <typename T>
bool Resolve(HMODULE m, const char* name, T& fn)
{
    fn = reinterpret_cast<T>(GetProcAddress(m, name));
    return fn != nullptr;
}

} // namespace

bool Load()
{
    if (gTried) return gLoaded;
    gTried = true;

    HMODULE m = LoadCudart();
    if (!m) return false;

    bool ok = true;
    ok &= Resolve(m, "cudaGetDeviceCount",       gApi.getDeviceCount);
    ok &= Resolve(m, "cudaD3D11GetDevice",       gApi.d3d11GetDevice);
    ok &= Resolve(m, "cudaGraphicsD3D11RegisterResource",
                  gApi.graphicsD3D11RegisterResource);
    ok &= Resolve(m, "cudaGraphicsUnregisterResource",
                  gApi.graphicsUnregisterResource);
    ok &= Resolve(m, "cudaGraphicsMapResources",   gApi.graphicsMapResources);
    ok &= Resolve(m, "cudaGraphicsUnmapResources", gApi.graphicsUnmapResources);
    ok &= Resolve(m, "cudaGraphicsSubResourceGetMappedArray",
                  gApi.graphicsSubResourceGetMappedArray);
    ok &= Resolve(m, "cudaMemcpy2DArrayToArray",   gApi.memcpy2DArrayToArray);
    ok &= Resolve(m, "cudaGetErrorString",         gApi.getErrorString);
    if (!ok) return false;

    int count = 0;
    if (gApi.getDeviceCount(&count) != kSuccess || count <= 0) return false;

    gLoaded = true;
    return true;
}

const Api* Get() { return gLoaded ? &gApi : nullptr; }

int FindCUDAAdapterOrdinal()
{
    if (!Load()) return -1;

    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory),
                                 reinterpret_cast<void**>(
                                     factory.ReleaseAndGetAddressOf()))))
        return -1;

    for (UINT i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (factory->EnumAdapters(i, adapter.ReleaseAndGetAddressOf()) ==
            DXGI_ERROR_NOT_FOUND)
            break;
        int dev = -1;
        if (gApi.d3d11GetDevice(&dev, adapter.Get()) == kSuccess && dev >= 0)
            return (int)i;
    }
    return -1;
}

bool AvailableForD3D11()
{
    return Load() && FindCUDAAdapterOrdinal() >= 0;
}

} // namespace tdrive::cuda

#endif // _WIN32
