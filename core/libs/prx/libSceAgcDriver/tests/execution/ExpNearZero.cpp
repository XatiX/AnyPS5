#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 4;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 12> Code{
    0x34020084u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0x7e140280u, 0x7e160280u,
    0x7e180280u, 0x7e1a0280u, 0x7e144b04u, 0xe0781000u, 0x80010a01u, 0xbf810000u,
};

constexpr std::array<std::uint32_t, Threads> Cases{
    0x00000000u, 0x80000000u, 0xb3b8aa3cu, 0xb380c374u, 0xb3d6bf95u, 0xb456bf95u, 0x33800000u, 0xb3800000u,
    0x33b8aa3cu, 0xb4000000u, 0x34000000u, 0xb4800000u, 0x34800000u, 0xb5000000u, 0x35000000u, 0xb58637bdu,
    0x358637bdu, 0xb6800000u, 0x36800000u, 0xb727c5acu, 0x3727c5acu, 0xb8d1b717u, 0x38d1b717u, 0xba83126fu,
    0x3a83126fu, 0xbc23d70au, 0x3c23d70au, 0xbd000000u, 0x3d000000u, 0xbd800000u, 0x3d800000u, 0xbdccccccu,
    0x3dccccccu, 0xbdffffffu, 0x3dffffffu, 0xbe000000u, 0x3e000000u, 0xbe000001u, 0x3e000001u, 0xbe800000u,
    0x3e800000u, 0xbf000000u, 0x3f000000u, 0xbf800000u, 0x3f800000u, 0x40000000u, 0xc0000000u, 0x40800000u,
    0xc0800000u, 0x41200000u, 0xc1200000u, 0x41a00000u, 0xc1a00000u, 0x42000000u, 0xc2000000u, 0x42fc0000u,
    0xc2fc0000u, 0x43000000u, 0xc3160000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::uint32_t Ordered(std::uint32_t bits) {
    constexpr std::uint32_t sign = 0x80000000u;
    return (bits & sign) != 0u ? sign - (bits & ~sign) : bits | sign;
}

std::uint32_t Distance(std::uint32_t actual, std::uint32_t expected) {
    const std::uint32_t a = Ordered(actual);
    const std::uint32_t e = Ordered(expected);
    return a > e ? a - e : e - a;
}

void Run(AgcDriver::VulkanDevice& device) {
    Input.fill(0u);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) Input[tid * Inputs] = Cases[tid];
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = ShaderRecompiler::ShaderFloatMode{0xc0u, true, false, false};
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check() {
    Require(Output[2u * Results] == 0x3f7fffffu, "exp near zero: volumetric opticalDepth threshold 0xb3b8aa3c produced " + Hex(Output[2u * Results]) + " instead of 0x3f7fffff (< 1.0f)");
    Require(Output[3u * Results] == 0x3f7fffffu, "exp near zero: -6e-8 (0xb380c374) produced " + Hex(Output[3u * Results]) + " instead of 0x3f7fffff (< 1.0f)");
    Require(Output[4u * Results] == 0x3f7fffffu, "exp near zero: -1e-7 (0xb3d6bf95) produced " + Hex(Output[4u * Results]) + " instead of 0x3f7fffff");
    Require(Output[5u * Results] == 0x3f7ffffeu, "exp near zero: -2e-7 (0xb456bf95) produced " + Hex(Output[5u * Results]) + " instead of 0x3f7ffffe");

    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint32_t actual = Output[tid * Results];
        const float x = std::bit_cast<float>(Cases[tid]);
        const std::string label = "exp near zero: lane " + std::to_string(tid) + " v_exp_f32 of " + Hex(Cases[tid]) + " is " + Hex(actual);
        if (std::isnan(x)) {
            Require((actual & 0x7fffffffu) > 0x7f800000u, label + ", expected NaN");
            continue;
        }
        if (std::isinf(x)) {
            const std::uint32_t expected = x > 0.0f ? 0x7f800000u : 0x00000000u;
            Require(actual == expected, label + ", expected " + Hex(expected));
            continue;
        }
        const float expectedFloat = static_cast<float>(std::exp2(static_cast<double>(x)));
        const std::uint32_t expected = std::bit_cast<std::uint32_t>(expectedFloat);
        const std::uint32_t tolerance = std::abs(x) <= 0.125f ? 1u : 3u;
        Require(Distance(actual, expected) <= tolerance, label + ", expected " + Hex(expected) + " (tolerance " + std::to_string(tolerance) + " ulp)");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        Check();
        std::puts("exp near zero tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
