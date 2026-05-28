// TDRiveTOP.mm
//
// TouchDesigner TOP that renders a .riv (Rive) file via the Rive Renderer
// (PLS) on Metal, reads pixels back to CPU, and uploads them through the
// TOP CPU buffer path. Mirrors the structure of BGRemoverTOP.

#include "TOP_CPlusPlusBase.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unordered_map>

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/scene.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/linear_animation_instance.hpp"
#include "rive/layout.hpp"
#include "rive/math/aabb.hpp"
#include "rive/text/text_value_run.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_string_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_number_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_boolean_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_trigger_runtime.hpp"
#include "rive/data_bind/data_values/data_type.hpp"

#include "rive/renderer/render_context.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/metal/render_context_metal_impl.h"

using namespace TD;

namespace {

constexpr uint32_t kDefaultWidth  = 1280;
constexpr uint32_t kDefaultHeight = 720;

rive::Fit FitFromIndex(int idx)
{
    switch (idx)
    {
        case 0: return rive::Fit::contain;
        case 1: return rive::Fit::cover;
        case 2: return rive::Fit::fill;
        case 3: return rive::Fit::fitWidth;
        case 4: return rive::Fit::fitHeight;
        case 5: return rive::Fit::none;
        case 6: return rive::Fit::scaleDown;
        default: return rive::Fit::contain;
    }
}

rive::Alignment AlignmentFromIndex(int idx)
{
    switch (idx)
    {
        case 0: return rive::Alignment::topLeft;
        case 1: return rive::Alignment::topCenter;
        case 2: return rive::Alignment::topRight;
        case 3: return rive::Alignment::centerLeft;
        case 4: return rive::Alignment::center;
        case 5: return rive::Alignment::centerRight;
        case 6: return rive::Alignment::bottomLeft;
        case 7: return rive::Alignment::bottomCenter;
        case 8: return rive::Alignment::bottomRight;
        default: return rive::Alignment::center;
    }
}

} // namespace

class TDRiveTOP : public TOP_CPlusPlusBase {
public:
    TDRiveTOP(const OP_NodeInfo* info, TOP_Context* context);
    virtual ~TDRiveTOP();

    virtual void getGeneralInfo(TOP_GeneralInfo*, const OP_Inputs*, void*) override;
    virtual void execute(TOP_Output*, const OP_Inputs*, void*) override;
    virtual void setupParameters(OP_ParameterManager*, void*) override;
    virtual void pulsePressed(const char* name, void*) override;
    virtual void getErrorString(OP_String* error, void*) override;
    virtual void buildDynamicMenu(const OP_Inputs*, OP_BuildDynamicMenuInfo*, void*) override;
    virtual bool getInfoDATSize(OP_InfoDATSize*, void*) override;
    virtual void getInfoDATEntries(int32_t index, int32_t nEntries,
                                   OP_InfoDATEntries* entries, void*) override;

private:
    bool ensureMetal();
    bool ensureRenderTarget(uint32_t w, uint32_t h);
    bool loadFileIfNeeded(const char* absPath);
    bool selectArtboardIfNeeded(const char* name);
    bool selectSceneIfNeeded(const char* stateMachineName);
    void applyInputsFromCHOP(const OP_CHOPInput* chop);
    void applyStringsFromDAT(const OP_DATInput* dat);
    void bindArtboardViewModel();
    rive::StateMachineInstance* currentSMI();
    void setError(const std::string& msg) { mError = msg; }
    void clearError() { mError.clear(); }

    TOP_Context* mContext = nullptr;

    // Metal
    id<MTLDevice>       mDevice  = nil;
    id<MTLCommandQueue> mQueue   = nil;
    id<MTLTexture>      mTexture = nil;     // BGRA8Unorm, RenderTarget+ShaderRead
    id<MTLBuffer>       mReadBuf = nil;     // pixel readback buffer
    uint32_t            mTexW = 0;
    uint32_t            mTexH = 0;

    // Rive
    std::unique_ptr<rive::gpu::RenderContext>   mRenderContext;
    rive::rcp<rive::gpu::RenderTargetMetal>     mRenderTarget;
    rive::rcp<rive::File>                       mFile;
    std::unique_ptr<rive::ArtboardInstance>     mArtboard;
    std::unique_ptr<rive::Scene>                mScene;
    // Non-owning - points at mScene when it's a state machine, else nullptr.
    rive::StateMachineInstance*                 mSMI = nullptr;
    // Default view model bound to the artboard, if any.
    rive::rcp<rive::ViewModelInstanceRuntime>   mVMRuntime;

    std::string mLoadedPath;
    std::string mLoadedArtboard;
    std::string mLoadedStateMachine;

    // Playback
    std::chrono::steady_clock::time_point mLastTick;
    bool mHasTick = false;

    // For trigger edge detection: last value of each CHOP channel we read.
    std::unordered_map<std::string, float> mPrevChopValues;
    // For VM-trigger edge detection from the Strings DAT.
    std::unordered_map<std::string, std::string> mPrevDatValues;

    std::string mError;
};

// SMI input type keys from rive/generated/animation/state_machine_*_base.hpp.
// Used to identify input kind without crossing typeinfo across hidden visibility.
static constexpr uint16_t kInputTypeNumber  = 56;
static constexpr uint16_t kInputTypeTrigger = 58;
static constexpr uint16_t kInputTypeBool    = 59;

static const char* InputTypeName(uint16_t t)
{
    switch (t) {
        case kInputTypeNumber:  return "number";
        case kInputTypeTrigger: return "trigger";
        case kInputTypeBool:    return "bool";
        default:                return "unknown";
    }
}

// =============================================================================
// Plugin entry points
// =============================================================================

// DLLEXPORT is a no-op on macOS in CPlusPlus_Common.hpp. We build the plugin
// with -fvisibility=hidden, so the three required entry points need an
// explicit default-visibility attribute or TouchDesigner's loader can't find
// them and reports "Plugin is missing required exported C functions".
#define TD_EXPORT __attribute__((visibility("default")))

extern "C" {

TD_EXPORT DLLEXPORT void FillTOPPluginInfo(TOP_PluginInfo* info)
{
    info->apiVersion = TOPCPlusPlusAPIVersion;
    info->executeMode = TOP_ExecuteMode::CPUMem;

    auto& custom = info->customOPInfo;
    custom.opType->setString("Rive");
    custom.opLabel->setString("Rive");
    custom.opIcon->setString("RIV");
    custom.authorName->setString("Evan Clark");
    custom.authorEmail->setString("you@example.com");
    custom.minInputs = 0;
    custom.maxInputs = 0;
}

TD_EXPORT DLLEXPORT TOP_CPlusPlusBase* CreateTOPInstance(const OP_NodeInfo* info, TOP_Context* context)
{
    return new TDRiveTOP(info, context);
}

TD_EXPORT DLLEXPORT void DestroyTOPInstance(TOP_CPlusPlusBase* instance, TOP_Context*)
{
    delete (TDRiveTOP*)instance;
}

} // extern "C"

// =============================================================================
// Lifecycle
// =============================================================================

TDRiveTOP::TDRiveTOP(const OP_NodeInfo* /*info*/, TOP_Context* context)
    : mContext(context)
{
}

TDRiveTOP::~TDRiveTOP()
{
    @autoreleasepool {
        mVMRuntime.reset();
        mSMI = nullptr;
        mScene.reset();
        mArtboard.reset();
        mFile.reset();
        mRenderTarget.reset();
        if (mRenderContext) {
            mRenderContext->releaseResources();
            mRenderContext.reset();
        }
        mTexture = nil;
        mReadBuf = nil;
        mQueue   = nil;
        mDevice  = nil;
    }
}

void TDRiveTOP::getGeneralInfo(TOP_GeneralInfo* ginfo, const OP_Inputs*, void*)
{
    ginfo->cookEveryFrame = true;
}

// =============================================================================
// Parameters
// =============================================================================

void TDRiveTOP::setupParameters(OP_ParameterManager* m, void*)
{
    {
        OP_StringParameter sp("File");
        sp.label = "Riv File";
        sp.page  = "Rive";
        sp.defaultValue = "";
        m->appendFile(sp);
    }
    {
        OP_NumericParameter np("Reload");
        np.label = "Reload";
        np.page  = "Rive";
        m->appendPulse(np);
    }
    {
        OP_StringParameter sp("Artboard");
        sp.label = "Artboard";
        sp.page  = "Rive";
        sp.defaultValue = "";
        m->appendDynamicStringMenu(sp);
    }
    {
        OP_StringParameter sp("Statemachine");
        sp.label = "State Machine";
        sp.page  = "Rive";
        sp.defaultValue = "";
        m->appendDynamicStringMenu(sp);
    }
    {
        OP_StringParameter sp("Inputs");
        sp.label = "Inputs CHOP";
        sp.page  = "Rive";
        sp.defaultValue = "";
        m->appendCHOP(sp);
    }
    {
        OP_StringParameter sp("Strings");
        sp.label = "Strings DAT";
        sp.page  = "Rive";
        sp.defaultValue = "";
        m->appendDAT(sp);
    }
    {
        OP_StringParameter sp("Fit");
        sp.label = "Fit";
        sp.page  = "Rive";
        sp.defaultValue = "contain";
        const char* names[]  = {"contain","cover","fill","fitwidth","fitheight","none","scaledown"};
        const char* labels[] = {"Contain","Cover","Fill","Fit Width","Fit Height","None","Scale Down"};
        m->appendMenu(sp, 7, names, labels);
    }
    {
        OP_StringParameter sp("Alignment");
        sp.label = "Alignment";
        sp.page  = "Rive";
        sp.defaultValue = "center";
        const char* names[]  = {"topleft","topcenter","topright",
                                "centerleft","center","centerright",
                                "bottomleft","bottomcenter","bottomright"};
        const char* labels[] = {"Top Left","Top Center","Top Right",
                                "Center Left","Center","Center Right",
                                "Bottom Left","Bottom Center","Bottom Right"};
        m->appendMenu(sp, 9, names, labels);
    }
    {
        OP_NumericParameter np("Speed");
        np.label = "Speed";
        np.page  = "Rive";
        np.defaultValues[0] = 1.0;
        np.minSliders[0] = 0.0;
        np.maxSliders[0] = 4.0;
        m->appendFloat(np);
    }
    {
        OP_NumericParameter np("Bgcolor");
        np.label = "Background Color";
        np.page  = "Rive";
        np.defaultValues[0] = 0.0; np.minSliders[0] = 0.0; np.maxSliders[0] = 1.0;
        np.defaultValues[1] = 0.0; np.minSliders[1] = 0.0; np.maxSliders[1] = 1.0;
        np.defaultValues[2] = 0.0; np.minSliders[2] = 0.0; np.maxSliders[2] = 1.0;
        np.defaultValues[3] = 0.0; np.minSliders[3] = 0.0; np.maxSliders[3] = 1.0;
        m->appendRGBA(np);
    }
    {
        OP_NumericParameter np("Resolution");
        np.label = "Resolution";
        np.page  = "Rive";
        np.defaultValues[0] = (double)kDefaultWidth;
        np.defaultValues[1] = (double)kDefaultHeight;
        np.minSliders[0] = 16;   np.maxSliders[0] = 4096;
        np.minSliders[1] = 16;   np.maxSliders[1] = 4096;
        m->appendWH(np);
    }
}

void TDRiveTOP::pulsePressed(const char* name, void*)
{
    if (name && std::string(name) == "Reload") {
        mLoadedPath.clear();
        mLoadedArtboard.clear();
        mLoadedStateMachine.clear();
        mVMRuntime.reset();
        mSMI = nullptr;
        mScene.reset();
        mArtboard.reset();
        mFile.reset();
        mPrevChopValues.clear();
        mPrevDatValues.clear();
    }
}

void TDRiveTOP::getErrorString(OP_String* err, void*)
{
    if (!mError.empty()) {
        err->setString(mError.c_str());
    }
}

// =============================================================================
// Dynamic menus (Artboard, State Machine)
// =============================================================================

void TDRiveTOP::buildDynamicMenu(const OP_Inputs* inputs,
                                 OP_BuildDynamicMenuInfo* info,
                                 void*)
{
    // Make sure Rive is up and the file is parsed before populating menus.
    if (!ensureMetal()) return;

    const char* path = inputs->getParFilePath("File");
    if (!loadFileIfNeeded(path)) {
        return; // no file - leave menu empty
    }

    std::string name = info->name ? info->name : "";

    if (name == "Artboard") {
        size_t n = mFile->artboardCount();
        for (size_t i = 0; i < n; ++i) {
            std::string ab = mFile->artboardNameAt(i);
            info->addMenuEntry(ab.c_str(), ab.c_str());
        }
        return;
    }

    if (name == "Statemachine") {
        // Use whatever artboard is currently selected via the Artboard menu,
        // falling back to the default artboard.
        const char* abName = inputs->getParString("Artboard");
        rive::Artboard* ab = nullptr;
        if (abName && *abName) {
            ab = mFile->artboard(std::string(abName));
        }
        if (!ab) ab = mFile->artboard(); // default

        if (!ab) return;
        size_t n = ab->stateMachineCount();
        for (size_t i = 0; i < n; ++i) {
            rive::StateMachine* sm = ab->stateMachine(i);
            if (!sm) continue;
            std::string smn = sm->name();
            info->addMenuEntry(smn.c_str(), smn.c_str());
        }
        return;
    }
}

// =============================================================================
// Info DAT - exposes the active state machine's inputs (index, name, type,
// current value) so users can see what to wire to the Inputs CHOP.
// =============================================================================

// Map view-model DataType to a short string for display in the Info DAT.
static const char* DataTypeName(rive::DataType t)
{
    using rive::DataType;
    switch (t) {
        case DataType::string:    return "vm:string";
        case DataType::number:    return "vm:number";
        case DataType::boolean:   return "vm:bool";
        case DataType::trigger:   return "vm:trigger";
        case DataType::color:     return "vm:color";
        case DataType::enumType:  return "vm:enum";
        case DataType::integer:   return "vm:integer";
        case DataType::list:      return "vm:list";
        case DataType::viewModel: return "vm:viewModel";
        case DataType::artboard:  return "vm:artboard";
        default:                  return "vm:?";
    }
}

bool TDRiveTOP::getInfoDATSize(OP_InfoDATSize* size, void*)
{
    auto* smi = currentSMI();
    int32_t smiRows = smi ? (int32_t)smi->inputCount() : 0;
    int32_t vmRows  = mVMRuntime ? (int32_t)mVMRuntime->propertyCount() : 0;
    size->cols = 4;
    size->rows = 1 + smiRows + vmRows; // +1 header
    size->byColumn = false;
    return true;
}

void TDRiveTOP::getInfoDATEntries(int32_t row, int32_t /*nEntries*/,
                                  OP_InfoDATEntries* entries, void*)
{
    if (row == 0) {
        entries->values[0]->setString("index");
        entries->values[1]->setString("name");
        entries->values[2]->setString("type");
        entries->values[3]->setString("value");
        return;
    }

    auto* smi = currentSMI();
    int32_t smiRows = smi ? (int32_t)smi->inputCount() : 0;
    int32_t k = row - 1;
    char buf[64];

    if (k < smiRows) {
        auto* in = smi->input((size_t)k);
        std::snprintf(buf, sizeof(buf), "%d", k);
        entries->values[0]->setString(buf);
        entries->values[1]->setString(in->name().c_str());
        entries->values[2]->setString(InputTypeName(in->inputCoreType()));

        switch (in->inputCoreType()) {
            case kInputTypeNumber: {
                auto* n = static_cast<rive::SMINumber*>(in);
                std::snprintf(buf, sizeof(buf), "%g", n->value());
                entries->values[3]->setString(buf);
                break;
            }
            case kInputTypeBool: {
                auto* b = static_cast<rive::SMIBool*>(in);
                entries->values[3]->setString(b->value() ? "1" : "0");
                break;
            }
            case kInputTypeTrigger:
                entries->values[3]->setString("(pulse)");
                break;
            default:
                entries->values[3]->setString("");
                break;
        }
        return;
    }

    // VM property rows (index relative to the view model).
    if (!mVMRuntime) return;
    int32_t vk = k - smiRows;
    auto props = mVMRuntime->properties();
    if (vk < 0 || (size_t)vk >= props.size()) return;
    const auto& p = props[(size_t)vk];

    std::snprintf(buf, sizeof(buf), "%d", vk);
    entries->values[0]->setString(buf);
    entries->values[1]->setString(p.name.c_str());
    entries->values[2]->setString(DataTypeName(p.type));

    switch (p.type) {
        case rive::DataType::string: {
            auto* sp = mVMRuntime->propertyString(p.name);
            entries->values[3]->setString(sp ? sp->value().c_str() : "");
            break;
        }
        case rive::DataType::number: {
            auto* np = mVMRuntime->propertyNumber(p.name);
            if (np) {
                std::snprintf(buf, sizeof(buf), "%g", np->value());
                entries->values[3]->setString(buf);
            } else {
                entries->values[3]->setString("");
            }
            break;
        }
        case rive::DataType::boolean: {
            auto* bp = mVMRuntime->propertyBoolean(p.name);
            entries->values[3]->setString(bp ? (bp->value() ? "1" : "0") : "");
            break;
        }
        case rive::DataType::trigger:
            entries->values[3]->setString("(pulse)");
            break;
        default:
            entries->values[3]->setString("");
            break;
    }
}

rive::StateMachineInstance* TDRiveTOP::currentSMI()
{
    return mSMI;
}

// =============================================================================
// CHOP -> state machine inputs
// =============================================================================

void TDRiveTOP::applyInputsFromCHOP(const OP_CHOPInput* chop)
{
    auto* smi = currentSMI();
    if (!smi || !chop || chop->numChannels <= 0 || chop->numSamples <= 0) return;

    // Index inputs by name for O(1) lookup.
    std::unordered_map<std::string, rive::SMIInput*> byName;
    byName.reserve(smi->inputCount());
    for (size_t i = 0; i < smi->inputCount(); ++i) {
        auto* in = smi->input(i);
        byName.emplace(in->name(), in);
    }

    for (int32_t c = 0; c < chop->numChannels; ++c) {
        const char* cn = chop->getChannelName(c);
        if (!cn) continue;
        auto it = byName.find(std::string(cn));
        if (it == byName.end()) continue;

        // Last sample is the "current" value.
        float v = chop->getChannelData(c)[chop->numSamples - 1];

        switch (it->second->inputCoreType()) {
            case kInputTypeNumber: {
                auto* n = static_cast<rive::SMINumber*>(it->second);
                if (n->value() != v) n->value(v);
                break;
            }
            case kInputTypeBool: {
                auto* b = static_cast<rive::SMIBool*>(it->second);
                bool nb = v != 0.0f;
                if (b->value() != nb) b->value(nb);
                break;
            }
            case kInputTypeTrigger: {
                // Edge-detect a rising edge from <=0 to >0.
                auto pit = mPrevChopValues.find(cn);
                float prev = (pit == mPrevChopValues.end()) ? 0.0f : pit->second;
                if (prev <= 0.0f && v > 0.0f) {
                    static_cast<rive::SMITrigger*>(it->second)->fire();
                }
                break;
            }
            default: break;
        }
        mPrevChopValues[cn] = v;
    }
}

// =============================================================================
// View-model binding + DAT-driven strings/numbers/bools/triggers
// =============================================================================

void TDRiveTOP::bindArtboardViewModel()
{
    mVMRuntime.reset();
    if (!mFile || !mArtboard) return;

    // Default view model attached to this artboard in the Rive editor.
    auto* vmr = mFile->defaultArtboardViewModel(mArtboard.get());
    if (!vmr) return;

    auto runtime = vmr->createDefaultInstance();
    if (!runtime) {
        runtime = vmr->createInstance();
    }
    if (!runtime) return;

    // Bind the underlying ViewModelInstance to the artboard so data-bound
    // properties (text runs etc.) update when we mutate the runtime view.
    mArtboard->bindViewModelInstance(runtime->instance());
    mVMRuntime = std::move(runtime);
}

static bool dat_value_truthy(const std::string& s)
{
    if (s.empty()) return false;
    // Common "on" tokens.
    if (s == "1" || s == "true" || s == "True" || s == "TRUE" ||
        s == "fire" || s == "on" || s == "On" || s == "yes") return true;
    // Numeric > 0.
    try { return std::stof(s) > 0.0f; } catch (...) { return false; }
}

void TDRiveTOP::applyStringsFromDAT(const OP_DATInput* dat)
{
    if (!dat || dat->numRows <= 0 || dat->numCols < 2) return;

    int32_t startRow = 0;
    // Skip a "name,value" / "key,value" header row if present.
    if (dat->numRows > 0) {
        const char* c0 = dat->getCell(0, 0);
        if (c0 && (!std::strcmp(c0, "name") || !std::strcmp(c0, "Name") ||
                   !std::strcmp(c0, "key")  || !std::strcmp(c0, "Key"))) {
            startRow = 1;
        }
    }

    for (int32_t r = startRow; r < dat->numRows; ++r) {
        const char* nameC  = dat->getCell(r, 0);
        const char* valueC = dat->getCell(r, 1);
        if (!nameC || !*nameC) continue;
        std::string name  = nameC;
        std::string value = valueC ? valueC : "";

        bool handled = false;

        // 1. View model properties take precedence.
        if (mVMRuntime) {
            if (auto* sp = mVMRuntime->propertyString(name)) {
                if (sp->value() != value) sp->value(value);
                handled = true;
            } else if (auto* np = mVMRuntime->propertyNumber(name)) {
                try {
                    float v = std::stof(value);
                    if (np->value() != v) np->value(v);
                } catch (...) {}
                handled = true;
            } else if (auto* bp = mVMRuntime->propertyBoolean(name)) {
                bool nb = dat_value_truthy(value);
                if (bp->value() != nb) bp->value(nb);
                handled = true;
            } else if (auto* tp = mVMRuntime->propertyTrigger(name)) {
                // Edge-detect: fire when the cell content changes AND is truthy.
                auto pit = mPrevDatValues.find(name);
                bool changed = (pit == mPrevDatValues.end()) || (pit->second != value);
                if (changed && dat_value_truthy(value)) {
                    tp->trigger();
                }
                handled = true;
            }
        }

        // 2. Fallback: artboard text run with that name.
        if (!handled && mArtboard) {
            if (auto* tvr = mArtboard->getTextRun(name, "")) {
                if (tvr->text() != value) tvr->text(value);
                handled = true;
            }
        }

        mPrevDatValues[name] = value;
    }
}

// =============================================================================
// Metal / Rive setup
// =============================================================================

bool TDRiveTOP::ensureMetal()
{
    if (mRenderContext) return true;

    @autoreleasepool {
        mDevice = MTLCreateSystemDefaultDevice();
        if (!mDevice) {
            setError("No Metal device available.");
            return false;
        }
        mQueue = [mDevice newCommandQueue];

        rive::gpu::RenderContextMetalImpl::ContextOptions opts;
        // Default options work for offscreen rendering.
        mRenderContext = rive::gpu::RenderContextMetalImpl::MakeContext(mDevice, opts);
        if (!mRenderContext) {
            setError("Failed to create Rive Metal render context.");
            mQueue = nil; mDevice = nil;
            return false;
        }
    }
    return true;
}

bool TDRiveTOP::ensureRenderTarget(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0) return false;
    if (mTexture && mTexW == w && mTexH == h && mRenderTarget) return true;

    @autoreleasepool {
        MTLTextureDescriptor* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:w
                                        height:h
                                     mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModePrivate;
        mTexture = [mDevice newTextureWithDescriptor:desc];
        if (!mTexture) {
            setError("Failed to allocate offscreen MTLTexture.");
            return false;
        }

        auto* impl = mRenderContext->static_impl_cast<rive::gpu::RenderContextMetalImpl>();
        mRenderTarget = impl->makeRenderTarget(MTLPixelFormatBGRA8Unorm, w, h);

        // Shared storage so we can memcpy directly out of contents().
        NSUInteger bytes = (NSUInteger)w * (NSUInteger)h * 4;
        mReadBuf = [mDevice newBufferWithLength:bytes
                                        options:MTLResourceStorageModeShared];

        mTexW = w;
        mTexH = h;
    }
    return true;
}

bool TDRiveTOP::loadFileIfNeeded(const char* absPath)
{
    std::string path = absPath ? absPath : "";
    if (path.empty()) {
        if (mFile) {
            mFile.reset();
            mArtboard.reset();
            mScene.reset();
            mSMI = nullptr;
            mLoadedPath.clear();
            mLoadedArtboard.clear();
            mLoadedStateMachine.clear();
            mPrevChopValues.clear();
        }
        return false;
    }
    if (path == mLoadedPath && mFile) return true;

    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        setError("Could not open .riv file: " + path);
        mFile.reset();
        mArtboard.reset();
        mScene.reset();
        mSMI = nullptr;
        mVMRuntime.reset();
        mLoadedPath.clear();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());

    rive::ImportResult ir;
    rive::rcp<rive::File> file = rive::File::import(
        rive::Span<const uint8_t>(bytes.data(), bytes.size()),
        mRenderContext.get(),
        &ir);
    if (!file || ir != rive::ImportResult::success) {
        setError("Failed to parse .riv: " + path);
        mFile.reset();
        mArtboard.reset();
        mScene.reset();
        mSMI = nullptr;
        mVMRuntime.reset();
        mLoadedPath.clear();
        return false;
    }
    mFile = std::move(file);
    mArtboard.reset();
    mScene.reset();
    mSMI = nullptr;
    mVMRuntime.reset();
    mPrevChopValues.clear();
    mPrevDatValues.clear();
    mLoadedPath = path;
    mLoadedArtboard.clear();
    mLoadedStateMachine.clear();
    clearError();
    return true;
}

bool TDRiveTOP::selectArtboardIfNeeded(const char* nameC)
{
    if (!mFile) return false;
    std::string name = nameC ? nameC : "";
    if (mArtboard && name == mLoadedArtboard) return true;

    std::unique_ptr<rive::ArtboardInstance> ab;
    if (name.empty()) {
        ab = mFile->artboardDefault();
    } else {
        ab = mFile->artboardNamed(name);
    }
    if (!ab) {
        setError(name.empty()
                 ? std::string("No default artboard in file.")
                 : std::string("Artboard not found: ") + name);
        mArtboard.reset();
        mScene.reset();
        mSMI = nullptr;
        return false;
    }
    mArtboard = std::move(ab);
    mLoadedArtboard = name;
    mSMI = nullptr;
    mPrevChopValues.clear();
    mPrevDatValues.clear();
    bindArtboardViewModel();
    mScene.reset();
    mLoadedStateMachine.clear();
    clearError();
    return true;
}

bool TDRiveTOP::selectSceneIfNeeded(const char* smC)
{
    if (!mArtboard) return false;
    std::string sm = smC ? smC : "";
    if (mScene && sm == mLoadedStateMachine) return true;

    // Track whether the resulting Scene is a state machine so we can expose
    // its inputs without relying on dynamic_cast (hidden typeinfo).
    std::unique_ptr<rive::StateMachineInstance> smiOwned;
    std::unique_ptr<rive::Scene> scene;

    if (!sm.empty()) {
        smiOwned = mArtboard->stateMachineNamed(sm);
        if (!smiOwned) {
            setError(std::string("State machine not found: ") + sm);
            return false;
        }
        scene = std::move(smiOwned);
        // After move, scene points at the SMI - keep a non-owning back-pointer.
        mSMI = static_cast<rive::StateMachineInstance*>(scene.get());
    } else {
        // Prefer the default state machine for input introspection. Only fall
        // back to defaultScene() (which may give a LinearAnimationInstance)
        // when there is no state machine at all.
        if (mArtboard->stateMachineCount() > 0) {
            int defIdx = mArtboard->defaultStateMachineIndex();
            size_t idx = (defIdx >= 0) ? (size_t)defIdx : 0;
            smiOwned = mArtboard->stateMachineAt(idx);
            if (smiOwned) {
                scene = std::move(smiOwned);
                mSMI = static_cast<rive::StateMachineInstance*>(scene.get());
            }
        }
        if (!scene) {
            scene = mArtboard->defaultScene();
            mSMI = nullptr;
        }
    }

    mScene = std::move(scene);
    mLoadedStateMachine = sm;
    mPrevChopValues.clear();
    clearError();
    return true;
}

// =============================================================================
// Execute
// =============================================================================

void TDRiveTOP::execute(TOP_Output* output, const OP_Inputs* inputs, void*)
{
    if (!ensureMetal()) return;

    // Resolution
    int32_t resW = 0, resH = 0;
    inputs->getParInt2("Resolution", resW, resH);
    if (resW <= 0) resW = kDefaultWidth;
    if (resH <= 0) resH = kDefaultHeight;
    if (!ensureRenderTarget((uint32_t)resW, (uint32_t)resH)) return;

    // Parameters
    const char* filePath = inputs->getParFilePath("File");
    const char* artboard = inputs->getParString("Artboard");
    const char* stateMch = inputs->getParString("Statemachine");
    int fitIdx   = inputs->getParInt("Fit");
    int alignIdx = inputs->getParInt("Alignment");
    double speed = inputs->getParDouble("Speed");
    double bg[4] = {0,0,0,0};
    inputs->getParDouble4("Bgcolor", bg[0], bg[1], bg[2], bg[3]);

    bool ok = loadFileIfNeeded(filePath);
    if (ok) ok = selectArtboardIfNeeded(artboard);
    if (ok) ok = selectSceneIfNeeded(stateMch);

    // Push parameter CHOP values into state machine inputs before advancing.
    if (ok && currentSMI()) {
        const OP_CHOPInput* inputsChop = inputs->getParCHOP("Inputs");
        if (inputsChop) applyInputsFromCHOP(inputsChop);
    }

    // Push DAT-driven strings/numbers/bools/triggers into the view model or
    // (fallback) named text runs on the artboard.
    if (ok) {
        const OP_DATInput* stringsDat = inputs->getParDAT("Strings");
        if (stringsDat) applyStringsFromDAT(stringsDat);
    }

    // Compute dt for advance().
    auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (mHasTick) {
        dt = std::chrono::duration<float>(now - mLastTick).count();
        if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
    }
    mLastTick = now;
    mHasTick = true;
    dt *= (float)speed;

    if (ok && mScene) {
        mScene->advanceAndApply(dt);
    } else if (ok && mArtboard) {
        mArtboard->advance(dt);
    }

    // Render
    @autoreleasepool {
        // Attach the offscreen texture to the Rive render target.
        mRenderTarget->setTargetTexture(mTexture);

        // Build the Rive frame.
        rive::gpu::RenderContext::FrameDescriptor fd;
        fd.renderTargetWidth  = mTexW;
        fd.renderTargetHeight = mTexH;
        fd.loadAction = rive::gpu::LoadAction::clear;
        uint8_t r8 = (uint8_t)(std::max(0.0, std::min(1.0, bg[0])) * 255.0 + 0.5);
        uint8_t g8 = (uint8_t)(std::max(0.0, std::min(1.0, bg[1])) * 255.0 + 0.5);
        uint8_t b8 = (uint8_t)(std::max(0.0, std::min(1.0, bg[2])) * 255.0 + 0.5);
        uint8_t a8 = (uint8_t)(std::max(0.0, std::min(1.0, bg[3])) * 255.0 + 0.5);
        // ColorInt = 0xAARRGGBB
        fd.clearColor = ((uint32_t)a8 << 24) | ((uint32_t)r8 << 16)
                      | ((uint32_t)g8 <<  8) |  (uint32_t)b8;
        mRenderContext->beginFrame(fd);

        if (ok && mArtboard) {
            auto renderer = std::make_unique<rive::RiveRenderer>(mRenderContext.get());
            renderer->save();
            renderer->align(FitFromIndex(fitIdx),
                            AlignmentFromIndex(alignIdx),
                            rive::AABB(0, 0, (float)mTexW, (float)mTexH),
                            mArtboard->bounds());
            mArtboard->draw(renderer.get());
            renderer->restore();
        }

        id<MTLCommandBuffer> riveCb = [mQueue commandBuffer];
        rive::gpu::RenderContext::FlushResources flush;
        flush.renderTarget = mRenderTarget.get();
        flush.externalCommandBuffer = (__bridge void*)riveCb;
        mRenderContext->flush(flush);
        [riveCb commit];

        // Blit Metal texture into the shared CPU buffer.
        id<MTLCommandBuffer> blitCb = [mQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [blitCb blitCommandEncoder];
        [blit copyFromTexture:mTexture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(mTexW, mTexH, 1)
                     toBuffer:mReadBuf
            destinationOffset:0
       destinationBytesPerRow:mTexW * 4
     destinationBytesPerImage:mTexW * mTexH * 4];
        [blit endEncoding];
        [blitCb commit];
        [blitCb waitUntilCompleted];

        // Upload to TouchDesigner.
        const uint64_t byteSize = (uint64_t)mTexW * (uint64_t)mTexH * 4;
        TD::OP_SmartRef<TD::TOP_Buffer> buf =
            mContext->createOutputBuffer(byteSize, TD::TOP_BufferFlags::None, nullptr);
        std::memcpy(buf->data, [mReadBuf contents], (size_t)byteSize);

        TD::TOP_UploadInfo up;
        up.textureDesc.width  = mTexW;
        up.textureDesc.height = mTexH;
        up.textureDesc.depth  = 1;
        up.textureDesc.texDim = TD::OP_TexDim::e2D;
        up.textureDesc.pixelFormat = TD::OP_PixelFormat::BGRA8Fixed;
        // Rive renders into Metal with origin at top-left; TD expects bottom-left
        // unless we say otherwise.
        up.firstPixel = TD::TOP_FirstPixel::TopLeft;
        up.colorBufferIndex = 0;
        output->uploadBuffer(&buf, up, nullptr);

        mRenderTarget->setTargetTexture(nil);
    }
}
