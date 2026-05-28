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
    void buildDynamicMenu(const TD::OP_Inputs*, TD::OP_BuildDynamicMenuInfo*, void*) override;
    bool getInfoDATSize(TD::OP_InfoDATSize*, void*) override;
    void getInfoDATEntries(int32_t index, int32_t nEntries,
                           TD::OP_InfoDATEntries* entries, void*) override;

private:
    bool loadFileIfNeeded(const char* absPath);
    bool selectArtboardIfNeeded(const char* name);
    bool selectSceneIfNeeded(const char* stateMachineName);
    void applyInputsFromCHOP(const TD::OP_CHOPInput* chop);
    void applyStringsFromDAT(const TD::OP_DATInput* dat);
    void bindArtboardViewModel();
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

    std::string mLoadedPath;
    std::string mLoadedArtboard;
    std::string mLoadedStateMachine;

    // Playback timing
    std::chrono::steady_clock::time_point mLastTick;
    bool mHasTick = false;

    // Edge detection
    std::unordered_map<std::string, float>       mPrevChopValues;
    std::unordered_map<std::string, std::string> mPrevDatValues;

    std::string mError;
};
