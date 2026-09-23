#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr const char *kLabel = "org.clash-qt.service";
const char *kTools = "/Library/PrivilegedHelperTools/org.clash-qt.service";
const char *kHelper = "/Library/PrivilegedHelperTools/org.clash-qt.service/helper";
const char *kCore = "/Library/PrivilegedHelperTools/org.clash-qt.service/mihomo";
constexpr const char *kPlist = "/Library/LaunchDaemons/org.clash-qt.service.plist";
const char *kRuntime = "/Library/Application Support/org.clash-qt.service";
const char *kSocketDir = "/var/run/org.clash-qt.service";
const char *kSocket = "/var/run/org.clash-qt.service/socket";
constexpr size_t kMaxFrame = 8 * 1024 * 1024;
volatile sig_atomic_t stopping = 0;
#ifdef CLASH_QT_HELPER_TESTING
bool fixtureMode = false;
#endif

uid_t fileOwner() {
#ifdef CLASH_QT_HELPER_TESTING
    if (fixtureMode) return getuid();
#endif
    return 0;
}

long long frameTimeout() {
#ifdef CLASH_QT_HELPER_TESTING
    if (fixtureMode) return 250;
#endif
    return 10000;
}

NSString *str(const char *value) { return [NSString stringWithUTF8String:value]; }
void fail(NSString *message) { @throw [NSException exceptionWithName:@"ServiceError" reason:message userInfo:nil]; }
void check(bool valid, NSString *message) { if (!valid) fail(message); }
void signalStop(int) { stopping = 1; }
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool writeAll(int fd, const void *data, size_t size) {
    const auto *bytes = static_cast<const char *>(data);
    while (size) {
        const ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        bytes += written; size -= written;
    }
    return true;
}

void ownedDirectory(const char *path, mode_t mode) {
    struct stat status{};
    if (mkdir(path, mode) != 0 && errno != EEXIST) fail([NSString stringWithFormat:@"Cannot create %s: %s", path, strerror(errno)]);
    check(lstat(path, &status) == 0 && S_ISDIR(status.st_mode) && status.st_uid == fileOwner() &&
          !(status.st_mode & (S_IWGRP | S_IWOTH)), [NSString stringWithFormat:@"Unsafe service directory: %s", path]);
    check(chmod(path, mode) == 0, @"Cannot protect service directory.");
}

void atomicData(NSString *path, NSData *data, mode_t mode) {
    std::string temporary = [path fileSystemRepresentation];
    temporary += ".XXXXXX";
    std::vector<char> name(temporary.begin(), temporary.end()); name.push_back(0);
    int fd = mkstemp(name.data());
    check(fd >= 0, @"Cannot create private staging file.");
    const bool good = fchmod(fd, mode) == 0 && writeAll(fd, data.bytes, data.length) && fsync(fd) == 0;
    close(fd);
    if (!good || rename(name.data(), path.fileSystemRepresentation) != 0) {
        unlink(name.data()); fail(@"Could not atomically install service file.");
    }
}

void copyExecutable(const char *source, const char *destination, uid_t owner) {
    int fd = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    check(fd >= 0, @"Cannot open the approved executable (symbolic links are not accepted).");
    struct stat status{};
    bool valid = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
        (status.st_uid == owner || status.st_uid == 0) && (status.st_mode & S_IXUSR) &&
        !(status.st_mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) &&
        status.st_size > 4 && status.st_size <= 256 * 1024 * 1024;
    uint32_t magic = 0;
    valid = valid && pread(fd, &magic, sizeof(magic), 0) == sizeof(magic) &&
        (magic == 0xfeedfacf || magic == 0xcffaedfe || magic == 0xfeedface ||
         magic == 0xcefaedfe || magic == 0xcafebabe || magic == 0xbebafeca ||
         magic == 0xcafebabf || magic == 0xbfbafeca);
    if (!valid) { close(fd); fail(@"Approved source must be a regular, owned Mach-O executable without group/world write or set-ID permissions."); }
    NSMutableData *bytes = [NSMutableData dataWithLength:static_cast<NSUInteger>(status.st_size)];
    size_t offset = 0;
    while (offset < bytes.length) {
        ssize_t count = read(fd, static_cast<char *>(bytes.mutableBytes) + offset, bytes.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { close(fd); fail(@"Cannot read approved executable."); }
        offset += count;
    }
    struct stat after{};
    valid = fstat(fd, &after) == 0 && after.st_size == status.st_size &&
        after.st_mtimespec.tv_sec == status.st_mtimespec.tv_sec &&
        after.st_mtimespec.tv_nsec == status.st_mtimespec.tv_nsec;
    close(fd);
    check(valid, @"Approved executable changed while it was being copied.");
    atomicData(str(destination), bytes, 0755);
}

NSUInteger seedGeoFromDirectory(NSString *sourceDirectory, NSString *destinationDirectory, uid_t owner) {
    int sourceDir = open(sourceDirectory.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (sourceDir < 0) return 0;
    struct stat directoryStatus{};
    if (fstat(sourceDir, &directoryStatus) != 0 || !S_ISDIR(directoryStatus.st_mode) ||
        (directoryStatus.st_uid != owner && directoryStatus.st_uid != 0) || (directoryStatus.st_mode & 0022)) {
        close(sourceDir); return 0;
    }
    NSUInteger copied = 0;
    for (const char *name : {"Country.mmdb", "geoip.dat", "geosite.dat"}) {
        NSString *destination = [destinationDirectory stringByAppendingPathComponent:str(name)];
        struct stat existing{};
        if (lstat(destination.fileSystemRepresentation, &existing) == 0 || errno != ENOENT) continue;
        int source = openat(sourceDir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (source < 0) continue;
        struct stat before{};
        bool safe = fstat(source, &before) == 0 && S_ISREG(before.st_mode) &&
            (before.st_uid == owner || before.st_uid == 0) && !(before.st_mode & (0022 | S_ISUID | S_ISGID)) &&
            before.st_size > 0 && before.st_size <= 128 * 1024 * 1024;
        if (!safe) { close(source); continue; }
        std::string pattern(destination.fileSystemRepresentation); pattern += ".seed-XXXXXX";
        std::vector<char> temporary(pattern.begin(), pattern.end()); temporary.push_back(0);
        int target = mkstemp(temporary.data());
        if (target < 0) { close(source); continue; }
        safe = fchmod(target, 0600) == 0;
        char bytes[65536]; off_t remaining = before.st_size;
        while (safe && remaining > 0) {
            ssize_t count = read(source, bytes, static_cast<size_t>(std::min<off_t>(remaining, sizeof(bytes))));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 || !writeAll(target, bytes, static_cast<size_t>(count))) { safe = false; break; }
            remaining -= count;
        }
        struct stat after{};
        safe = safe && fstat(source, &after) == 0 && after.st_size == before.st_size &&
            after.st_mtimespec.tv_sec == before.st_mtimespec.tv_sec &&
            after.st_mtimespec.tv_nsec == before.st_mtimespec.tv_nsec && fsync(target) == 0;
        close(source); close(target);
        // Do not replace a database concurrently produced by the core itself.
        if (safe && link(temporary.data(), destination.fileSystemRepresentation) == 0) ++copied;
        unlink(temporary.data());
    }
    close(sourceDir);
    return copied;
}

void seedGeoDatabases(uid_t owner) {
#ifdef CLASH_QT_HELPER_TESTING
    if (fixtureMode) return;
#endif
    const passwd *account = getpwuid(owner);
    if (!account || !account->pw_dir || account->pw_dir[0] != '/') return;
    NSString *source = [str(account->pw_dir) stringByAppendingPathComponent:@"Library/Application Support/clash-qt"];
    seedGeoFromDirectory(source, str(kRuntime), owner);
}

int launchctl(const std::vector<std::string> &arguments) {
    pid_t child = fork();
    if (child == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO); dup2(null, STDOUT_FILENO); dup2(null, STDERR_FILENO);
        std::vector<char *> argv{const_cast<char *>("/bin/launchctl")};
        for (const auto &arg : arguments) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        char *env[] = {const_cast<char *>("PATH=/usr/bin:/bin:/usr/sbin:/sbin"), nullptr};
        execve(argv[0], argv.data(), env); _exit(127);
    }
    check(child > 0, @"Cannot start launchctl.");
    int status = 0;
    const auto deadline = nowMs() + 15000;
    while (waitpid(child, &status, WNOHANG) == 0) {
        if (nowMs() > deadline) { kill(child, SIGKILL); waitpid(child, &status, 0); return -1; }
        usleep(10000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void stopDaemon() {
    const int result = launchctl({"bootout", "system/org.clash-qt.service"});
    check(result == 0 || launchctl({"print", "system/org.clash-qt.service"}) != 0,
          @"The existing service could not be stopped; refusing to replace or remove its files.");
}

uid_t parseOwner(const char *value) {
    char *end = nullptr; errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    check(!errno && end && !*end && parsed >= 501 && parsed <= UINT32_MAX, @"A non-root local owner UID is required.");
    auto *account = getpwuid(static_cast<uid_t>(parsed));
    check(account != nullptr, @"Service owner account does not exist.");
    return static_cast<uid_t>(parsed);
}

void install(uid_t owner, const char *core) {
    check(geteuid() == 0, @"Installation requires administrator authorization.");
    check(core[0] == '/', @"The approved core path must be absolute.");
    ownedDirectory("/Library/PrivilegedHelperTools", 0755);
    ownedDirectory(kTools, 0755);
    ownedDirectory("/Library/LaunchDaemons", 0755);
    ownedDirectory(kRuntime, 0700);
    uint32_t size = 0; _NSGetExecutablePath(nullptr, &size);
    std::vector<char> source(size + 1);
    check(_NSGetExecutablePath(source.data(), &size) == 0, @"Cannot locate the approved helper executable.");
    char absolute[PATH_MAX];
    check(realpath(source.data(), absolute) != nullptr, @"Cannot resolve helper executable.");
    copyExecutable(absolute, kHelper, owner);
    copyExecutable(core, kCore, owner);
    NSDictionary *plist = @{@"Label":str(kLabel), @"ProgramArguments":@[str(kHelper), @"--serve", [NSString stringWithFormat:@"%u", owner]],
        @"RunAtLoad":@YES, @"KeepAlive":@YES, @"ProcessType":@"Interactive", @"ThrottleInterval":@5,
        @"UserName":@"root", @"GroupName":@"wheel", @"Umask":@077,
        @"EnvironmentVariables":@{@"PATH":@"/usr/bin:/bin:/usr/sbin:/sbin"}};
    NSError *error = nil;
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:plist format:NSPropertyListXMLFormat_v1_0 options:0 error:&error];
    check(data != nil, @"Cannot serialize the launch daemon.");
    stopDaemon();
    seedGeoDatabases(owner);
    atomicData(str(kPlist), data, 0644);
    check(launchctl({"bootstrap", "system", kPlist}) == 0, @"Service files were installed but launchd could not start the service.");
}

void removeOwnedFile(const char *path) {
    struct stat status{};
    if (lstat(path, &status) < 0 && errno == ENOENT) return;
    check(S_ISREG(status.st_mode) && status.st_uid == 0, @"Refusing to remove an unexpected service file.");
    check(unlink(path) == 0, @"Cannot remove service file.");
}

void uninstall() {
    check(geteuid() == 0, @"Removal requires administrator authorization.");
    stopDaemon();
    removeOwnedFile(kPlist); removeOwnedFile(kCore); removeOwnedFile(kHelper);
    rmdir(kTools);
    struct stat runtime{};
    if (lstat(kRuntime, &runtime) == 0) {
        check(S_ISDIR(runtime.st_mode) && runtime.st_uid == 0 && !(runtime.st_mode & 0022), @"Unsafe runtime directory; refusing removal.");
        NSError *error = nil;
        check([[NSFileManager defaultManager] removeItemAtPath:str(kRuntime) error:&error], @"Could not remove private runtime files.");
    }
}

void validateTree(id object, NSUInteger depth = 0, NSString *parentKey = @"") {
    check(depth <= 64, @"Configuration nesting is too deep.");
    if ([object isKindOfClass:[NSDictionary class]]) {
        for (NSString *key in object) {
            check([key isKindOfClass:[NSString class]], @"Configuration keys must be strings.");
            NSString *lower = key.lowercaseString;
            const bool transportPath = [lower isEqualToString:@"path"] &&
                ([@"ws-opts" isEqualToString:parentKey] || [@"http-opts" isEqualToString:parentKey] || [@"h2-opts" isEqualToString:parentKey]);
            const bool fileField = (!transportPath && [lower isEqualToString:@"path"]) || [lower hasSuffix:@"-path"] ||
                [lower isEqualToString:@"file"] || [lower hasSuffix:@"-file"] ||
                [lower containsString:@"certificate"] || [lower containsString:@"private-key"] ||
                [lower isEqualToString:@"ca"] || [lower isEqualToString:@"ca-str"] ||
                [lower isEqualToString:@"client-key"] || [lower isEqualToString:@"client-cert"] ||
                [lower isEqualToString:@"unix"] || [lower isEqualToString:@"socket"];
            check(!fileField, [NSString stringWithFormat:@"Privileged mode rejects file-backed field '%@'. Use an inline configuration without local certificates or file providers.", key]);
            validateTree(object[key], depth + 1, lower);
        }
    } else if ([object isKindOfClass:[NSArray class]]) {
        for (id value in object) validateTree(value, depth + 1, parentKey);
    } else if ([object isKindOfClass:[NSString class]]) {
        NSString *lower = [object lowercaseString];
        check(![lower hasPrefix:@"file:"] && ![lower hasPrefix:@"unix:"] && ![lower hasPrefix:@"unixgram:"], @"Local file/socket URLs are unavailable in privileged mode.");
    }
}

NSMutableDictionary *sanitize(NSDictionary *input, NSString *runtime, NSDictionary **endpoint) {
    check([input isKindOfClass:[NSDictionary class]], @"start.config must be a JSON object.");
    NSError *error = nil;
    NSData *json = [NSJSONSerialization dataWithJSONObject:input options:0 error:&error];
    check(json && json.length <= kMaxFrame, @"Configuration is invalid or too large.");
    NSMutableDictionary *config = [NSJSONSerialization JSONObjectWithData:json options:NSJSONReadingMutableContainers error:&error];
    NSString *controller = config[@"external-controller"];
    check([controller isKindOfClass:[NSString class]], @"A loopback external-controller is required.");
    NSArray *parts = [controller componentsSeparatedByString:@":"];
    check(parts.count == 2 && [parts[0] isEqualToString:@"127.0.0.1"], @"Privileged controller must bind to 127.0.0.1.");
    NSString *portString = parts[1];
    check(portString.length && [portString rangeOfCharacterFromSet:[[NSCharacterSet decimalDigitCharacterSet] invertedSet]].location == NSNotFound,
          @"Controller port must be numeric.");
    NSInteger port = portString.integerValue;
    check(port >= 1024 && port <= 65535, @"Controller port must be between 1024 and 65535.");
    NSString *secret = config[@"secret"];
    check([secret isKindOfClass:[NSString class]] && secret.length >= 32 && secret.length <= 256 &&
          [secret rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_"] invertedSet]].location == NSNotFound,
          @"Privileged controller requires a generated secret of at least 32 characters.");
    for (NSString *key in [config.allKeys copy]) {
        NSString *lower = key.lowercaseString;
        if (([lower hasPrefix:@"external-controller"] && ![key isEqualToString:@"external-controller"]) ||
            [lower hasPrefix:@"external-ui"] || [lower isEqualToString:@"tls"] ||
            [lower isEqualToString:@"geox-url"] || [lower isEqualToString:@"geodata-loader"])
            [config removeObjectForKey:key];
    }
    NSMutableArray *providerPaths = [NSMutableArray array];
    NSUInteger index = 0;
    for (NSString *kind in @[@"proxy-providers", @"rule-providers"]) {
        id providers = config[kind];
        if (!providers) continue;
        check([providers isKindOfClass:[NSDictionary class]], @"Providers must be a mapping.");
        for (NSString *name in providers) {
            NSMutableDictionary *provider = providers[name];
            check([provider isKindOfClass:[NSMutableDictionary class]], @"Provider must be a mapping.");
            NSString *type = provider[@"type"];
            check([type isKindOfClass:[NSString class]] &&
                  ([type isEqualToString:@"http"] || [type isEqualToString:@"inline"]),
                  @"Privileged mode supports HTTP(S) or inline providers only. Convert local file providers to inline payloads.");
            if ([type isEqualToString:@"http"]) {
                NSString *urlString = provider[@"url"];
                check([urlString isKindOfClass:[NSString class]], @"HTTP provider needs a URL.");
                NSURL *url = [NSURL URLWithString:urlString];
                check(url.host.length && ([@"https" isEqualToString:url.scheme.lowercaseString] || [@"http" isEqualToString:url.scheme.lowercaseString]), @"Provider URL must use HTTP(S).");
            }
            [provider removeObjectForKey:@"path"];
            [providerPaths addObject:@{@"provider":provider, @"path":[runtime stringByAppendingPathComponent:[NSString stringWithFormat:@"provider-%lu.yaml", static_cast<unsigned long>(index++)]]}];
        }
    }
    [config removeObjectForKey:@"profile"];
    validateTree(config);
    for (NSDictionary *entry in providerPaths) entry[@"provider"][@"path"] = entry[@"path"];
    config[@"profile"] = @{@"store-selected":@YES, @"store-fake-ip":@NO};
    config[@"geo-auto-update"] = @NO;
    config[@"external-controller-cors"] = @{@"allow-origins":@[], @"allow-private-network":@NO};
    config[@"external-controller"] = [NSString stringWithFormat:@"127.0.0.1:%ld", static_cast<long>(port)];
    *endpoint = @{@"host":@"127.0.0.1", @"port":@(port), @"secret":secret};
    return config;
}

NSData *configurationData(NSDictionary *configuration) {
    // mihomo parses this file as YAML, whose quoted scalars do not accept JSON's optional \/ escape.
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:configuration
                                                 options:NSJSONWritingWithoutEscapingSlashes error:&error];
    check(data != nil, @"Cannot serialize the managed core configuration.");
    return data;
}

struct Core {
    uid_t authorizedOwner = 0;
    pid_t pid = -1;
    int output = -1;
    std::string logs;
    NSString *__strong directory = nil;
    NSDictionary *__strong endpoint = nil;
    void drain() {
        if (output < 0) return;
        char bytes[8192]; ssize_t count;
        size_t consumed = 0;
        while (consumed < 65536 && (count = read(output, bytes, sizeof(bytes))) > 0) {
            consumed += static_cast<size_t>(count);
            logs.append(bytes, static_cast<size_t>(count));
            if (logs.size() > 65536) logs.erase(0, logs.size() - 65536);
        }
    }
    void reap() {
        drain();
        if (pid <= 0) return;
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) { pid = -1; if (output >= 0) close(output); output = -1; }
    }
    void stop() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            const auto deadline = nowMs() + 3000;
            while (pid > 0 && nowMs() < deadline) { reap(); usleep(10000); }
            if (pid > 0) { kill(pid, SIGKILL); int status; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} pid = -1; }
        }
        if (output >= 0) close(output);
        output = -1;
        if (directory) { [[NSFileManager defaultManager] removeItemAtPath:directory error:nil]; directory = nil; }
        endpoint = nil;
    }
    void start(NSDictionary *config) {
        struct stat binary{};
        check(lstat(kCore, &binary) == 0 && S_ISREG(binary.st_mode) && binary.st_uid == fileOwner() && !(binary.st_mode & 0022), @"Installed core has unsafe ownership or permissions.");
        seedGeoDatabases(authorizedOwner);
        std::string pattern = std::string(kRuntime) + "/session-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
        check(mkdtemp(name.data()) != nullptr, @"Cannot create private runtime directory.");
        NSString *nextDirectory = str(name.data());
        NSDictionary *nextEndpoint = nil;
        @try {
            NSMutableDictionary *safe = sanitize(config, nextDirectory, &nextEndpoint);
            NSData *data = configurationData(safe);
            atomicData([nextDirectory stringByAppendingPathComponent:@"config.json"], data, 0600);
        } @catch (NSException *exception) {
            [[NSFileManager defaultManager] removeItemAtPath:nextDirectory error:nil]; @throw exception;
        }
        stop(); directory = nextDirectory; endpoint = nextEndpoint; logs.clear();
        int pipes[2]; check(pipe(pipes) == 0, @"Cannot create core output pipe.");
        fcntl(pipes[0], F_SETFD, FD_CLOEXEC); fcntl(pipes[0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
        const std::string configuration = [[directory stringByAppendingPathComponent:@"config.json"] fileSystemRepresentation];
        sigset_t blocked, previousMask;
        sigemptyset(&blocked); sigaddset(&blocked, SIGTERM); sigaddset(&blocked, SIGINT);
        sigprocmask(SIG_BLOCK, &blocked, &previousMask);
        pid = fork();
        if (pid == 0) {
            signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);
            sigprocmask(SIG_SETMASK, &previousMask, nullptr);
            dup2(pipes[1], STDOUT_FILENO); dup2(pipes[1], STDERR_FILENO);
            int null = open("/dev/null", O_RDONLY); dup2(null, STDIN_FILENO);
            close(pipes[0]); close(pipes[1]);
            chdir(kRuntime);
            const char *argv[] = {kCore, "-d", kRuntime, "-f", configuration.c_str(), nullptr};
            char *env[] = {const_cast<char *>("PATH=/usr/bin:/bin:/usr/sbin:/sbin"), const_cast<char *>("HOME=/var/root"), nullptr};
            execve(kCore, const_cast<char **>(argv), env); _exit(127);
        }
        sigprocmask(SIG_SETMASK, &previousMask, nullptr);
        close(pipes[1]);
        if (pid < 0) { close(pipes[0]); stop(); fail(@"Cannot launch installed core."); }
        output = pipes[0];
    }
};

NSMutableDictionary *response(Core &core, id identifier, bool ok, NSString *error = @"") {
    core.reap();
    NSMutableDictionary *result = [@{@"protocol":@1, @"id":identifier ?: @0, @"ok":@(ok), @"error":error,
        @"state":core.pid > 0 ? @"running" : @"stopped", @"pid":@(std::max(core.pid, 0))} mutableCopy];
    if (core.endpoint) result[@"endpoint"] = core.endpoint;
    return result;
}

bool sendFrame(int socket, NSDictionary *reply) {
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:reply options:0 error:nil];
    if (!bytes || bytes.length > kMaxFrame) return false;
    uint32_t length = htonl(static_cast<uint32_t>(bytes.length));
    NSMutableData *frame = [NSMutableData dataWithBytes:&length length:4]; [frame appendData:bytes];
    const char *data = static_cast<const char *>(frame.bytes); size_t remaining = frame.length;
    auto deadline = nowMs() + 3000;
    while (remaining && !stopping) {
        ssize_t count = send(socket, data, remaining, 0);
        if (count > 0) { data += count; remaining -= count; continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && nowMs() < deadline) {
            pollfd p{socket, POLLOUT, 0}; poll(&p, 1, 100); continue;
        }
        return false;
    }
    return remaining == 0;
}

void serve(uid_t owner) {
    check(geteuid() == fileOwner(), @"The service must be launched by root launchd.");
    umask(0077); signal(SIGTERM, signalStop); signal(SIGINT, signalStop); signal(SIGPIPE, SIG_IGN);
    ownedDirectory(kSocketDir, 0755); ownedDirectory(kRuntime, 0700);
    struct stat existing{};
    if (lstat(kSocket, &existing) == 0) {
        check(S_ISSOCK(existing.st_mode) && (existing.st_uid == owner || existing.st_uid == 0), @"Unexpected object at service socket path.");
        check(unlink(kSocket) == 0, @"Cannot replace stale socket.");
    }
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    check(listener >= 0, @"Cannot create service socket.");
    fcntl(listener, F_SETFD, FD_CLOEXEC); fcntl(listener, F_SETFL, O_NONBLOCK);
    sockaddr_un address{}; address.sun_family = AF_UNIX; strlcpy(address.sun_path, kSocket, sizeof(address.sun_path));
    check(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
          chown(kSocket, owner, fileOwner() == 0 ? 0 : getgid()) == 0 &&
          chmod(kSocket, 0600) == 0 && listen(listener, 4) == 0, @"Cannot expose protected service socket.");
    struct Client { int fd; std::vector<char> incoming; long long deadline = 0; };
    std::vector<Client> clients;
    int ownerSocket = -1;
    Core core;
    core.authorizedOwner = owner;
    const auto disconnect = [&](size_t index) {
        if (clients[index].fd == ownerSocket) { core.stop(); ownerSocket = -1; }
        close(clients[index].fd);
        clients.erase(clients.begin() + index);
    };
    while (!stopping) {
        @autoreleasepool {
            core.reap();
            std::vector<pollfd> descriptors{{listener, POLLIN, 0}, {core.output, POLLIN, 0}};
            for (const auto &client : clients) descriptors.push_back({client.fd, POLLIN, 0});
            if (poll(descriptors.data(), descriptors.size(), 250) < 0 && errno != EINTR) break;
            core.drain();
            for (size_t position = clients.size(); position > 0; --position) {
                const size_t index = position - 1;
                auto &client = clients[index];
                const auto events = descriptors[index + 2].revents;
                bool alive = !(events & (POLLHUP | POLLERR | POLLNVAL));
                if (alive && (events & POLLIN)) {
                    char bytes[16384]; ssize_t count = read(client.fd, bytes, sizeof(bytes));
                    if (count == 0) alive = false;
                    if (count > 0) {
                        if (client.incoming.empty()) client.deadline = nowMs() + frameTimeout();
                        client.incoming.insert(client.incoming.end(), bytes, bytes + count);
                    } else if (count < 0 && errno != EAGAIN && errno != EINTR) alive = false;
                }
                while (alive && client.incoming.size() >= 4) {
                    uint32_t length; memcpy(&length, client.incoming.data(), 4); length = ntohl(length);
                    if (length == 0 || length > kMaxFrame) { alive = false; break; }
                    if (client.incoming.size() < length + 4) break;
                    NSData *bytes = [NSData dataWithBytes:client.incoming.data() + 4 length:length];
                    client.incoming.erase(client.incoming.begin(), client.incoming.begin() + length + 4);
                    client.deadline = client.incoming.empty() ? 0 : nowMs() + frameTimeout();
                    id request = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
                    id identifier = [request isKindOfClass:[NSDictionary class]] ? request[@"id"] : @0;
                    NSMutableDictionary *reply;
                    @try {
                        check([request isKindOfClass:[NSDictionary class]] && [identifier isKindOfClass:[NSNumber class]] &&
                              [identifier doubleValue] >= 0 && [identifier doubleValue] <= 9007199254740991.0 &&
                              [identifier doubleValue] == [identifier longLongValue], @"Expected an object with a nonnegative integer id.");
                        check([request[@"protocol"] isKindOfClass:[NSNumber class]] && [request[@"protocol"] doubleValue] == 1.0,
                              @"Unsupported service protocol. Reinstall the helper to match this client.");
                        NSString *command = request[@"command"];
                        check([command isKindOfClass:[NSString class]], @"Command must be a string.");
                        if ([command isEqualToString:@"start"]) {
                            check(ownerSocket < 0 || ownerSocket == client.fd, @"Another connection owns the core lease.");
                            core.start(request[@"config"]); ownerSocket = client.fd;
                        } else if ([command isEqualToString:@"stop"]) {
                            check(ownerSocket < 0 || ownerSocket == client.fd, @"Only the connection owning the core lease can stop it.");
                            core.stop(); ownerSocket = -1;
                        } else check([command isEqualToString:@"status"] || [command isEqualToString:@"logs"], @"Unknown command.");
                        reply = response(core, identifier, true);
                        if ([command isEqualToString:@"logs"]) {
                            NSString *text = [[NSString alloc] initWithBytes:core.logs.data() length:core.logs.size() encoding:NSUTF8StringEncoding];
                            NSArray *lines = [(text ?: @"") componentsSeparatedByString:@"\n"];
                            reply[@"logs"] = lines.count > 200 ? [lines subarrayWithRange:NSMakeRange(lines.count - 200, 200)] : lines;
                        }
                    } @catch (NSException *exception) {
                        if (![identifier isKindOfClass:[NSNumber class]]) identifier = @0;
                        reply = response(core, identifier, false, exception.reason ?: @"Invalid request.");
                    }
                    if (!sendFrame(client.fd, reply)) alive = false;
                }
                if (client.deadline && nowMs() > client.deadline) alive = false;
                if (!alive) disconnect(index);
            }
            if (descriptors[0].revents & POLLIN) {
                int next = accept(listener, nullptr, nullptr);
                if (next >= 0) {
                    uid_t peer; gid_t group;
                    if (clients.size() >= 8 || getpeereid(next, &peer, &group) != 0 || peer != owner) close(next);
                    else {
                        fcntl(next, F_SETFD, FD_CLOEXEC); fcntl(next, F_SETFL, O_NONBLOCK);
                        clients.push_back({next, {}, 0});
                    }
                }
            }
        }
    }
    while (!clients.empty()) disconnect(clients.size() - 1);
    core.stop(); close(listener); unlink(kSocket);
}

#ifdef CLASH_QT_HELPER_TESTING
int coreConfigurationSelfTest(const char *corePath) {
    check(geteuid() != 0 && getuid() == geteuid(), @"Core configuration self-test must run without privilege.");
    check(corePath[0] == '/', @"Test core path must be absolute.");
    char temporary[] = "/tmp/clash-qt-helper-config-XXXXXX";
    check(mkdtemp(temporary) != nullptr, @"Cannot create configuration fixture directory.");
    NSDictionary *endpoint = nil;
    NSDictionary *safe = sanitize(@{@"external-controller":@"127.0.0.1:29197",
        @"secret":@"0123456789abcdef0123456789abcdef", @"mixed-port":@27990,
        @"proxies":@[], @"proxy-groups":@[], @"rules":@[@"MATCH,DIRECT"],
        @"tun":@{@"enable":@NO}, @"dns":@{@"enable":@NO},
        @"external-controller-cors":@{@"allow-origins":@[@"https://example.com/test"]},
        @"url-test":@"https://example.com/path/to/test"}, str(temporary), &endpoint);
    NSString *path = [str(temporary) stringByAppendingPathComponent:@"config.json"];
    atomicData(path, configurationData(safe), 0600);
    const std::string filename = path.fileSystemRepresentation;
    pid_t child = fork();
    if (child == 0) {
        const char *args[] = {corePath, "-t", "-d", temporary, "-f", filename.c_str(), nullptr};
        execv(corePath, const_cast<char **>(args)); _exit(127);
    }
    check(child > 0, @"Cannot launch validation fixture.");
    int status = 0;
    const auto deadline = nowMs() + 20000;
    while (waitpid(child, &status, WNOHANG) == 0) {
        if (nowMs() > deadline) { kill(child, SIGKILL); waitpid(child, &status, 0); break; }
        usleep(10000);
    }
    [[NSFileManager defaultManager] removeItemAtPath:str(temporary) error:nil];
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, @"Real mihomo rejected the helper's generated configuration.");
    puts("macOS helper real-core configuration test passed (unprivileged validation only).");
    return 0;
}

bool readExact(int fd, void *data, size_t size) {
    auto *bytes = static_cast<char *>(data);
    while (size) {
        ssize_t count = read(fd, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count; size -= count;
    }
    return true;
}

int connectFixture() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    check(fd >= 0, @"Fixture client socket failed.");
    sockaddr_un address{}; address.sun_family = AF_UNIX; strlcpy(address.sun_path, kSocket, sizeof(address.sun_path));
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(fd); fail(@"Fixture client could not connect.");
    }
    timeval timeout{6, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

NSDictionary *exchange(int fd, NSString *command, NSDictionary *config = nil, NSNumber *protocol = @1) {
    static long long requestId = 0;
    NSMutableDictionary *request = [@{@"protocol":protocol, @"id":@(++requestId), @"command":command} mutableCopy];
    if (config) request[@"config"] = config;
    check(sendFrame(fd, request), @"Could not send fixture command.");
    uint32_t size;
    check(readExact(fd, &size, 4), [NSString stringWithFormat:@"No framed helper response for %@ (id %lld): %s", command, requestId, strerror(errno)]); size = ntohl(size);
    check(size > 0 && size <= kMaxFrame, @"Invalid helper response size.");
    NSMutableData *bytes = [NSMutableData dataWithLength:size];
    check(readExact(fd, bytes.mutableBytes, size), @"Incomplete helper response.");
    NSDictionary *response = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
    check([response isKindOfClass:[NSDictionary class]] && [response[@"protocol"] isEqual:@1] &&
          [response[@"id"] isEqual:request[@"id"]], @"Response version/id mismatch.");
    return response;
}

void awaitStopped(int observer, pid_t child) {
    const auto deadline = nowMs() + 4500;
    while (nowMs() < deadline) {
        NSDictionary *reply = exchange(observer, @"status");
        if ([reply[@"state"] isEqualToString:@"stopped"] && kill(child, 0) < 0 && errno == ESRCH) return;
        usleep(20000);
    }
    fail(@"Core lease was not stopped and reaped.");
}

int ipcSelfTest() {
    check(geteuid() != 0 && getuid() == geteuid(), @"IPC self-test must run without privilege.");
    signal(SIGPIPE, SIG_IGN);
    char temporary[] = "/tmp/clash-qt-helper-ipc-XXXXXX";
    check(mkdtemp(temporary) != nullptr, @"Cannot create fixture directory.");
    const std::string root(temporary), runtime = root + "/runtime", socketDirectory = root + "/socket-dir", socketPath = socketDirectory + "/socket";
    uint32_t size = 0; _NSGetExecutablePath(nullptr, &size);
    std::vector<char> executable(size + 1); check(_NSGetExecutablePath(executable.data(), &size) == 0, @"Cannot locate fixture core.");
    char absolute[PATH_MAX]; check(realpath(executable.data(), absolute), @"Cannot resolve fixture core.");
    kCore = absolute; kRuntime = runtime.c_str(); kSocketDir = socketDirectory.c_str(); kSocket = socketPath.c_str();
    fixtureMode = true;
    pid_t server = fork();
    check(server >= 0, @"Cannot fork fixture service.");
    if (server == 0) {
        @try { serve(getuid()); _exit(0); }
        @catch (NSException *error) { fprintf(stderr, "Fixture service failed: %s\n", error.reason.UTF8String); _exit(1); }
    }
    int owner = -1, observer = -1;
    @try {
        const auto deadline = nowMs() + 3000;
        while (access(kSocket, F_OK) != 0 && nowMs() < deadline) usleep(10000);
        owner = connectFixture(); observer = connectFixture();
        uid_t peer; gid_t group;
        check(getpeereid(owner, &peer, &group) == 0 && peer == getuid(), @"Peer authentication did not report the fixture owner.");
        check([exchange(observer, @"status")[@"state"] isEqualToString:@"stopped"], @"Initial service state is not stopped.");
        NSDictionary *config = @{@"external-controller":@"127.0.0.1:29097", @"secret":@"0123456789abcdef0123456789abcdef", @"tun":@{@"enable":@YES}};
        NSDictionary *started = exchange(owner, @"start", config);
        check([started[@"ok"] boolValue] && [started[@"pid"] intValue] > 0, @"Owner start failed.");
        pid_t child = [started[@"pid"] intValue];
        check([exchange(observer, @"status")[@"pid"] intValue] == child, @"Observer changed the core state.");
        check(![exchange(observer, @"stop")[@"ok"] boolValue], @"Observer stopped another connection's lease.");
        check(![exchange(observer, @"start", config)[@"ok"] boolValue], @"Observer replaced another connection's lease.");
        close(observer); observer = -1;
        check([exchange(owner, @"status")[@"pid"] intValue] == child, @"Observer disconnect stopped the owner core.");
        check(![exchange(owner, @"status", nil, @2)[@"ok"] boolValue], @"Invalid protocol version was accepted.");
        NSDictionary *stopped = exchange(owner, @"stop");
        check([stopped[@"ok"] boolValue] && [stopped[@"state"] isEqualToString:@"stopped"], @"Owner stop did not acknowledge child exit.");
        check(kill(child, 0) < 0 && errno == ESRCH, @"Stopped fixture process remains alive.");
        started = exchange(owner, @"start", config); child = [started[@"pid"] intValue];
        check([started[@"ok"] boolValue], @"Second owner start failed.");
        observer = connectFixture();
        close(owner); owner = -1;
        awaitStopped(observer, child);

        owner = connectFixture(); started = exchange(owner, @"start", config); child = [started[@"pid"] intValue];
        check([started[@"ok"] boolValue], @"Invalid-frame fixture could not start.");
        uint32_t invalid = htonl(static_cast<uint32_t>(kMaxFrame + 1));
        check(writeAll(owner, &invalid, sizeof(invalid)), @"Could not send oversized frame header.");
        awaitStopped(observer, child); close(owner); owner = -1;

        owner = connectFixture(); started = exchange(owner, @"start", config); child = [started[@"pid"] intValue];
        check([started[@"ok"] boolValue], @"Partial-frame fixture could not start.");
        const char partial[] = {0, 0};
        check(writeAll(owner, partial, sizeof(partial)), @"Could not send partial frame.");
        awaitStopped(observer, child); close(owner); owner = -1;
        check([exchange(observer, @"logs")[@"logs"] isKindOfClass:[NSArray class]], @"Logs response is not an array.");
        close(observer); observer = -1;
        kill(server, SIGTERM);
        int status; while (waitpid(server, &status, 0) < 0 && errno == EINTR) {}
        server = -1;
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, @"Fixture server did not exit cleanly.");
        check(access(kSocket, F_OK) != 0, @"Server left its socket behind.");
        puts("macOS helper IPC self-test: real server framing/peer UID/observer/lease/start-stop/disconnect/invalid-frame/partial-timeout passed.");
    } @catch (NSException *error) {
        if (owner >= 0) close(owner);
        if (observer >= 0) close(observer);
        if (server > 0) { kill(server, SIGTERM); int status; while (waitpid(server, &status, 0) < 0 && errno == EINTR) {} }
        [[NSFileManager defaultManager] removeItemAtPath:str(temporary) error:nil];
        @throw error;
    }
    [[NSFileManager defaultManager] removeItemAtPath:str(temporary) error:nil];
    return 0;
}
#endif

int selfTest() {
    check(geteuid() != 0, @"Self-test must not run as root.");
    NSDictionary *base = @{@"external-controller":@"127.0.0.1:29097", @"secret":@"0123456789abcdef0123456789abcdef", @"tun":@{@"enable":@YES}};
    NSDictionary *endpoint = nil;
    NSMutableDictionary *safe = sanitize(base, @"/private/test-only", &endpoint);
    check([endpoint[@"port"] intValue] == 29097 && [safe[@"tun"][@"enable"] boolValue], @"Endpoint/TUN preservation failed.");
    for (NSDictionary *bad in @[@{@"external-controller":@"0.0.0.0:29097"}, @{@"secret":@"weak"},
        @{@"proxies":@[@{@"name":@"bad", @"private-key":@"/etc/root-secret"}]},
        @{@"proxy-providers":@{@"bad":@{@"type":@"file", @"path":@"/etc/passwd"}}},
        @{@"rule-providers":@{@"bad":@{@"type":@"http", @"url":@"file:///etc/passwd"}}}]) {
        NSMutableDictionary *candidate = [base mutableCopy]; [candidate addEntriesFromDictionary:bad];
        bool rejected = false;
        @try { sanitize(candidate, @"/private/test-only", &endpoint); } @catch (NSException *) { rejected = true; }
        check(rejected, @"Unsafe configuration was accepted.");
    }
    NSMutableDictionary *candidate = [base mutableCopy];
    candidate[@"external-ui"] = @"/etc"; candidate[@"external-ui-url"] = @"https://example.com/archive.zip";
    candidate[@"external-controller-unix"] = @"/etc/socket";
    candidate[@"proxy-providers"] = @{@"../escape":@{@"type":@"http", @"url":@"https://example.com/provider", @"path":@"../../escape"}};
    safe = sanitize(candidate, @"/private/test-only", &endpoint);
    check(!safe[@"external-ui"] && !safe[@"external-controller-unix"] &&
        [safe[@"proxy-providers"][@"../escape"][@"path"] hasPrefix:@"/private/test-only/provider-"], @"Path confinement failed.");
    candidate[@"proxies"] = @[@{@"name":@"transport", @"type":@"vmess", @"ws-opts":@{@"path":@"/websocket"},
        @"http-opts":@{@"path":@[@"/transport"]}, @"h2-opts":@{@"path":@"/h2"}}];
    safe = sanitize(candidate, @"/private/test-only", &endpoint);
    check([safe[@"proxies"][0][@"ws-opts"][@"path"] isEqualToString:@"/websocket"], @"Transport URL paths must remain usable.");
    NSString *serialized = [[NSString alloc] initWithData:configurationData(safe) encoding:NSUTF8StringEncoding];
    check(![serialized containsString:@"\\/"] && [serialized containsString:@"https://example.com/provider"],
          @"Core config must preserve literal slashes; mihomo's YAML parser rejects JSON slash escapes.");
    char temporary[] = "/tmp/clash-qt-helper-seed-XXXXXX";
    check(mkdtemp(temporary) != nullptr, @"Cannot create geodata fixture.");
    NSString *root = str(temporary), *source = [root stringByAppendingPathComponent:@"source"],
             *destination = [root stringByAppendingPathComponent:@"destination"];
    @try {
        check(mkdir(source.fileSystemRepresentation, 0700) == 0 && mkdir(destination.fileSystemRepresentation, 0700) == 0,
              @"Cannot create geodata fixture directories.");
        atomicData([source stringByAppendingPathComponent:@"Country.mmdb"], [@"synthetic-mmdb" dataUsingEncoding:NSUTF8StringEncoding], 0600);
        NSString *geoip = [source stringByAppendingPathComponent:@"geoip.dat"];
        NSString *geosite = [source stringByAppendingPathComponent:@"geosite.dat"];
        check(symlink("Country.mmdb", geoip.fileSystemRepresentation) == 0, @"Cannot create symlink rejection fixture.");
        atomicData(geosite, [@"synthetic-geosite" dataUsingEncoding:NSUTF8StringEncoding], 0666);
        check(seedGeoFromDirectory(source, destination, getuid()) == 1, @"Geodata must reject symlinks and writable inputs.");
        atomicData([source stringByAppendingPathComponent:@"Country.mmdb"], [@"replacement" dataUsingEncoding:NSUTF8StringEncoding], 0600);
        check(chmod(geosite.fileSystemRepresentation, 0600) == 0 && unlink(geoip.fileSystemRepresentation) == 0,
              @"Cannot prepare safe geodata fixture.");
        int oversized = open(geoip.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
        check(oversized >= 0, @"Cannot create size-limit fixture.");
        bool resized = ftruncate(oversized, 128 * 1024 * 1024 + 1) == 0; close(oversized);
        check(resized && seedGeoFromDirectory(source, destination, getuid()) == 1, @"Geodata size limit was not enforced.");
        NSData *preserved = [NSData dataWithContentsOfFile:[destination stringByAppendingPathComponent:@"Country.mmdb"]];
        check([preserved isEqualToData:[@"synthetic-mmdb" dataUsingEncoding:NSUTF8StringEncoding]], @"Existing geodata was replaced.");
        unlink(geoip.fileSystemRepresentation);
        check(mkfifo(geoip.fileSystemRepresentation, 0600) == 0, @"Cannot create nonregular geodata fixture.");
        check(seedGeoFromDirectory(source, destination, getuid()) == 0, @"Nonregular geodata must be rejected without blocking.");
        struct stat mode{};
        check(lstat([destination stringByAppendingPathComponent:@"Country.mmdb"].fileSystemRepresentation, &mode) == 0 &&
              (mode.st_mode & 0777) == 0600 && mode.st_uid == getuid(), @"Seeded geodata must be private and nonexecutable.");
    } @catch (NSException *exception) {
        [[NSFileManager defaultManager] removeItemAtPath:root error:nil]; @throw exception;
    }
    [[NSFileManager defaultManager] removeItemAtPath:root error:nil];
    puts("macOS helper self-test: 10 cases passed (no installation, root execution or network).");
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    @autoreleasepool {
        bool mutation = argc >= 2 && (!strcmp(argv[1], "--install") || !strcmp(argv[1], "--uninstall"));
        @try {
#ifdef CLASH_QT_HELPER_TESTING
            if (argc == 2 && !strcmp(argv[1], "--ipc-self-test")) return ipcSelfTest();
            if (argc == 3 && !strcmp(argv[1], "--core-config-self-test")) return coreConfigurationSelfTest(argv[2]);
            if (argc == 5 && !strcmp(argv[1], "-d") && !strcmp(argv[3], "-f")) {
                check(geteuid() != 0 && getuid() == geteuid(), @"Fixture core cannot run privileged.");
                signal(SIGTERM, signalStop); signal(SIGINT, signalStop);
                puts("Fixture core started"); fflush(stdout);
                while (!stopping) pause();
                return 0;
            }
#endif
            if (argc == 2 && !strcmp(argv[1], "--self-test")) return selfTest();
            if (argc == 4 && !strcmp(argv[1], "--install")) install(parseOwner(argv[2]), argv[3]);
            else if (argc == 2 && !strcmp(argv[1], "--uninstall")) uninstall();
            else if (argc == 3 && !strcmp(argv[1], "--serve")) { serve(parseOwner(argv[2])); return 0; }
            else fail(@"Usage: helper --install UID ABSOLUTE_CORE | --uninstall | --serve UID | --self-test");
            puts("CLASH_QT_SERVICE_RESULT {\"ok\":true,\"error\":\"\"}");
            return 0;
        } @catch (NSException *exception) {
            NSDictionary *result = @{@"ok":@NO, @"error":exception.reason ?: @"Helper failure."};
            NSData *data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
            fprintf(mutation ? stdout : stderr, "%s%s\n", mutation ? "CLASH_QT_SERVICE_RESULT " : "", [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
            return 1;
        }
    }
}
