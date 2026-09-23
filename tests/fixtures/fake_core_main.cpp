// A portable, compiled stand-in for the mihomo core binary.
//
// It replaces the `#!/bin/sh` and `#!/usr/bin/python3` stubs the current suite
// writes into a temporary directory, which skip on Windows and hard-fail on any
// POSIX box without /usr/bin/python3. This helper is plain C++ with no Qt and no
// interpreter dependency, so the same core-lifecycle contract runs everywhere.
//
// Behaviour comes from a script file beside the executable ("<argv[0]>.script",
// also reachable through CLASH_QT_FAKE_CORE_SCRIPT), never from the command
// line: CoreProcess owns argv. Gate files are resolved against the executable's
// own directory, so a test releases a gate without knowing the -d work dir.
//
// Script grammar, one directive per line, '#' starts a comment:
//   validate-exit=<int>        exit code for a `-t` run              (default 0)
//   validate-message=<text>    printed to stdout during a `-t` run
//   validate-wait=<gate>       a `-t` run blocks until <gate> exists
//   ignore-terminate           POSIX: SIG_IGN for TERM/INT/HUP (see below)
//   stdout=<line>              print a line and flush        ] executed in file
//   touch=<marker>             create <marker>               ] order, for a run
//   wait=<gate>                block until <gate> exists     ] (non `-t`)
//   crash                      abort / fault                 ]
//   exit=<int>                 exit with that code           ]
//   run-forever                idle until terminated or killed
//
// OS-specific termination contract (see testsupport::terminationContract()):
//   POSIX   QProcess::terminate() delivers SIGTERM, which `ignore-terminate`
//           refuses; QProcess::kill() sends SIGKILL and always wins. `crash`
//           calls abort(), which QProcess reports as QProcess::CrashExit.
//   Windows QProcess::terminate() posts WM_CLOSE to the process's top-level
//           windows. A console helper has none, so terminate() cannot be
//           observed and `ignore-terminate` is accepted but redundant - the
//           process is stubborn by construction. QProcess::kill() calls
//           TerminateProcess and always wins. `crash` calls abort() with the
//           abort dialog disabled, which exits 3 and is reported as
//           QProcess::NormalExit, not CrashExit.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <csignal>
#  include <ctime>
#  include <unistd.h>
#endif

namespace {

void sleepMilliseconds(int milliseconds) {
#if defined(_WIN32)
    Sleep(static_cast<DWORD>(milliseconds));
#else
    timespec request{};
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = static_cast<long>(milliseconds % 1000) * 1000000L;
    nanosleep(&request, nullptr);
#endif
}

std::string directoryOf(const std::string &path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool fileExists(const std::string &path) {
    std::ifstream probe(path.c_str());
    return probe.good();
}

std::string trimmed(const std::string &text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n'))
        --end;
    return text.substr(begin, end - begin);
}

struct Directive {
    std::string verb;
    std::string argument;
};

struct Script {
    int validateExit = 0;
    std::string validateMessage;
    std::string validateGate;
    bool ignoreTerminate = false;
    std::vector<Directive> run;
};

std::string scriptPathFor(const std::string &executable) {
    if (const char *override = std::getenv("CLASH_QT_FAKE_CORE_SCRIPT")) return override;
    const std::string direct = executable + ".script";
    if (fileExists(direct)) return direct;
    // Windows callers may pass the path with or without the .exe suffix.
    if (executable.size() > 4 && executable.compare(executable.size() - 4, 4, ".exe") == 0)
        return executable.substr(0, executable.size() - 4) + ".script";
    return direct;
}

Script loadScript(const std::string &path) {
    Script script;
    std::ifstream input(path.c_str());
    std::string line;
    while (std::getline(input, line)) {
        const std::string entry = trimmed(line);
        if (entry.empty() || entry[0] == '#') continue;
        const std::size_t equals = entry.find('=');
        const std::string verb = equals == std::string::npos ? entry : entry.substr(0, equals);
        const std::string argument = equals == std::string::npos ? std::string() : entry.substr(equals + 1);
        if (verb == "validate-exit") script.validateExit = std::atoi(argument.c_str());
        else if (verb == "validate-message") script.validateMessage = argument;
        else if (verb == "validate-wait") script.validateGate = argument;
        else if (verb == "ignore-terminate") script.ignoreTerminate = true;
        else script.run.push_back({verb, argument});
    }
    return script;
}

void recordInvocation(const std::string &executable, int argc, char **argv) {
    std::ofstream log((executable + ".args").c_str(), std::ios::app);
    if (!log.good()) return;
    for (int index = 1; index < argc; ++index) {
        if (index > 1) log << ' ';
        log << argv[index];
    }
    log << '\n';
}

void awaitGate(const std::string &root, const std::string &gate) {
    const std::string path = root + "/" + gate;
    // Polling a file is the gate: the parent decides when this returns, so no
    // test ever waits on a fixed duration.
    while (!fileExists(path)) sleepMilliseconds(5);
}

void crashNow() {
#if defined(_WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

} // namespace

int main(int argc, char **argv) {
    const std::string executable = argc > 0 ? argv[0] : "fake-core";
    const std::string root = directoryOf(executable);
    const Script script = loadScript(scriptPathFor(executable));

    bool validating = false;
    for (int index = 1; index < argc; ++index)
        if (std::strcmp(argv[index], "-t") == 0) validating = true;

    recordInvocation(executable, argc, argv);

    if (validating) {
        if (!script.validateGate.empty()) awaitGate(root, script.validateGate);
        if (!script.validateMessage.empty()) std::cout << script.validateMessage << std::endl;
        return script.validateExit;
    }

#if !defined(_WIN32)
    if (script.ignoreTerminate) {
        std::signal(SIGTERM, SIG_IGN);
        std::signal(SIGINT, SIG_IGN);
        std::signal(SIGHUP, SIG_IGN);
    }
#endif

    for (const Directive &directive : script.run) {
        if (directive.verb == "stdout") {
            std::cout << directive.argument << std::endl;
        } else if (directive.verb == "touch") {
            std::ofstream marker((root + "/" + directive.argument).c_str());
            marker << "1";
        } else if (directive.verb == "wait") {
            awaitGate(root, directive.argument);
        } else if (directive.verb == "crash") {
            crashNow();
        } else if (directive.verb == "exit") {
            return std::atoi(directive.argument.c_str());
        } else if (directive.verb == "run-forever") {
            for (;;) sleepMilliseconds(25);
        } else {
            std::cerr << "fake-core: unknown directive '" << directive.verb << "'" << std::endl;
            return 64;
        }
    }
    return 0;
}
