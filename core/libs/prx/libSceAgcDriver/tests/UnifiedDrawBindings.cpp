#include "prx/libSceAgcDriver/Unified/include/DrawBindings.hpp"
#include "RuntimeAbi.hpp"
#include <cstdio>

using namespace AgcDriver::Unified;
using ShaderRecompiler::DescriptorRole;
using ShaderRecompiler::ShaderStage;

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

ShaderRecompiler::DescriptorBinding bindingOf(DescriptorRole role, std::uint32_t set, std::uint32_t binding, std::uint32_t count = 1) {
    ShaderRecompiler::DescriptorBinding result{};
    result.kind = ShaderRecompiler::DescriptorKind::StorageBuffer;
    result.role = role;
    result.descriptorSet = set;
    result.binding = binding;
    result.count = count;
    return result;
}

void testBuffersOfTwoStagesAreDescribedInOrder() {
    ShaderRecompiler::RecompileResult vertex;
    vertex.bindings.push_back(bindingOf(DescriptorRole::ShaderData, 0, 3));
    vertex.bindings.push_back(bindingOf(DescriptorRole::GuestBuffers, 0, 1, 2));
    vertex.bindings.push_back(bindingOf(DescriptorRole::GuestImages, ShaderRecompiler::RuntimeAbi::DirectHeapSet, ShaderRecompiler::RuntimeAbi::DirectHeapBindingBase));
    ShaderRecompiler::RecompileResult fragment;
    fragment.bindings.push_back(bindingOf(DescriptorRole::FlattenedSrt, 1, 0));
    fragment.bindings.push_back(bindingOf(DescriptorRole::BdaPagetable, 0, 7));
    fragment.bindings.push_back(bindingOf(DescriptorRole::FaultBuffer, 0, 8));
    const Bindings::StageInput stages[] = {{ShaderStage::Vertex, &vertex}, {ShaderStage::Fragment, &fragment}};
    Bindings::Layout layout;
    std::string reason;
    Expect(Bindings::Describe(stages, layout, reason), "buffer bindings with heap images are described");
    Expect(layout.bindings.size() == 5, "heap-set bindings are left to set 4");
    Expect(layout.split, "a fragment-set binding splits the layout");
    Expect(layout.bindings[0].binding == 1 && layout.bindings[0].count == 2 && layout.bindings[0].source == Bindings::Source::Guest, "set 0 is sorted by binding and keeps the element count");
    Expect(layout.bindings[1].binding == 3 && layout.bindings[1].stage == 0 && layout.bindings[1].index == 0, "a binding names the stage and result binding it comes from");
    Expect(layout.bindings[4].set == 1 && layout.bindings[4].source == Bindings::Source::Words && layout.bindings[4].stages == Bindings::StageBit(ShaderStage::Fragment), "the fragment set comes last");
    Expect(layout.keys[0].size() == 12 && layout.keys[1].size() == 3, "each set has a key of three words per binding");
    Bindings::SourceCounts counts;
    Bindings::CountSources(layout, counts);
    Expect(counts.words == 2 && counts.guest == 1 && counts.pageTable == 1 && counts.fault == 1 && counts.gds == 0, "sources are counted");
}

void testImagesOutsideTheHeapAreRefused() {
    ShaderRecompiler::RecompileResult vertex;
    vertex.bindings.push_back(bindingOf(DescriptorRole::ImageTable, 0, 2));
    const Bindings::StageInput stages[] = {{ShaderStage::Vertex, &vertex}};
    Bindings::Layout layout;
    std::string reason;
    Expect(!Bindings::Describe(stages, layout, reason) && reason.find("outside the heap") != std::string::npos, "an image table binding is refused");
}

void testSharedBindingIsRefused() {
    ShaderRecompiler::RecompileResult vertex;
    vertex.bindings.push_back(bindingOf(DescriptorRole::ShaderData, 0, 3));
    ShaderRecompiler::RecompileResult fragment;
    fragment.bindings.push_back(bindingOf(DescriptorRole::ShaderData, 0, 3));
    const Bindings::StageInput stages[] = {{ShaderStage::Vertex, &vertex}, {ShaderStage::Fragment, &fragment}};
    Bindings::Layout layout;
    std::string reason;
    Expect(!Bindings::Describe(stages, layout, reason) && reason.find("shared") != std::string::npos, "two stages on one binding are refused");
}

void testOutputLocationsComeFromOutputVariables() {
    const std::vector<std::uint32_t> spirv{
        0x07230203u, 0x00010000u, 0u, 20u, 0u,
        (4u << 16u) | 71u, 10u, 30u, 0u,
        (4u << 16u) | 71u, 11u, 30u, 2u,
        (4u << 16u) | 71u, 12u, 30u, 5u,
        (4u << 16u) | 59u, 1u, 10u, 3u,
        (4u << 16u) | 59u, 1u, 11u, 3u,
        (4u << 16u) | 59u, 1u, 12u, 1u,
    };
    Expect(Bindings::OutputLocations(spirv) == 0b101u, "output variables give their locations, inputs do not");
    Expect(Bindings::OutputLocations({}) == 0u, "an empty module has no outputs");
}

}

int main() {
    testOutputLocationsComeFromOutputVariables();
    testBuffersOfTwoStagesAreDescribedInOrder();
    testImagesOutsideTheHeapAreRefused();
    testSharedBindingIsRefused();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("unified draw bindings: layout description checks passed\n");
    return 0;
}
