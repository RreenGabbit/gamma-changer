#pragma once

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gamma_changer {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }

    T* Get() const noexcept { return pointer_; }
    T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    // Releases any current object and exposes the slot for an out parameter.
    T** Put() noexcept {
        Reset();
        return &pointer_;
    }

    void Reset() noexcept {
        if (pointer_ != nullptr) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

private:
    T* pointer_ = nullptr;
};

struct SampledImage {
    int width = 0;
    int height = 0;
    const unsigned char* bgra = nullptr;  // tightly packed, width * 4 bytes per row
};

// GPU-side brightness sampling through DXGI Desktop Duplication. The desktop
// frame is cropped and box-filtered down by the GPU (GenerateMips); only a
// tiny image is read back. Anything unsupported reports Unavailable so the
// caller can fall back to GDI.
class DesktopDuplicationSampler {
public:
    enum class Result {
        Frame,        // image holds a fresh sample
        Unchanged,    // nothing was presented since the last Frame for this region
        Unavailable,  // use the fallback capture path
    };

    ~DesktopDuplicationSampler() { Reset(); }

    void Reset() noexcept {
        staging_.Reset();
        cropView_.Reset();
        crop_.Reset();
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
        outputName_.clear();
        cropWidth_ = 0;
        cropHeight_ = 0;
        haveFrame_ = false;
    }

    Result Capture(
        const std::wstring& deviceName,
        const RECT& screenRect,
        SampledImage& image) {
        const ULONGLONG now = GetTickCount64();
        if (!duplication_ || _wcsicmp(outputName_.c_str(), deviceName.c_str()) != 0) {
            Reset();
            if (deviceName.empty() || now < retryAfter_) {
                return Result::Unavailable;
            }
            if (!Initialize(deviceName)) {
                Reset();
                retryAfter_ = now + 5000U;
                return Result::Unavailable;
            }
        }

        RECT region{
            screenRect.left - desktop_.left,
            screenRect.top - desktop_.top,
            screenRect.right - desktop_.left,
            screenRect.bottom - desktop_.top,
        };
        region.left = Clamp(region.left, 0, static_cast<LONG>(textureWidth_));
        region.right = Clamp(region.right, 0, static_cast<LONG>(textureWidth_));
        region.top = Clamp(region.top, 0, static_cast<LONG>(textureHeight_));
        region.bottom = Clamp(region.bottom, 0, static_cast<LONG>(textureHeight_));
        const int width = static_cast<int>(region.right - region.left);
        const int height = static_cast<int>(region.bottom - region.top);
        if (width < 160 || height < 120) {
            return Result::Unavailable;
        }
        if (!EnsureResources(width, height)) {
            Reset();
            retryAfter_ = now + 5000U;
            return Result::Unavailable;
        }

        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        const HRESULT acquired = duplication_->AcquireNextFrame(
            haveFrame_ ? 0U : 100U, &info, resource.Put());
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            const bool same = haveFrame_ && EqualRect(&region, &lastRegion_) != FALSE;
            return same ? Result::Unchanged : Result::Unavailable;
        }
        if (FAILED(acquired)) {
            // Typically DXGI_ERROR_ACCESS_LOST after a mode change or desktop switch.
            Reset();
            retryAfter_ = now + 1000U;
            return Result::Unavailable;
        }

        bool copied = false;
        ComPtr<ID3D11Texture2D> frame;
        if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(frame.Put())))) {
            const D3D11_BOX box{
                static_cast<UINT>(region.left),
                static_cast<UINT>(region.top),
                0U,
                static_cast<UINT>(region.right),
                static_cast<UINT>(region.bottom),
                1U,
            };
            context_->CopySubresourceRegion(crop_.Get(), 0U, 0U, 0U, 0U, frame.Get(), 0U, &box);
            copied = true;
        }
        frame.Reset();
        resource.Reset();
        duplication_->ReleaseFrame();
        if (!copied) {
            Reset();
            retryAfter_ = now + 5000U;
            return Result::Unavailable;
        }

        context_->GenerateMips(cropView_.Get());
        context_->CopySubresourceRegion(
            staging_.Get(), 0U, 0U, 0U, 0U, crop_.Get(), static_cast<UINT>(mip_), nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context_->Map(staging_.Get(), 0U, D3D11_MAP_READ, 0U, &mapped))) {
            Reset();
            retryAfter_ = now + 5000U;
            return Result::Unavailable;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(stagingWidth_) * 4U;
        pixels_.resize(rowBytes * static_cast<std::size_t>(stagingHeight_));
        for (int row = 0; row < stagingHeight_; ++row) {
            std::memcpy(
                pixels_.data() + rowBytes * static_cast<std::size_t>(row),
                static_cast<const unsigned char*>(mapped.pData) +
                    static_cast<std::size_t>(mapped.RowPitch) * static_cast<std::size_t>(row),
                rowBytes);
        }
        context_->Unmap(staging_.Get(), 0U);

        lastRegion_ = region;
        haveFrame_ = true;
        image.width = stagingWidth_;
        image.height = stagingHeight_;
        image.bgra = pixels_.data();
        return Result::Frame;
    }

private:
    static LONG Clamp(const LONG value, const LONG low, const LONG high) noexcept {
        return value < low ? low : (value > high ? high : value);
    }

    // d3d11.dll and dxgi.dll are loaded on first use so users who never enable
    // automatic mode do not pay for them at startup.
    bool Initialize(const std::wstring& deviceName) {
        using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
        const HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
        const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        if (dxgi == nullptr || d3d11 == nullptr) {
            return false;
        }
        const auto createFactory = reinterpret_cast<CreateFactoryFn>(
            reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")));
        const auto createDevice = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
            reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")));
        if (createFactory == nullptr || createDevice == nullptr) {
            return false;
        }

        ComPtr<IDXGIFactory1> factory;
        if (FAILED(createFactory(__uuidof(IDXGIFactory1),
                                 reinterpret_cast<void**>(factory.Put())))) {
            return false;
        }

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(adapterIndex, adapter.Put()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            for (UINT outputIndex = 0;; ++outputIndex) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(outputIndex, output.Put()) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                DXGI_OUTPUT_DESC description{};
                if (FAILED(output->GetDesc(&description)) ||
                    _wcsicmp(description.DeviceName, deviceName.c_str()) != 0) {
                    continue;
                }
                // Rotated outputs would need the sample region transformed.
                if (description.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                    description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                    return false;
                }
                if (FAILED(createDevice(
                        adapter.Get(),
                        D3D_DRIVER_TYPE_UNKNOWN,
                        nullptr,
                        0U,
                        nullptr,
                        0U,
                        D3D11_SDK_VERSION,
                        device_.Put(),
                        nullptr,
                        context_.Put()))) {
                    return false;
                }
                ComPtr<IDXGIOutput1> output1;
                if (FAILED(output->QueryInterface(IID_PPV_ARGS(output1.Put()))) ||
                    FAILED(output1->DuplicateOutput(device_.Get(), duplication_.Put()))) {
                    return false;
                }
                DXGI_OUTDUPL_DESC duplicationDescription{};
                duplication_->GetDesc(&duplicationDescription);
                // HDR / wide-gamut desktops use a float format; leave those to GDI.
                if (duplicationDescription.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                    return false;
                }
                desktop_ = description.DesktopCoordinates;
                textureWidth_ = duplicationDescription.ModeDesc.Width;
                textureHeight_ = duplicationDescription.ModeDesc.Height;
                outputName_ = deviceName;
                return true;
            }
        }
        return false;
    }

    bool EnsureResources(const int width, const int height) {
        if (crop_ && width == cropWidth_ && height == cropHeight_) {
            return true;
        }
        staging_.Reset();
        cropView_.Reset();
        crop_.Reset();
        haveFrame_ = false;

        D3D11_TEXTURE2D_DESC cropDescription{};
        cropDescription.Width = static_cast<UINT>(width);
        cropDescription.Height = static_cast<UINT>(height);
        cropDescription.MipLevels = 0U;  // full chain
        cropDescription.ArraySize = 1U;
        cropDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        cropDescription.SampleDesc.Count = 1U;
        cropDescription.Usage = D3D11_USAGE_DEFAULT;
        cropDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        cropDescription.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(device_->CreateTexture2D(&cropDescription, nullptr, crop_.Put())) ||
            FAILED(device_->CreateShaderResourceView(crop_.Get(), nullptr, cropView_.Put()))) {
            return false;
        }

        // Smallest mip that is still at least 80x60: plenty for a median.
        mip_ = 0;
        while ((width >> (mip_ + 1)) >= 80 && (height >> (mip_ + 1)) >= 60) {
            ++mip_;
        }
        stagingWidth_ = width >> mip_;
        stagingHeight_ = height >> mip_;

        D3D11_TEXTURE2D_DESC stagingDescription{};
        stagingDescription.Width = static_cast<UINT>(stagingWidth_);
        stagingDescription.Height = static_cast<UINT>(stagingHeight_);
        stagingDescription.MipLevels = 1U;
        stagingDescription.ArraySize = 1U;
        stagingDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stagingDescription.SampleDesc.Count = 1U;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&stagingDescription, nullptr, staging_.Put()))) {
            return false;
        }
        cropWidth_ = width;
        cropHeight_ = height;
        return true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> crop_;
    ComPtr<ID3D11ShaderResourceView> cropView_;
    ComPtr<ID3D11Texture2D> staging_;
    std::wstring outputName_;
    RECT desktop_{};
    RECT lastRegion_{};
    UINT textureWidth_ = 0U;
    UINT textureHeight_ = 0U;
    int cropWidth_ = 0;
    int cropHeight_ = 0;
    int mip_ = 0;
    int stagingWidth_ = 0;
    int stagingHeight_ = 0;
    bool haveFrame_ = false;
    ULONGLONG retryAfter_ = 0U;
    std::vector<unsigned char> pixels_;
};

}  // namespace gamma_changer
