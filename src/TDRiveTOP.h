// TDRiveTOP.h
//
// Cross-platform TouchDesigner Custom TOP that renders a .riv via Rive's
// PLS renderer. All platform-specific GPU code lives in an IBackend
// implementation; this class handles file/scene/view-model state,
// parameters, CHOP/DAT inputs, and the Info DAT.

#pragma once

#include "TOP_CPlusPlusBase.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/scene.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"

#include "IBackend.h"

class TDRiveTOP : public TD::TOP_CPlusPlusBase {
public:
    TDRiveTOP(const TD::OP_NodeInfo* info, TD::TOP_Context* context);
    ~TDRiveTOP() override;

    // TOP_CPlusPlusBase overrides
    void getGeneralInfo(TD::TOP_GeneralInfo*, const TD::OP_Inputs*, void*) override;
    void execute(TD::TOP_Output*, const TD::OP_Inputs*, void*) override;
    void setupParameters(TD::OP_ParameterManager*, void*) override;
    void pulsePressed(const char* name, void*) override;
    void getErrorString(TD::OP_String* error, void*) override;
    void getWarningString(TD::OP_String* warning, void*) override;
    void buildDynamicMenu(const TD::OP_Inputs*, TD::OP_BuildDynamicMenuInfo*, void*) override;
    int32_t getNumInfoCHOPChans(void*) override;
    void getInfoCHOPChan(int32_t index, TD::OP_InfoCHOPChan* chan, void*) override;
    bool getInfoDATSize(TD::OP_InfoDATSize*, void*) override;
    void getInfoDATEntries(int32_t index, int32_t nEntries,
                           TD::OP_InfoDATEntries* entries, void*) override;

    // One addressable input on the loaded artboard.
    //
    // Rive's input surface comes in two generations and this flattens both
    // into one list: state-machine inputs first (the original API - flat,
    // number/bool/trigger only), then the view-model tree (data binding -
    // nested, many types, addressed by '/'-delimited path).
    //
    // This is the single source of truth behind BOTH the Info DAT and the
    // node's Python `propertySchema`. They must not format values
    // independently: the two halves used to be walked separately, which is
    // how the Info DAT's index column ended up restarting at 0 partway down
    // the table.
    struct SchemaEntry {
        std::string source;    // "smi" | "vm"
        std::string path;      // "Hover", or "payoffCard/barGraph1Label"
        std::string type;      // "number"/"bool"/"trigger", or "vm:string"/...
        std::string value;     // current value, stringified
        std::vector<std::string> options;  // closed-set values; else empty
        bool container = false;            // vm:viewModel - branch, not a leaf
    };

    // Walks the live state, so it is not const and not cheap - call it once
    // per cook, not per row.
    std::vector<SchemaEntry> propertySchema();

private:
    bool loadFileIfNeeded(const char* absPath);
    // Destroys the loaded file and everything built from it, dependents first.
    void releaseFile();
    void releaseArtboard();
    bool selectArtboardIfNeeded(const char* name);
    bool selectSceneIfNeeded(const char* stateMachineName);
    void applyInputsFromCHOP(const TD::OP_CHOPInput* chop);
    void applyStringsFromDAT(const TD::OP_DATInput* dat);
    void bindArtboardViewModel();

    // Output size from the node's built-in Common page (no custom Resolution
    // parameter), and the content box handed to Renderer::align(). Both need
    // the artboard, so both run after the file/artboard are resolved.
    void computeResolution(const TD::OP_Inputs* inputs,
                           int32_t& outW, int32_t& outH) const;
    rive::AABB artboardFrame(bool layoutFit) const;

    // Flattened view-model property tree, rebuilt whenever the view model is
    // (re)bound. A view model can hold child view models, so the Info DAT
    // reports one row per property at every depth, addressed by the same
    // '/'-delimited path Rive's own runtime accessors take - e.g.
    // "payoffCard/title". That path is what the Strings DAT writes to.
    struct VmProp {
        std::string    path;
        rive::DataType type;
    };
    std::vector<VmProp> mVmProps;
    // Rebuilt in getInfoDATSize() and walked by getInfoDATEntries(). TD asks
    // for the size before it walks the rows, so one rebuild covers the table.
    std::vector<SchemaEntry> mSchemaCache;
    void rebuildVmProps();
    void collectVmProps(rive::ViewModelInstanceRuntime* vm,
                        const std::string& prefix, int depth);

    // Texture injection (Image1..N params -> view-model image properties).
    // CPU download path; used on macOS and as the Windows non-CUDA fallback.
    void applyImageInputsCPU(const TD::OP_Inputs* inputs);
    // Binds 'img' to the named view-model image property (once per change).
    void bindSlotImage(int slot, const char* propName, rive::RenderImage* img);
    rive::StateMachineInstance* currentSMI() { return mSMI; }
    void setError(const std::string& s) { mError = s; }
    void clearError()                   { mError.clear(); }

    TD::TOP_Context*               mContext = nullptr;
    std::unique_ptr<tdrive::IBackend> mBackend;
    bool                           mBackendReady = false;

    // Loaded file + scene
    rive::rcp<rive::File>                       mFile;
    std::unique_ptr<rive::ArtboardInstance>     mArtboard;
    std::unique_ptr<rive::Scene>                mScene;
    rive::StateMachineInstance*                 mSMI = nullptr;  // non-owning
    rive::rcp<rive::ViewModelInstanceRuntime>   mVMRuntime;

    // The artboard's authored frame, snapshotted when the instance is created.
    // See selectArtboardIfNeeded() for why Rive's own originalWidth() and
    // resetSize() cannot be used for this.
    float mArtboardW = 0.0f;
    float mArtboardH = 0.0f;

    std::string mLoadedPath;
    std::string mLoadedArtboard;
    std::string mLoadedStateMachine;
    // An artboard refused by findBrokenNestedAnimation(). A flag as well as a
    // name, because "" is itself a valid name (the default artboard).
    bool        mArtboardRejected = false;
    std::string mRejectedArtboard;

    // Playback timing
    std::chrono::steady_clock::time_point mLastTick;
    bool mHasTick = false;

    // Skip Idle Frames (see execute()). Everything outside Rive that changes
    // what a render would produce; a difference from the last cook forces a
    // render, the same as the scene reporting a change.
    struct RenderKey {
        int32_t     w = -1, h = -1, fit = -1, align = -1;
        double      bg[4] = {-1.0, -1.0, -1.0, -1.0};
        const void* artboard = nullptr;
        const void* scene    = nullptr;
        bool        loaded   = false;
        bool sameAs(const RenderKey& o) const
        {
            return w == o.w && h == o.h && fit == o.fit && align == o.align &&
                   bg[0] == o.bg[0] && bg[1] == o.bg[1] && bg[2] == o.bg[2] &&
                   bg[3] == o.bg[3] && artboard == o.artboard && scene == o.scene &&
                   loaded == o.loaded;
        }
    };
    RenderKey mLastRenderKey;
    // absFrame of the last execute(), for spotting cooks TouchDesigner skipped.
    int64_t mLastCookFrame = -1;
    // Cooks still to render. A change sets it to 2, not 1: the CPU readback
    // hands TouchDesigner the PREVIOUS cook's frame (double-buffered staging),
    // so the cook after the last change has to render once more to deliver it.
    int  mRenderFramesLeft = 2;
    // Set by the CHOP/DAT/texture input paths whenever they push a new value
    // into Rive; cleared once per cook.
    bool mInputsChanged = false;
    // Whether the last cook rendered (Info CHOP 'rendered').
    bool mRendered = false;

    // Edge detection
    std::unordered_map<std::string, float>       mPrevChopValues;
    std::unordered_map<std::string, std::string> mPrevDatValues;

    // Texture injection state. mPendingDl holds the async CPU download
    // started on the previous cook (1-frame latency avoids a GPU stall);
    // mBoundSlotImage tracks which RenderImage each VM property last saw so
    // we only rebind on identity change.
    TD::OP_SmartRef<TD::OP_TOPDownloadResult> mPendingDl[tdrive::kMaxImageSlots];
    rive::RenderImage*   mBoundSlotImage[tdrive::kMaxImageSlots] = {};

    // TouchDesigner can allocate the output smaller than requested (e.g. the
    // Non-Commercial 1280x1280 cap). In CUDA mode the real size is visible
    // once the array exists; remember the request->actual pair so later cooks
    // request the actual size up front instead of reallocating every frame.
    int32_t mTdClampReqW = 0, mTdClampReqH = 0;
    int32_t mTdClampW    = 0, mTdClampH    = 0;
    // What the last cook actually produced, for the Info CHOP.
    int32_t mOutW = 0, mOutH = 0;

    // CUDA-mode bracket timings (CPU wall-clock ms) for the Info CHOP.
    double mCudaBeginMs  = 0.0;
    double mCudaInjectMs = 0.0;
    double mCudaEndMs    = 0.0;

    std::string mError;
    std::string mWarning;   // cleared every cook
    void addWarning(const std::string& s)
    {
        if (!mWarning.empty()) mWarning += "\n";
        mWarning += s;
    }
};
