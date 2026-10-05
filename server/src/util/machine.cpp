#include "machine.h"

#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace fernsdr {

namespace {

bool read_file(const char* path, std::string& out) {
    std::FILE* file = std::fopen(path, "re");
    if (!file) return false;
    out.clear();
    char buffer[4096];
    size_t got;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) out.append(buffer, got);
    std::fclose(file);
    return true;
}

// The number after `key` at the start of a line, or false.
bool field_kb(const std::string& text, const char* key, uint64_t& out) {
    size_t at = text.find(key);
    while (at != std::string::npos && at > 0 && text[at - 1] != '\n') at = text.find(key, at + 1);
    if (at == std::string::npos) return false;
    const char* start = text.c_str() + at + std::char_traits<char>::length(key);
    char* end = nullptr;
    const unsigned long long value = std::strtoull(start, &end, 10);
    if (end == start) return false;
    out = value;
    return true;
}

}  // namespace

bool parse_proc_stat(const std::string& text, CpuTimes& out) {
    // "cpu  user nice system idle iowait irq softirq steal guest guest_nice":
    // guest time is already inside user, so only the first eight are added.
    if (text.compare(0, 4, "cpu ") != 0) return false;
    std::istringstream line(text.substr(4, text.find('\n') - 4));
    std::vector<uint64_t> values;
    uint64_t value;
    while (values.size() < 8 && line >> value) values.push_back(value);
    if (values.size() < 4) return false;
    out.idle = values[3] + (values.size() > 4 ? values[4] : 0);
    out.total = 0;
    for (uint64_t v : values) out.total += v;
    return true;
}

bool parse_process_ticks(const std::string& text, uint64_t& ticks) {
    // The command name in parentheses may hold spaces and parentheses itself,
    // so the fields are counted from the last ')': state is the first after
    // it, utime the twelfth and stime the thirteenth.
    const size_t close = text.rfind(')');
    if (close == std::string::npos) return false;
    std::istringstream rest(text.substr(close + 1));
    std::string field;
    uint64_t utime = 0, stime = 0;
    for (int i = 1; i <= 13; i++) {
        if (!(rest >> field)) return false;
        if (i == 12) utime = std::strtoull(field.c_str(), nullptr, 10);
        if (i == 13) stime = std::strtoull(field.c_str(), nullptr, 10);
    }
    ticks = utime + stime;
    return true;
}

bool parse_meminfo(const std::string& text, uint64_t& total_kb, uint64_t& available_kb) {
    if (!field_kb(text, "MemTotal:", total_kb)) return false;
    if (field_kb(text, "MemAvailable:", available_kb)) return true;
    // Kernels before 3.14 have no MemAvailable; free memory and the caches the
    // kernel gives back on demand are the estimate it later made official.
    uint64_t free_kb = 0, buffers_kb = 0, cached_kb = 0;
    if (!field_kb(text, "MemFree:", free_kb)) return false;
    field_kb(text, "Buffers:", buffers_kb);
    field_kb(text, "Cached:", cached_kb);
    available_kb = std::min(total_kb, free_kb + buffers_kb + cached_kb);
    return true;
}

std::string parse_cgroup_path(const std::string& text, const std::string& controller) {
    // One line per hierarchy, "id:controllers:path"; v2's is "0::path".
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t first = line.find(':');
        const size_t second = first == std::string::npos ? first : line.find(':', first + 1);
        if (second == std::string::npos) continue;
        const std::string controllers = line.substr(first + 1, second - first - 1);
        if (controller.empty()) {
            if (line.compare(0, first, "0") == 0 && controllers.empty()) return line.substr(second + 1);
            continue;
        }
        std::istringstream list(controllers);
        std::string name;
        while (std::getline(list, name, ',')) {
            if (name == controller) return line.substr(second + 1);
        }
    }
    return "";
}

double parse_cpu_max(const std::string& text) {
    std::istringstream in(text);
    std::string quota;
    double period = 0;
    if (!(in >> quota >> period) || quota == "max" || period <= 0) return 0;
    char* end = nullptr;
    const double value = std::strtod(quota.c_str(), &end);
    if (end == quota.c_str() || value <= 0) return 0;
    return value / period;
}

uint64_t parse_memory_limit(const std::string& text) {
    if (text.compare(0, 3, "max") == 0) return 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str()) return 0;
    // cgroup v1 writes "no limit" as the largest page-aligned number.
    if (value >= (1ULL << 60)) return 0;
    return value;
}

bool parse_resident_kb(const std::string& text, uint64_t& kb) { return field_kb(text, "VmRSS:", kb); }

void MachineSampler::tick(int64_t now_ms, int64_t every_ms) {
    if (primed_ && now_ms - last_ms_ < every_ms) return;
    std::string text;
    CpuTimes cpu;
    uint64_t process_ticks = 0;
    if (!read_file("/proc/stat", text) || !parse_proc_stat(text, cpu)) return;
    if (!read_file("/proc/self/stat", text) || !parse_process_ticks(text, process_ticks)) return;

    if (primed_ && cpu.total > last_cpu_.total && now_ms > last_ms_) {
        const double all = static_cast<double>(cpu.total - last_cpu_.total);
        const double idle = static_cast<double>(cpu.idle - last_cpu_.idle);
        figures_.system_cpu = 100.0 * (all - idle) / all;
        // Process time is in the same ticks; against the wall clock it is a
        // share of one core, as top shows it.
        const double tick_hz = static_cast<double>(::sysconf(_SC_CLK_TCK));
        const double seconds = static_cast<double>(now_ms - last_ms_) / 1000.0;
        figures_.process_cpu = 100.0 * static_cast<double>(process_ticks - last_process_ticks_) / tick_hz / seconds;
        figures_.known = true;
    }
    // The CPUs this process may use: a container or taskset may allow fewer
    // than the machine has.
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    figures_.cores = ::sched_getaffinity(0, sizeof(allowed), &allowed) == 0
                         ? CPU_COUNT(&allowed)
                         : static_cast<int>(::sysconf(_SC_NPROCESSORS_ONLN));

    uint64_t total_kb = 0, available_kb = 0, resident_kb = 0;
    if (read_file("/proc/meminfo", text) && parse_meminfo(text, total_kb, available_kb)) {
        figures_.memory_total_bytes = total_kb * 1024;
        figures_.memory_available_bytes = available_kb * 1024;
    }
    // Inside a container or a service with a memory limit, that limit and
    // what the cgroup uses are the figures that matter: a container capped at
    // 1 GB is not helped by being told the host has 15 GB free.
    CgroupLimits limits;
    if (read_file("/proc/self/cgroup", text)) limits = read_cgroup_limits("/sys/fs/cgroup", text);
    figures_.cpu_limit = limits.cpu_cores;
    figures_.memory_limited = limits.memory_bytes > 0 && limits.memory_bytes < figures_.memory_total_bytes;
    if (figures_.memory_limited) {
        figures_.memory_total_bytes = limits.memory_bytes;
        figures_.memory_available_bytes =
            limits.memory_bytes > limits.memory_used_bytes ? limits.memory_bytes - limits.memory_used_bytes : 0;
    }
    if (read_file("/proc/self/status", text) && parse_resident_kb(text, resident_kb)) {
        figures_.process_memory_bytes = resident_kb * 1024;
    }

    last_cpu_ = cpu;
    last_process_ticks_ = process_ticks;
    last_ms_ = now_ms;
    primed_ = true;
}

namespace {

// `mount` + `path` and each directory above it up to `mount` itself.
std::vector<std::string> cgroup_dirs(const std::string& mount, std::string path) {
    std::vector<std::string> dirs;
    while (!path.empty() && path != "/") {
        dirs.push_back(mount + path);
        path.erase(path.rfind('/'));
    }
    dirs.push_back(mount);
    return dirs;
}

// What a cgroup uses counts the page cache, and the waterfall archive keeps
// a lot of it; the inactive part is what the kernel drops first under the
// limit, so it is taken off, as docker stats and the kubelet do.
void tighten_memory(CgroupLimits& limits, uint64_t limit, const std::string& dir, const char* used_file,
                    const char* inactive_key) {
    if (limit == 0 || (limits.memory_bytes > 0 && limit >= limits.memory_bytes)) return;
    std::string text;
    uint64_t used = read_file((dir + used_file).c_str(), text) ? std::strtoull(text.c_str(), nullptr, 10) : 0;
    uint64_t inactive = 0;
    if (read_file((dir + "/memory.stat").c_str(), text) && field_kb(text, inactive_key, inactive)) {
        used = used > inactive ? used - inactive : 0;
    }
    limits.memory_bytes = limit;
    limits.memory_used_bytes = used;
}

void tighten_cpu(CgroupLimits& limits, double cores) {
    if (cores > 0 && (limits.cpu_cores == 0 || cores < limits.cpu_cores)) limits.cpu_cores = cores;
}

}  // namespace

CgroupLimits read_cgroup_limits(const std::string& root, const std::string& proc_self_cgroup) {
    CgroupLimits limits;
    std::string text;
    // A unified (v2) mount has cgroup.controllers at its root; a v1 or hybrid
    // one is a directory per controller, as older Docker and distributions
    // (Debian 10, CentOS 7, Raspberry Pi OS before Bookworm) mount it.
    if (read_file((root + "/cgroup.controllers").c_str(), text)) {
        for (const std::string& dir : cgroup_dirs(root, parse_cgroup_path(proc_self_cgroup, ""))) {
            if (read_file((dir + "/cpu.max").c_str(), text)) tighten_cpu(limits, parse_cpu_max(text));
            if (read_file((dir + "/memory.max").c_str(), text)) {
                tighten_memory(limits, parse_memory_limit(text), dir, "/memory.current", "inactive_file ");
            }
        }
        return limits;
    }
    for (const std::string& dir : cgroup_dirs(root + "/memory", parse_cgroup_path(proc_self_cgroup, "memory"))) {
        if (read_file((dir + "/memory.limit_in_bytes").c_str(), text)) {
            tighten_memory(limits, parse_memory_limit(text), dir, "/memory.usage_in_bytes", "total_inactive_file ");
        }
    }
    for (const std::string& dir : cgroup_dirs(root + "/cpu", parse_cgroup_path(proc_self_cgroup, "cpu"))) {
        std::string quota, period;
        if (!read_file((dir + "/cpu.cfs_quota_us").c_str(), quota) || !read_file((dir + "/cpu.cfs_period_us").c_str(), period)) {
            continue;
        }
        const long q = std::strtol(quota.c_str(), nullptr, 10);
        const long p = std::strtol(period.c_str(), nullptr, 10);
        if (q > 0 && p > 0) tighten_cpu(limits, static_cast<double>(q) / static_cast<double>(p));
    }
    return limits;
}

}  // namespace fernsdr
