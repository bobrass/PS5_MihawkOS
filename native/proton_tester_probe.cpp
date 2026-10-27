// SPDX-License-Identifier: GPL-3.0-or-later
#include "proton_tester_probe.h"
#include "proton_flex_probe.h"
#include "proton_loading.h"
#include "../src/pw_firmware.h"
#include <ps5platform/elevation.h>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int sceKernelDlsym(int module, const char *name, void **address);
int sceKernelGetModuleList(int32_t *handles, size_t capacity, size_t *count);
int sceKernelGetModuleInfo(int handle, void *info);
int sceKernelAvailableFlexibleMemorySize(size_t *);
int sceKernelUsleep(unsigned);
}

namespace {
constexpr int kVersion = 1;          // the probe's own version, in its file and on its screen
constexpr int kLibkernel = 0x2001;   // libkernel's module handle in a title
const char *const kWays[FLEX_EXEC_WAYS] = {"direct0", "direct12", "main12", "mapx", "anon", "jit"};

// A fault or a stall in one step becomes that step's answer, not the probe's end: SIGSEGV, SIGBUS and SIGILL from a
// call into memory the kernel said was executable, and SIGALRM from a step that does not return.
sigjmp_buf guard_jump;
volatile sig_atomic_t guard_armed;
void guard_signal(int signal)
{
    if (guard_armed) siglongjmp(guard_jump, signal);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}
template <class Step> int guarded(unsigned seconds, Step step)
{
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGALRM}) std::signal(signal, guard_signal);
    const int caught = sigsetjmp(guard_jump, 1);
    if (!caught) {
        guard_armed = 1;
        alarm(seconds);
        step();
    }
    alarm(0);
    guard_armed = 0;
    return caught;
}

// Text for the file and the screen, with no identifier of the console or its owner in it (docs/FLEXIBLE_MEMORY.md).
std::string model_name(std::string &source)
{
    // Every loaded libkernel variant (libkernel, libkernel_sys, ...) by its handle; the call is looked up by name in each.
    int32_t handles[256];
    size_t count = 0;
    std::vector<std::pair<int, std::string>> kernels = {{kLibkernel, "libkernel"}};
    if (!sceKernelGetModuleList(handles, 256, &count))
        for (size_t i = 0; i < count && i < 256; ++i) {
            alignas(8) unsigned char info[0x160] = {};
            *reinterpret_cast<uint64_t *>(info) = sizeof(info);
            if (sceKernelGetModuleInfo(handles[i], info)) continue;
            const char *name = reinterpret_cast<const char *>(info + 8);   // SceKernelModuleInfo: size, then the name
            if (!strncmp(name, "libkernel", 9)) kernels.emplace_back(handles[i], std::string(name, strnlen(name, 64)));
        }
    for (const auto &kernel : kernels) {
        int (*hw_model)(char *) = nullptr;
        char name[64] = {};
        if (sceKernelDlsym(kernel.first, "sceKernelGetHwModelName", reinterpret_cast<void **>(&hw_model)) || !hw_model) continue;
        if (guarded(5, [&] { (void)hw_model(name); }) || !name[0]) continue;
        std::string clean;
        for (const char *c = name; *c && clean.size() < 32; ++c)
            if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-') clean += *c;
        if (clean.empty()) continue;
        source = kernel.second;
        return clean;
    }
    source = "unavailable";
    return "unknown-model";
}

int bit22_shipped(const std::string &root)
{
    char text[8192] = {};
    const int fd = open((root + "/sce_sys/param.json").c_str(), O_RDONLY);
    if (fd < 0) return -1;
    const ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    const char *at = got > 0 ? strstr(text, "\"attribute3\"") : nullptr;
    if (!at || !(at = strchr(at, ':'))) return -1;
    return (strtoull(at + 1, nullptr, 10) >> 22) & 1;
}

std::string exec_word(const ProtonFlexExec &e, int fault)
{
    if (fault) return "faulted";
    if (e.made) return "refused";
    if (e.mapped) return "map refused";
    if (e.value == 42) return "runs";
    if (e.prot >= 0 && !(e.prot & PROT_EXEC)) return "no execute";
    return "no";
}

// 0, or the errno of the open or the write.
int write_file(const std::string &path, const std::string &text)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return errno ? errno : EIO;
    const bool ok = write(fd, text.data(), text.size()) == ssize_t(text.size());
    const int error = ok ? 0 : (errno ? errno : EIO);
    close(fd);
    return error;
}
}

int proton_tester_probe_elevate()
{
    // MihawkOS's own request (ps5/src/proton_launch.cpp proton_data_grant), asked once with its 3 s wait: a tester who
    // started MihawkOS since boot has the daemon resident. Nothing else of MihawkOS's is touched.
    static const struct ps5_elevation_config contract = {
        "/download0/elevate_proc", "/download0", "/download0/lapy_owned_result", "/data", 60, 50000,
    };
    return int(ps5_elevation_request_with(&contract, PS5_ELEVATION_FILESYSTEM));
}

int proton_tester_probe_main(int elevation)
{
    std::vector<std::string> lines;
    // Its own folder, wherever this process sees it: /app0, or when granted the sandbox's path or the install folder
    // (ps5/src/proton_launch.cpp proton_app_root); on the base PS5 granted, /data/homebrew/PPSA99171 was not there (ENOENT).
    std::string root = "/app0";
    for (const char *candidate : {"/app0", "/mnt/sandbox/PPSA99171_000/app0", "/data/homebrew/PPSA99171"}) {
        struct stat status;
        if (stat((std::string(candidate) + "/eboot.bin").c_str(), &status) == 0) { root = candidate; break; }
    }
    // The loading screen draws straight through VideoOut with the title's own fonts (assets/hui/fonts in the folder).
    proton_loading_start("MihawkOS memory probe", (root + "/assets/hui/fonts").c_str());
    proton_loading_report("MihawkOS memory probe", {"Measuring, about a minute. Leave the console alone."});

    // The console: what it is and what it runs.
    std::string model_source;
    const std::string model = model_name(model_source);
    unsigned reported = 0;
    const unsigned installed = pw_firmware_installed(&reported);
    char firmware[8] = "unknown", shown[8] = "unknown";
    (void)pw_firmware_text(installed, firmware, sizeof(firmware));
    (void)pw_firmware_text(reported, shown, sizeof(shown));
    size_t flexible = 0;
    const int flexible_status = sceKernelAvailableFlexibleMemorySize(&flexible);
    const int64_t direct = sceKernelGetDirectMemorySize();
    const int bit22 = bit22_shipped(root);

    // Executable memory in the title, each way under the guard.
    ProtonFlexExec title[FLEX_EXEC_WAYS] = {};
    int title_fault[FLEX_EXEC_WAYS] = {};
    for (unsigned way = 0; way < FLEX_EXEC_WAYS; ++way)
        title_fault[way] = guarded(10, [&] { proton_flex_exec_try(way, &title[way]); });

    // The same in a Sony local process, and which pool its memory is charged to (proton_service_preflight.cpp).
    ProtonFlexRun run = {};
    std::string run_log;
    const int run_fault = guarded(90, [&] { (void)proton_flex_run(&run, &run_log); });

    // The verdict.
    auto runs = [](const ProtonFlexExec *e, const int *fault, unsigned way) { return !fault[way] && e[way].value == 42; };
    const int none[FLEX_EXEC_WAYS] = {};
    bool title_direct = false, helper_direct = false;
    for (unsigned way : {FLEX_EXEC_DIRECT0, FLEX_EXEC_DIRECT12, FLEX_EXEC_MAIN12, FLEX_EXEC_MAPX}) {
        title_direct = title_direct || runs(title, title_fault, way);
        helper_direct = helper_direct || (run.answered && runs(run.reply.exec, none, way));
    }
    const bool title_jit = runs(title, title_fault, FLEX_EXEC_JIT);
    const bool helper_jit = run.answered && runs(run.reply.exec, none, FLEX_EXEC_JIT);
    const int64_t charged = run.answered && run.title_before > 0 && run.title_held > 0 ? (run.title_before - run.title_held) >> 20 : -1;
    char line[200];
    // A Pro in its memory mode has a larger direct pool (12928 MiB against 11712 when bit 22 is shipped, 2026-10-24/25).
    const char *memory_mode = bit22 == 1 ? (direct >> 20) >= 12800 ? "Pro memory mode on" : "Pro memory mode off" : "Pro memory mode not asked";
    snprintf(line, sizeof(line), "%s, firmware %s (the system says %s); %s", model.c_str(), firmware, shown, memory_mode);
    lines.push_back(line);
    snprintf(line, sizeof(line), "MihawkOS's /data grant: %s", elevation == PS5_ELEVATION_OK ? "granted"
             : "not granted (start MihawkOS once, then this probe)");
    lines.push_back(line);
    snprintf(line, sizeof(line), "This probe's title: code runs from %s", title_direct ? "direct memory and JIT" : title_jit ? "JIT only" : "nothing (JIT refused)");
    lines.push_back(line);
    if (!run.spawned)
        snprintf(line, sizeof(line), "Sony helper: not started (%#x)", unsigned(run.spawn_status));
    else if (!run.answered)
        snprintf(line, sizeof(line), "Sony helper: started, no answer");
    else
        snprintf(line, sizeof(line), "Sony helper: code runs from %s; JIT %s", helper_direct ? "direct memory" : helper_jit ? "JIT only" : "nothing",
                 run.reply.jit_made ? "refused" : "allowed");
    lines.push_back(line);
    if (charged >= 0) {
        snprintf(line, sizeof(line), "Sony helper's 64 MiB came from the title's memory: %s (%lld MiB)", charged >= 48 ? "yes" : "no", (long long)charged);
        lines.push_back(line);
    }

    // The record: text and JSON, named by model and firmware.
    std::string json = "{\n";
    auto field = [&json](const char *key, const std::string &value, bool quote = true) {
        json += std::string("  \"") + key + "\": " + (quote ? "\"" + value + "\"" : value) + ",\n";
    };
    field("probe_version", std::to_string(kVersion), false);
    field("model", model);
    field("model_source", model_source);
    field("firmware_installed", firmware);
    field("firmware_reported", shown);
    field("pro_mode_bit22_shipped", std::to_string(bit22), false);
    field("pro_memory_mode", memory_mode);
    field("elevation_status", ps5_elevation_status_name(ps5_elevation_status(elevation)));
    field("elevated", std::to_string(elevation == PS5_ELEVATION_OK), false);
    field("direct_pool_mib", std::to_string(direct >> 20), false);
    field("title_flexible_free_mib", flexible_status ? "null" : std::to_string(flexible >> 20), false);
    std::string ways = "{";
    for (unsigned way = 0; way < FLEX_EXEC_WAYS; ++way) {
        char one[200];
        snprintf(one, sizeof(one), "%s\"%s\": {\"title\": \"%s\", \"title_prot\": %d, \"helper\": \"%s\", \"helper_prot\": %d}",
                 way ? ", " : "", kWays[way], exec_word(title[way], title_fault[way]).c_str(), title[way].prot,
                 run.answered ? exec_word(run.reply.exec[way], 0).c_str() : "not run", run.answered ? run.reply.exec[way].prot : -1);
        ways += one;
    }
    field("execute", ways + "}", false);
    char helper[400];
    snprintf(helper, sizeof(helper), "{\"spawned\": %d, \"spawn_status\": \"%#x\", \"answered\": %d, \"faulted\": %d, \"jit_create\": \"%#x\", "
             "\"title_flexible_before_mib\": %lld, \"title_flexible_held_mib\": %lld, \"title_flexible_after_mib\": %lld}",
             run.spawned, unsigned(run.spawn_status), run.answered, run_fault != 0, unsigned(run.reply.jit_made),
             (long long)(run.title_before >> 20), (long long)(run.title_held >> 20), (long long)(run.title_after >> 20));
    field("sony_helper", helper, false);
    std::string verdict = "[";
    for (size_t i = 0; i < lines.size(); ++i) verdict += (i ? ", \"" : "\"") + lines[i] + "\"";
    json += "  \"verdict\": " + verdict + "]\n}\n";
    std::string text = "MihawkOS memory probe " + std::to_string(kVersion) + "\n";
    for (const auto &l : lines) text += l + "\n";
    text += "\n" + run_log;

    const std::string name = "memprobe-" + model + "-" + firmware;
    std::string saved, attempts;
    for (const std::string folder : {root, std::string("/mnt/usb0"), std::string("/mnt/usb1"), std::string("/download0")}) {
        const std::string base = folder + "/" + name;
        // Each place's answer goes into the record itself (the copies written after it carry it).
        attempts += folder + "=" + std::to_string(write_file(base + ".probe-test", "")) + " ";
        unlink((base + ".probe-test").c_str());
    }
    json.insert(json.rfind("  \"verdict\""), "  \"save_errno\": \"" + attempts + "\",\n");
    text += "\nsave errno: " + attempts + "\n";
    for (const std::string folder : {root, std::string("/mnt/usb0"), std::string("/mnt/usb1"), std::string("/download0")}) {
        const std::string base = folder + "/" + name;
        if (!write_file(base + ".txt", text) && !write_file(base + ".json", json)) {
            if (saved.empty()) saved = base + ".txt";
            if (folder != root) break;   // the title folder, and one copy beside it
        }
    }
    lines.push_back(saved.empty() ? "Not saved: photograph this screen." : "Saved: " + saved);
    lines.push_back("Save tries (0 = written): " + attempts);
    lines.push_back("Send the photo or the file to the MihawkOS owner. This closes in 60 seconds.");
    proton_loading_report("MihawkOS memory probe", lines);
    sceKernelUsleep(60u * 1000u * 1000u);
    (void)proton_loading_stop("tester probe done");
    return 0;
}
