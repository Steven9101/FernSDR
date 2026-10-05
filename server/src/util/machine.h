// What the machine is doing, for the admin panel: how much CPU FernSDR and
// the whole system use, and how much memory. Read from /proc, as Linux keeps
// it; the parsers take the text so the tests can hand them any.
#pragma once
#include <cstdint>
#include <string>

namespace fernsdr {

// All CPUs together, from the first line of /proc/stat, in clock ticks.
struct CpuTimes {
    uint64_t idle = 0;   // idle and waiting for I/O
    uint64_t total = 0;  // everything the line counts
};
bool parse_proc_stat(const std::string& text, CpuTimes& out);
// User and system time of a process, in clock ticks, from /proc/<pid>/stat.
bool parse_process_ticks(const std::string& text, uint64_t& ticks);
// MemTotal and MemAvailable, in kB, from /proc/meminfo.
bool parse_meminfo(const std::string& text, uint64_t& total_kb, uint64_t& available_kb);
// VmRSS, in kB, from /proc/<pid>/status.
bool parse_resident_kb(const std::string& text, uint64_t& kb);

// Limits a container or a systemd service may set, read from the process's
// cgroup, since /proc/meminfo and the CPU count describe the whole host.
// The path of this process's cgroup from /proc/self/cgroup: the cgroup v2
// line ("0::/path") for an empty `controller`, otherwise the v1 line that
// lists that controller ("4:cpu,cpuacct:/path"). Empty when there is none.
std::string parse_cgroup_path(const std::string& text, const std::string& controller);
// cpu.max ("200000 100000" is two cores' worth; "max ..." is none), as cores,
// or 0 for no limit.
double parse_cpu_max(const std::string& text);
// memory.max and v1's memory.limit_in_bytes: bytes, or 0 for "max" or a
// value so large it means none.
uint64_t parse_memory_limit(const std::string& text);

struct CgroupLimits {
    double cpu_cores = 0;       // 0 for no quota
    uint64_t memory_bytes = 0;  // 0 for no limit
    uint64_t memory_used_bytes = 0;  // what the limited cgroup holds
};
// The tightest limits on this process's cgroup and every cgroup above it, as
// mounted under `root` (/sys/fs/cgroup), v2 or v1. A limit may sit on a
// parent (a systemd slice, a pod) rather than on the process's own cgroup,
// and inside a container the cgroup files of the host's path may be missing
// while the container's own sit at the mount root; walking up covers both.
CgroupLimits read_cgroup_limits(const std::string& root, const std::string& proc_self_cgroup);

struct MachineFigures {
    bool known = false;       // false until two samples are apart
    double process_cpu = 0;   // percent of one core, so 250 on two and a half
    double system_cpu = 0;    // percent of every core together, 0 to 100
    int cores = 0;             // the CPUs this process may run on
    double cpu_limit = 0;      // a container's or service's CPU quota in cores, 0 for none
    uint64_t process_memory_bytes = 0;
    uint64_t memory_total_bytes = 0;
    uint64_t memory_available_bytes = 0;
    bool memory_limited = false;  // the totals are a container's or service's limit, not the host's
};

// Samples /proc on the application's tick and keeps the figures between two
// samples, so that a reading is the average over the last few seconds rather
// than a guess from one instant.
class MachineSampler {
public:
    // Takes a sample once `every_ms` has passed since the last one.
    void tick(int64_t now_ms, int64_t every_ms = 2000);
    const MachineFigures& figures() const { return figures_; }

private:
    int64_t last_ms_ = 0;
    CpuTimes last_cpu_;
    uint64_t last_process_ticks_ = 0;
    bool primed_ = false;
    MachineFigures figures_;
};

}  // namespace fernsdr
