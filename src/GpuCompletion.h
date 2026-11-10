#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace SkyrimGpu {
    inline Microsoft::WRL::ComPtr<ID3D11Query> CreateCompletion(ID3D11Device* device) {
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        if (FAILED(device->CreateQuery(&desc, query.GetAddressOf()))) return {};
        return query;
    }
    inline HRESULT PollCompletion(ID3D11DeviceContext* context, ID3D11Query* query) {
        BOOL finished = FALSE;
        const auto result = context->GetData(query, &finished, sizeof(finished), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        return result == S_OK && !finished ? S_FALSE : result;
    }
    inline bool EqualRows(const D3D11_MAPPED_SUBRESOURCE& mapped, std::uint32_t rowBytes,
                          std::uint32_t rows, std::span<const std::byte> expected) {
        if (!mapped.pData || mapped.RowPitch < rowBytes || std::uint64_t(rowBytes) * rows > expected.size()) return false;
        for (UINT row = 0; row < rows; ++row)
            if (std::memcmp(static_cast<const std::byte*>(mapped.pData) + std::uint64_t(row) * mapped.RowPitch,
                            expected.data() + std::uint64_t(row) * rowBytes, rowBytes) != 0) return false;
        return true;
    }
}
