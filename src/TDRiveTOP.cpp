// TDRiveTOP.cpp
//
// Cross-platform implementation. All GPU work goes through IBackend.

#include "TDRiveTOP.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "rive/artboard.hpp"
#include "rive/file.hpp"
#include "rive/scene.hpp"
#include "rive/layout.hpp"
#include "rive/math/aabb.hpp"
#include "rive/animation/linear_animation_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/text/text_value_run.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_string_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_number_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_boolean_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_trigger_runtime.hpp"
#include "rive/data_bind/data_values/data_type.hpp"
#include "rive/renderer/rive_renderer.hpp"

#if defined(_WIN32)
#include "cuda_interop_win.h"
#endif

using namespace TD;

// =============================================================================
// Local helpers
// =============================================================================

namespace {

constexpr uint32_t kDefaultWidth  = 1280;
constexpr uint32_t kDefaultHeight = 720;

// SMI input type keys from rive/generated/animation/state_machine_*_base.hpp.
constexpr uint16_t kInputTypeNumber  = 56;
constexpr uint16_t kInputTypeTrigger = 58;
constexpr uint16_t kInputTypeBool    = 59;

const char* InputTypeName(uint16_t t)
{
    switch (t) {
        case kInputTypeNumber:  return "number";
        case kInputTypeTrigger: return "trigger";
        case kInputTypeBool:    return "bool";
        default:                return "unknown";
    }
}

const char* DataTypeName(rive::DataType t)
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
        case DataType::assetImage: return "vm:image";
        case DataType::assetFont:  return "vm:font";
        default:                  return "vm:?";
    }
}

rive::Fit FitFromIndex(int idx)
{
    switch (idx) {
        case 0: return rive::Fit::contain;
        case 1: return rive::Fit::cover;
        case 2: return rive::Fit::fill;
        case 3: return rive::Fit::fitWidth;
        case 4: return rive::Fit::fitHeight;
        case 5: return rive::Fit::none;
        case 6: return rive::Fit::scaleDown;
        case 7: return rive::Fit::layout;
        default: return rive::Fit::contain;
    }
}

rive::Alignment AlignmentFromIndex(int idx)
{
    switch (idx) {
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

bool dat_value_truthy(const std::string& s)
{
    if (s.empty()) return false;
    if (s == "1" || s == "true" || s == "True" || s == "TRUE" ||
        s == "fire" || s == "on" || s == "On" || s == "yes") return true;
    try { return std::stof(s) > 0.0f; } catch (...) { return false; }
}

// Rive samples images as premultiplied alpha (its decoders premultiply on
// load), so injected straight-alpha pixels from TD get premultiplied here.
// Fully-opaque pixels are left untouched.
void premultiply_rgba(uint8_t* p, size_t pixelCount)
{
    for (size_t i = 0; i < pixelCount; ++i, p += 4) {
        const uint32_t a = p[3];
        if (a == 255) continue;
        p[0] = (uint8_t)((p[0] * a + 127) / 255);
        p[1] = (uint8_t)((p[1] * a + 127) / 255);
        p[2] = (uint8_t)((p[2] * a + 127) / 255);
    }
}

// True when the plugin registered with TOP_ExecuteMode::CUDA (decided once
// at DLL load in FillTOPPluginInfo; Windows + NVIDIA only).
bool gCUDAMode = false;

} // namespace

// =============================================================================
// Lifecycle
// =============================================================================

TDRiveTOP::TDRiveTOP(const OP_NodeInfo* /*info*/, TOP_Context* context)
    : mContext(context)
{
    mBackend = tdrive::CreateBackend(gCUDAMode);
}

TDRiveTOP::~TDRiveTOP()
{
    mVMRuntime.reset();
    mSMI = nullptr;
    mScene.reset();
    mArtboard.reset();
    mFile.reset();
    mBackend.reset();
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
        const char* names[]  = {"contain","cover","fill","fitwidth","fitheight","none","scaledown","layout"};
        const char* labels[] = {"Contain","Cover","Fill","Fit Width","Fit Height","None","Scale Down","Layout"};
        m->appendMenu(sp, 8, names, labels);
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
        OP_NumericParameter np("Resolution");
        np.label = "Resolution";
        np.page  = "Rive";
        np.defaultValues[0] = (double)kDefaultWidth;
        np.defaultValues[1] = (double)kDefaultHeight;
        np.minSliders[0] = 16;  np.maxSliders[0] = 4096;
        np.minSliders[1] = 16;  np.maxSliders[1] = 4096;
        m->appendWH(np);
    }
    // Texture injection: each slot pairs a source TOP with the name of the
    // view-model image property it drives (see the Info DAT for the "vm:image"
    // properties the loaded .riv exposes).
    for (int i = 0; i < tdrive::kMaxImageSlots; ++i) {
        char nameBuf[16], labelBuf[32];
        {
            std::snprintf(nameBuf, sizeof(nameBuf), "Image%d", i + 1);
            std::snprintf(labelBuf, sizeof(labelBuf), "Image %d TOP", i + 1);
            OP_StringParameter sp(nameBuf);
            sp.label = labelBuf;
            sp.page  = "Textures";
            sp.defaultValue = "";
            m->appendTOP(sp);
        }
        {
            std::snprintf(nameBuf, sizeof(nameBuf), "Imageprop%d", i + 1);
            std::snprintf(labelBuf, sizeof(labelBuf), "Image %d Property", i + 1);
            OP_StringParameter sp(nameBuf);
            sp.label = labelBuf;
            sp.page  = "Textures";
            sp.defaultValue = "";
            m->appendString(sp);
        }
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
    if (!mError.empty()) err->setString(mError.c_str());
}

// =============================================================================
// Dynamic menus
// =============================================================================

void TDRiveTOP::buildDynamicMenu(const OP_Inputs* inputs,
                                 OP_BuildDynamicMenuInfo* info, void*)
{
    if (!mBackendReady) {
        std::string err;
        if (mBackend && mBackend->init(err)) {
            mBackendReady = true;
        } else {
            if (!err.empty()) setError(err);
            return;
        }
    }

    const char* path = inputs->getParFilePath("File");
    if (!loadFileIfNeeded(path)) return;

    std::string name = info->name ? info->name : "";

    if (name == "Artboard") {
        for (size_t i = 0; i < mFile->artboardCount(); ++i) {
            std::string ab = mFile->artboardNameAt(i);
            info->addMenuEntry(ab.c_str(), ab.c_str());
        }
        return;
    }

    if (name == "Statemachine") {
        const char* abName = inputs->getParString("Artboard");
        rive::Artboard* ab = nullptr;
        if (abName && *abName) ab = mFile->artboard(std::string(abName));
        if (!ab) ab = mFile->artboard();
        if (!ab) return;
        for (size_t i = 0; i < ab->stateMachineCount(); ++i) {
            rive::StateMachine* sm = ab->stateMachine(i);
            if (!sm) continue;
            std::string smn = sm->name();
            info->addMenuEntry(smn.c_str(), smn.c_str());
        }
    }
}

// =============================================================================
// Info DAT
// =============================================================================

bool TDRiveTOP::getInfoDATSize(OP_InfoDATSize* size, void*)
{
    auto* smi = currentSMI();
    int32_t smiRows = smi ? (int32_t)smi->inputCount() : 0;
    int32_t vmRows  = mVMRuntime ? (int32_t)mVMRuntime->propertyCount() : 0;
    size->cols = 4;
    size->rows = 1 + smiRows + vmRows;
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
            if (np) { std::snprintf(buf, sizeof(buf), "%g", np->value()); entries->values[3]->setString(buf); }
            else    { entries->values[3]->setString(""); }
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

// =============================================================================
// File / artboard / scene loading
// =============================================================================

bool TDRiveTOP::loadFileIfNeeded(const char* absPath)
{
    std::string path = absPath ? absPath : "";
    if (path.empty()) {
        if (mFile) {
            mFile.reset(); mArtboard.reset(); mScene.reset();
            mSMI = nullptr; mVMRuntime.reset();
            mLoadedPath.clear(); mLoadedArtboard.clear(); mLoadedStateMachine.clear();
            mPrevChopValues.clear(); mPrevDatValues.clear();
        }
        return false;
    }
    if (path == mLoadedPath && mFile) return true;

    std::ifstream f(path, std::ios::binary);
    if (!f.good()) {
        setError("Could not open .riv file: " + path);
        mFile.reset(); mArtboard.reset(); mScene.reset();
        mSMI = nullptr; mVMRuntime.reset();
        mLoadedPath.clear();
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());

    rive::ImportResult ir;
    auto file = rive::File::import(
        rive::Span<const uint8_t>(bytes.data(), bytes.size()),
        mBackend->factory(), &ir);
    if (!file || ir != rive::ImportResult::success) {
        setError("Failed to parse .riv: " + path);
        mFile.reset(); mArtboard.reset(); mScene.reset();
        mSMI = nullptr; mVMRuntime.reset();
        mLoadedPath.clear();
        return false;
    }
    mFile = std::move(file);
    mArtboard.reset(); mScene.reset();
    mSMI = nullptr; mVMRuntime.reset();
    mPrevChopValues.clear(); mPrevDatValues.clear();
    mLoadedPath = path;
    mLoadedArtboard.clear(); mLoadedStateMachine.clear();
    clearError();
    return true;
}

bool TDRiveTOP::selectArtboardIfNeeded(const char* nameC)
{
    if (!mFile) return false;
    std::string name = nameC ? nameC : "";
    if (mArtboard && name == mLoadedArtboard) return true;

    std::unique_ptr<rive::ArtboardInstance> ab =
        name.empty() ? mFile->artboardDefault() : mFile->artboardNamed(name);
    if (!ab) {
        setError(name.empty()
                 ? std::string("No default artboard in file.")
                 : std::string("Artboard not found: ") + name);
        mArtboard.reset(); mScene.reset(); mSMI = nullptr;
        return false;
    }
    mArtboard = std::move(ab);
    mLoadedArtboard = name;
    mSMI = nullptr;
    mPrevChopValues.clear();
    mPrevDatValues.clear();
    bindArtboardViewModel();
    clearError();
    return true;
}

bool TDRiveTOP::selectSceneIfNeeded(const char* smC)
{
    if (!mArtboard) return false;
    std::string sm = smC ? smC : "";
    if (mScene && sm == mLoadedStateMachine) return true;

    std::unique_ptr<rive::StateMachineInstance> smiOwned;
    std::unique_ptr<rive::Scene> scene;

    if (!sm.empty()) {
        smiOwned = mArtboard->stateMachineNamed(sm);
        if (!smiOwned) {
            setError(std::string("State machine not found: ") + sm);
            return false;
        }
        scene = std::move(smiOwned);
        mSMI = static_cast<rive::StateMachineInstance*>(scene.get());
    } else {
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

    // Data binding: bindArtboardViewModel() binds the view-model instance to
    // the artboard, but the freshly-created scene / state machine instance
    // needs it bound too - otherwise data-bound inputs and conditions inside
    // the state machine never resolve (this is the "view-model data binding
    // didn't work" report). Mirrors Rive's own path_fiddle sample, which binds
    // the same instance to both the artboard and the scene.
    if (mScene && mVMRuntime) {
        mScene->bindViewModelInstance(mVMRuntime->instance());
    }

    clearError();
    return true;
}

void TDRiveTOP::bindArtboardViewModel()
{
    mVMRuntime.reset();
    // A new VM runtime knows nothing about previously bound images.
    for (auto& p : mBoundSlotImage) p = nullptr;
    if (!mFile || !mArtboard) return;
    auto* vmr = mFile->defaultArtboardViewModel(mArtboard.get());
    if (!vmr) return;
    auto runtime = vmr->createDefaultInstance();
    if (!runtime) runtime = vmr->createInstance();
    if (!runtime) return;
    mArtboard->bindViewModelInstance(runtime->instance());
    mVMRuntime = std::move(runtime);
}

// =============================================================================
// CHOP / DAT -> Rive
// =============================================================================

void TDRiveTOP::applyInputsFromCHOP(const OP_CHOPInput* chop)
{
    auto* smi = currentSMI();
    if (!smi || !chop || chop->numChannels <= 0 || chop->numSamples <= 0) return;

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

void TDRiveTOP::applyStringsFromDAT(const OP_DATInput* dat)
{
    if (!dat || dat->numRows <= 0 || dat->numCols < 2) return;

    int32_t startRow = 0;
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

        if (mVMRuntime) {
            if (auto* sp = mVMRuntime->propertyString(name)) {
                if (sp->value() != value) sp->value(value);
                handled = true;
            } else if (auto* np = mVMRuntime->propertyNumber(name)) {
                try { float v = std::stof(value); if (np->value() != v) np->value(v); }
                catch (...) {}
                handled = true;
            } else if (auto* bp = mVMRuntime->propertyBoolean(name)) {
                bool nb = dat_value_truthy(value);
                if (bp->value() != nb) bp->value(nb);
                handled = true;
            } else if (auto* tp = mVMRuntime->propertyTrigger(name)) {
                auto pit = mPrevDatValues.find(name);
                bool changed = (pit == mPrevDatValues.end()) || (pit->second != value);
                if (changed && dat_value_truthy(value)) tp->trigger();
                handled = true;
            }
        }

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
// Texture injection
// =============================================================================

void TDRiveTOP::bindSlotImage(int slot, const char* propName,
                              rive::RenderImage* img)
{
    if (!mVMRuntime || !propName || !*propName || !img) return;
    if (mBoundSlotImage[slot] == img) return;
    auto* ip = mVMRuntime->propertyImage(propName);
    if (!ip) return;  // property doesn't exist / isn't an image - skip
    ip->value(img);
    mBoundSlotImage[slot] = img;
}

void TDRiveTOP::applyImageInputsCPU(const OP_Inputs* inputs)
{
    for (int slot = 0; slot < tdrive::kMaxImageSlots; ++slot) {
        char parName[16];
        std::snprintf(parName, sizeof(parName), "Image%d", slot + 1);
        const OP_TOPInput* top = inputs->getParTOP(parName);
        std::snprintf(parName, sizeof(parName), "Imageprop%d", slot + 1);
        const char* prop = inputs->getParString(parName);

        if (!top || !prop || !*prop) {
            mPendingDl[slot] = TD::OP_SmartRef<TD::OP_TOPDownloadResult>();
            continue;
        }

        // Consume the download started on the previous cook (waiting a frame
        // avoids stalling the GPU pipeline; see the CPUMemoryTOP SDK sample).
        if (mPendingDl[slot]) {
            const uint32_t w = mPendingDl[slot]->textureDesc.width;
            const uint32_t h = mPendingDl[slot]->textureDesc.height;
            void* data = mPendingDl[slot]->getData();
            if (data && w > 0 && h > 0) {
                const size_t bytes = (size_t)w * h * 4;
                mPremulScratch.resize(bytes);
                std::memcpy(mPremulScratch.data(), data, bytes);
                premultiply_rgba(mPremulScratch.data(), (size_t)w * h);
                std::string err;
                auto img = mBackend->updateImageSlot(
                    slot, w, h, mPremulScratch.data(), err);
                if (img) bindSlotImage(slot, prop, img.get());
                else if (!err.empty()) setError(err);
            }
        }

        // Kick off this cook's download (RGBA8, converted by TD if needed).
        TD::OP_TOPInputDownloadOptions opts;
        opts.pixelFormat = TD::OP_PixelFormat::RGBA8Fixed;
        mPendingDl[slot] = top->downloadTexture(opts, nullptr);
    }
}

// =============================================================================
// Execute
// =============================================================================

void TDRiveTOP::execute(TOP_Output* output, const OP_Inputs* inputs, void*)
{
    if (!mBackendReady) {
        std::string err;
        if (!mBackend || !mBackend->init(err)) {
            if (!err.empty()) setError(err);
            return;
        }
        mBackendReady = true;
    }

    int32_t resW = 0, resH = 0;
    inputs->getParInt2("Resolution", resW, resH);
    if (resW <= 0) resW = kDefaultWidth;
    if (resH <= 0) resH = kDefaultHeight;
    {
        std::string err;
        if (!mBackend->ensureRenderTarget((uint32_t)resW, (uint32_t)resH, err)) {
            if (!err.empty()) setError(err);
            return;
        }
    }

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

    if (ok && currentSMI()) {
        if (const auto* chop = inputs->getParCHOP("Inputs")) {
            applyInputsFromCHOP(chop);
        }
    }
    if (ok) {
        if (const auto* dat = inputs->getParDAT("Strings")) {
            applyStringsFromDAT(dat);
        }
    }

    // Texture injection, CPU download path. (In CUDA mode the inputs are
    // instead copied GPU->GPU inside the CUDA-operations bracket below.)
    if (ok && !gCUDAMode) {
        applyImageInputsCPU(inputs);
    }

    auto now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (mHasTick) {
        dt = std::chrono::duration<float>(now - mLastTick).count();
        if (dt < 0.0f || dt > 1.0f) dt = 1.0f / 60.0f;
    }
    mLastTick = now;
    mHasTick = true;
    dt *= (float)speed;

    // In layout mode resize the artboard to the render target so Rive's internal
    // layout constraints (fill, etc.) apply to the actual output dimensions.
    // In all other modes restore the artboard's intrinsic size from the .riv file.
    if (ok && mArtboard) {
        if (FitFromIndex(fitIdx) == rive::Fit::layout) {
            mArtboard->width((float)resW);
            mArtboard->height((float)resH);
        } else {
            mArtboard->resetSize();
        }
    }

    if (ok && mScene)        mScene->advanceAndApply(dt);
    else if (ok && mArtboard) mArtboard->advance(dt);

    // Build the frame descriptor.
    rive::gpu::RenderContext::FrameDescriptor fd;
    fd.renderTargetWidth  = (uint32_t)resW;
    fd.renderTargetHeight = (uint32_t)resH;
    fd.loadAction = rive::gpu::LoadAction::clear;
    uint8_t r8 = (uint8_t)(std::clamp(bg[0], 0.0, 1.0) * 255.0 + 0.5);
    uint8_t g8 = (uint8_t)(std::clamp(bg[1], 0.0, 1.0) * 255.0 + 0.5);
    uint8_t b8 = (uint8_t)(std::clamp(bg[2], 0.0, 1.0) * 255.0 + 0.5);
    uint8_t a8 = (uint8_t)(std::clamp(bg[3], 0.0, 1.0) * 255.0 + 0.5);
    fd.clearColor = ((uint32_t)a8 << 24) | ((uint32_t)r8 << 16)
                  | ((uint32_t)g8 <<  8) |  (uint32_t)b8;

    auto drawFn = [this, fitIdx, alignIdx, resW, resH](rive::Renderer* r) {
        if (!mArtboard) return;
        r->save();
        r->align(FitFromIndex(fitIdx),
                 AlignmentFromIndex(alignIdx),
                 rive::AABB(0, 0, (float)resW, (float)resH),
                 mArtboard->bounds());
        mArtboard->draw(r);
        r->restore();
    };

#if defined(_WIN32)
    if (gCUDAMode) {
        // ---------------------------------------------------------------------
        // CUDA path: inject input TOPs and hand the frame back to TD entirely
        // on the GPU. The cudaArray* fields of every OP_CUDAArrayInfo only
        // become valid once beginCUDAOperations() runs, so gather them all
        // first.
        // ---------------------------------------------------------------------
        struct SlotAcq {
            const OP_CUDAArrayInfo* info = nullptr;
            const char*             prop = nullptr;
        };
        SlotAcq acq[tdrive::kMaxImageSlots];
        if (ok) {
            for (int slot = 0; slot < tdrive::kMaxImageSlots; ++slot) {
                char parName[16];
                std::snprintf(parName, sizeof(parName), "Image%d", slot + 1);
                const OP_TOPInput* top = inputs->getParTOP(parName);
                std::snprintf(parName, sizeof(parName), "Imageprop%d", slot + 1);
                const char* prop = inputs->getParString(parName);
                if (!top || !prop || !*prop) continue;
                if (top->textureDesc.pixelFormat != OP_PixelFormat::RGBA8Fixed) {
                    setError("Image inputs must be RGBA8 (8-bit fixed) in "
                             "CUDA mode.");
                    continue;
                }
                OP_CUDAAcquireInfo acquire;
                acq[slot].info = top->getCUDAArray(acquire, nullptr);
                acq[slot].prop = prop;
            }
        }

        TOP_CUDAOutputInfo co;
        co.textureDesc.width       = (uint32_t)resW;
        co.textureDesc.height      = (uint32_t)resH;
        co.textureDesc.depth       = 1;
        co.textureDesc.texDim      = OP_TexDim::e2D;
        co.textureDesc.pixelFormat = OP_PixelFormat::RGBA8Fixed;
        co.colorBufferIndex        = 0;
        const OP_CUDAArrayInfo* out = output->createCUDAArray(co, nullptr);
        if (!out) { setError("createCUDAArray failed."); return; }

        if (!mContext->beginCUDAOperations(nullptr)) {
            setError("beginCUDAOperations failed.");
            return;
        }

        for (int slot = 0; slot < tdrive::kMaxImageSlots; ++slot) {
            if (!acq[slot].info || !acq[slot].info->cudaArray) continue;
            const auto& desc = acq[slot].info->textureDesc;
            std::string ierr;
            auto img = mBackend->updateImageSlotCUDA(
                slot, desc.width, desc.height,
                acq[slot].info->cudaArray, ierr);
            if (img) bindSlotImage(slot, acq[slot].prop, img.get());
            else if (!ierr.empty()) setError(ierr);
        }

        std::string rerr;
        bool rendered = mBackend->renderToCUDA(fd, drawFn, out->cudaArray, rerr);
        mContext->endCUDAOperations(nullptr);
        if (!rendered && !rerr.empty()) setError(rerr);
        return;
    }
#endif

    // -------------------------------------------------------------------------
    // CPUMem path: render, read back, and hand TD a CPU buffer to upload.
    // -------------------------------------------------------------------------
    const uint64_t byteSize = (uint64_t)resW * (uint64_t)resH * 4;
    TD::OP_SmartRef<TD::TOP_Buffer> buf =
        mContext->createOutputBuffer(byteSize, TD::TOP_BufferFlags::None, nullptr);

    std::string err;
    if (!mBackend->renderAndReadback(fd, drawFn, buf->data, err)) {
        if (!err.empty()) setError(err);
        return;
    }

    TD::TOP_UploadInfo up;
    up.textureDesc.width       = (uint32_t)resW;
    up.textureDesc.height      = (uint32_t)resH;
    up.textureDesc.depth       = 1;
    up.textureDesc.texDim      = TD::OP_TexDim::e2D;
    up.textureDesc.pixelFormat = TD::OP_PixelFormat::BGRA8Fixed;
    up.firstPixel              = TD::TOP_FirstPixel::TopLeft;
    up.colorBufferIndex        = 0;
    output->uploadBuffer(&buf, up, nullptr);
}

// =============================================================================
// Plugin entry points
// =============================================================================

#if defined(_WIN32)
  #define TD_VIS
#else
  #define TD_VIS __attribute__((visibility("default")))
#endif

extern "C" {

TD_VIS DLLEXPORT void FillTOPPluginInfo(TD::TOP_PluginInfo* info)
{
    info->apiVersion  = TD::TOPCPlusPlusAPIVersion;

    // Prefer CUDA execute mode when the machine can do zero-copy texture
    // sharing (Windows + an NVIDIA GPU whose adapter D3D11 can also use).
    // Everything else - macOS, AMD/Intel GPUs, missing CUDA runtime - falls
    // back to the CPUMem readback path.
    info->executeMode = TD::TOP_ExecuteMode::CPUMem;
#if defined(_WIN32)
    if (tdrive::cuda::AvailableForD3D11()) {
        info->executeMode = TD::TOP_ExecuteMode::CUDA;
        gCUDAMode = true;
    }
#endif

    auto& custom = info->customOPInfo;
    custom.opType->setString("Rive");
    custom.opLabel->setString("Rive");
    custom.opIcon->setString("RIV");
    custom.authorName->setString("Evan Clark");
    custom.authorEmail->setString("you@example.com");
    custom.minInputs = 0;
    custom.maxInputs = 0;
}

TD_VIS DLLEXPORT TD::TOP_CPlusPlusBase*
CreateTOPInstance(const TD::OP_NodeInfo* info, TD::TOP_Context* context)
{
    return new TDRiveTOP(info, context);
}

TD_VIS DLLEXPORT void
DestroyTOPInstance(TD::TOP_CPlusPlusBase* instance, TD::TOP_Context*)
{
    delete static_cast<TDRiveTOP*>(instance);
}

} // extern "C"
