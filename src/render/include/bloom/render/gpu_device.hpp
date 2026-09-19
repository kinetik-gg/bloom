#pragma once

// Bloom-owned, Qt-free and Vulkan-free GPU device surface. Nothing in this header exposes a
// Vulkan/native handle, a Vulkan flag, or a catch-all backend interface: callers receive typed
// Bloom values, an immutable capability report, ordered structured diagnostics, and opaque
// move-only resource ownership. src/render owns the concrete Vulkan bootstrap behind a pimpl, so a
// build without Vulkan dependencies compiles the same API against an explicit Unavailable stub.
//
// A dedicated runtime GPU service will call create() only on its service thread; the created
// device records that thread and fails closed when an exposed operation is called from another
// thread. This slice does not integrate with the runtime, UI, or task system.

#include <bloom/render/gpu_presentation_types.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bloom::render {

// Per-operation-and-precision outcome. It is never a device-wide marketing tier: a Ready device
// grants no operation a qualification outcome.
enum class GpuQualification : std::uint8_t {
    Unavailable,
    PreviewOnly,
    ReferenceParity,
};

enum class GpuDeviceState : std::uint8_t {
    Initializing,
    Ready,
    Lost,
    Recovering,
    Unavailable,
    ShuttingDown,
    Stopped,
};

// The initial closed operation vocabulary from docs/architecture/gpu-backend.md. Declaring the
// vocabulary does not claim any implementation; the report's operation list grows one tested
// capability at a time instead of inventing every operation up front.
enum class GpuOperationId : std::uint8_t {
    SolidV1,
    TranslationOpacityBilinearV1,
    SourceOverV1,
    OcioDisplayV1,
    PackedDisplayV1,
};

enum class GpuPrecision : std::uint8_t {
    Rgba32f,
    Rgba16f,
    PackedRgba8,
};

// Typed reason a device or resource operation could not be completed. The code is stable and
// machine-checkable; the message is a bounded, human-readable diagnostic.
enum class GpuDiagnosticCode : std::uint8_t {
    None,
    BackendNotBuilt,
    LoaderUnavailable,
    InstanceCreationFailed,
    NoPhysicalDevice,
    DeviceIncompatible,
    DeviceCreationFailed,
    AllocatorUnavailable,
    AllocationLimitExceeded,
    BufferAllocationFailed,
    WrongThread,
    InvalidArgument,
    DeviceUnavailable,
};

struct GpuDiagnostic final {
    GpuDiagnosticCode code = GpuDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuDiagnostic&, const GpuDiagnostic&) = default;
};

// Selected backend, physical device, and driver identity. This is diagnostic evidence and cache
// identity, not project authoring state.
struct GpuDeviceIdentity final {
    std::string backend;
    std::string device_name;
    std::string driver;
    std::uint32_t driver_version = 0;
    std::uint32_t api_version_major = 0;
    std::uint32_t api_version_minor = 0;
    std::uint32_t vendor_id = 0;
    std::string device_type;

    friend bool operator==(const GpuDeviceIdentity&, const GpuDeviceIdentity&) = default;
};

// One operation-and-precision entry. A missing entry means Unavailable; unsupported operations do
// not inherit another operation's outcome. `qualification` stays Unavailable until the frozen
// fixture gate passes, and the ordered `diagnostics` explain why it has not.
struct GpuOperationCapability final {
    GpuOperationId operation = GpuOperationId::SolidV1;
    GpuPrecision precision = GpuPrecision::Rgba32f;
    GpuQualification qualification = GpuQualification::Unavailable;
    std::vector<std::string> required_features;
    std::string tested_revision;
    std::string numeric_contract;
    std::string fixture_set_digest;
    std::vector<GpuDiagnostic> diagnostics;
};

// Immutable, generation-scoped report. `Ready` means the bootstrap requirements needed to probe
// and submit work passed; it grants no operation qualification.
struct GpuCapabilityReport final {
    std::uint32_t generation = 0;
    GpuDeviceState state = GpuDeviceState::Unavailable;
    GpuDeviceIdentity identity;
    bool compute_queue = false;
    bool timeline_semaphore = false;
    bool memory_budget_supported = false;
    // Sum of DEVICE_LOCAL memory heap sizes. A bounded diagnostic fact, not a VRAM capacity or a
    // usable allocation budget: integrated GPUs share host memory and drivers may migrate blocks.
    std::uint64_t device_memory_bytes = 0;
    // Presentation bootstrap facts for this generation. NotRequested unless create() was asked for
    // presentation; Ready only when every required capability was actually enabled.
    GpuPresentationStatus presentation;
    std::vector<GpuOperationCapability> operations;
};

// Facts about one opaque allocation. No native handle is exposed.
struct GpuBufferInfo final {
    std::uint64_t size_bytes = 0;
    bool host_visible = false;
};

class GpuDevice;
class GpuRendererAccess;

// Opaque, move-only ownership of one allocation. An allocation co-owns the device's allocator
// generation, so the underlying Vulkan allocator and device handles stay alive until the last
// buffer is released even if the GpuDevice object is destroyed first. The GpuDevice object itself
// must still be destroyed on its creating owner thread, and releasing an allocation on any other
// thread trips that same owner-thread assertion.
class GpuBufferAllocation final {
  public:
    GpuBufferAllocation() noexcept;
    GpuBufferAllocation(const GpuBufferAllocation&) = delete;
    GpuBufferAllocation& operator=(const GpuBufferAllocation&) = delete;
    GpuBufferAllocation(GpuBufferAllocation&& other) noexcept;
    GpuBufferAllocation& operator=(GpuBufferAllocation&& other) noexcept;
    ~GpuBufferAllocation();

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] GpuBufferInfo info() const noexcept;

  private:
    friend class GpuDevice;

    struct Impl;
    explicit GpuBufferAllocation(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuBufferAllocationResult final {
    GpuBufferAllocation allocation;
    GpuDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return allocation.isValid(); }
    explicit operator bool() const noexcept { return hasValue(); }
};

// Explicit loader override for controlled tests and diagnostics. Production callers leave this
// empty and receive the platform loader name; no workspace or build path is ever hardcoded.
struct GpuDeviceCreationOptions final {
    std::filesystem::path loader_path;
    // Optional presentation bootstrap. The default (false / None) reproduces the compute-only
    // device exactly as before. When requested, only actually-supported surface extensions are
    // enabled; a missing capability leaves the compute path intact and reports a typed Unavailable
    // status.
    bool request_presentation = false;
    GpuPresentationPlatform presentation_platform = GpuPresentationPlatform::None;
};

struct GpuDeviceCreationResult;

class GpuDevice final {
  public:
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;
    GpuDevice(GpuDevice&& other) noexcept;
    GpuDevice& operator=(GpuDevice&& other) noexcept;
    ~GpuDevice();

    // Probe and bootstrap one logical device generation on the calling thread. A missing loader,
    // incompatible or absent device, or allocator failure produces a typed Unavailable diagnostic
    // and a null device instead of a crash.
    [[nodiscard]] static GpuDeviceCreationResult
    create(const GpuDeviceCreationOptions& options = {});

    [[nodiscard]] GpuDeviceState state() const noexcept;
    // True only on the thread that created this device generation. False for a null/moved-from or
    // portable-stub device. No allocation or native call is involved.
    [[nodiscard]] bool isOwnerThread() const noexcept;
    [[nodiscard]] const GpuCapabilityReport& capabilityReport() const noexcept;

    // Unique ownership identity for this exact GpuDevice generation. It is minted once per created
    // device from the same per-process monotonic counter that backs the presentation epoch, so two
    // GpuDevice instances -- even two created from the same physical GPU -- never share it. The
    // bootstrap capability report's generation is always 1 and cannot distinguish two instances;
    // qualification identity must use this instead. Zero only for a null/moved-from or
    // portable-stub device. No native handle is exposed.
    [[nodiscard]] std::uint64_t ownershipEpoch() const noexcept;

    // Missing operation/precision entries are Unavailable, matching the architecture contract.
    [[nodiscard]] GpuQualification qualificationFor(GpuOperationId operation,
                                                    GpuPrecision precision) const noexcept;

    // Presentation bootstrap facts for this device generation. Ready means the requested platform's
    // surface extension, VK_KHR_surface, VK_KHR_swapchain, and a present-capable queue were all
    // actually enabled; it never claims support for any specific surface.
    [[nodiscard]] GpuPresentationStatus presentationStatus() const noexcept;

    // Non-owning borrowed view of the live instance for a UI-side QVulkanInstance::setVkInstance().
    // The returned fields are immutable after create(), so this is safe to call from the UI thread.
    // `valid` is false unless presentationStatus() is Ready. The caller must keep this GpuDevice
    // alive on its owner thread until every borrowed surface is retired and acknowledged; this view
    // never owns or destroys the instance.
    [[nodiscard]] GpuBorrowedInstanceView borrowedInstanceView() const noexcept;

    // Owner-thread validation of a surface the UI created against the borrowed instance. Device
    // state, presentation epoch, and calling thread are checked BEFORE any driver call; a stale
    // epoch or a non-owner thread is rejected without touching the driver. Only then is
    // vkGetPhysicalDeviceSurfaceSupportKHR queried on the preselected present queue family.
    [[nodiscard]] GpuSurfaceSupportResult
    validateBorrowedSurface(const GpuBorrowedSurface& surface) const;

    // Bounded host-visible buffer allocation. Exposed here as the minimal owned resource that
    // proves the allocator is initialized with explicit dynamic functions.
    [[nodiscard]] GpuBufferAllocationResult allocateHostBuffer(std::uint64_t size_bytes);

  private:
    friend class GpuRendererAccess;

    struct Impl;
    explicit GpuDevice(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

struct GpuDeviceCreationResult final {
    std::unique_ptr<GpuDevice> device;
    GpuDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return device != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }
};

// Upper bound checked before any allocator call. Deployment policy may lower it later; the
// allocation path must never silently clamp or overflow.
inline constexpr std::uint64_t kMaxGpuHostBufferBytes = 64ULL * 1024ULL * 1024ULL;

} // namespace bloom::render
