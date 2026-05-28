// Quick standalone .riv inspector. Prints artboards, state machines, and
// their input names/types. Uses the same RenderContextMetalImpl that the
// plugin uses as the rive::Factory - sidesteps NoOpFactory typeinfo issues
// in a hidden-visibility build, no rendering actually happens.

#import <Metal/Metal.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/linear_animation_instance.hpp"
#include "rive/text/text_value_run.hpp"
#include "rive/generated/text/text_value_run_base.hpp"
#include "rive/viewmodel/runtime/viewmodel_runtime.hpp"
#include "rive/data_bind/data_values/data_type.hpp"
#include "rive/renderer/metal/render_context_metal_impl.h"

// Generated type keys from rive/generated/animation/state_machine_*_base.hpp.
// Using these directly avoids dynamic_cast across hidden-visibility typeinfo.
static const char* input_kind(rive::SMIInput* in)
{
    switch (in->inputCoreType())
    {
        case 56: return "number";
        case 58: return "trigger";
        case 59: return "bool";
        default: return "unknown";
    }
}

static const char* data_type_name(rive::DataType t)
{
    using rive::DataType;
    switch (t) {
        case DataType::none:            return "none";
        case DataType::string:          return "string";
        case DataType::number:          return "number";
        case DataType::boolean:         return "bool";
        case DataType::color:           return "color";
        case DataType::list:            return "list";
        case DataType::enumType:        return "enum";
        case DataType::trigger:         return "trigger";
        case DataType::viewModel:       return "viewModel";
        case DataType::integer:         return "integer";
        case DataType::symbolListIndex: return "symbolListIndex";
        case DataType::assetImage:      return "assetImage";
        case DataType::artboard:        return "artboard";
        case DataType::input:           return "input";
        case DataType::any:             return "any";
        default:                        return "?";
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.riv>\n", argv[0]);
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f.good()) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            std::fprintf(stderr, "no Metal device\n");
            return 1;
        }
        auto ctx = rive::gpu::RenderContextMetalImpl::MakeContext(dev);
        if (!ctx) {
            std::fprintf(stderr, "failed to make rive Metal context\n");
            return 1;
        }

        rive::ImportResult ir;
        auto file = rive::File::import(
            rive::Span<const uint8_t>(bytes.data(), bytes.size()),
            ctx.get(), &ir);
        if (!file || ir != rive::ImportResult::success) {
            std::fprintf(stderr, "failed to parse .riv (ir=%d)\n", (int)ir);
            return 1;
        }

        std::printf("file: %s\n", argv[1]);
        std::printf("artboards: %zu\n", file->artboardCount());

        for (size_t a = 0; a < file->artboardCount(); ++a) {
            auto abi = file->artboardAt(a);
            std::printf("\n  [%zu] artboard \"%s\"  (%.0f x %.0f)\n",
                        a, abi->name().c_str(),
                        abi->width(), abi->height());
            std::printf("        animations: %zu\n", abi->animationCount());
            for (size_t i = 0; i < abi->animationCount(); ++i) {
                auto anim = abi->animationAt(i);
                std::printf("          - \"%s\" (%.3fs)\n",
                            anim->name().c_str(), anim->durationSeconds());
            }
            std::printf("        state machines: %zu", abi->stateMachineCount());
            int defIdx = abi->defaultStateMachineIndex();
            if (defIdx >= 0) std::printf("  (default index: %d)", defIdx);
            std::printf("\n");
            for (size_t i = 0; i < abi->stateMachineCount(); ++i) {
                auto smi = abi->stateMachineAt(i);
                std::printf("          - \"%s\"  inputs: %zu\n",
                            smi->name().c_str(), smi->inputCount());
                for (size_t k = 0; k < smi->inputCount(); ++k) {
                    auto in = smi->input(k);
                    std::printf("              [%zu] %-10s  %s\n",
                                k, input_kind(in), in->name().c_str());
                }
            }

            // Text runs - no public enumeration on ArtboardInstance, so walk
            // the object list looking for TextValueRunBase nodes (typeKey 135).
            size_t textRunCount = 0;
            for (rive::Core* obj : abi->objects()) {
                if (obj && obj->coreType() == rive::TextValueRunBase::typeKey) {
                    ++textRunCount;
                }
            }
            std::printf("        text runs: %zu\n", textRunCount);
            for (rive::Core* obj : abi->objects()) {
                if (!obj || obj->coreType() != rive::TextValueRunBase::typeKey) continue;
                auto* tvr = static_cast<rive::TextValueRun*>(obj);
                std::printf("          - \"%s\" = \"%s\"\n",
                            tvr->name().c_str(), tvr->text().c_str());
            }

            // Default view model on this artboard, if any.
            if (auto* vmr = file->defaultArtboardViewModel(abi.get())) {
                std::printf("        view model: \"%s\"  (default for artboard)\n",
                            vmr->name().c_str());
                auto props = vmr->properties();
                for (auto& p : props) {
                    std::printf("          - %-9s  %s\n",
                                data_type_name(p.type), p.name.c_str());
                }
            } else {
                std::printf("        view model: (none bound to artboard)\n");
            }
        }

        // File-level view models.
        size_t vmCount = file->viewModelCount();
        std::printf("\nview models (file-level): %zu\n", vmCount);
        for (size_t i = 0; i < vmCount; ++i) {
            auto* vmr = file->viewModelByIndex(i);
            if (!vmr) continue;
            std::printf("  [%zu] \"%s\"  properties: %zu\n",
                        i, vmr->name().c_str(), vmr->propertyCount());
            auto props = vmr->properties();
            for (auto& p : props) {
                std::printf("        - %-9s  %s\n",
                            data_type_name(p.type), p.name.c_str());
            }
            auto instNames = vmr->instanceNames();
            if (!instNames.empty()) {
                std::printf("        instances:\n");
                for (auto& nm : instNames) {
                    std::printf("          - \"%s\"\n", nm.c_str());
                }
            }
        }
    }
    return 0;
}
