// TDRiveTOP.cpp
//
// Cross-platform implementation. All GPU work goes through IBackend.

#include "TDRiveTOP.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "rive/artboard.hpp"
#include "rive/file.hpp"
#include "rive/scene.hpp"
#include "rive/layout.hpp"
#include "rive/math/aabb.hpp"
#include "rive/math/mat2d.hpp"
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

#if defined(TDRIVE_PYTHON)
// Python.h comes AFTER the TD SDK headers on purpose. CPlusPlus_Common.hpp
// forward-declares PyObject / PyGetSetDef / PyMethodDef behind
// `#ifndef PyObject_HEAD`; re-typedef'ing the same type is legal in C++, so
// either order compiles. This matches Derivative's own CHOPWithPythonClass
// sample, which includes the SDK header first and Python.h in the .cpp.
#include <Python.h>
#endif

using namespace TD;

// =============================================================================
// Local helpers
// =============================================================================

namespace {

constexpr uint32_t kDefaultWidth  = 1280;
constexpr uint32_t kDefaultHeight = 720;

#if defined(TDRIVE_PYTHON)
// customOPInfo.pythonVersion is TD's load-time gate on plugins that hand back
// CPython objects. The SDK says to pass PY_VERSION from the headers you built
// against, and Derivative's own sample does exactly that - but that pins the
// DLL to a single CPython PATCH release, and TD's patch level moves: 2023.x
// ships 3.11.1, 2025.32460 ships 3.11.10, 2025.33230 ships 3.11.15.
//
// We build against the PEP 384 stable ABI (Py_LIMITED_API + python3.lib), so
// the binary genuinely runs on any of them, and we report the 3.11 floor
// instead of the patch we happened to compile on. The real ABI gate is the
// import library, not this string: on a host without a matching python3.dll
// the import simply fails to resolve and the DLL never loads at all.
constexpr const char* kTDRivePythonVersion = "3.11.1";
#endif

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

// Joins the valid values for a property into the Info DAT's "options" cell.
// Comma separated, because Rive enum values and artboard names routinely
// contain spaces - a space separator would be ambiguous to split on.
std::string join_options(const std::vector<std::string>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += v[i];
    }
    return out;
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
    // NOTE: no custom Resolution parameter. The output size comes from the
    // node's built-in Common page, like any other TOP - see computeResolution().
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

void TDRiveTOP::getWarningString(OP_String* warning, void*)
{
    if (!mWarning.empty()) warning->setString(mWarning.c_str());
}

// =============================================================================
// Dynamic menus
// =============================================================================

void TDRiveTOP::buildDynamicMenu(const OP_Inputs* inputs,
                                 OP_BuildDynamicMenuInfo* info, void*)
{
    // In CUDA execute mode TouchDesigner refuses OP_Inputs/OP_Parameters for a
    // node once beginCUDAOperations() has run for it:
    //
    //   Error: OP_Inputs and OP_Parameters can not be used after
    //          beginCUDAOperations() has been called.
    //
    // This callback fires outside execute(), so reading a parameter here is
    // what left both menus empty in CUDA mode - the plugin's bug, not
    // TouchDesigner's. Nothing here actually needs a parameter: the loaded file
    // and the selected artboard are already cached from the last cook, so in
    // CUDA mode we serve the menus from that cache and never touch 'inputs'.
    //
    // The cost is that a node which has not cooked yet has no file to list.
    // getGeneralInfo() sets cookEveryFrame, so that resolves itself on the next
    // frame rather than needing the menu to be reopened.
    const bool useInputs = !gCUDAMode;

    if (useInputs) {
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
    }
    if (!mFile) return;

    std::string name = info->name ? info->name : "";

    if (name == "Artboard") {
        for (size_t i = 0; i < mFile->artboardCount(); ++i) {
            std::string ab = mFile->artboardNameAt(i);
            info->addMenuEntry(ab.c_str(), ab.c_str());
        }
        return;
    }

    if (name == "Statemachine") {
        // mLoadedArtboard is what the last cook actually selected, which is the
        // same value the Artboard parameter holds by the time the menu opens.
        const char* abPar  = useInputs ? inputs->getParString("Artboard") : nullptr;
        const std::string abName = (abPar && *abPar) ? std::string(abPar)
                                                     : mLoadedArtboard;
        rive::Artboard* ab = nullptr;
        if (!abName.empty()) ab = mFile->artboard(abName);
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
// Info CHOP - readback cost breakdown
// =============================================================================

// Where a cook's time actually went, in milliseconds, for the last frame, plus
// which execute mode produced it.
//
// The CPU round-trip (render -> staging -> map -> memcpy -> hand to TD) is the
// dominant cost at high resolutions, and these channels say which part of it so
// the attribution doesn't have to be guessed. cuda_mode is 1 when the plugin
// registered TOP_ExecuteMode::CUDA (a usable NVIDIA adapter, TDRIVE_CUDA not 0)
// and 0 for the CPUMem path - worth having on the node itself, because
// the env var is set before TouchDesigner launches and there is otherwise no
// way to tell from inside which mode you ended up in.
//
// In CUDA mode copy_ms / map_ms / unmap_ms time the CUDA interop calls,
// memcpy_ms is 0, and cuda_begin_ms / cuda_inject_ms / cuda_end_ms time
// TouchDesigner's beginCUDAOperations(), the input-TOP copies and
// endCUDAOperations(). render_gpu_ms is the Rive render's own GPU time on our
// D3D11 device (timestamp queries, both modes) - work TouchDesigner's
// gpuCookTime cannot see in CPUMem mode. out_w / out_h are the size produced.
namespace {
constexpr const char* kInfoChanNames[] = {
    "render_ms", "copy_ms", "map_ms", "memcpy_ms", "readback_total_ms",
    "cuda_mode",
    "unmap_ms", "cuda_begin_ms", "cuda_inject_ms", "cuda_end_ms",
    "render_gpu_ms", "out_w", "out_h",
};
constexpr int32_t kNumInfoChans =
    (int32_t)(sizeof(kInfoChanNames) / sizeof(kInfoChanNames[0]));
} // namespace

int32_t TDRiveTOP::getNumInfoCHOPChans(void*)
{
    return kNumInfoChans;
}

void TDRiveTOP::getInfoCHOPChan(int32_t index, OP_InfoCHOPChan* chan, void*)
{
    if (!chan || index < 0 || index >= kNumInfoChans) return;
    const tdrive::ReadbackTimings t =
        mBackend ? mBackend->lastTimings() : tdrive::ReadbackTimings{};
    const double values[] = {
        t.renderMs, t.copyMs, t.mapMs, t.memcpyMs, t.totalMs,
        gCUDAMode ? 1.0 : 0.0,
        t.unmapMs, mCudaBeginMs, mCudaInjectMs, mCudaEndMs,
        t.renderGpuMs, (double)mOutW, (double)mOutH,
    };
    // These two lists are indexed by the same 'index'; keep them in step.
    static_assert((int32_t)(sizeof(values) / sizeof(values[0])) == kNumInfoChans,
                  "kInfoChanNames and values must have the same length");
    chan->name->setString(kInfoChanNames[index]);
    chan->value = (float)values[index];
}

// =============================================================================
// Info DAT
// =============================================================================

// Flattens both generations of Rive input into one list: state-machine
// inputs first, then the view-model tree. Everything that reports the
// artboard's input surface - the Info DAT, the node's Python schema - goes
// through here, so a value only has to be formatted once.
std::vector<TDRiveTOP::SchemaEntry> TDRiveTOP::propertySchema()
{
    std::vector<SchemaEntry> out;
    char buf[64];

    // ---- State-machine inputs (the original API: flat, three types) -------
    if (auto* smi = currentSMI()) {
        out.reserve(smi->inputCount() + mVmProps.size());
        for (size_t i = 0; i < smi->inputCount(); ++i) {
            auto* in = smi->input(i);
            if (!in) continue;

            SchemaEntry e;
            e.source = "smi";
            e.path   = in->name();
            e.type   = InputTypeName(in->inputCoreType());

            switch (in->inputCoreType()) {
                case kInputTypeNumber:
                    std::snprintf(buf, sizeof(buf), "%g",
                                  static_cast<rive::SMINumber*>(in)->value());
                    e.value = buf;
                    break;
                case kInputTypeBool:
                    e.value   = static_cast<rive::SMIBool*>(in)->value() ? "1" : "0";
                    e.options = {"0", "1"};
                    break;
                case kInputTypeTrigger:
                    e.value = "(pulse)";
                    break;
                default:
                    break;
            }
            out.push_back(std::move(e));
        }
    } else {
        out.reserve(mVmProps.size());
    }

    // ---- View-model tree (data binding: nested, many types) ---------------
    if (!mVMRuntime) return out;

    for (const auto& p : mVmProps) {
        // Full path, so a nested property reads as "payoffCard/title" - that
        // string is exactly what the Strings DAT wants in its name column, and
        // what Rive's own path-taking accessors below expect.
        const std::string&   path = p.path;
        const rive::DataType type = p.type;

        SchemaEntry e;
        e.source    = "vm";
        e.path      = path;
        e.type      = DataTypeName(type);
        e.container = (type == rive::DataType::viewModel);

        switch (type) {
            case rive::DataType::string: {
                auto* sp = mVMRuntime->propertyString(path);
                if (sp) e.value = sp->value();
                break;
            }
            case rive::DataType::number: {
                if (auto* np = mVMRuntime->propertyNumber(path)) {
                    std::snprintf(buf, sizeof(buf), "%g", np->value());
                    e.value = buf;
                }
                break;
            }
            case rive::DataType::boolean: {
                auto* bp = mVMRuntime->propertyBoolean(path);
                if (bp) e.value = bp->value() ? "1" : "0";
                e.options = {"0", "1"};
                break;
            }
            case rive::DataType::trigger:
                e.value = "(pulse)";
                break;
            case rive::DataType::enumType: {
                if (auto* ep = mVMRuntime->propertyEnum(path)) {
                    e.value   = ep->value();
                    e.options = ep->values();
                }
                break;
            }
            case rive::DataType::artboard: {
                if (auto* ap = mVMRuntime->propertyArtboard(path))
                    e.value = ap->artboardName();
                // An artboard property accepts any artboard in the file.
                if (mFile) {
                    e.options.reserve(mFile->artboardCount());
                    for (size_t i = 0; i < mFile->artboardCount(); ++i)
                        e.options.push_back(mFile->artboardNameAt(i));
                }
                break;
            }
            default:
                break;
        }

        out.push_back(std::move(e));
    }

    return out;
}

bool TDRiveTOP::getInfoDATSize(OP_InfoDATSize* size, void*)
{
    // Built here rather than per row: TD asks for the size before it walks
    // the rows, so one rebuild covers the whole table.
    mSchemaCache = propertySchema();
    size->cols = 5;
    size->rows = 1 + (int32_t)mSchemaCache.size();
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
        entries->values[4]->setString("options");
        return;
    }

    // Defensive even though getInfoDATSize() runs first: a stale row index
    // would otherwise read off the end of the cache.
    const int32_t k = row - 1;
    if (k < 0 || (size_t)k >= mSchemaCache.size()) return;
    const SchemaEntry& e = mSchemaCache[(size_t)k];

    // One monotonic index across BOTH blocks. It used to restart at 0 at the
    // state-machine/view-model boundary, so two rows could share an index.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", k);
    entries->values[0]->setString(buf);
    entries->values[1]->setString(e.path.c_str());
    entries->values[2]->setString(e.type.c_str());
    entries->values[3]->setString(e.value.c_str());
    entries->values[4]->setString(join_options(e.options).c_str());
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
        mLoadedStateMachine.clear();
        return false;
    }

    // Tear down everything derived from the OUTGOING artboard before it is
    // destroyed, and in this order.
    //
    // mScene is a StateMachineInstance built from the current ArtboardInstance
    // and holds raw pointers into its components, so it must be destroyed while
    // that artboard is still alive - resetting it after the assignment below
    // would run ~StateMachineInstance against freed memory.
    //
    // mLoadedStateMachine must be cleared too: selectSceneIfNeeded() short
    // circuits on "mScene && sm == mLoadedStateMachine", so leaving the name set
    // makes the next cook reuse a Scene belonging to an artboard that no longer
    // exists. execute() then calls advanceAndApply() on it, which walks dead
    // components and aborts in _purecall - the crash on switching artboards.
    mScene.reset();
    mSMI = nullptr;
    mLoadedStateMachine.clear();

    mArtboard = std::move(ab);
    mLoadedArtboard = name;

    // Snapshot the artboard's authored frame NOW, before anything advances it.
    //
    // This has to be our own snapshot. Rive's own Artboard::originalWidth() is
    // populated from the width/height properties in Artboard::deserialize(),
    // and for every .riv tested here it stays 0 - the artboard carries its size
    // some other way, so that case never fires. width()/height() ARE correct at
    // this point, but only until the first advance: the layout pass writes its
    // own result back over them.
    //
    // A zero original size also makes Rive's resetSize() actively harmful -
    // it assigns width(0), height(0) - which is why execute() restores this
    // snapshot instead of calling it.
    mArtboardW = mArtboard->width();
    mArtboardH = mArtboard->height();

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
    mVmProps.clear();
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
    rebuildVmProps();
}

// Walks the view model and every child view model, recording one entry per
// property keyed by its full '/'-delimited path.
//
// The child runtimes returned by propertyViewModel() are cached inside the
// parent runtime, so recursing here does not create anything that outlives
// mVMRuntime.
void TDRiveTOP::collectVmProps(rive::ViewModelInstanceRuntime* vm,
                               const std::string& prefix, int depth)
{
    // A view model may legally contain a property of its own type, so this
    // walk has to be bounded on both axes or a self-referencing file would
    // recurse (or fan out) forever.
    constexpr int    kMaxDepth = 8;
    constexpr size_t kMaxProps = 2000;

    if (!vm || depth > kMaxDepth || mVmProps.size() >= kMaxProps) return;

    for (const auto& p : vm->properties()) {
        if (mVmProps.size() >= kMaxProps) return;
        std::string path = prefix.empty() ? p.name : prefix + "/" + p.name;
        mVmProps.push_back({path, p.type});

        if (p.type == rive::DataType::viewModel) {
            if (auto child = vm->propertyViewModel(p.name)) {
                collectVmProps(child.get(), path, depth + 1);
            }
        }
    }
}

void TDRiveTOP::rebuildVmProps()
{
    mVmProps.clear();
    if (mVMRuntime) collectVmProps(mVMRuntime.get(), std::string(), 0);
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
        // "label" matters as much as "name" here: a Parameter DAT set to
        // emit labels (which is how a control COMP hands us Rive paths, since
        // TD par NAMES cannot contain '/') writes "label" into this cell.
        // Without it the header row gets applied as a property called "label".
        if (c0 && (!std::strcmp(c0, "name")  || !std::strcmp(c0, "Name")  ||
                   !std::strcmp(c0, "key")   || !std::strcmp(c0, "Key")   ||
                   !std::strcmp(c0, "label") || !std::strcmp(c0, "Label"))) {
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
            } else if (auto* ep = mVMRuntime->propertyEnum(name)) {
                // Rive ignores a value that isn't one of the enum's cases, so a
                // typo just leaves the property where it was. The Info DAT's
                // "options" column lists the accepted values.
                if (ep->value() != value) ep->value(value);
                handled = true;
            } else if (auto* ap = mVMRuntime->propertyArtboard(name)) {
                // An artboard property takes the NAME of an artboard in this
                // file. An empty cell means "leave it alone" rather than
                // "unbind" - these are usually authored with a default, and
                // silently clearing one on a blank row would be a nasty
                // surprise.
                //
                // artboardName() reads back through the bound asset, so this
                // compare is what stops us building a fresh ArtboardInstance
                // every single cook.
                if (!value.empty() && mFile && ap->artboardName() != value) {
                    if (auto bindable = mFile->bindableArtboardNamed(value)) {
                        ap->value(std::move(bindable));
                    }
                }
                handled = true;
            }
        }

        // State-machine inputs. These have their own CHOP parameter, which
        // stays the better path for ANIMATED numerics (no float->string->float
        // round trip per frame) - but routing them here too means one
        // Parameter DAT can drive a whole artboard, including older files that
        // predate data binding.
        if (!handled) {
            if (auto* smi = currentSMI()) {
                for (size_t i = 0; i < smi->inputCount(); ++i) {
                    auto* in = smi->input(i);
                    if (!in || in->name() != name) continue;
                    switch (in->inputCoreType()) {
                        case kInputTypeNumber: {
                            auto* n = static_cast<rive::SMINumber*>(in);
                            try {
                                float v = std::stof(value);
                                if (n->value() != v) n->value(v);
                            } catch (...) {}
                            break;
                        }
                        case kInputTypeBool: {
                            auto* b = static_cast<rive::SMIBool*>(in);
                            const bool nb = dat_value_truthy(value);
                            if (b->value() != nb) b->value(nb);
                            break;
                        }
                        case kInputTypeTrigger: {
                            // Same edge rule as the CHOP path: fire when the
                            // cell CHANGES to something truthy, so a constant
                            // "1" fires once instead of every cook.
                            auto pit = mPrevDatValues.find(name);
                            const bool changed = (pit == mPrevDatValues.end()) ||
                                                 (pit->second != value);
                            if (changed && dat_value_truthy(value))
                                static_cast<rive::SMITrigger*>(in)->fire();
                            break;
                        }
                        default: break;
                    }
                    handled = true;
                    break;
                }
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
// Output size and the align() content box
// =============================================================================

// The content AABB handed to Renderer::align().
//
// Rive's own Artboard::bounds() reports the *layout* box - layoutWidth() /
// layoutHeight(), i.e. whatever Yoga computed - not the artboard frame the
// designer drew in. For every .riv tested here Yoga collapses that box on the
// horizontal axis (measured ~0-6 px wide against artboards more than a
// thousand pixels tall), because the artboard carries a layout style whose
// children are ordinary shapes rather than layout components, so "hug the
// content" hugs nothing.
//
// Aligning against a zero-width box makes every Fit except None degenerate:
// contain/cover/fill divide the frame width by ~0, so the artboard is scaled
// into oblivion and the TOP comes out empty. That is the "Fit and Alignment
// don't work" symptom - only Fit None + Alignment Top Left survives, because
// that combination is the one that reduces to the identity transform and never
// touches the content's width or height.
//
// So: align against the artboard's own frame, which is what the Fit and
// Alignment menus are meant to describe. Fit::layout is the exception - that
// mode exists precisely to hand the box to Rive's layout engine (and execute()
// resizes the artboard to the render target for it), so there we use bounds().
rive::AABB TDRiveTOP::artboardFrame(bool layoutFit) const
{
    const rive::AABB b = mArtboard->bounds();
    if (layoutFit) return b;

    const float w = mArtboardW;
    const float h = mArtboardH;
    if (w <= 0.0f || h <= 0.0f) return b;

    // Preserve the artboard's origin offset (bounds() is offset by
    // -layoutWidth * originX when the artboard doesn't use a frame origin) by
    // carrying the ratio over to the authored size.
    const float ox = (b.width()  > 0.0f) ? -b.left() / b.width()  : 0.0f;
    const float oy = (b.height() > 0.0f) ? -b.top()  / b.height() : 0.0f;
    return rive::AABB::fromLTWH(-w * ox, -h * oy, w, h);
}

// Output resolution, from the node's built-in Common page - the same Output
// Resolution / Resolution parameters every other TOP has, rather than a custom
// one duplicating them.
//
// Reading built-in parameters from a custom operator is quietly inconsistent,
// so the two calls used here are the ones verified to work on this SDK:
//
//   getParString("outputresolution")     resolves
//   getParInt2("resolution", &w, &h)     resolves (the tuplet's BASE name)
//   getParInt("resolutionw") / ("...h")  does NOT - OP_Inputs raises
//                                        "Cannot find parameter named"
//
// The menu arithmetic is done here rather than through TouchDesigner's own
// TOP_Output::getSuggestedOutputDesc(), for two reasons. It is API v12 (TD 2025
// series) and td_sdk/ vendors v11, so adopting it would stop this plugin
// loading in TD 2023. And it would not help anyway: this TOP declares no TOP
// inputs, so every input-relative mode comes back measured against a phantom
// 127x127 input - asking for Half of a 460x140 artboard returned 63x63.
//
// With no input to inherit from, "Use Input" means the artboard's own size.
// That is the right default for a .riv - a fresh node comes up at the size the
// file was designed at, and Fit / Alignment then have nothing to do - and it is
// also the base the scale and Fit/Limit options measure.
//
// Not supported: Use Global Res Multiplier (the C++ API exposes no way to read
// the global multiplier) and Parent Panel Size (a plugin has no handle on the
// panel hosting it); both fall back to the artboard size. Parent Panel Size
// could be supported by moving td_sdk/ to the 2025 SDK and calling
// getSuggestedOutputDesc() for that one mode.
void TDRiveTOP::computeResolution(const OP_Inputs* inputs,
                                  int32_t& outW, int32_t& outH) const
{
    double baseW = (double)kDefaultWidth;
    double baseH = (double)kDefaultHeight;
    if (mArtboard && mArtboardW > 0.0f && mArtboardH > 0.0f) {
        baseW = (double)mArtboardW;
        baseH = (double)mArtboardH;
    }

    const char* modeStr = inputs->getParString("outputresolution");
    const std::string mode = modeStr ? modeStr : "useinput";

    double parW = 0.0, parH = 0.0;
    {
        int32_t pw = 0, ph = 0;
        if (inputs->getParInt2("resolution", pw, ph)) {
            parW = (double)pw;
            parH = (double)ph;
        }
    }

    double w = baseW, h = baseH;
    double mult = 0.0;
    if      (mode == "eighth")  mult = 1.0 / 8.0;
    else if (mode == "quarter") mult = 1.0 / 4.0;
    else if (mode == "half")    mult = 1.0 / 2.0;
    else if (mode == "2x")      mult = 2.0;
    else if (mode == "4x")      mult = 4.0;
    else if (mode == "8x")      mult = 8.0;

    if (mult > 0.0) {
        w = baseW * mult;
        h = baseH * mult;
    } else if (mode == "fit" || mode == "limit") {
        if (parW > 0.0 && parH > 0.0) {
            double sc = std::min(parW / baseW, parH / baseH);
            if (mode == "limit") sc = std::min(sc, 1.0);   // Limit only shrinks
            w = baseW * sc;
            h = baseH * sc;
        }
    } else if (mode == "custom") {
        if (parW > 0.0 && parH > 0.0) {
            w = parW;
            h = parH;
        }
    }
    // "useinput", "parpanel", and anything unrecognised, keep the artboard
    // size - see the note above on Parent Panel Size.

    outW = std::clamp((int32_t)std::lround(w), 1, 32768);
    outH = std::clamp((int32_t)std::lround(h), 1, 32768);
}

// =============================================================================
// Execute
// =============================================================================

void TDRiveTOP::execute(TOP_Output* output, const OP_Inputs* inputs, void*)
{
    mWarning.clear();
    if (!mBackendReady) {
        std::string err;
        if (!mBackend || !mBackend->init(err)) {
            if (!err.empty()) setError(err);
            return;
        }
        mBackendReady = true;
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

    // The output size can depend on the artboard (Use Input and the scale
    // options are all measured from it), so the file has to be resolved first.
    int32_t resW = 0, resH = 0;
    computeResolution(inputs, resW, resH);
    const int32_t reqW = resW, reqH = resH;
    if (gCUDAMode && reqW == mTdClampReqW && reqH == mTdClampReqH) {
        resW = mTdClampW;
        resH = mTdClampH;
    }
    {
        std::string err;
        if (!mBackend->ensureRenderTarget((uint32_t)resW, (uint32_t)resH, err)) {
            if (!err.empty()) setError(err);
            return;
        }
    }

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
    // In all other modes restore the artboard's authored size.
    //
    // Restoring it is not optional: the layout pass inside advanceAndApply()
    // overwrites width()/height() with what Yoga computed, so without this the
    // artboard shrinks a little more every cook. We restore our own snapshot
    // rather than calling Rive's resetSize(), which resets to originalWidth() /
    // originalHeight() - zero for these files (see selectArtboardIfNeeded).
    if (ok && mArtboard) {
        if (FitFromIndex(fitIdx) == rive::Fit::layout) {
            mArtboard->width((float)resW);
            mArtboard->height((float)resH);
        } else if (mArtboardW > 0.0f && mArtboardH > 0.0f) {
            mArtboard->width(mArtboardW);
            mArtboard->height(mArtboardH);
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

    auto makeDrawFn = [this, fitIdx, alignIdx](int32_t w, int32_t h) {
        return [this, fitIdx, alignIdx, w, h](rive::Renderer* r) {
            if (!mArtboard) return;
            const rive::Fit fit = FitFromIndex(fitIdx);
            r->save();
            if (gCUDAMode) {
                // The CPU path tells TouchDesigner firstPixel = TopLeft, which
                // is what makes our top-down D3D11 rows come out the right way
                // up. TOP_CUDAOutputInfo has no equivalent field, so in CUDA
                // mode TD reads the array bottom-up and the frame arrives
                // upside down. Flip the scene about the middle of the render
                // target instead - it is free, where flipping the texture
                // afterwards is another full-surface copy.
                r->transform(rive::Mat2D(1.0f, 0.0f, 0.0f, -1.0f, 0.0f, (float)h));
            }
            r->align(fit,
                     AlignmentFromIndex(alignIdx),
                     rive::AABB(0, 0, (float)w, (float)h),
                     artboardFrame(fit == rive::Fit::layout));
            mArtboard->draw(r);
            r->restore();
        };
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
        // Declared on every array TD hands us, so TD orders its Vulkan work
        // against our stream rather than the legacy default stream.
        const auto stream = static_cast<cudaStream_t>(mBackend->cudaStream());
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
                acquire.stream = stream;
                acq[slot].info = top->getCUDAArray(acquire, nullptr);
                acq[slot].prop = prop;
            }
        }

        TOP_CUDAOutputInfo co;
        co.stream                  = stream;
        co.textureDesc.width       = (uint32_t)resW;
        co.textureDesc.height      = (uint32_t)resH;
        co.textureDesc.depth       = 1;
        co.textureDesc.texDim      = OP_TexDim::e2D;
        co.textureDesc.pixelFormat = OP_PixelFormat::RGBA8Fixed;
        co.colorBufferIndex        = 0;
        const OP_CUDAArrayInfo* out = output->createCUDAArray(co, nullptr);
        if (!out) { setError("createCUDAArray failed."); return; }

        using Clock = std::chrono::steady_clock;
        auto msSince = [](Clock::time_point a) {
            return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
        };

        auto tb = Clock::now();
        if (!mContext->beginCUDAOperations(nullptr)) {
            setError("beginCUDAOperations failed.");
            return;
        }
        mCudaBeginMs = msSince(tb);

        // TouchDesigner may have allocated less than we asked for (the
        // Non-Commercial license caps TOPs at 1280x1280). Copying our frame
        // into a smaller array fails with "invalid argument", so render at
        // the size that actually exists, and remember it for later cooks.
        int32_t outW = resW, outH = resH;
        if (const auto* api = tdrive::cuda::Get(); api && out->cudaArray) {
            tdrive::cuda::ChannelFormatDesc fmt{};
            tdrive::cuda::Extent ext{};
            unsigned int flags = 0;
            if (api->arrayGetInfo(&fmt, &ext, &flags, out->cudaArray) ==
                    tdrive::cuda::kSuccess && ext.width && ext.height) {
                outW = (int32_t)ext.width;
                outH = (int32_t)ext.height;
            }
        }
        if (outW != resW || outH != resH) {
            std::string err;
            if (!mBackend->ensureRenderTarget((uint32_t)outW, (uint32_t)outH, err)) {
                mContext->endCUDAOperations(nullptr);
                if (!err.empty()) setError(err);
                return;
            }
            mTdClampReqW = reqW;  mTdClampReqH = reqH;
            mTdClampW    = outW;  mTdClampH    = outH;
            resW = outW;
            resH = outH;
            fd.renderTargetWidth  = (uint32_t)resW;
            fd.renderTargetHeight = (uint32_t)resH;
        }
        if (resW != reqW || resH != reqH) {
            mWarning = "TouchDesigner limited this TOP to " +
                       std::to_string(resW) + "x" + std::to_string(resH) +
                       " (requested " + std::to_string(reqW) + "x" +
                       std::to_string(reqH) + "), e.g. the Non-Commercial "
                       "1280x1280 cap. Rendering at the limited size.";
        }
        mOutW = resW;
        mOutH = resH;

        auto ti = Clock::now();
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
        mCudaInjectMs = msSince(ti);

        std::string rerr;
        bool rendered = mBackend->renderToCUDA(fd, makeDrawFn(resW, resH),
                                               out->cudaArray, rerr);
        // First cook only: creates the stream the next cook declares and uses.
        mBackend->ensureCudaStream();
        auto te = Clock::now();
        mContext->endCUDAOperations(nullptr);
        mCudaEndMs = msSince(te);
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
    if (!mBackend->renderAndReadback(fd, makeDrawFn(resW, resH), buf->data, err)) {
        if (!err.empty()) setError(err);
        return;
    }
    mOutW = resW;
    mOutH = resH;

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
// Python class on the node
// =============================================================================
//
// TD builds a Python class for this operator out of customOPInfo.pythonGetSets,
// so `op('rive1').<name>` reaches straight into the plugin. That is the only
// way to hand the loaded .riv's property schema to Python: an OP_Inputs can
// read a CHOP, a DAT or a TOP, but nothing lets it read a COMP's parameters,
// and Python cannot read a custom OP's Info DAT without a real Info DAT
// operator wired up.

#if defined(TDRIVE_PYTHON)
namespace {

// Bumped whenever the emitted schema's shape changes, so a control COMP built
// against an older plugin can notice and rebuild rather than silently
// mis-mapping fields.
constexpr const char* kTDRiveSchemaVersion = "1.0";

// The PyObject TD hands a getter IS a PY_Struct, and its context can return
// our C++ instance. A null result is legitimate rather than exceptional - a
// script can hold the Python object after its node was deleted - and
// getNodeInstance() has already set the Python error in that case, so callers
// just propagate the null.
TDRiveTOP* PyNodeInstance(PyObject* self, bool autoCook)
{
    auto* me = reinterpret_cast<PY_Struct*>(self);
    PY_GetInfo info;
    info.autoCook = autoCook;
    return static_cast<TDRiveTOP*>(me->context->getNodeInstance(info));
}

PyObject* pyGetSchemaVersion(PyObject* self, void*)
{
    // A constant, so there is nothing to cook for - but we still resolve the
    // instance, because that is what proves the node is still alive.
    if (!PyNodeInstance(self, /*autoCook=*/false)) return nullptr;
    return PyUnicode_FromString(kTDRiveSchemaVersion);
}

// PyDict_SetItemString does NOT steal a reference, so each value built here is
// released whether or not the insert succeeded. Getting this wrong leaks on
// every cook of every node - and a getter that leaks takes TouchDesigner down
// with it, not just this operator.
bool DictSetStr(PyObject* d, const char* key, const std::string& v)
{
    PyObject* o = PyUnicode_FromString(v.c_str());
    if (!o) return false;
    const bool ok = PyDict_SetItemString(d, key, o) == 0;
    Py_DECREF(o);
    return ok;
}

bool DictSetLong(PyObject* d, const char* key, long v)
{
    PyObject* o = PyLong_FromLong(v);
    if (!o) return false;
    const bool ok = PyDict_SetItemString(d, key, o) == 0;
    Py_DECREF(o);
    return ok;
}

bool DictSetBool(PyObject* d, const char* key, bool v)
{
    PyObject* o = PyBool_FromLong(v ? 1 : 0);
    if (!o) return false;
    const bool ok = PyDict_SetItemString(d, key, o) == 0;
    Py_DECREF(o);
    return ok;
}

// Hands Python a real list rather than the Info DAT's comma-joined cell:
// Rive enum values and artboard names can contain commas, so splitting the
// joined form back apart downstream would be lossy.
bool DictSetStrList(PyObject* d, const char* key,
                    const std::vector<std::string>& vals)
{
    PyObject* list = PyList_New(0);
    if (!list) return false;
    for (const auto& s : vals) {
        PyObject* o = PyUnicode_FromString(s.c_str());
        if (!o) { Py_DECREF(list); return false; }
        const bool appended = PyList_Append(list, o) == 0;
        Py_DECREF(o);
        if (!appended) { Py_DECREF(list); return false; }
    }
    const bool ok = PyDict_SetItemString(d, key, list) == 0;
    Py_DECREF(list);
    return ok;
}

PyObject* pyGetPropertySchema(PyObject* self, void*)
{
    // autoCook: the schema describes the loaded file/artboard, and a node that
    // has never cooked has neither. Cook first so a fresh node reports its real
    // surface rather than an empty list.
    TDRiveTOP* inst = PyNodeInstance(self, /*autoCook=*/true);
    if (!inst) return nullptr;

    const std::vector<TDRiveTOP::SchemaEntry> schema = inst->propertySchema();

    PyObject* list = PyList_New(0);
    if (!list) return nullptr;

    long index = 0;
    for (const auto& e : schema) {
        PyObject* d = PyDict_New();
        if (!d) { Py_DECREF(list); return nullptr; }

        const bool ok =
            DictSetLong(d, "index", index) &&
            DictSetStr(d, "source", e.source) &&
            DictSetStr(d, "path", e.path) &&
            DictSetStr(d, "type", e.type) &&
            DictSetStr(d, "value", e.value) &&
            DictSetStrList(d, "options", e.options) &&
            DictSetBool(d, "container", e.container);

        if (!ok) { Py_DECREF(d); Py_DECREF(list); return nullptr; }

        const bool appended = PyList_Append(list, d) == 0;
        Py_DECREF(d);
        if (!appended) { Py_DECREF(list); return nullptr; }
        ++index;
    }

    return list;
}

bool DictSetDouble(PyObject* d, const char* key, double v)
{
    PyObject* o = PyFloat_FromDouble(v);
    if (!o) return false;
    const bool ok = PyDict_SetItemString(d, key, o) == 0;
    Py_DECREF(o);
    return ok;
}

// The TD parameter style a schema entry maps to, or nullptr when the entry
// cannot be driven as a parameter.
//
// The list is deliberately narrower than the type list the schema reports: an
// entry only earns a parameter if applyStringsFromDAT() can actually apply it.
// vm:viewModel is a branch rather than a value; vm:list, vm:color, vm:image
// and vm:font have no write path yet, and generating dead parameters for them
// would look like support that isn't there.
const char* ParStyleForType(const std::string& type, bool container)
{
    if (container) return nullptr;
    if (type == "vm:string")   return "Str";
    if (type == "vm:number")   return "Float";
    if (type == "vm:integer")  return "Int";
    if (type == "vm:bool")     return "Toggle";
    if (type == "vm:trigger")  return "Pulse";
    if (type == "vm:enum")     return "Menu";
    if (type == "vm:artboard") return "Menu";
    // State-machine inputs.
    if (type == "number")      return "Float";
    if (type == "bool")        return "Toggle";
    if (type == "trigger")     return "Pulse";
    return nullptr;
}

// TD parameter names must start with a capital letter and hold only letters
// and digits, so a Rive path cannot be one. The LABEL carries the real path
// instead - labels are unconstrained, which is what lets a Parameter DAT emit
// the Rive path verbatim and feed it straight back into the Strings DAT. That
// makes this name a handle and nothing more: being unique and stable matters,
// being pretty does not.
std::string ManglePath(const std::string& path)
{
    // Capital first character, everything after it lower case. That looks
    // lossy next to a camelCase path, but it is exactly what TD enforces:
    // addParametersFromJSONList() takes "CopyCTA" and silently stores
    // "Copycta". Mangling to anything else would make the collision check
    // below run on a namespace TD does not actually use, so two paths
    // differing only in case would pass here and then clobber each other.
    std::string out;
    for (char c : path) {
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = (c >= '0' && c <= '9');
        if (!alpha && !digit) continue;   // '/', spaces and '-' just vanish
        if (out.empty() && digit) out += 'P';   // may not START with a digit
        out += out.empty() ? (char)std::toupper((unsigned char)c)
                           : (char)std::tolower((unsigned char)c);
    }
    if (out.empty()) out = "P";
    return out;
}

PyObject* pyGetTdJSONPars(PyObject* self, void*)
{
    TDRiveTOP* inst = PyNodeInstance(self, /*autoCook=*/true);
    if (!inst) return nullptr;

    const std::vector<TDRiveTOP::SchemaEntry> schema = inst->propertySchema();

    PyObject* list = PyList_New(0);
    if (!list) return nullptr;

    // Mangling is lossy, so two different Rive paths can collide on one name.
    // Collisions get a numeric suffix; order is stable because the schema walk
    // is, which keeps names the same across rebuilds of the same file.
    std::unordered_map<std::string, int> used;

    for (const auto& e : schema) {
        const char* style = ParStyleForType(e.type, e.container);
        if (!style) continue;

        std::string name = ManglePath(e.path);
        const int seen = ++used[name];
        if (seen > 1) name += std::to_string(seen);

        PyObject* d = PyDict_New();
        if (!d) { Py_DECREF(list); return nullptr; }

        // Nested view models become their own parameter page: one flat page of
        // 46 parameters is unusable, and the first path segment is already the
        // grouping the Rive author chose.
        const size_t slash = e.path.find('/');
        const std::string page =
            (slash == std::string::npos) ? std::string("Rive")
                                         : e.path.substr(0, slash);

        bool ok =
            DictSetStr(d, "name", name) &&
            // The whole point: the label is the Rive path, verbatim.
            DictSetStr(d, "label", e.path) &&
            DictSetStr(d, "page", page) &&
            DictSetStr(d, "style", style) &&
            DictSetLong(d, "size", 1) &&
            DictSetBool(d, "enable", true) &&
            DictSetBool(d, "readOnly", false) &&
            DictSetBool(d, "startSection", false) &&
            DictSetStr(d, "help", e.type + "  -  " + e.path) &&
            // Carried through so a generator can route by origin without
            // re-deriving it from the style.
            DictSetStr(d, "riveSource", e.source) &&
            DictSetStr(d, "rivePath", e.path) &&
            DictSetStr(d, "riveType", e.type);

        if (ok) {
            const std::string s = style;
            if (s == "Float" || s == "Int") {
                double v = 0.0;
                try { v = std::stod(e.value); } catch (...) { v = 0.0; }
                ok = ok && (s == "Int" ? DictSetLong(d, "default", (long)v)
                                       : DictSetDouble(d, "default", v));
                // Rive publishes no range for a number property, so nothing
                // here is authoritative: these are SLIDER HINTS only, and the
                // clamps stay off so any value can still be typed in. 0..100
                // covers the common Rive percentage case; anything outside it
                // gets a range built around the value we actually found.
                const double hi = (v > 100.0 || v < 0.0)
                                      ? std::abs(v) * 2.0 : 100.0;
                ok = ok && DictSetDouble(d, "normMin", 0.0)
                        && DictSetDouble(d, "normMax", hi)
                        && DictSetBool(d, "clampMin", false)
                        && DictSetBool(d, "clampMax", false);
            } else if (s == "Toggle") {
                ok = ok && DictSetBool(d, "default", e.value == "1");
            } else if (s == "Menu") {
                ok = ok && DictSetStr(d, "default", e.value)
                        && DictSetStrList(d, "menuNames", e.options)
                        && DictSetStrList(d, "menuLabels", e.options);
            } else if (s == "Pulse") {
                // A pulse holds no value; "(pulse)" is display text from the
                // Info DAT and must not leak into a default.
                ok = ok && DictSetLong(d, "default", 0);
            } else {
                ok = ok && DictSetStr(d, "default", e.value);
            }
        }

        if (!ok) { Py_DECREF(d); Py_DECREF(list); return nullptr; }

        const bool appended = PyList_Append(list, d) == 0;
        Py_DECREF(d);
        if (!appended) { Py_DECREF(list); return nullptr; }
    }

    return list;
}

PyGetSetDef gPyGetSets[] = {
    {"schemaVersion", pyGetSchemaVersion, nullptr,
     "Version of the property-schema format this plugin emits.", nullptr},
    {"propertySchema", pyGetPropertySchema, nullptr,
     "List of dicts describing every addressable input on the loaded artboard: "
     "index, source ('smi'/'vm'), path, type, value, options, container.",
     nullptr},
    {"tdJSONPars", pyGetTdJSONPars, nullptr,
     "Parameter definitions in TDJSON form, one per addressable Rive property. "
     "Each carries the Rive path in 'label' (TD labels are unconstrained) so a "
     "Parameter DAT can emit it straight back into the node's Strings DAT.",
     nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

} // namespace
#endif // TDRIVE_PYTHON

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

    // CUDA execute mode wherever an NVIDIA adapter supports D3D11 interop,
    // unless TDRIVE_CUDA=0 opts out; CPUMem otherwise. See cuda_interop_win.h.
    info->executeMode = TD::TOP_ExecuteMode::CPUMem;
#if defined(_WIN32)
    if (tdrive::cuda::AllowedByEnv() && tdrive::cuda::AvailableForD3D11()) {
        info->executeMode = TD::TOP_ExecuteMode::CUDA;
        gCUDAMode = true;
    }
#endif

    auto& custom = info->customOPInfo;
    custom.opType->setString("Rive");
    custom.opLabel->setString("Rive");
    custom.opIcon->setString("RIV");
    custom.authorName->setString("Evan Clark");
    custom.authorEmail->setString("djevanclark@gmail.com");

    // Bump the MINOR version only: TouchDesigner requires a project's saved
    // major version to MATCH the installed plugin's, so raising major would
    // stop every existing .toe containing a Rive node (major 0) from loading.
    // Minor only has to be >= what the project was saved with.
    custom.majorVersion = 0;
    custom.minorVersion = 7;

    custom.minInputs = 0;
    custom.maxInputs = 0;

#if defined(TDRIVE_PYTHON)
    // Declaring the version is what lets this node hand CPython objects back
    // to TD; pythonGetSets is what TD builds the node's Python class from.
    custom.pythonVersion->setString(kTDRivePythonVersion);
    custom.pythonGetSets = gPyGetSets;
#endif
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
