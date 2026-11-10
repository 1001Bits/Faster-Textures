// The DDS the game is shown instead of the file, and the full texture built later,
// on a real D3D11 device: DirectXTK's loader must accept the stand-in as a texture
// holding exactly the kept mips, and a texture built by mip layout must match.

#include "GpuCompletion.h"
#include "MipTail.h"

#include <DDSTextureLoader.h>
#include <wrl/client.h>

#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
    using Microsoft::WRL::ComPtr;
    using Bytes = std::vector<std::uint8_t>;

    void Require(bool value, const std::string& name) {
        if (!value) {
            std::cerr << "FAILED: " << name << '\n';
            std::exit(1);
        }
    }

    void Put(Bytes& a_bytes, std::size_t a_at, std::uint32_t a_value) { std::memcpy(a_bytes.data() + a_at, &a_value, 4); }

    // A DDS whose bytes differ everywhere, so a wrong offset cannot pass by accident.
    Bytes MakeFile(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, std::uint32_t a_format, bool a_dx10, std::uint32_t a_flags) {
        const std::size_t header = a_dx10 ? 148 : 128;
        const auto chain = TextureFormat::ChainBytes(static_cast<std::uint8_t>(a_format), a_width, a_height, 0, a_mips);
        Bytes file(header + static_cast<std::size_t>(chain));
        std::mt19937 random(a_width * 31 + a_format);
        for (std::size_t i = header; i < file.size(); ++i) file[i] = static_cast<std::uint8_t>(random());
        Put(file, 0, SkyrimSource::FourCC('D', 'D', 'S', ' '));
        Put(file, 4, 124);
        Put(file, 8, 0x1007u | a_flags);
        Put(file, 12, a_height);
        Put(file, 16, a_width);
        if (a_flags & 0x80000u) Put(file, 20, static_cast<std::uint32_t>(TextureFormat::MipBytes(static_cast<std::uint8_t>(a_format), a_width, a_height)));
        if (a_flags & 0x8u) Put(file, 20, static_cast<std::uint32_t>(TextureFormat::MipBytes(static_cast<std::uint8_t>(a_format), a_width, 1)));
        Put(file, 28, a_mips);
        Put(file, 76, 32);
        Put(file, 108, 0x1000);
        if (a_dx10) {
            Put(file, 80, 4);
            Put(file, 84, SkyrimSource::FourCC('D', 'X', '1', '0'));
            Put(file, 128, a_format);
            Put(file, 132, 3);
            Put(file, 140, 1);
        } else if (a_format == 71) {
            Put(file, 80, 4);
            Put(file, 84, SkyrimSource::FourCC('D', 'X', 'T', '1'));
        } else if (a_format == 77) {
            Put(file, 80, 4);
            Put(file, 84, SkyrimSource::FourCC('D', 'X', 'T', '5'));
        } else {  // 87: BGRA
            Put(file, 80, 0x41);
            Put(file, 88, 32);
            Put(file, 92, 0xFF0000u);
            Put(file, 96, 0xFF00u);
            Put(file, 100, 0xFFu);
            Put(file, 104, 0xFF000000u);
        }
        return file;
    }

    Bytes ReadMip(ID3D11Device* a_device, ID3D11DeviceContext* a_context, ID3D11Texture2D* a_texture, UINT a_mip, std::uint32_t a_rowBytes, std::uint32_t a_rows) {
        D3D11_TEXTURE2D_DESC desc{};
        a_texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        Require(SUCCEEDED(a_device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())), "staging texture");
        a_context->CopyResource(staging.Get(), a_texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(SUCCEEDED(a_context->Map(staging.Get(), a_mip, D3D11_MAP_READ, 0, &mapped)), "mip mapped");
        Bytes out(std::size_t{a_rowBytes} * a_rows);
        for (std::uint32_t row = 0; row < a_rows; ++row)
            std::memcpy(out.data() + std::size_t{row} * a_rowBytes, static_cast<const std::uint8_t*>(mapped.pData) + std::size_t{row} * mapped.RowPitch, a_rowBytes);
        a_context->Unmap(staging.Get(), a_mip);
        return out;
    }

    void Check(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, std::uint32_t a_format, bool a_dx10,
               std::uint32_t a_flags, std::uint32_t a_expectSkip, const std::string& a_name) {
        const auto format = static_cast<std::uint8_t>(a_format);
        const auto file = MakeFile(a_width, a_height, a_mips, a_format, a_dx10, a_flags);
        const auto info = SkyrimSource::Parse(std::span(reinterpret_cast<const std::byte*>(file.data()), MipTail::kHeaderMax), file.size());
        Require(info.has_value(), a_name + ": headers parsed");
        const auto plan = MipTail::PlanFor(*info, 2, 64, 1);
        Require(plan && plan->skip == a_expectSkip, a_name + ": planned");

        // The stand-in, as StandIn serves it.
        Bytes standIn(info->offset + static_cast<std::size_t>(plan->tailBytes));
        std::memcpy(standIn.data(), file.data(), info->offset);
        MipTail::RewriteHeader(std::span(reinterpret_cast<std::byte*>(standIn.data()), info->offset), *info, *plan);
        std::memcpy(standIn.data() + info->offset, file.data() + plan->tailOffset, static_cast<std::size_t>(plan->tailBytes));

        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11ShaderResourceView> view;
        Require(SUCCEEDED(DirectX::CreateDDSTextureFromMemory(a_device, standIn.data(), standIn.size(), resource.GetAddressOf(), view.GetAddressOf())), a_name + ": the loader accepts the stand-in");
        ComPtr<ID3D11Texture2D> small_;
        Require(SUCCEEDED(resource.As(&small_)), a_name + ": a 2D texture");
        D3D11_TEXTURE2D_DESC smallDesc{};
        small_->GetDesc(&smallDesc);
        Require(smallDesc.Width == plan->width && smallDesc.Height == plan->height && smallDesc.MipLevels == plan->mips, a_name + ": the stand-in's size and mips");

        // The full texture, as Upgrade builds it, from the small one's description.
        D3D11_TEXTURE2D_DESC fullDesc = smallDesc;
        fullDesc.Width = info->width;
        fullDesc.Height = info->height;
        fullDesc.MipLevels = info->mips;
        const auto mips = MipTail::MipsOf(*info);
        std::vector<D3D11_SUBRESOURCE_DATA> initial(mips.size());
        for (std::size_t i = 0; i < mips.size(); ++i) initial[i] = {file.data() + mips[i].offset, mips[i].rowPitch, mips[i].slicePitch};
        ComPtr<ID3D11Texture2D> full;
        Require(SUCCEEDED(a_device->CreateTexture2D(&fullDesc, initial.data(), full.GetAddressOf())), a_name + ": full texture created");
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
        view->GetDesc(&viewDesc);
        viewDesc.Texture2D.MostDetailedMip = 0;
        viewDesc.Texture2D.MipLevels = static_cast<UINT>(-1);
        ComPtr<ID3D11ShaderResourceView> fullView;
        Require(SUCCEEDED(a_device->CreateShaderResourceView(full.Get(), &viewDesc, fullView.GetAddressOf())), a_name + ": full texture's view");
        fullView->GetDesc(&viewDesc);
        Require(viewDesc.Texture2D.MipLevels == info->mips, a_name + ": the view covers every mip");

        // Every kept mip is the same in both, and equal to the file.
        for (UINT level = 0; level < plan->mips; ++level) {
            const auto fileLevel = level + plan->skip;
            const auto w = std::max(info->width >> fileLevel, 1u), h = std::max(info->height >> fileLevel, 1u);
            const auto block = TextureFormat::BlockBytes(format);
            const std::uint32_t rowBytes = block ? ((w + 3u) >> 2) * block : w * (TextureFormat::BitsPerPixel(format) / 8);
            const std::uint32_t rows = block ? (h + 3u) >> 2 : h;
            const auto fromSmall = ReadMip(a_device, a_context, small_.Get(), level, rowBytes, rows);
            const auto fromFull = ReadMip(a_device, a_context, full.Get(), fileLevel, rowBytes, rows);
            Require(fromSmall == fromFull, a_name + ": kept mip " + std::to_string(level) + " matches the full texture");
            Require(std::memcmp(fromSmall.data(), file.data() + mips[fileLevel].offset, fromSmall.size()) == 0, a_name + ": kept mip " + std::to_string(level) + " matches the file");
        }
    }
}

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf())), "software device");

    constexpr std::uint32_t kLinear = 0x80000u, kPitch = 0x8u;
    Check(device.Get(), context.Get(), 1024, 1024, 11, 71, false, kLinear, 2, "DXT1 with a linear size");
    Check(device.Get(), context.Get(), 1024, 512, 11, 77, false, kLinear, 2, "DXT5, wider than high");
    Check(device.Get(), context.Get(), 512, 1024, 11, 77, false, 0, 2, "DXT5 with neither size flag");
    Check(device.Get(), context.Get(), 1024, 1024, 11, 98, true, kLinear, 2, "BC7 with the extended header");
    Check(device.Get(), context.Get(), 1024, 1024, 11, 83, true, kLinear, 2, "BC5 with the extended header");
    Check(device.Get(), context.Get(), 256, 256, 9, 87, false, kPitch, 2, "BGRA with a pitch");
    Check(device.Get(), context.Get(), 1024, 1024, 4, 71, false, kLinear, 2, "a short mip chain");
    Check(device.Get(), context.Get(), 2048, 1024, 3, 71, false, kLinear, 2, "only one mip left");
    std::cout << "stand-in GPU tests passed\n";
    return 0;
}
