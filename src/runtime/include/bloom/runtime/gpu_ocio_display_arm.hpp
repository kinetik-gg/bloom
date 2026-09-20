#pragma once

// Owner-thread GPU display arm for the general local viewer display path.
//
// This is the runtime half of the general display route: one resident RGBA32F input is transformed
// by an already-prepared, immutable DisplayRgba8 OCIO command (an exact OCIO display/view program
// for the request's own display/view pair) into one resident RGBA8 display image, entirely on the
// device, with NO full-frame host readback.
//
// It deliberately reuses the producer's GpuOcioProgramExecutor rather than a second compiler or a
// second native program cache: the executor retains native programs keyed on the command's canonical
// identity, so a warm frame with the same command performs ZERO program creation and ZERO resource
// upload and only the dispatch runs. A changed display/view (or, once the producer wrapper carries
// it, a changed exposure/gamma uniform snapshot) is a different command identity and therefore a
// different program.
//
// The arm owns no thread and no device: begin/poll/take/cancel/destruction are owner-thread only and
// fail closed from another thread. It never decodes, evaluates, or re-renders the process scene; it
// only displays the resident scene-linear image the caller already produced. The final composited
// image is read back once only at the CPU codec/file boundary by the export route, never here.
//
// ViewAdjust: the CPU reference (runtime::ViewAdjust, see view_adjust.cpp / view_adjust.hpp) applies
// exposure on the LINEAR DISPLAY LIGHT after the OCIO display function and gamma on the ENCODED
// value before RGBA8 quantization. That is a post-display operation and is NOT the same as OCIO's
// ExposureContrast transform (which acts in the source/working space before the display transform).
// The producer DisplayRgba8 wrapper bakes this exact post-display step into the program (see
// buildGpuOcioWrapperGlsl(program, ViewAdjust)), so the arm dispatches the prepared command's own
// baked adjustment for both neutral and non-neutral values. A changed adjustment is a different
// command identity; a request adjustment that differs from the prepared command's is refused rather
// than substituted, and no guessed mapping is ever used.

#include <bloom/color/ocio_builtin_registry.hpp>
#include <bloom/color/ocio_cpu_display_processor.hpp>
#include <bloom/core/sha256.hpp>
#include <bloom/render/gpu_device.hpp>
#include <bloom/render/gpu_resident_display.hpp>
#include <bloom/runtime/evaluation.hpp>
#include <bloom/runtime/gpu_ocio_command.hpp>
#include <bloom/runtime/gpu_ocio_preparation.hpp>
#include <bloom/runtime/gpu_ocio_program_executor.hpp>
#include <bloom/runtime/view_adjust.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace bloom::runtime {

// Accepted only when the command is a real DisplayRgba8 program: a ProcessEffect (FinalRgba32f)
// command is a different pixel contract and is refused rather than displayed.
[[nodiscard]] bool
gpuOcioDisplayCommandIsDisplay(const PreparedGpuOcioCommand& command) noexcept;

// The exact OCIO uniform snapshot a DisplayRgba8 command dispatch uses. Empty means "use the
// command's immutable snapshot". A caller may override same-size uniform bytes per request (for
// example a future exposure/gamma property), but the bytes must match the declared UBO size and the
// command identity is unchanged only when the override equals the snapshot; a differing override is
// a different prepared command, not a display-arm concern.
struct GpuOcioDisplayRequest final {
    std::shared_ptr<const PreparedGpuOcioCommand> command;
    std::shared_ptr<const render::GpuImage> input;
    // The exact per-request geometry identity the command was prepared for; the arm validates it
    // against the actual input before any native call.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The request's view adjustment. It must equal the prepared command's own baked adjustment; a
    // mismatch is refused (a different adjustment is a different command identity).
    ViewAdjust viewAdjust{};
    // Native transient byte budget for this one dispatch.
    std::uint64_t byteBudget = 0;
};

enum class GpuOcioDisplayArmDiagnosticCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    DeviceLost,
    InvalidRequest,
    // The command is a ProcessEffect program, not a DisplayRgba8 display program.
    NotADisplayCommand,
    // The input geometry does not match the prepared command geometry.
    IdentityMismatch,
    // Retained for compatibility. The producer DisplayRgba8 wrapper now bakes the exact post-display
    // adjustment, so this arm no longer produces it; a mismatched adjustment reports IdentityMismatch.
    ViewAdjustUnsupported,
    Busy,
    OwnerDrainRequired,
    OverBudget,
    DispatchRefused,
    Cancelled,
    NativeTimeout,
    InternalInvariant,
};

struct GpuOcioDisplayArmDiagnostic final {
    GpuOcioDisplayArmDiagnosticCode code = GpuOcioDisplayArmDiagnosticCode::None;
    std::string message;

    friend bool operator==(const GpuOcioDisplayArmDiagnostic&,
                           const GpuOcioDisplayArmDiagnostic&) = default;
};

// Applies the command's DisplayRgba8 program to `request.input` on the device owner thread and
// publishes one resident RGBA8 GpuDisplayImage. The arm performs no full-frame readback; the 4-byte
// status word (already handled inside the executor's program) is the only host read.
class GpuOcioDisplayArm;

struct GpuOcioDisplayArmCreateResult final {
    std::unique_ptr<GpuOcioDisplayArm> arm;
    GpuOcioDisplayArmDiagnostic diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return arm != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

class GpuOcioDisplayArm final {
  public:
    GpuOcioDisplayArm(const GpuOcioDisplayArm&) = delete;
    GpuOcioDisplayArm& operator=(const GpuOcioDisplayArm&) = delete;
    GpuOcioDisplayArm(GpuOcioDisplayArm&&) noexcept;
    GpuOcioDisplayArm& operator=(GpuOcioDisplayArm&&) noexcept;
    ~GpuOcioDisplayArm();

    // Device owner thread only. A foreign thread returns a typed refusal without touching Vulkan.
    [[nodiscard]] static GpuOcioDisplayArmCreateResult create(render::GpuDevice& device);

    [[nodiscard]] bool isBoundTo(const render::GpuDevice& device) const noexcept;
    [[nodiscard]] GpuOcioProgramExecutor& executor() noexcept;
    [[nodiscard]] const GpuOcioProgramExecutor& executor() const noexcept;
    [[nodiscard]] GpuOcioExecutorCounters counters() const noexcept;
    [[nodiscard]] bool hasUnretiredSubmission() const noexcept;
    [[nodiscard]] bool deviceLost() const noexcept;

    // Begins one display job. Validates the command/input/geometry/view-adjust gate before any native
    // call; on refusal no native submission is made and the caller should take the CPU display path.
    [[nodiscard]] GpuOcioDisplayArmDiagnostic begin(const GpuOcioDisplayRequest& request);

    // Non-blocking. Advances the single in-flight job.
    [[nodiscard]] GpuOcioExecutorPollResult poll();

    // Valid only after a Ready job; moves the resident RGBA8 display image out. Nullopt otherwise.
    [[nodiscard]] std::optional<render::GpuDisplayImage> takeDisplayImage() noexcept;

    void cancel() noexcept;

    // True while any retained native program still holds a submission whose fence retirement is not
    // proven. The caller must retain the stage/completion/pins until this is false, or destroy the
    // arm on the owner thread (the executor destructor performs its bounded drain/quarantine).
    [[nodiscard]] static bool teardownDrainIncomplete() noexcept;

  private:
    explicit GpuOcioDisplayArm(std::unique_ptr<GpuOcioProgramExecutor> executor) noexcept;

    std::unique_ptr<GpuOcioProgramExecutor> executor_;
};

// The exact result of qualifying one DisplayRgba8 program against the CPU OCIO display oracle. Only
// the real native parity run inside qualifyGpuOcioDisplay() can construct an eligible value, so no
// later code can fabricate success.
enum class GpuOcioDisplayOutcome : std::uint8_t {
    // Any required operation failed parity, a native call failed, or a budget/identity gate refused.
    Unavailable,
    // Native display output matched the CPU OCIO display oracle within the documented contract.
    PreviewOnly,
};

enum class GpuOcioDisplayDiagnosticCode : std::uint8_t {
    None,
    WrongThread,
    DeviceUnavailable,
    NotADisplayCommand,
    IdentityMismatch,
    FixtureMismatch,
    Cancelled,
    ParityFailure,
    NativeFailure,
    CpuOracleFailure,
    OverBudget,
    InternalInvariant,
};

struct GpuOcioDisplayDiagnostic final {
    GpuOcioDisplayDiagnosticCode code = GpuOcioDisplayDiagnosticCode::None;
    std::string message;
};

// Pinned numeric contract: native display RGBA8 RGB within one straight code of the CPU OCIO oracle
// and alpha exact, for a neutral ViewAdjust. Reused verbatim from the resident preview contract.
inline constexpr std::uint32_t kGpuOcioDisplayRgbToleranceCodes = 1;
inline constexpr std::uint32_t kGpuOcioDisplayAlphaToleranceCodes = 0;

struct GpuOcioDisplayQualificationBudgets final {
    std::uint64_t maxImageBytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t maxMetadataBytes = 16ULL * 1024ULL * 1024ULL;
};

class GpuOcioDisplayQualificationReport;

// Runs one genuine display dispatch of `command` over a deterministic fixture and compares the
// native RGBA8 output to the independent CPU OCIO display oracle `cpuOracle` (the qualified CPU
// display processor built for the SAME display/view the command was extracted for). Owner thread
// only. The readback here exists ONLY to prove parity; the production route never calls it.
[[nodiscard]] GpuOcioDisplayQualificationReport
qualifyGpuOcioDisplay(std::shared_ptr<const PreparedGpuOcioCommand> command,
                      const color::PreparedCpuDisplayProcessorHandle& cpuOracle,
                      render::GpuDevice& device,
                      const GpuOcioDisplayQualificationBudgets& budgets = {}) noexcept;

// Immutable qualification product. Construction is private: only qualifyGpuOcioDisplay() may build
// one, so later code cannot forge a successful display qualification.
class [[nodiscard]] GpuOcioDisplayQualificationReport final {
  public:
    GpuOcioDisplayQualificationReport(GpuOcioDisplayQualificationReport&&) noexcept;
    GpuOcioDisplayQualificationReport& operator=(GpuOcioDisplayQualificationReport&&) noexcept;
    GpuOcioDisplayQualificationReport(const GpuOcioDisplayQualificationReport&) = delete;
    GpuOcioDisplayQualificationReport&
    operator=(const GpuOcioDisplayQualificationReport&) = delete;
    ~GpuOcioDisplayQualificationReport() = default;

    [[nodiscard]] GpuOcioDisplayOutcome outcome() const noexcept { return outcome_; }
    [[nodiscard]] bool eligible() const noexcept {
        return outcome_ == GpuOcioDisplayOutcome::PreviewOnly;
    }
    [[nodiscard]] const GpuOcioDisplayDiagnostic& diagnostic() const& noexcept {
        return diagnostic_;
    }
    [[nodiscard]] const GpuOcioDisplayDiagnostic& diagnostic() const&& = delete;
    // The exact command identity this report qualified. A consumer must match this to the command it
    // is about to dispatch; a report for another program never blesses this one.
    [[nodiscard]] const core::Sha256Digest& commandIdentity() const& noexcept {
        return commandIdentity_;
    }
    [[nodiscard]] const core::Sha256Digest& commandIdentity() const&& = delete;
    [[nodiscard]] std::uint64_t ownershipEpoch() const noexcept { return ownershipEpoch_; }
    [[nodiscard]] const std::string& numericContract() const& noexcept { return numericContract_; }
    [[nodiscard]] const std::string& numericContract() const&& = delete;

    [[nodiscard]] bool eligibleFor(const render::GpuDevice& device,
                                   const PreparedGpuOcioCommand& command) const noexcept;

  private:
    friend GpuOcioDisplayQualificationReport
    qualifyGpuOcioDisplay(std::shared_ptr<const PreparedGpuOcioCommand>,
                          const color::PreparedCpuDisplayProcessorHandle&, render::GpuDevice&,
                          const GpuOcioDisplayQualificationBudgets&) noexcept;

    GpuOcioDisplayQualificationReport() = default;

    GpuOcioDisplayOutcome outcome_ = GpuOcioDisplayOutcome::Unavailable;
    GpuOcioDisplayDiagnostic diagnostic_;
    core::Sha256Digest commandIdentity_{};
    std::uint64_t ownershipEpoch_ = 0;
    std::string numericContract_;
};

// -----------------------------------------------------------------------------------------------
// Off-UI per-request display-program preparation
// -----------------------------------------------------------------------------------------------
//
// One display request names the exact project color identity (immutable OCIO config locator +
// expected content revision + working color space), a display/view pair, the exact output geometry,
// and the session's ViewAdjust. This service turns that into an immutable PreparedGpuOcioCommand
// whose DisplayRgba8 program is the exact OCIO display transform for that pair in THAT config, plus
// the matching qualified CPU display processor used as the independent oracle.
//
// It is BLOCKING and must never run on the UI thread or the native GPU owner thread: it resolves the
// config, extracts the OCIO GPU program, generates the Bloom wrapper, and invokes glslang/spirv-val
// through the shared runtime::GpuOcioProgramPreparer (no second compiler and no second tool
// invocation path). Both the shader-tool resolution and the config resolution are lazy: the service
// is constructed with an options provider and performs its first I/O on the first prepare() call,
// which the stage runs on a CPU worker. A warm identical request returns the cached immutable
// command without re-resolving the config, re-extracting, or recompiling.
//
// The binding is part of the program: two configs that expose the SAME display/view names but have
// different content revisions or working spaces produce different bindings and different commands.
// A caller must never accept a program for a request whose binding differs, even when the
// display/view names and geometry agree.
//
// ViewAdjust: the CPU reference applies exposure on the linear display light after the OCIO display
// function and gamma on the encoded value before RGBA8 quantization (runtime::ViewAdjust, see
// view_adjust.hpp). That is a post-display operation, not OCIO's ExposureContrast. The producer
// DisplayRgba8 wrapper bakes that exact step into the program and binds it into the command
// identity, so both neutral and non-neutral adjustments are prepared exactly and never approximated.
struct GpuDisplayColorBinding final {
    color::OcioConfigLocatorKind locatorKind = color::OcioConfigLocatorKind::BloomBuiltIn;
    std::string locatorValue;
    // The project's persisted expected OCIO content revision. Never empty after normalization.
    core::Sha256Digest expectedRevision{};
    // The project's working color space. Empty means the config's scene-linear default; the service
    // normalizes the program's binding to the actual resolved working space.
    std::string workingColorSpaceId;
    std::string display;
    std::string view;

    friend bool operator==(const GpuDisplayColorBinding&,
                           const GpuDisplayColorBinding&) = default;
};

// The exact binding a program was prepared against. `workingColorSpaceId` is the actual resolved
// working space (never empty) and `locatorValue`/`display`/`view` are the resolved request values.
struct GpuDisplayProgramBinding final {
    color::OcioConfigLocatorKind locatorKind = color::OcioConfigLocatorKind::BloomBuiltIn;
    std::string locatorValue;
    core::Sha256Digest expectedRevision{};
    std::string workingColorSpaceId;
    std::string display;
    std::string view;

    friend bool operator==(const GpuDisplayProgramBinding&,
                           const GpuDisplayProgramBinding&) = default;
};

struct GpuDisplayProgram final {
    std::shared_ptr<const PreparedGpuOcioCommand> command;
    std::shared_ptr<const color::PreparedCpuDisplayProcessorHandle> cpuOracle;
    GpuDisplayProgramBinding binding;
    ViewAdjust viewAdjust{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool hasValue() const noexcept {
        return command != nullptr && cpuOracle != nullptr;
    }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

// True only when `program` was prepared for exactly `requested`: same config locator, same
// normalized content revision, same display/view, and a working space that is either the requested
// explicit one or (when the request left it empty) the program's resolved default. Geometry and
// display/view names alone never satisfy this.
[[nodiscard]] bool
gpuDisplayProgramMatchesRequest(const GpuDisplayProgram& program,
                                const GpuDisplayColorBinding& requested) noexcept;

// Derives the exact immutable color binding from the request's project color identity (the same
// `EvaluationColorIntent` the CPU display path is built from): the project's OCIO config URI (empty
// selects the embedded Bloom Neutral URI), its expected content revision (empty selects the Bloom
// Neutral digest; an explicit revision is used verbatim), and its working color space. The
// display/view names are carried as data only -- they never select the config.
[[nodiscard]] GpuDisplayColorBinding
gpuDisplayColorBindingForIntent(const EvaluationColorIntent& intent, std::string_view display,
                                std::string_view view) noexcept;

enum class GpuDisplayProgramError : std::uint8_t {
    None,
    InvalidRequest,
    ConfigUnavailable,
    TransformUnavailable,
    OracleUnavailable,
    ViewAdjustUnsupported,
    // The resolved config did not match the requested binding (revision or working space).
    BindingMismatch,
    // Preparation was cancelled by the caller's predicate (config resolution, extraction, or
    // compile). The caller must treat the request as cancelled, not as a display failure.
    Cancelled,
};

struct GpuDisplayProgramResult final {
    GpuDisplayProgram program;
    GpuDisplayProgramError error = GpuDisplayProgramError::None;
    std::string diagnostic;

    [[nodiscard]] bool hasValue() const noexcept { return program.hasValue(); }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue(); }
};

// Blocking, thread-safe per-config display-program preparer. `config` and `options` are immutable
// for the lifetime of the instance; the shared GpuOcioProgramPreparer owns the bounded command
// cache and the persistent compiler, so an identical (display, view, geometry, viewAdjust) request
// is a cache hit. The program's binding is taken from the resolved config, never from the caller.
class GpuDisplayProgramPreparer final {
  public:
    GpuDisplayProgramPreparer(color::ResolvedBloomNeutralConfig config,
                              GpuOcioCompileOptions options);
    ~GpuDisplayProgramPreparer();
    GpuDisplayProgramPreparer(const GpuDisplayProgramPreparer&) = delete;
    GpuDisplayProgramPreparer& operator=(const GpuDisplayProgramPreparer&) = delete;

    [[nodiscard]] GpuDisplayProgramResult
    prepare(std::string_view display, std::string_view view, std::uint32_t width,
            std::uint32_t height, ViewAdjust viewAdjust = {},
            const GpuOcioCancellation& cancel = {}) const;

    [[nodiscard]] GpuOcioPreparerCounters counters() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The production general-display program service. It is constructed with an options provider and
// resolves the shader tools lazily on the first prepare() call (never at construction), then
// resolves and caches one immutable config per exact (locator, revision, working space) identity.
// Every resolution and compile happens off the UI thread because prepare() runs on the GPU-scene
// CPU worker. No hash, config, or compile I/O is performed by the constructor.
class GpuDisplayProgramService final {
  public:
    using CompileOptionsProvider = std::function<GpuOcioCompileOptions()>;

    explicit GpuDisplayProgramService(CompileOptionsProvider optionsProvider,
                                      GpuOcioPreparerBudgets budgets = {});
    ~GpuDisplayProgramService();
    GpuDisplayProgramService(const GpuDisplayProgramService&) = delete;
    GpuDisplayProgramService& operator=(const GpuDisplayProgramService&) = delete;

    // Blocking. Resolves the requested config/working space on first use and caches the per-config
    // preparer by the exact config identity, so a warm identical request is a command-cache hit.
    [[nodiscard]] GpuDisplayProgramResult
    prepare(const GpuDisplayColorBinding& binding, std::uint32_t width, std::uint32_t height,
            ViewAdjust viewAdjust = {}, const GpuOcioCancellation& cancel = {}) const;

    [[nodiscard]] GpuOcioPreparerCounters counters() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bloom::runtime
