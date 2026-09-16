// RiveRSScene.hpp
//
// A thin, standalone Rive scene wrapper for RiveRenderStream. It owns a loaded
// rive::File and, for the currently selected scene (= one artboard), the
// artboard/state-machine/view-model instances. All of the Rive API calls here
// mirror the plugin's src/TDRiveTOP.cpp (loadFileIfNeeded, bindArtboardViewModel,
// bindSlotImage, the align/draw recipe) minus the TouchDesigner OP plumbing.
//
// RenderStream scenes map one-to-one to artboards; frameData.scene indexes them.

#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/renderer.hpp"
#include "rive/scene.hpp"
#include "rive/layout.hpp"
#include "rive/math/aabb.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_runtime.hpp"
#include "rive/viewmodel/runtime/viewmodel_instance_asset_image_runtime.hpp"
#include "rive/data_bind/data_values/data_type.hpp"

namespace tdrs {

inline rive::Fit fitFromIndex(int idx)
{
    switch (idx) {
        case 0: return rive::Fit::fill;
        case 1: return rive::Fit::contain;
        case 2: return rive::Fit::cover;
        case 3: return rive::Fit::fitWidth;
        case 4: return rive::Fit::fitHeight;
        case 5: return rive::Fit::none;
        case 6: return rive::Fit::scaleDown;
        case 7: return rive::Fit::layout;
        default: return rive::Fit::contain;
    }
}

inline rive::Alignment alignmentFromIndex(int idx)
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

class RiveRSScene {
public:
    // Read + parse a .riv. factory is the RenderContext (see RiveRSDevice).
    bool load(const std::string& path, rive::Factory* factory, std::string& err)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f.good()) { err = "Could not open .riv file: " + path; return false; }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());

        rive::ImportResult ir;
        auto file = rive::File::import(
            rive::Span<const uint8_t>(bytes.data(), bytes.size()), factory, &ir);
        if (!file || ir != rive::ImportResult::success) {
            err = "Failed to parse .riv: " + path; return false;
        }
        mFile = std::move(file);
        mActive = -1;
        mArtboard.reset(); mStateMachine.reset(); mVMRuntime.reset();
        return true;
    }

    size_t artboardCount() const { return mFile ? mFile->artboardCount() : 0; }
    std::string artboardNameAt(size_t i) const {
        return mFile ? mFile->artboardNameAt(i) : std::string();
    }

    // view-model image property names for artboard i (schema image params).
    // Instantiates the artboard's default view model transiently.
    std::vector<std::string> imagePropertyNames(size_t i) const
    {
        std::vector<std::string> out;
        if (!mFile || i >= mFile->artboardCount()) return out;
        auto ab = mFile->artboardAt(i);
        if (!ab) return out;
        auto* vmr = mFile->defaultArtboardViewModel(ab.get());
        if (!vmr) return out;
        auto inst = vmr->createDefaultInstance();
        if (!inst) inst = vmr->createInstance();
        if (!inst) return out;
        for (const auto& p : inst->properties()) {
            if (p.type == rive::DataType::assetImage) out.push_back(p.name);
        }
        return out;
    }

    // Make artboard 'index' the active scene: instantiate its artboard, default
    // state machine, and default view-model instance (mirrors the plugin's
    // bindArtboardViewModel). Cheap no-op when already active.
    bool setActiveArtboard(size_t index, std::string& err)
    {
        if (!mFile || index >= mFile->artboardCount()) {
            err = "Scene index out of range."; return false;
        }
        if (mActive == (int64_t)index && mArtboard) return true;

        mArtboard = mFile->artboardAt(index);
        if (!mArtboard) { err = "Failed to instantiate artboard."; return false; }

        mStateMachine = mArtboard->defaultStateMachine();

        // Bind the default view-model instance so image properties resolve.
        mVMRuntime.reset();
        for (auto& b : mBound) b = nullptr;
        if (auto* vmr = mFile->defaultArtboardViewModel(mArtboard.get())) {
            auto inst = vmr->createDefaultInstance();
            if (!inst) inst = vmr->createInstance();
            if (inst) {
                mArtboard->bindViewModelInstance(inst->instance());
                mVMRuntime = std::move(inst);
            }
        }
        mActive = (int64_t)index;
        return true;
    }

    void advance(float dt)
    {
        if (mStateMachine) mStateMachine->advanceAndApply(dt);
        else if (mArtboard) mArtboard->advance(dt);
    }

    // Bind a GPU-canvas-backed RenderImage to a named view-model image property.
    // Skips when the property is absent or already bound to this image (mirrors
    // TDRiveTOP::bindSlotImage). slot indexes the small dedup cache.
    void bindImage(int slot, const std::string& propName, rive::RenderImage* img)
    {
        if (!mVMRuntime || propName.empty() || !img) return;
        if (slot >= 0 && slot < kMaxImageSlots && mBound[slot] == img) return;
        auto* ip = mVMRuntime->propertyImage(propName);
        if (!ip) return;
        ip->value(img);
        if (slot >= 0 && slot < kMaxImageSlots) mBound[slot] = img;
    }

    // The align/draw recipe from TDRiveTOP::execute. For Fit::layout the
    // artboard is resized to the target so Rive's layout constraints apply.
    void draw(rive::Renderer* r, int fitIdx, int alignIdx,
              uint32_t w, uint32_t h)
    {
        if (!mArtboard) return;
        rive::Fit fit = fitFromIndex(fitIdx);
        if (fit == rive::Fit::layout) {
            mArtboard->width((float)w);
            mArtboard->height((float)h);
        } else {
            mArtboard->resetSize();
        }
        r->save();
        r->align(fit, alignmentFromIndex(alignIdx),
                 rive::AABB(0, 0, (float)w, (float)h), mArtboard->bounds());
        mArtboard->draw(r);
        r->restore();
    }

    static constexpr int kMaxImageSlots = 8;

private:
    rive::rcp<rive::File>                     mFile;
    std::unique_ptr<rive::ArtboardInstance>   mArtboard;
    std::unique_ptr<rive::StateMachineInstance> mStateMachine;
    rive::rcp<rive::ViewModelInstanceRuntime> mVMRuntime;
    int64_t              mActive = -1;
    rive::RenderImage*   mBound[kMaxImageSlots] = {};
};

} // namespace tdrs
