// Texture Downscaler cuts textures in a hook on the device's CreateTexture2D entry (vtable slot 5). Beside it, Faster Textures
// creates its full versions with ID3D11Device3::CreateTexture2D1, at the size Texture Downscaler would give them, so that the
// hook does not cut them again. This checks, on this machine's d3d11.dll, that CreateTexture2D1 does not pass through slot 5.
#include <d3d11_3.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdlib>
#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
    void Require(bool value, const char* name) {
        if (!value) {
            std::cerr << "FAILED: " << name << '\n';
            std::exit(1);
        }
    }

    using CreateTexture2D_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
    CreateTexture2D_t g_original = nullptr;
    int g_calls = 0;

    HRESULT STDMETHODCALLTYPE Counting(ID3D11Device* a_self, const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data, ID3D11Texture2D** a_out) {
        ++g_calls;
        return g_original(a_self, a_desc, a_data, a_out);
    }

    void Patch(ID3D11Device* a_device, void* a_function, void** a_previous) {
        auto** entry = &(*reinterpret_cast<void***>(a_device))[5];
        DWORD protection = 0;
        Require(VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &protection) != 0, "slot writable");
        if (a_previous) *a_previous = *entry;
        *entry = a_function;
        VirtualProtect(entry, sizeof(void*), protection, &protection);
    }
}

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT result = E_FAIL;
    for (const auto type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        result = D3D11CreateDevice(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), nullptr, context.ReleaseAndGetAddressOf());
        if (SUCCEEDED(result)) break;
    }
    Require(SUCCEEDED(result), "a device");
    ComPtr<ID3D11Device3> device3;
    Require(SUCCEEDED(device.As(&device3)), "ID3D11Device3");

    // A BC1 texture of 256 x 256 with all its levels, the shape Faster Textures creates.
    constexpr UINT kSide = 256, kLevels = 9;
    std::vector<std::vector<std::byte>> levels;
    std::vector<D3D11_SUBRESOURCE_DATA> data;
    for (UINT level = 0; level < kLevels; ++level) {
        const UINT side = kSide >> level, blocks = side < 4 ? 1 : side / 4;
        levels.emplace_back(static_cast<std::size_t>(blocks) * blocks * 8);
        data.push_back({levels.back().data(), blocks * 8, blocks * blocks * 8});
    }
    D3D11_TEXTURE2D_DESC desc{kSide, kSide, kLevels, 1, DXGI_FORMAT_BC1_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};

    Patch(device.Get(), reinterpret_cast<void*>(&Counting), reinterpret_cast<void**>(&g_original));

    ComPtr<ID3D11Texture2D> plain;
    Require(SUCCEEDED(device->CreateTexture2D(&desc, data.data(), plain.GetAddressOf())), "CreateTexture2D works");
    Require(g_calls == 1, "a hook in slot 5 sees CreateTexture2D");

    D3D11_TEXTURE2D_DESC1 desc1{desc.Width, desc.Height, desc.MipLevels, desc.ArraySize, desc.Format, desc.SampleDesc, desc.Usage, desc.BindFlags,
                                desc.CPUAccessFlags, desc.MiscFlags, D3D11_TEXTURE_LAYOUT_UNDEFINED};
    ComPtr<ID3D11Texture2D1> bypassed;
    Require(SUCCEEDED(device3->CreateTexture2D1(&desc1, data.data(), bypassed.GetAddressOf())), "CreateTexture2D1 works");
    Require(g_calls == 1, "CreateTexture2D1 does not pass through slot 5");
    D3D11_TEXTURE2D_DESC made{};
    bypassed->GetDesc(&made);
    Require(made.Width == kSide && made.MipLevels == kLevels, "CreateTexture2D1 makes the texture asked for");

    Patch(device.Get(), reinterpret_cast<void*>(g_original), nullptr);
    std::cout << "downscaler bypass: all passed\n";
    return 0;
}
