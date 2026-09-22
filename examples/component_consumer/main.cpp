// A consumer of the backend module that is not this application.
//
// WHAT IT PROVES, AND WHY IT IS A SEPARATE PROJECT
//   module-r1 requires an "independent sample project [that] builds outside the
//   main CMake graph using public ABI headers and platform loader/Qt Core only,
//   and drives pinned local engine". The point is not the demo. The point is
//   that the published header set is SUFFICIENT: this file includes nothing
//   from src/ except core/component/**, links no project library, and knows
//   nothing about MihomoBackend, BackendBridge or the private marshalling the
//   application shares with the module. If a published header ever grows a
//   dependency on project-internal code, this file stops compiling - which is a
//   better detector than a review.
//
//   It therefore carries its own ~60-line encoder for the wire format of
//   wire.h. That duplication is deliberate: a foreign consumer has exactly that
//   job, and doing it here is how we find out whether the format is documented
//   well enough to do it.
//
// WHAT IT DOES
//   Loads a module, performs the handshake, creates a session, installs itself
//   as the host, points the engine at a binary and a configuration the caller
//   named, waits for the core to become ready, stops it, and unloads.
//
// WHAT IT DELIBERATELY DOES NOT DO
//   It injects no privileged service: every host command is answered
//   kInvalidState, which is the honest answer for a host that has none. So it
//   never asks for elevation, never contacts an installed helper and never
//   touches system proxy, VPN or network settings. The engine it runs is the
//   one on the command line and nothing else.
//
// USAGE
//   component_consumer --module <path> --engine <mihomo> --config <file>
//                      [--work-dir <dir>] [--timeout-ms <n>]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "core/component/abi/abi.h"

namespace com = clashqt::com;
namespace abi = clashqt::com::abi;

namespace {

// ------------------------------------------------- the wire format, by hand
//
// Little-endian fixed width; a string is a u32 byte length and that many UTF-8
// bytes. Sixty lines, no dependencies - which is the claim being tested.

class Writer {
  public:
    void u8(std::uint8_t value) { bytes_.push_back(value); }
    void u64(std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
        }
    }
    void u32(std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
        }
    }
    void text(const std::string &value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    const void *data() const { return bytes_.empty() ? nullptr : bytes_.data(); }
    std::size_t size() const { return bytes_.size(); }

  private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
  public:
    Reader(const void *data, std::size_t size)
        : data_(static_cast<const std::uint8_t *>(data)), size_(data == nullptr ? 0 : size) {}

    std::uint8_t u8() {
        const std::uint8_t *at = take(1);
        return at == nullptr ? 0 : *at;
    }
    std::uint16_t u16() {
        const std::uint8_t *at = take(2);
        if (at == nullptr) return 0;
        return static_cast<std::uint16_t>(at[0] | (static_cast<std::uint16_t>(at[1]) << 8));
    }
    std::uint32_t u32() {
        const std::uint8_t *at = take(4);
        if (at == nullptr) return 0;
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(at[i]) << (i * 8);
        return value;
    }
    std::uint64_t u64() {
        const std::uint8_t *at = take(8);
        if (at == nullptr) return 0;
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(at[i]) << (i * 8);
        return value;
    }
    std::string text() {
        const std::uint32_t length = u32();
        if (!ok_ || length > size_ - cursor_) {
            ok_ = false;
            return {};
        }
        const std::uint8_t *at = take(length);
        return at == nullptr ? std::string()
                             : std::string(reinterpret_cast<const char *>(at), length);
    }
    bool ok() const { return ok_; }

  private:
    const std::uint8_t *take(std::size_t bytes) {
        if (!ok_ || bytes > size_ - cursor_) {
            ok_ = false;
            return nullptr;
        }
        const std::uint8_t *at = data_ + cursor_;
        cursor_ += bytes;
        return at;
    }
    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cursor_ = 0;
    bool ok_ = true;
};

// ------------------------------------------------------------ the host side

/// The minimum a consumer has to implement: an event sink. Reference counting
/// is three lines, and getting it wrong is how a module ends up calling into
/// freed memory, so it is written out rather than hidden behind a helper.
class ConsoleHost final : public abi::IBackendHost {
  public:
    com::Result QueryInterface(const com::InterfaceId &id, void **out) noexcept override {
        if (out == nullptr) return com::kInvalidArgument;
        if (id == abi::kIBackendHostId || id == com::kIObjectId) {
            *out = static_cast<void *>(this);
            AddRef();
            return com::kOk;
        }
        *out = nullptr;
        return com::kNoInterface;
    }
    std::int32_t AddRef() noexcept override { return ++references_; }
    std::int32_t Release() noexcept override {
        const std::int32_t remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    com::Result Notify(std::uint32_t event, const void *data, std::size_t size) noexcept override {
        Reader in(data, size);
        // wire.h's envelope: every event payload starts with the sequence the
        // module's backend assigned when it PRODUCED the event. A consumer with
        // one observer has nothing to admit and can skip it; one that
        // re-broadcasts has to compare it against what each of its own
        // observers was told had already been produced, which is what
        // ModuleBackend does on the application's side.
        const std::uint64_t produced = in.u64();
        (void)produced;
        switch (event) {
            case abi::kEvtCoreStateChanged: {
                const std::uint64_t generation = in.u64();
                state = in.u8();
                std::printf("  state=%u (generation %llu)\n", state,
                            static_cast<unsigned long long>(generation));
                return com::kOk;
            }
            case abi::kEvtCoreReady: {
                // Completion: request, generation, status, error{code,message}
                in.u64();
                in.u64();
                in.u8();
                in.u32();
                in.text();
                const std::string host = in.text();
                const std::uint16_t port = in.u16();
                ready = true;
                std::printf("  core ready at %s:%u\n", host.c_str(), port);
                return com::kOk;
            }
            case abi::kEvtCoreFailed: {
                in.u64();
                in.u64();
                in.u8();
                const std::uint32_t code = in.u32();
                const std::string message = in.text();
                failed = true;
                std::printf("  core failed: code %u, %s\n", code, message.c_str());
                return com::kOk;
            }
            case abi::kEvtCoreLogLine: {
                in.u64();
                const std::string line = in.text();
                std::printf("  core: %s\n", line.c_str());
                return com::kOk;
            }
            case abi::kEvtStopCompleted: {
                in.u64();
                in.u64();
                in.u8();
                confirmedStop = in.u8() != 0;
                stopped = true;
                std::printf("  stop completed, confirmed=%s\n", confirmedStop ? "yes" : "no");
                return com::kOk;
            }
            default:
                // A consumer does not have to understand every event. Saying so
                // is how a newer module learns this host is older, instead of
                // being told the payload was malformed.
                return com::kNotImplemented;
        }
    }

    com::Result Invoke(std::uint32_t command, const void *data, std::size_t size,
                       com::IBuffer **reply) noexcept override {
        (void)command;
        (void)data;
        (void)size;
        if (reply != nullptr) *reply = nullptr;
        // No privileged service is injected. kInvalidState is "this host has
        // none", which is different from "the platform does not support one" -
        // and the module reports the difference rather than guessing.
        return com::kInvalidState;
    }

    bool ready = false;
    bool failed = false;
    bool stopped = false;
    bool confirmedStop = false;
    std::uint8_t state = 0;

  private:
    ~ConsoleHost() = default;
    std::int32_t references_ = 1;
};

// ------------------------------------------------------------- the platform

void *openLibrary(const char *path) {
#if defined(_WIN32)
    return static_cast<void *>(::LoadLibraryA(path));
#else
    return ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *findSymbol(void *handle, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
}

void closeLibrary(void *handle) {
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
}

/// Whether the artifact is STILL mapped into this process. Asked of the
/// platform loader, because "dlclose returned 0" is not the same statement.
bool imageIsStillMapped(const char *path) {
#if defined(_WIN32)
    HMODULE handle = nullptr;
    if (::GetModuleHandleExA(0, path, &handle) == 0) {
        return false;
    }
    ::FreeLibrary(handle);
    return true;
#else
    void *probe = ::dlopen(path, RTLD_NOLOAD | RTLD_LAZY);
    if (probe == nullptr) {
        return false;
    }
    ::dlclose(probe);
    return true;
#endif
}

/// The runtime tag the handshake compares. A foreign consumer computes it the
/// same way the application does: hash what this binary was compiled against
/// and what it is running against. A consumer that guesses here gets a refusal,
/// which is the correct outcome.
std::uint64_t runtimeTag() {
    std::uint64_t tag = abi::detail::HashText(QT_VERSION_STR, abi::detail::kHashSeed);
    tag = abi::detail::HashText(qVersion(), tag);
    tag = abi::detail::HashValue(static_cast<std::uint64_t>(sizeof(QString)), tag);
    tag = abi::detail::HashValue(static_cast<std::uint64_t>(abi::kWireRevision), tag);
    return tag;
}

std::string argumentAfter(int argc, char **argv, const char *flag) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], flag) == 0) return argv[index + 1];
    }
    return {};
}

com::Result command(abi::IBackendSession *session, std::uint32_t code, const Writer &args,
                    std::vector<std::uint8_t> *reply = nullptr) {
    com::IBuffer *buffer = nullptr;
    const com::Result status =
        session->Invoke(code, args.data(), args.size(), reply == nullptr ? nullptr : &buffer);
    if (reply != nullptr && buffer != nullptr) {
        const auto *first = static_cast<const std::uint8_t *>(buffer->Data());
        if (first != nullptr) reply->assign(first, first + buffer->Size());
    }
    if (buffer != nullptr) buffer->Release();
    return status;
}

}  // namespace

int main(int argc, char **argv) {
    // The module runs its work on the host's event loop, so a QCoreApplication
    // has to exist before anything is loaded.
    QCoreApplication application(argc, argv);

    const std::string modulePath = argumentAfter(argc, argv, "--module");
    const std::string enginePath = argumentAfter(argc, argv, "--engine");
    const std::string configPath = argumentAfter(argc, argv, "--config");
    const std::string workDir = argumentAfter(argc, argv, "--work-dir");
    const std::string timeoutText = argumentAfter(argc, argv, "--timeout-ms");
    const int timeoutMs = timeoutText.empty() ? 30000 : std::stoi(timeoutText);

    if (modulePath.empty() || enginePath.empty() || configPath.empty()) {
        std::fprintf(stderr,
                     "usage: component_consumer --module <path> --engine <mihomo> "
                     "--config <file> [--work-dir <dir>] [--timeout-ms <n>]\n");
        return 2;
    }

    void *handle = openLibrary(modulePath.c_str());
    if (handle == nullptr) {
        std::fprintf(stderr, "could not load %s\n", modulePath.c_str());
        return 1;
    }

    auto entry =
        reinterpret_cast<abi::ModuleEntryFn>(findSymbol(handle, CLASHQT_COM_MODULE_ENTRY_NAME));
    if (entry == nullptr) {
        std::fprintf(stderr, "%s exports no %s\n", modulePath.c_str(),
                     CLASHQT_COM_MODULE_ENTRY_NAME);
        closeLibrary(handle);
        return 1;
    }

    const abi::ModuleHandshakeRequest request =
        abi::MakeHandshakeRequest(runtimeTag(), abi::kMihomoModuleId);
    abi::ModuleHandshakeResponse response{};
    response.structSize = static_cast<std::uint32_t>(sizeof(response));

    com::IComponentModule *root = nullptr;
    const com::Result handshake = entry(&request, &response, &root);
    if (com::IsFailure(handshake) || root == nullptr) {
        std::fprintf(stderr,
                     "handshake refused (%d): module abi %u, wire %u, target %llx, runtime %llx\n",
                     handshake, response.abiVersion, response.wireRevision,
                     static_cast<unsigned long long>(response.targetAbiTag),
                     static_cast<unsigned long long>(response.runtimeTag));
        closeLibrary(handle);
        return 1;
    }
    std::printf("loaded: %s (module abi %u)\n", root->Description(), root->AbiVersion());

    abi::IBackendSession *session = nullptr;
    const com::Result created = root->CreateObject(
        abi::kBackendSessionClassId, abi::kIBackendSessionId, reinterpret_cast<void **>(&session));
    if (com::IsFailure(created) || session == nullptr) {
        std::fprintf(stderr, "the module would not create a session (%d)\n", created);
        root->Release();
        closeLibrary(handle);
        return 1;
    }

    auto *host = new ConsoleHost();
    if (com::IsFailure(session->SetHost(host))) {
        std::fprintf(stderr, "the module refused the host\n");
        host->Release();
        session->Release();
        root->Release();
        closeLibrary(handle);
        return 1;
    }

    int exitCode = 0;
    {
        Writer binary;
        binary.text(enginePath);
        command(session, abi::kCmdSetBinaryPath, binary);

        Writer launch;
        launch.text(configPath);
        launch.text(workDir);
        std::vector<std::uint8_t> reply;
        if (com::IsFailure(command(session, abi::kCmdStart, launch, &reply))) {
            std::fprintf(stderr, "start was refused\n");
            exitCode = 1;
        } else {
            Reader in(reply.data(), reply.size());
            std::printf("start submitted as request %llu\n",
                        static_cast<unsigned long long>(in.u64()));

            // Ready is an HTTP answer, not a process start (backend-r4 section
            // 3), so waiting means pumping the shared event loop until the
            // module says so.
            QElapsedTimer elapsed;
            elapsed.start();
            QEventLoop loop;
            while (!host->ready && !host->failed && elapsed.elapsed() < timeoutMs) {
                loop.processEvents(QEventLoop::AllEvents, 50);
            }
            if (!host->ready) {
                std::fprintf(stderr, "the core did not become ready within %d ms\n", timeoutMs);
                exitCode = 1;
            }
        }

        // Stop, and WAIT for the answer: an unconfirmed stop is not a success,
        // and a consumer that exits without reading it has not stopped
        // anything.
        Writer none;
        command(session, abi::kCmdStop, none);
        QElapsedTimer stopping;
        stopping.start();
        QEventLoop loop;
        while (!host->stopped && stopping.elapsed() < timeoutMs) {
            loop.processEvents(QEventLoop::AllEvents, 50);
        }
        if (host->stopped && !host->confirmedStop) {
            std::fprintf(stderr, "the stop was NOT confirmed\n");
            exitCode = 1;
        }
    }

    // Close before releasing: the session cancels what it can, stops a managed
    // core it still owns and releases its reference to the host. Only then is
    // unloading safe, and the module says so through IModuleLifetime.
    session->Close(abi::kCloseStopManagedCore, 3000);
    session->Release();

    // Ask the module whether its image may be unmapped, and honour the answer.
    // kFalse is a SUCCESSFUL "no" - nothing is alive, but this module could not
    // pin the shared runtime it loaded, and unmapping it would take that
    // runtime with it.
    //
    // THIS IS WHERE THE ONE REAL DEFECT OF THIS SAMPLE WAS FOUND. The first
    // version called dlclose unconditionally and segfaulted every time it had
    // driven a real engine - EXC_BAD_ACCESS in
    // QtPrivate::QCallableObject<void (*)()>::impl, in QtNetwork, reached from
    // doActivate inside ~QCoreApplication below. Not the module's own code:
    // this process links only QtCore, so QtNetwork was mapped ONLY because the
    // module needed it, and dlclose took it away while QtNetwork's host-lookup
    // manager still held a registration on QCoreApplication::destroyed. The
    // module pins its runtime now, and the image genuinely goes.
    bool mayUnmap = true;
    abi::IModuleLifetime *lifetime = nullptr;
    if (com::IsSuccess(root->QueryInterface(abi::kIModuleLifetimeId,
                                            reinterpret_cast<void **>(&lifetime))) &&
        lifetime != nullptr) {
        std::printf("live objects before unload: %d\n", lifetime->LiveObjectCount());
        const com::Result prepared = lifetime->PrepareUnload();
        mayUnmap = prepared == com::kOk || prepared == com::kAlreadyClosed;
        std::printf("unmappable: %s\n", mayUnmap ? "yes" : "no (image stays mapped)");
        lifetime->Release();
    }

    host->Release();
    // The LAST root reference. Nothing may be released after this that came
    // from the module, and nothing here does: the wire decoding above is this
    // file's own, which is the point of writing it by hand.
    root->Release();

    if (mayUnmap) {
        closeLibrary(handle);
        // Verified, not assumed. A dlclose that returns 0 has not necessarily
        // unmapped anything - the image stays if any other reference to it
        // exists - and "we called dlclose" is not the claim being made.
        if (imageIsStillMapped(modulePath.c_str())) {
            std::fprintf(stderr, "the module said it was unmappable and its image is still there\n");
            exitCode = 1;
        } else {
            std::printf("image unmapped: yes\n");
        }
    }
    std::printf("teardown complete\n");
    return exitCode;
}
