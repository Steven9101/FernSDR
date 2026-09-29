#include "limits.h"

#include <sys/resource.h>

#include <algorithm>

namespace fernsdr {

std::uint64_t raise_open_file_limit() {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return 0;
    const rlim_t wanted = limit.rlim_max == RLIM_INFINITY ? rlim_t(1) << 20 : std::min<rlim_t>(limit.rlim_max, rlim_t(1) << 20);
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= wanted) return limit.rlim_cur;
    rlimit raised = limit;
    raised.rlim_cur = wanted;
    if (setrlimit(RLIMIT_NOFILE, &raised) != 0) return limit.rlim_cur;
    return raised.rlim_cur;
}

}  // namespace fernsdr
