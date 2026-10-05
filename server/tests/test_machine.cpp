#include "../src/util/machine.h"
#include "test_util.h"

#include <stdlib.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

// Lines as Linux writes them, so the parsers are held to the real layout
// rather than to what the code happens to expect.
TEST_CASE(machine_reads_all_cpus_from_proc_stat) {
    fernsdr::CpuTimes cpu;
    CHECK(fernsdr::parse_proc_stat("cpu  100 5 50 800 20 3 2 1 40 0\ncpu0 50 2 25 400 10 1 1 0 20 0\n", cpu));
    // idle is idle and iowait; guest time is inside user and is not added twice.
    CHECK_EQ(cpu.idle, 820u);
    CHECK_EQ(cpu.total, 100u + 5 + 50 + 800 + 20 + 3 + 2 + 1);
    CHECK(fernsdr::parse_proc_stat("cpu  1 2 3 4\n", cpu));
    CHECK_EQ(cpu.idle, 4u);
    CHECK(!fernsdr::parse_proc_stat("cpu0 1 2 3 4\n", cpu));
    CHECK(!fernsdr::parse_proc_stat("cpu  1 2\n", cpu));
    CHECK(!fernsdr::parse_proc_stat("", cpu));
}

TEST_CASE(machine_reads_a_process_time_past_a_name_with_spaces_and_parentheses) {
    uint64_t ticks = 0;
    // pid (comm) state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime ...
    CHECK(fernsdr::parse_process_ticks("1234 (fern sdr (x)) S 1 1234 1234 0 -1 4194560 100 0 0 0 750 250 0 0 20 0 9\n", ticks));
    CHECK_EQ(ticks, 1000u);
    CHECK(!fernsdr::parse_process_ticks("1234 fernsdr S 1", ticks));
    CHECK(!fernsdr::parse_process_ticks("1234 (fernsdr) S 1 2 3", ticks));
}

TEST_CASE(machine_reads_memory_and_resident_size) {
    uint64_t total = 0, available = 0, resident = 0;
    CHECK(fernsdr::parse_meminfo("MemTotal:       16303364 kB\nMemFree:  100 kB\nMemAvailable:    9200000 kB\n", total, available));
    CHECK_EQ(total, 16303364u);
    CHECK_EQ(available, 9200000u);
    CHECK(!fernsdr::parse_meminfo("MemTotal:       16303364 kB\n", total, available));
    // A key that only ends another one's name is not it.
    CHECK(!fernsdr::parse_meminfo("XMemTotal: 1 kB\nXMemAvailable: 1 kB\n", total, available));
    CHECK(fernsdr::parse_meminfo("XMemTotal: 1 kB\nMemTotal: 8 kB\nMemAvailable: 2 kB\n", total, available));
    CHECK_EQ(total, 8u);
    // Kernels before 3.14 have no MemAvailable: free plus buffers plus cache.
    CHECK(fernsdr::parse_meminfo("MemTotal: 1000 kB\nMemFree: 300 kB\nBuffers: 50 kB\nCached: 200 kB\n", total, available));
    CHECK_EQ(available, 550u);
    CHECK(fernsdr::parse_resident_kb("Name:\tfernsdr\nVmPeak:\t 9000 kB\nVmRSS:\t    7884 kB\n", resident));
    CHECK_EQ(resident, 7884u);
}

TEST_CASE(machine_sampler_gives_figures_once_two_samples_are_apart) {
    fernsdr::MachineSampler sampler;
    sampler.tick(1000);
    CHECK(!sampler.figures().known);
    // Some work between the samples, so the process has time to count.
    volatile double sink = 0;
    for (int i = 0; i < 20000000; i++) sink = sink + i * 0.5;
    sampler.tick(3100);
    const fernsdr::MachineFigures& f = sampler.figures();
    CHECK(f.known);
    CHECK(f.cores >= 1);
    CHECK(f.system_cpu >= 0.0 && f.system_cpu <= 100.0);
    CHECK(f.process_cpu >= 0.0);
    CHECK(f.memory_total_bytes > f.memory_available_bytes);
    CHECK(f.process_memory_bytes > 0);
    // Asked again too soon, it keeps what it has.
    sampler.tick(3200);
    CHECK(sampler.figures().known);
}

TEST_CASE(machine_reads_cgroup_paths_and_limits) {
    const std::string v2 = "0::/system.slice/fernsdr.service\n";
    const std::string v1 = "12:memory:/docker/abc\n4:cpu,cpuacct:/docker/abc\n1:name=systemd:/docker/abc\n0::/\n";
    CHECK_EQ_STR(fernsdr::parse_cgroup_path(v2, ""), "/system.slice/fernsdr.service");
    CHECK_EQ_STR(fernsdr::parse_cgroup_path(v1, "memory"), "/docker/abc");
    CHECK_EQ_STR(fernsdr::parse_cgroup_path(v1, "cpu"), "/docker/abc");
    CHECK_EQ_STR(fernsdr::parse_cgroup_path(v1, "cpuset"), "");
    CHECK_EQ_STR(fernsdr::parse_cgroup_path(v1, ""), "/");

    CHECK_EQ(fernsdr::parse_cpu_max("200000 100000\n"), 2.0);
    CHECK_EQ(fernsdr::parse_cpu_max("50000 100000\n"), 0.5);
    CHECK_EQ(fernsdr::parse_cpu_max("max 100000\n"), 0.0);
    CHECK_EQ(fernsdr::parse_cpu_max(""), 0.0);
    CHECK_EQ(fernsdr::parse_memory_limit("1073741824\n"), 1073741824u);
    CHECK_EQ(fernsdr::parse_memory_limit("max\n"), 0u);
    // cgroup v1's "no limit".
    CHECK_EQ(fernsdr::parse_memory_limit("9223372036854771712\n"), 0u);
    CHECK_EQ(fernsdr::parse_memory_limit(""), 0u);
}

namespace {

struct FakeCgroupfs {
    std::string root;
    FakeCgroupfs() {
        char pattern[] = "/tmp/fernsdr-cgroup-XXXXXX";
        root = ::mkdtemp(pattern);
    }
    ~FakeCgroupfs() {
        if (std::system(("rm -rf '" + root + "'").c_str()) != 0) std::fprintf(stderr, "could not remove %s\n", root.c_str());
    }
    void write(const std::string& path, const std::string& text) {
        size_t at = 1;
        while ((at = path.find('/', at)) != std::string::npos) {
            ::mkdir((root + path.substr(0, at)).c_str(), 0755);
            at++;
        }
        std::ofstream(root + path) << text;
    }
};

}  // namespace

TEST_CASE(machine_finds_the_tightest_cgroup_limit_above_the_process) {
    {
        // A systemd service under a slice with MemoryMax, as cgroup v2 hosts
        // (Debian 11+, Raspberry Pi OS Bookworm, Fedora) lay it out.
        FakeCgroupfs fs;
        fs.write("/cgroup.controllers", "cpu memory\n");
        fs.write("/system.slice/fernsdr.service/memory.max", "max\n");
        fs.write("/system.slice/fernsdr.service/memory.current", "300\n");
        fs.write("/system.slice/fernsdr.service/cpu.max", "150000 100000\n");
        fs.write("/system.slice/memory.max", "2000\n");
        fs.write("/system.slice/memory.current", "1200\n");
        fs.write("/system.slice/cpu.max", "max 100000\n");
        const fernsdr::CgroupLimits limits = fernsdr::read_cgroup_limits(fs.root, "0::/system.slice/fernsdr.service\n");
        CHECK_EQ(limits.cpu_cores, 1.5);
        CHECK_EQ(limits.memory_bytes, 2000u);
        CHECK_EQ(limits.memory_used_bytes, 1200u);
    }
    {
        // Docker with its own cgroup namespace: "0::/" and the container's
        // limits at the mount root.
        FakeCgroupfs fs;
        fs.write("/cgroup.controllers", "cpu memory\n");
        fs.write("/memory.max", "1073741824\n");
        fs.write("/memory.current", "93741824\n");
        // The page cache the kernel drops first is not counted as used.
        fs.write("/memory.stat", "anon 50000000\nfile 40000000\nactive_file 20000000\ninactive_file 20000000\n");
        fs.write("/cpu.max", "max 100000\n");
        const fernsdr::CgroupLimits limits = fernsdr::read_cgroup_limits(fs.root, "0::/\n");
        CHECK_EQ(limits.cpu_cores, 0.0);
        CHECK_EQ(limits.memory_bytes, 1073741824u);
        CHECK_EQ(limits.memory_used_bytes, 73741824u);
    }
    {
        // Docker on cgroup v1: /proc/self/cgroup names the host's path, but
        // the container sees its own cgroup at each controller's mount root.
        FakeCgroupfs fs;
        fs.write("/memory/memory.limit_in_bytes", "536870912\n");
        fs.write("/memory/memory.usage_in_bytes", "46870912\n");
        fs.write("/memory/memory.stat", "cache 20000000\ninactive_file 1\ntotal_inactive_file 10000000\n");
        fs.write("/cpu/cpu.cfs_quota_us", "200000\n");
        fs.write("/cpu/cpu.cfs_period_us", "100000\n");
        const fernsdr::CgroupLimits limits =
            fernsdr::read_cgroup_limits(fs.root, "12:memory:/docker/abc\n4:cpu,cpuacct:/docker/abc\n");
        CHECK_EQ(limits.cpu_cores, 2.0);
        CHECK_EQ(limits.memory_bytes, 536870912u);
        CHECK_EQ(limits.memory_used_bytes, 36870912u);
    }
    {
        // A v1 host with no limits anywhere: v1's huge "unlimited" and a
        // quota of -1.
        FakeCgroupfs fs;
        fs.write("/memory/memory.limit_in_bytes", "9223372036854771712\n");
        fs.write("/cpu/cpu.cfs_quota_us", "-1\n");
        fs.write("/cpu/cpu.cfs_period_us", "100000\n");
        const fernsdr::CgroupLimits limits = fernsdr::read_cgroup_limits(fs.root, "12:memory:/\n4:cpu,cpuacct:/\n");
        CHECK_EQ(limits.cpu_cores, 0.0);
        CHECK_EQ(limits.memory_bytes, 0u);
    }
    {
        // No cgroup filesystem at all (some minimal containers, chroots).
        const fernsdr::CgroupLimits limits = fernsdr::read_cgroup_limits("/nonexistent-cgroup-root", "");
        CHECK_EQ(limits.cpu_cores, 0.0);
        CHECK_EQ(limits.memory_bytes, 0u);
    }
}
