#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "GpuCompletion.h"
#include "TextureFormat.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
void Require(bool value, const char* name) {
    if (!value) { std::cerr << "FAILED: " << name << '\n'; std::exit(1); }
}
void Complete(ID3D11DeviceContext* context, ID3D11Query* query) {
    // A windowless test has no Present to submit commands. Flush once here;
    // the product does not flush or block while polling a completion event.
    context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    HRESULT result = S_FALSE;
    while ((result = SkyrimGpu::PollCompletion(context, query)) == S_FALSE && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Require(result == S_OK, "event completes in software device");
}
void Check(ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT format) {
    constexpr UINT width = 16, height = 8, mips = 5;
    std::vector<std::vector<std::byte>> expected(mips);
    std::vector<D3D11_SUBRESOURCE_DATA> initial(mips);
    for (UINT mip = 0; mip < mips; ++mip) {
        const auto w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
        const auto f = static_cast<std::uint8_t>(format);
        expected[mip].resize(static_cast<std::size_t>(TextureFormat::MipBytes(f, w, h)));
        for (std::size_t i = 0; i < expected[mip].size(); ++i) expected[mip][i] = static_cast<std::byte>((i * 31 + mip * 13) & 255);
        const auto block = TextureFormat::BlockBytes(f);
        initial[mip] = {expected[mip].data(), block ? ((w + 3) / 4) * block : w * 4, static_cast<UINT>(expected[mip].size())};
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = mips; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source, staging, lowered;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, initial.data(), source.GetAddressOf())), "source texture created");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())), "staging texture created");
    auto event = SkyrimGpu::CreateCompletion(device);
    Require(event.Get() != nullptr, "event query created");
    context->CopyResource(staging.Get(), source.Get()); context->End(event.Get()); Complete(context, event.Get());
    for (UINT mip = 0; mip < mips; ++mip) {
        const auto w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
        const auto block = TextureFormat::BlockBytes(static_cast<std::uint8_t>(format));
        const auto rows = block ? (h + 3) / 4 : h;
        const auto rowBytes = block ? ((w + 3) / 4) * block : w * 4;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(SUCCEEDED(context->Map(staging.Get(), mip, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)), "completed readback maps without waiting");
        Require(SkyrimGpu::EqualRows(mapped, rowBytes, rows, expected[mip]), "all-mip exact source identity across row padding");
        expected[mip][0] ^= std::byte{1};
        Require(!SkyrimGpu::EqualRows(mapped, rowBytes, rows, expected[mip]), "single-byte source replacement rejected");
        expected[mip][0] ^= std::byte{1};
        Require(!SkyrimGpu::EqualRows(mapped, rowBytes, rows, std::span(expected[mip]).first(expected[mip].size() - 1)), "short expected slice rejected");
        context->Unmap(staging.Get(), mip);
    }
    desc.Width >>= 1; desc.Height >>= 1; --desc.MipLevels;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE; desc.CPUAccessFlags = 0;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, lowered.GetAddressOf())), "smaller chain created");
    for (UINT mip = 0; mip < desc.MipLevels; ++mip)
        context->CopySubresourceRegion(lowered.Get(), mip, 0, 0, 0, source.Get(), mip + 1, nullptr);
    context->End(event.Get()); Complete(context, event.Get());
    ComPtr<ID3D11ShaderResourceView> view;
    Require(SUCCEEDED(device->CreateShaderResourceView(lowered.Get(), nullptr, view.GetAddressOf())), "completed smaller chain accepts SRV");
    source.Reset();
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.Reset();
    Require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())), "smaller staging created");
    context->CopyResource(staging.Get(), lowered.Get()); context->End(event.Get()); Complete(context, event.Get());
    for (UINT mip = 0; mip < desc.MipLevels; ++mip) {
        const auto w = std::max(desc.Width >> mip, 1u), h = std::max(desc.Height >> mip, 1u);
        const auto block = TextureFormat::BlockBytes(static_cast<std::uint8_t>(format));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(SUCCEEDED(context->Map(staging.Get(), mip, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)), "lower mip maps");
        Require(SkyrimGpu::EqualRows(mapped, block ? ((w + 3) / 4) * block : w * 4,
                                    block ? (h + 3) / 4 : h, expected[mip + 1]), "lowered chain remains exact after source release");
        context->Unmap(staging.Get(), mip);
    }
}
int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), nullptr, context.GetAddressOf())), "windowless software D3D11 device");
    Check(device.Get(), context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    Check(device.Get(), context.Get(), DXGI_FORMAT_BC1_UNORM);
    Check(device.Get(), context.Get(), DXGI_FORMAT_BC3_UNORM);
    std::cout << "WARP: RGBA8/BC1/BC3 query-gated full identity and lower-chain copies passed\n";
}
