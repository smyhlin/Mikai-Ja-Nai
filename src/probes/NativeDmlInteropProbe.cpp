// Hand-run capability probe for D3D11 shared-texture import into Qt's WGL-backed
// QOpenGLWidget framebuffer. This proves only the interop primitive; it does not
// load DirectML, ORT, aji_dml, or claim inference/provider delivery.

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#include <QStringList>
#include <QTimer>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kTextureSide = 128;
constexpr GLenum kGlTexture2D = 0x0DE1;
constexpr GLenum kGlFramebufferComplete = 0x8CD5;
constexpr GLenum kWglAccessReadOnlyNv = 0x0000;
constexpr GLenum kWglAccessReadWriteNv = 0x0001;

// include/aji.h's DirectML frame contract: the caller's textures must be created
// with SHARED + SHARED_NTHANDLE, because aji_dml opens them as D3D12 resources
// through IDXGIResource1::CreateSharedHandle.
constexpr UINT kAjiContractMiscFlags =
    D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
// WDDM legacy share-handle sharing. The Direct3D 11 documentation for
// D3D11_RESOURCE_MISC_SHARED_NTHANDLE states that setting it "deprecate[s] the
// use of existing HANDLE values", which is the handle WGL's D3D11 open path
// consumes; this second form is probed to establish whether importing a
// DirectML-contract texture is possible at all on the observed adapter.
constexpr UINT kLegacyMiscFlags = D3D11_RESOURCE_MISC_SHARED;

using WglGetExtensionsStringArb = const char*(WINAPI*)(HDC);
using WglDxOpenDeviceNv = HANDLE(WINAPI*)(void*);
using WglDxCloseDeviceNv = BOOL(WINAPI*)(HANDLE);
using WglDxRegisterObjectNv = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using WglDxUnregisterObjectNv = BOOL(WINAPI*)(HANDLE, HANDLE);
using WglDxLockObjectsNv = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
using WglDxUnlockObjectsNv = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
using WglDxSetResourceShareHandleNv = BOOL(WINAPI*)(void*, HANDLE);

enum class SharingMode { AjiNtHandle, LegacyShareHandle };

QString sharingModeName(SharingMode mode) {
    return mode == SharingMode::AjiNtHandle ? QStringLiteral("aji-shared-nthandle")
                                            : QStringLiteral("legacy-shared-handle");
}

QString miscFlagsText(SharingMode mode) {
    return mode == SharingMode::AjiNtHandle ? QStringLiteral("SHARED|SHARED_NTHANDLE")
                                            : QStringLiteral("SHARED");
}

// The WGL_NV_DX_interop specification defines the exact failure reporting for
// every entry point as GetLastError with a fixed error set; naming those values
// keeps the measured reason factual instead of a generic "rejected".
QString win32Text(DWORD error) {
    const char* name = nullptr;
    switch (error) {
        case ERROR_SUCCESS: name = "ERROR_SUCCESS"; break;
        case ERROR_INVALID_HANDLE: name = "ERROR_INVALID_HANDLE"; break;
        case ERROR_INVALID_DATA: name = "ERROR_INVALID_DATA"; break;
        case ERROR_OPEN_FAILED: name = "ERROR_OPEN_FAILED"; break;
        case ERROR_NOT_SUPPORTED: name = "ERROR_NOT_SUPPORTED"; break;
        case ERROR_BUSY: name = "ERROR_BUSY"; break;
        case ERROR_LOCK_FAILED: name = "ERROR_LOCK_FAILED"; break;
        case ERROR_NOT_LOCKED: name = "ERROR_NOT_LOCKED"; break;
        default: break;
    }
    return name != nullptr ? QStringLiteral("%1 (%2)").arg(static_cast<qulonglong>(error)).arg(QString::fromLatin1(name))
                           : QStringLiteral("%1").arg(static_cast<qulonglong>(error));
}

struct WglInterop {
    WglGetExtensionsStringArb getExtensions = nullptr;
    WglDxOpenDeviceNv openDevice = nullptr;
    WglDxCloseDeviceNv closeDevice = nullptr;
    WglDxRegisterObjectNv registerObject = nullptr;
    WglDxUnregisterObjectNv unregisterObject = nullptr;
    WglDxLockObjectsNv lockObjects = nullptr;
    WglDxUnlockObjectsNv unlockObjects = nullptr;
    WglDxSetResourceShareHandleNv setShareHandle = nullptr;
    bool extension = false;
    bool extension2 = false;
    bool functions = false;
};

enum class Outcome { NotRun, Imported, Passed, CapabilityUnavailable, Failed };

struct SharingAttempt {
    SharingMode mode = SharingMode::AjiNtHandle;
    QString adapter;
    bool textureCreated = false;
    QString textureError;
    QString sharedHandleError;
    bool shareHandleSet = false;
    QString shareHandleSetError;
    bool registered = false;
    QString registerAccess;
    QString registerError;

    QString summary() const {
        const QString head = QStringLiteral("%1 [%2]").arg(sharingModeName(mode), adapter);
        if (registered) return QStringLiteral("%1 imported").arg(head);
        if (!textureError.isEmpty()) return QStringLiteral("%1 texture creation failed: %2").arg(head, textureError);
        return QStringLiteral("%1 wglDXRegisterObjectNV failed: %2").arg(head, registerError);
    }

    QJsonObject json() const {
        return QJsonObject{
            {QStringLiteral("mode"), sharingModeName(mode)},
            {QStringLiteral("miscFlags"), miscFlagsText(mode)},
            {QStringLiteral("d3dAdapter"), adapter},
            {QStringLiteral("textureCreated"), textureCreated},
            {QStringLiteral("textureError"), textureError},
            {QStringLiteral("sharedHandleError"), sharedHandleError},
            {QStringLiteral("shareHandleSet"), shareHandleSet},
            {QStringLiteral("shareHandleSetError"), shareHandleSetError},
            {QStringLiteral("wglDXRegisterObjectNV"), registered},
            {QStringLiteral("registerAccess"), registerAccess},
            {QStringLiteral("registerLastError"), registerError},
        };
    }
};

QString hrText(HRESULT hr) {
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

QString luidText(const LUID& luid) {
    return QStringLiteral("%1:%2")
        .arg(static_cast<qulonglong>(static_cast<quint32>(luid.HighPart)), 8, 16, QLatin1Char('0'))
        .arg(static_cast<qulonglong>(luid.LowPart), 8, 16, QLatin1Char('0'));
}

QString adapterName(const DXGI_ADAPTER_DESC1& desc) {
    size_t length = 0;
    while (length < 128 && desc.Description[length] != L'\0') ++length;
    return QString::fromWCharArray(desc.Description, static_cast<qsizetype>(length));
}

bool hasExtension(const char* list, const char* extension) {
    if (list == nullptr || extension == nullptr || *extension == '\0' || std::strchr(extension, ' ') != nullptr)
        return false;
    const size_t length = std::strlen(extension);
    const char* at = list;
    while ((at = std::strstr(at, extension)) != nullptr) {
        const bool left = at == list || at[-1] == ' ';
        const char after = at[length];
        if (left && (after == '\0' || after == ' '))
            return true;
        at += length;
    }
    return false;
}

template <typename T>
T wglProc(const char* name) {
    PROC proc = wglGetProcAddress(name);
    if (proc == nullptr || proc == reinterpret_cast<PROC>(1) || proc == reinterpret_cast<PROC>(2) ||
        proc == reinterpret_cast<PROC>(3) || proc == reinterpret_cast<PROC>(-1))
        return nullptr;
    return reinterpret_cast<T>(proc);
}

WglInterop loadWglInterop(HDC dc) {
    WglInterop api;
    api.getExtensions = wglProc<WglGetExtensionsStringArb>("wglGetExtensionsStringARB");
    const char* extensions = api.getExtensions != nullptr ? api.getExtensions(dc) : nullptr;
    api.extension = hasExtension(extensions, "WGL_NV_DX_interop");
    api.extension2 = hasExtension(extensions, "WGL_NV_DX_interop2");
    api.openDevice = wglProc<WglDxOpenDeviceNv>("wglDXOpenDeviceNV");
    api.closeDevice = wglProc<WglDxCloseDeviceNv>("wglDXCloseDeviceNV");
    api.registerObject = wglProc<WglDxRegisterObjectNv>("wglDXRegisterObjectNV");
    api.unregisterObject = wglProc<WglDxUnregisterObjectNv>("wglDXUnregisterObjectNV");
    api.lockObjects = wglProc<WglDxLockObjectsNv>("wglDXLockObjectsNV");
    api.unlockObjects = wglProc<WglDxUnlockObjectsNv>("wglDXUnlockObjectsNV");
    api.setShareHandle = wglProc<WglDxSetResourceShareHandleNv>("wglDXSetResourceShareHandleNV");
    api.functions = api.openDevice != nullptr && api.closeDevice != nullptr && api.registerObject != nullptr &&
                    api.unregisterObject != nullptr && api.lockObjects != nullptr && api.unlockObjects != nullptr;
    return api;
}

QJsonObject colorJson(const QColor& color) {
    return QJsonObject{{QStringLiteral("r"), color.red()}, {QStringLiteral("g"), color.green()},
                       {QStringLiteral("b"), color.blue()}, {QStringLiteral("a"), color.alpha()}};
}

class DmlInteropWidget final : public QOpenGLWidget {
public:
    explicit DmlInteropWidget(QString pngPath) : pngPath_(std::move(pngPath)) {
        setWindowTitle(QStringLiteral("D3D11 to Qt WGL interop capability probe"));
        resize(kTextureSide, kTextureSide);
    }

    ~DmlInteropWidget() override {
        if (context() != nullptr && context()->isValid()) {
            makeCurrent();
            cleanupInterop();
            doneCurrent();
        }
    }

    bool finished() const { return finished_; }
    int exitCode() const { return exitCode_; }
    void finishProbe() { finishProbeImpl(); }

protected:
    void initializeGL() override {
        glReady_ = gl_.initializeOpenGLFunctions();
        if (!glReady_) {
            unavailable(QStringLiteral("Qt could not initialize the requested OpenGL 3.3 core functions"));
            return;
        }
        const GLubyte* renderer = gl_.glGetString(GL_RENDERER);
        const GLubyte* version = gl_.glGetString(GL_VERSION);
        glRenderer_ = renderer != nullptr ? QString::fromLatin1(reinterpret_cast<const char*>(renderer))
                                         : QStringLiteral("unknown");
        glVersion_ = version != nullptr ? QString::fromLatin1(reinterpret_cast<const char*>(version))
                                       : QStringLiteral("unknown");
        interop_ = loadWglInterop(wglGetCurrentDC());
        if (!interop_.extension || !interop_.functions) {
            QStringList missing;
            if (!interop_.extension) missing.append(QStringLiteral("WGL_NV_DX_interop extension"));
            if (interop_.openDevice == nullptr) missing.append(QStringLiteral("wglDXOpenDeviceNV"));
            if (interop_.closeDevice == nullptr) missing.append(QStringLiteral("wglDXCloseDeviceNV"));
            if (interop_.registerObject == nullptr) missing.append(QStringLiteral("wglDXRegisterObjectNV"));
            if (interop_.unregisterObject == nullptr) missing.append(QStringLiteral("wglDXUnregisterObjectNV"));
            if (interop_.lockObjects == nullptr) missing.append(QStringLiteral("wglDXLockObjectsNV"));
            if (interop_.unlockObjects == nullptr) missing.append(QStringLiteral("wglDXUnlockObjectsNV"));
            unavailable(QStringLiteral("missing %1; WGL_NV_DX_interop2=%2")
                            .arg(missing.join(QStringLiteral(", ")),
                                 interop_.extension2 ? QStringLiteral("yes") : QStringLiteral("no")));
            return;
        }
        tryAdapters();
    }

    void paintGL() override {
        if (!glReady_) return;
        if (outcome_ != Outcome::Imported || finished_) {
            gl_.glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
            gl_.glClearColor(0.035f, 0.035f, 0.035f, 1.0f);
            gl_.glClear(GL_COLOR_BUFFER_BIT);
            return;
        }
        HANDLE object = glObject_;
        SetLastError(ERROR_SUCCESS);
        if (interop_.lockObjects(interopDevice_, 1, &object) != TRUE) {
            fail(QStringLiteral("wglDXLockObjectsNV failed for the registered D3D11 texture: GetLastError %1")
                     .arg(win32Text(GetLastError())));
            return;
        }
        locked_ = true;
        // Every GL access to an interop object, including attaching it to a
        // framebuffer and querying that framebuffer, must happen while locked.
        if (sourceFramebuffer_ == 0) gl_.glGenFramebuffers(1, &sourceFramebuffer_);
        gl_.glBindFramebuffer(GL_FRAMEBUFFER, sourceFramebuffer_);
        gl_.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, kGlTexture2D, glTexture_, 0);
        const GLenum status = gl_.glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != kGlFramebufferComplete) {
            gl_.glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
            unlockAfterDraw();
            fail(QStringLiteral("imported D3D11 texture did not form a complete GL framebuffer while locked (0x%1)")
                     .arg(static_cast<quint32>(status), 4, 16, QLatin1Char('0')));
            return;
        }
        gl_.glBindFramebuffer(GL_READ_FRAMEBUFFER, sourceFramebuffer_);
        gl_.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, defaultFramebufferObject());
        gl_.glBlitFramebuffer(0, 0, kTextureSide, kTextureSide,
                              0, 0, width(), height(), GL_COLOR_BUFFER_BIT, GL_NEAREST);
        const GLenum glError = gl_.glGetError();
        gl_.glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
        if (glError != GL_NO_ERROR) {
            unlockAfterDraw();
            fail(QStringLiteral("OpenGL blit failed with error 0x%1")
                     .arg(static_cast<quint32>(glError), 4, 16, QLatin1Char('0')));
            return;
        }
        if (!unlockAfterDraw()) {
            fail(QStringLiteral("wglDXUnlockObjectsNV failed after the framebuffer blit: GetLastError %1")
                     .arg(lastWin32Error_));
            return;
        }
        painted_ = true;
    }

private:
    void tryAdapters() {
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) {
            unavailable(QStringLiteral("CreateDXGIFactory1 failed: %1").arg(hrText(hr)));
            return;
        }

        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) {
                unavailable(QStringLiteral("IDXGIFactory1::EnumAdapters1 failed: %1").arg(hrText(hr)));
                return;
            }
            DXGI_ADAPTER_DESC1 desc{};
            hr = adapter->GetDesc1(&desc);
            if (FAILED(hr)) continue;
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

            const QString name = adapterName(desc);
            const QString luid = luidText(desc.AdapterLuid);
            attemptedAdapters_.append(QStringLiteral("%1 [%2]").arg(name, luid));

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> deviceContext;
            D3D_FEATURE_LEVEL obtained{};
            static constexpr D3D_FEATURE_LEVEL featureLevels[] = {
                D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
                                   static_cast<UINT>(std::size(featureLevels)), D3D11_SDK_VERSION,
                                   device.GetAddressOf(), &obtained, deviceContext.GetAddressOf());
            if (hr == E_INVALIDARG) {
                static constexpr D3D_FEATURE_LEVEL fallback[] = {D3D_FEATURE_LEVEL_11_0};
                hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, fallback,
                                       static_cast<UINT>(std::size(fallback)), D3D11_SDK_VERSION,
                                       device.GetAddressOf(), &obtained, deviceContext.GetAddressOf());
            }
            if (FAILED(hr)) continue;

            HANDLE candidateInterop = interop_.openDevice(device.Get());
            if (candidateInterop == nullptr) {
                openDeviceFailures_.append(QStringLiteral("%1 [%2]: wglDXOpenDeviceNV GetLastError %3")
                                               .arg(name, luid, win32Text(GetLastError())));
                continue;
            }

            ComPtr<ID3D11DeviceContext1> context1;
            hr = deviceContext.As(&context1);
            if (FAILED(hr)) {
                if (interop_.closeDevice(candidateInterop) != TRUE) {
                    unavailable(QStringLiteral("ID3D11DeviceContext1 is unavailable and WGL interop device close failed for adapter %1 [%2]")
                                    .arg(name, luid));
                    return;
                }
                continue;
            }

            // The aji.h DirectML texture contract is tried first; the legacy
            // share-handle form is tried only if that contract cannot be
            // registered, and its outcome is reported separately.
            if (createAndRegisterTexture(device.Get(), deviceContext.Get(), context1.Get(),
                                         candidateInterop, name, SharingMode::AjiNtHandle) ||
                createAndRegisterTexture(device.Get(), deviceContext.Get(), context1.Get(),
                                         candidateInterop, name, SharingMode::LegacyShareHandle)) {
                d3dAdapter_ = name;
                d3dLuid_ = luid;
                d3dVendorId_ = desc.VendorId;
                d3dDeviceId_ = desc.DeviceId;
                d3dFeatureLevel_ = static_cast<quint32>(obtained);
                device_ = std::move(device);
                deviceContext_ = std::move(deviceContext);
                outcome_ = Outcome::Imported;
                update();
                return;
            }

            if (!cleanupInterop()) {
                unavailable(QStringLiteral("WGL interop cleanup failed after texture registration attempts on %1 [%2]")
                                .arg(name, luid));
                return;
            }
            if (interop_.closeDevice(candidateInterop) != TRUE) {
                unavailable(QStringLiteral("WGL interop device close failed after texture setup refusal on %1 [%2]; GetLastError %3")
                                .arg(name, luid, win32Text(GetLastError())));
                return;
            }
        }

        if (attemptedAdapters_.isEmpty()) {
            unavailable(QStringLiteral("no non-software DXGI hardware adapter was enumerated"));
            return;
        }
        QString reason = QStringLiteral("no enumerated D3D11 hardware adapter could be registered by WGL_NV_DX_interop; attempted %1; attempts: %2")
                             .arg(attemptedAdapters_.join(QStringLiteral("; ")), attemptSummary());
        if (!openDeviceFailures_.isEmpty())
            reason.append(QStringLiteral("; device open failures: %1"))
                .append(openDeviceFailures_.join(QStringLiteral("; ")));
        unavailable(reason);
    }

    bool createAndRegisterTexture(ID3D11Device* device, ID3D11DeviceContext* deviceContext,
                                  ID3D11DeviceContext1* context1, HANDLE candidateInterop,
                                  const QString& adapter, SharingMode mode) {
        SharingAttempt attempt;
        attempt.mode = mode;
        attempt.adapter = adapter;
        activeMode_ = mode;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kTextureSide;
        desc.Height = kTextureSide;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // SHADER_RESOURCE is required because the resource is registered as
        // GL_TEXTURE_2D, i.e. the GL side consumes it as a sampled resource, and
        // D3D11 cannot form a shader resource view without the bind flag.
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = mode == SharingMode::AjiNtHandle ? kAjiContractMiscFlags : kLegacyMiscFlags;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, texture_.GetAddressOf());
        if (FAILED(hr)) {
            attempt.textureError = QStringLiteral("CreateTexture2D(%1) failed: %2")
                                       .arg(miscFlagsText(mode), hrText(hr));
            attempts_.push_back(std::move(attempt));
            return false;
        }
        attempt.textureCreated = true;
        textureCreated_ = true;
        hr = device->CreateRenderTargetView(texture_.Get(), nullptr, renderTarget_.GetAddressOf());
        if (FAILED(hr)) {
            attempt.textureError = QStringLiteral("CreateRenderTargetView failed: %1").arg(hrText(hr));
            texture_.Reset();
            attempts_.push_back(std::move(attempt));
            return false;
        }

        static constexpr std::array<float, 4> background{0.0625f, 0.0625f, 0.0625f, 1.0f};
        deviceContext->ClearRenderTargetView(renderTarget_.Get(), background.data());
        const std::array<D3D11_RECT, 4> rects{{
            {0, 0, kTextureSide / 2, kTextureSide / 2},
            {kTextureSide / 2, 0, kTextureSide, kTextureSide / 2},
            {0, kTextureSide / 2, kTextureSide / 2, kTextureSide},
            {kTextureSide / 2, kTextureSide / 2, kTextureSide, kTextureSide},
        }};
        static constexpr std::array<std::array<float, 4>, 4> colors{{
            {{1.0f, 0.0f, 0.0f, 1.0f}},
            {{0.0f, 1.0f, 0.0f, 1.0f}},
            {{0.0f, 0.0f, 1.0f, 1.0f}},
            {{1.0f, 1.0f, 0.0f, 1.0f}},
        }};
        for (size_t index = 0; index < rects.size(); ++index)
            context1->ClearView(renderTarget_.Get(), colors[index].data(), &rects[index], 1);
        deviceContext->Flush();

        if (mode == SharingMode::LegacyShareHandle) {
            ComPtr<IDXGIResource> resource;
            hr = texture_.As(&resource);
            if (FAILED(hr)) {
                attempt.sharedHandleError = QStringLiteral("IDXGIResource query failed: %1").arg(hrText(hr));
            } else {
                HANDLE shareHandle = nullptr;
                hr = resource->GetSharedHandle(&shareHandle);
                if (FAILED(hr)) {
                    attempt.sharedHandleError = QStringLiteral("IDXGIResource::GetSharedHandle failed: %1").arg(hrText(hr));
                } else if (interop_.setShareHandle != nullptr) {
                    SetLastError(ERROR_SUCCESS);
                    if (interop_.setShareHandle(texture_.Get(), shareHandle) == TRUE)
                        attempt.shareHandleSet = true;
                    else
                        attempt.shareHandleSetError = win32Text(GetLastError());
                }
            }
        }

        gl_.glGenTextures(1, &glTexture_);
        gl_.glBindTexture(kGlTexture2D, glTexture_);
        gl_.glTexParameteri(kGlTexture2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl_.glTexParameteri(kGlTexture2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl_.glTexParameteri(kGlTexture2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl_.glTexParameteri(kGlTexture2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_.glBindTexture(kGlTexture2D, 0);

        SetLastError(ERROR_SUCCESS);
        HANDLE object = interop_.registerObject(candidateInterop, texture_.Get(), glTexture_,
                                                kGlTexture2D, kWglAccessReadOnlyNv);
        DWORD firstError = object == nullptr ? GetLastError() : ERROR_SUCCESS;
        attempt.registerAccess = QStringLiteral("WGL_ACCESS_READ_ONLY_NV");
        if (object == nullptr) {
            attempt.registerError = win32Text(firstError);
            // ERROR_INVALID_DATA is the specification's error for an incorrect
            // <name>/<type>/<access>; re-issuing with the extension's own sample
            // access value isolates the <access> parameter from the others. An
            // unset error is retried the same way because it carries no fact.
            if (firstError == ERROR_INVALID_DATA || firstError == ERROR_SUCCESS) {
                SetLastError(ERROR_SUCCESS);
                object = interop_.registerObject(candidateInterop, texture_.Get(), glTexture_,
                                                 kGlTexture2D, kWglAccessReadWriteNv);
                if (object == nullptr) {
                    attempt.registerAccess = QStringLiteral("WGL_ACCESS_READ_WRITE_NV (retry)");
                    attempt.registerError = QStringLiteral("READ_ONLY %1; READ_WRITE %2")
                                                .arg(win32Text(firstError), win32Text(GetLastError()));
                } else {
                    attempt.registerAccess = QStringLiteral("WGL_ACCESS_READ_WRITE_NV (retried after READ_ONLY %1)")
                                                 .arg(win32Text(firstError));
                    attempt.registerError.clear();
                }
            }
        }
        if (object == nullptr) {
            renderTarget_.Reset();
            texture_.Reset();
            if (glTexture_ != 0) gl_.glDeleteTextures(1, &glTexture_);
            glTexture_ = 0;
            attempts_.push_back(std::move(attempt));
            return false;
        }

        attempt.registered = true;
        attempts_.push_back(std::move(attempt));
        interopDevice_ = candidateInterop;
        glObject_ = object;
        sharingMode_ = sharingModeName(mode);
        textureImported_ = true;
        return true;
    }

    bool unlockAfterDraw() {
        if (!locked_) return true;
        HANDLE object = glObject_;
        SetLastError(ERROR_SUCCESS);
        const BOOL unlocked = interop_.unlockObjects(interopDevice_, 1, &object);
        lastWin32Error_ = unlocked == TRUE ? QString() : win32Text(GetLastError());
        locked_ = false;
        return unlocked == TRUE;
    }

    bool cleanupInterop() {
        bool ok = true;
        if (locked_ && !unlockAfterDraw()) ok = false;
        if (glReady_) {
            gl_.glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
            if (sourceFramebuffer_ != 0) gl_.glDeleteFramebuffers(1, &sourceFramebuffer_);
            sourceFramebuffer_ = 0;
        }
        if (interopDevice_ != nullptr && glObject_ != nullptr) {
            SetLastError(ERROR_SUCCESS);
            if (interop_.unregisterObject(interopDevice_, glObject_) != TRUE) {
                cleanupError_ = QStringLiteral("wglDXUnregisterObjectNV GetLastError %1").arg(win32Text(GetLastError()));
                ok = false;
            }
            glObject_ = nullptr;
        }
        if (interopDevice_ != nullptr) {
            SetLastError(ERROR_SUCCESS);
            if (interop_.closeDevice(interopDevice_) != TRUE) {
                cleanupError_ = cleanupError_.isEmpty()
                                    ? QStringLiteral("wglDXCloseDeviceNV GetLastError %1").arg(win32Text(GetLastError()))
                                    : QStringLiteral("%1; wglDXCloseDeviceNV GetLastError %2")
                                          .arg(cleanupError_, win32Text(GetLastError()));
                ok = false;
            }
            interopDevice_ = nullptr;
        }
        if (glReady_ && glTexture_ != 0) gl_.glDeleteTextures(1, &glTexture_);
        glTexture_ = 0;
        renderTarget_.Reset();
        texture_.Reset();
        deviceContext_.Reset();
        device_.Reset();
        return ok;
    }

    void unavailable(QString reason) {
        outcome_ = Outcome::CapabilityUnavailable;
        reason_ = std::move(reason);
        update();
    }

    void fail(QString reason) {
        outcome_ = Outcome::Failed;
        reason_ = std::move(reason);
        update();
    }

    static bool nearColor(const QColor& actual, const QColor& expected) {
        constexpr int tolerance = 3;
        return std::abs(actual.red() - expected.red()) <= tolerance &&
               std::abs(actual.green() - expected.green()) <= tolerance &&
               std::abs(actual.blue() - expected.blue()) <= tolerance &&
               actual.alpha() >= 250;
    }

    bool patternMatches(const QImage& image, bool verticallyFlipped) const {
        if (image.isNull() || image.width() < 8 || image.height() < 8) return false;
        const int left = image.width() / 4;
        const int right = image.width() * 3 / 4;
        const int top = image.height() / 4;
        const int bottom = image.height() * 3 / 4;
        const int topY = verticallyFlipped ? bottom : top;
        const int bottomY = verticallyFlipped ? top : bottom;
        const std::array<QColor, 4> actual{
            image.pixelColor(left, topY), image.pixelColor(right, topY),
            image.pixelColor(left, bottomY), image.pixelColor(right, bottomY)};
        static const std::array<QColor, 4> expected{
            QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255), QColor(255, 255, 0)};
        for (size_t index = 0; index < expected.size(); ++index)
            if (!nearColor(actual[index], expected[index])) return false;
        return true;
    }

    void finishProbeImpl() {
        if (finished_ || finishing_) return;
        finishing_ = true;
        if (context() == nullptr || !context()->isValid()) {
            unavailable(QStringLiteral("Qt did not create a valid desktop OpenGL context"));
        }
        QImage image;
        if (outcome_ == Outcome::Imported) {
            image = grabFramebuffer();
            // grabFramebuffer() repaints through paintGL, which may have failed at
            // a named operation; that measured reason is kept rather than being
            // replaced by a pixel mismatch against the untouched clear colour.
            if (outcome_ != Outcome::Imported) {
                image = QImage();
            } else if (image.isNull()) {
                fail(QStringLiteral("Qt returned a null QOpenGLWidget framebuffer capture"));
            } else {
                const bool normal = patternMatches(image, false);
                const bool flipped = !normal && patternMatches(image, true);
                measured_ = QJsonObject{
                    {QStringLiteral("topLeft"), colorJson(image.pixelColor(image.width() / 4, image.height() / 4))},
                    {QStringLiteral("topRight"), colorJson(image.pixelColor(image.width() * 3 / 4, image.height() / 4))},
                    {QStringLiteral("bottomLeft"), colorJson(image.pixelColor(image.width() / 4, image.height() * 3 / 4))},
                    {QStringLiteral("bottomRight"), colorJson(image.pixelColor(image.width() * 3 / 4, image.height() * 3 / 4))},
                };
                if (normal || flipped) {
                    outcome_ = Outcome::Passed;
                    reason_ = flipped ? QStringLiteral("measured expected D3D11 pattern with vertical GL image orientation")
                                      : QStringLiteral("measured expected D3D11 pattern in Qt framebuffer");
                } else {
                    outcome_ = Outcome::Failed;
                    reason_ = QStringLiteral("Qt framebuffer pixels did not match the deterministic D3D11 texture pattern");
                }
                pngWritten_ = image.save(pngPath_, "PNG");
                if (!pngWritten_ && outcome_ == Outcome::Passed) {
                    outcome_ = Outcome::Failed;
                    reason_ = QStringLiteral("pixel pattern matched but PNG could not be saved to %1").arg(pngPath_);
                }
            }
        }
        if (context() == nullptr || !context()->isValid()) {
            if (outcome_ == Outcome::Imported) fail(QStringLiteral("Qt OpenGL context became invalid before cleanup"));
        } else {
            makeCurrent();
            const bool cleanupOk = cleanupInterop();
            doneCurrent();
            if (!cleanupOk)
                fail(QStringLiteral("WGL interop unlock, unregister, or device close failed during cleanup: %1")
                         .arg(cleanupError_));
        }
        finished_ = true;
        emitReport();
    }

    QJsonArray attemptsJson() const {
        QJsonArray array;
        for (const SharingAttempt& attempt : attempts_) array.append(attempt.json());
        return array;
    }

    QString attemptSummary() const {
        QStringList parts;
        for (const SharingAttempt& attempt : attempts_) parts.append(attempt.summary());
        return parts.isEmpty() ? QStringLiteral("no texture registration attempt was made")
                               : parts.join(QStringLiteral("; "));
    }

    QString requiredContractRefusal() const {
        for (const SharingAttempt& attempt : attempts_)
            if (attempt.mode == SharingMode::AjiNtHandle && !attempt.registered) return attempt.summary();
        return QStringLiteral("no aji SHARED|SHARED_NTHANDLE registration attempt was recorded");
    }

    void emitReport() {
        QString result;
        switch (outcome_) {
            case Outcome::Passed:
                if (activeMode_ == SharingMode::AjiNtHandle) {
                    result = QStringLiteral("PASS");
                    exitCode_ = 0;
                } else {
                    // The aji DirectML texture contract is the required one. Legacy
                    // SHARED registering is real pixel evidence, but it is not that
                    // contract, so it can never be an exit-0 PASS.
                    result = QStringLiteral("CAPABILITY_UNAVAILABLE");
                    exitCode_ = 2;
                    reason_ = QStringLiteral("legacy SHARED pixel evidence only: %1; the required aji texture contract (SHARED|SHARED_NTHANDLE) was refused by wglDXRegisterObjectNV [%2], so this is not a DirectML-capable interop proof")
                                  .arg(reason_, requiredContractRefusal());
                }
                break;
            case Outcome::CapabilityUnavailable: result = QStringLiteral("CAPABILITY_UNAVAILABLE"); exitCode_ = 2; break;
            case Outcome::Failed: result = QStringLiteral("FAIL"); exitCode_ = 1; break;
            case Outcome::Imported:
            case Outcome::NotRun:
                result = QStringLiteral("CAPABILITY_UNAVAILABLE");
                reason_ = QStringLiteral("probe did not complete a measured framebuffer capture");
                exitCode_ = 2;
                break;
        }
        QJsonObject report{
            {QStringLiteral("probe"), QStringLiteral("native_dml_interop_probe")},
            {QStringLiteral("result"), result},
            {QStringLiteral("reason"), reason_},
            {QStringLiteral("glRenderer"), glRenderer_},
            {QStringLiteral("glVersion"), glVersion_},
            {QStringLiteral("wglNvDxInterop"), interop_.extension},
            {QStringLiteral("wglNvDxInterop2"), interop_.extension2},
            {QStringLiteral("requiredWglFunctions"), interop_.functions},
            {QStringLiteral("requiredWglFunctionsResolved"), interop_.functions},
            {QStringLiteral("wglFunctions"),
             QJsonObject{
                 {QStringLiteral("wglGetExtensionsStringARB"), interop_.getExtensions != nullptr},
                 {QStringLiteral("wglDXOpenDeviceNV"), interop_.openDevice != nullptr},
                 {QStringLiteral("wglDXCloseDeviceNV"), interop_.closeDevice != nullptr},
                 {QStringLiteral("wglDXRegisterObjectNV"), interop_.registerObject != nullptr},
                 {QStringLiteral("wglDXUnregisterObjectNV"), interop_.unregisterObject != nullptr},
                 {QStringLiteral("wglDXLockObjectsNV"), interop_.lockObjects != nullptr},
                 {QStringLiteral("wglDXUnlockObjectsNV"), interop_.unlockObjects != nullptr},
             }},
            {QStringLiteral("wglDxSetResourceShareHandleNVResolved"), interop_.setShareHandle != nullptr},
            {QStringLiteral("attemptedD3dAdapters"), QJsonArray::fromStringList(attemptedAdapters_)},
            {QStringLiteral("d3dAdapter"), d3dAdapter_},
            {QStringLiteral("d3dAdapterLuid"), d3dLuid_},
            {QStringLiteral("d3dVendorId"), static_cast<int>(d3dVendorId_)},
            {QStringLiteral("d3dDeviceId"), static_cast<int>(d3dDeviceId_)},
            {QStringLiteral("d3dFeatureLevel"), static_cast<int>(d3dFeatureLevel_)},
            {QStringLiteral("requestedTextureFormat"), QStringLiteral("DXGI_FORMAT_R8G8B8A8_UNORM")},
            {QStringLiteral("requestedTextureMiscFlags"), QStringLiteral("SHARED|SHARED_NTHANDLE")},
            {QStringLiteral("requiredAjiTextureMiscFlags"), QStringLiteral("SHARED|SHARED_NTHANDLE")},
            {QStringLiteral("testedTextureMiscFlags"),
             activeMode_ == SharingMode::AjiNtHandle ? QStringLiteral("SHARED|SHARED_NTHANDLE")
                                                     : QStringLiteral("SHARED")},
            {QStringLiteral("sharingMode"), sharingMode_},
            {QStringLiteral("ajiTextureContractRegistrable"),
             outcome_ == Outcome::Passed && activeMode_ == SharingMode::AjiNtHandle},
            {QStringLiteral("sharingAttempts"), attemptsJson()},
            {QStringLiteral("textureCreated"), textureCreated_},
            {QStringLiteral("d3dTextureImported"), textureImported_},
            {QStringLiteral("framebufferBlit"), painted_},
            {QStringLiteral("measuredColors"), measured_},
            {QStringLiteral("pngPath"), pngPath_},
            {QStringLiteral("pngWritten"), pngWritten_},
            {QStringLiteral("scope"), QStringLiteral("D3D11 shared texture to Qt WGL framebuffer interop only; no DirectML, ORT, aji_dml, or inference claim")},
            {QStringLiteral("directMlMayBeEnabled"), false},
        };
        std::fprintf(stdout, "%s\n", QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
        QCoreApplication::exit(exitCode_);
    }

    QString pngPath_;
    QString glRenderer_;
    QString glVersion_;
    QString d3dAdapter_;
    QString d3dLuid_;
    QString reason_;
    QString sharingMode_;
    QString cleanupError_;
    QString lastWin32Error_;
    QStringList attemptedAdapters_;
    QStringList openDeviceFailures_;
    std::vector<SharingAttempt> attempts_;
    quint32 d3dVendorId_ = 0;
    quint32 d3dDeviceId_ = 0;
    quint32 d3dFeatureLevel_ = 0;
    QOpenGLFunctions_3_3_Core gl_;
    WglInterop interop_;
    SharingMode activeMode_ = SharingMode::AjiNtHandle;
    HANDLE interopDevice_ = nullptr;
    HANDLE glObject_ = nullptr;
    GLuint glTexture_ = 0;
    GLuint sourceFramebuffer_ = 0;
    bool glReady_ = false;
    bool locked_ = false;
    bool painted_ = false;
    bool textureCreated_ = false;
    bool textureImported_ = false;
    bool finishing_ = false;
    bool finished_ = false;
    bool pngWritten_ = false;
    int exitCode_ = 2;
    Outcome outcome_ = Outcome::NotRun;
    QJsonObject measured_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> deviceContext_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
};

}  // namespace

int main(int argc, char** argv) {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    const QString pngPath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                     : QStringLiteral("native_dml_interop_probe.png");
    DmlInteropWidget widget(pngPath);
    widget.show();

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &widget, [&widget] { widget.finishProbe(); });
    timeout.start(10000);
    QTimer::singleShot(300, &widget, [&widget] { widget.finishProbe(); });
    const int eventResult = app.exec();
    return widget.finished() ? widget.exitCode() : eventResult;
}
