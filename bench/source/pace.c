// SPDX-License-Identifier: AGPL-3.0-or-later
//
// pace: hands a sample stream to a receiver at a fixed rate, the way SDR
// hardware does.
//
// A receiver under test has to see its samples arrive as they would from a
// dongle: in regular transfers, no faster than its sample clock, and lost
// rather than delayed when it falls behind. Every receiver in the lab reads
// its input as fast as it arrives, so the rate is enforced here, by a
// program that does nothing else. scene.py, which renders the samples, is
// Python and can stop for tens of milliseconds at a time; it writes ahead
// into the pipe on our stdin, and we deliver.
//
// Timing. Sample n is taken at t0 + n / clock, where clock is the nominal
// rate times (1 + ppm / 1e6) and t0 is when the receiver connected. A
// transfer of `chunk` samples leaves when its last sample has been taken,
// as a USB transfer does, so the antenna time of every sample follows from
// its index alone, whatever happened to the transfers before it.
//
// Falling behind. Up to `buffer` worth of transfers wait for the receiver,
// as librtlsdr's transfer buffers do. A transfer that finds no room is
// dropped whole and counted, and the clock goes on. The lab this replaces
// slowed the clock instead, which made an overloaded receiver look like a
// slow but healthy one. Dropped transfers still consume their samples from
// the source, so sample n has the same content whether or not anything
// before it was dropped.
//
// The pipe or socket towards the receiver buffers a little on its own, 64
// KiB or 8 ms of samples, whichever is more, and at most 1 MiB (for rtl_tcp
// at least 256 KiB, see accept_client), so that this program, not the
// kernel, decides what waits and what is lost. For rtl_tcp the receiver's
// own socket buffer holds more, out of our reach.
//
// A receiver that goes away loses what waited for it: the queue, the pipe
// or socket buffer, and a sample it got only part of. Those are logged as
// "discarded" ranges of sample indices, like drops, so that every sample
// either arrives or is accounted for once. The next receiver starts with the
// next whole transfer.
//
// Being late costs the lab its timing, so as root pace asks for real-time
// scheduling (SCHED_FIFO) for its delivery thread, locks its memory and
// tightens its timer slack; the start event says which it got.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct format {
    const char* name;
    size_t frame;  // bytes per sample: I and Q together for complex formats
};

static const struct format formats[] = {
    {"cu8", 2},
    {"cs16", 4},
    {"cf32", 8},
    {"s16", 2},  // real samples, as from an RX888
};

static struct {
    double rate;
    double ppm;
    double chunk_ms;
    double buffer_ms;
    double duration;
    double noise_rms;
    uint64_t seed;
    const struct format* format;
    const char* out;
    const char* log;
    const char* loop;
    uint32_t tuner;
    uint32_t gains;
    bool no_realtime;
} opt = {
    .chunk_ms = 4,
    .buffer_ms = 1000,
    .seed = 1,
    .tuner = 5,  // R820T, what rtl_tcp clients expect most
    .gains = 29,
};

static volatile sig_atomic_t stop_requested;
static FILE* log_file;

static void on_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void die(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("pace: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static int64_t clock_ns(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static struct timespec ns_to_timespec(int64_t ns) {
    if (ns < 0) ns = 0;
    struct timespec ts = {.tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000};
    return ts;
}

// Samples from stdin, read ahead by a thread of their own so that a slow
// writer never stalls the delivery loop. head and tail count bytes since
// the start and only grow.
struct input_ring {
    uint8_t* buf;
    size_t cap;
    _Atomic size_t head;
    _Atomic size_t tail;
    _Atomic bool ended;
};

static void* input_main(void* arg) {
    struct input_ring* r = arg;
    for (;;) {
        size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        size_t space = r->cap - (head - tail);
        if (space == 0) {
            struct timespec ms = {0, 1000000};
            nanosleep(&ms, NULL);
            continue;
        }
        size_t off = head % r->cap;
        size_t want = space < r->cap - off ? space : r->cap - off;
        if (want > (1u << 20)) want = 1u << 20;
        ssize_t got = read(STDIN_FILENO, r->buf + off, want);
        if (got > 0) {
            atomic_store_explicit(&r->head, head + (size_t)got, memory_order_release);
        } else if (got < 0 && errno == EINTR) {
            continue;
        } else {
            atomic_store_explicit(&r->ended, true, memory_order_release);
            return NULL;
        }
    }
}

static size_t input_available(struct input_ring* r) {
    return atomic_load_explicit(&r->head, memory_order_acquire) - atomic_load_explicit(&r->tail, memory_order_relaxed);
}

static void input_take(struct input_ring* r, uint8_t* dst, size_t n) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t off = tail % r->cap;
    size_t first = n < r->cap - off ? n : r->cap - off;
    memcpy(dst, r->buf + off, first);
    memcpy(dst + first, r->buf, n - first);
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
}

// xoshiro256**, seeded through splitmix64.
static uint64_t rng_state[4];

static uint64_t splitmix64(uint64_t* x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15u);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
    return z ^ (z >> 31);
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static inline uint64_t rng_next(void) {
    const uint64_t result = rotl(rng_state[1] * 5, 7) * 9;
    const uint64_t t = rng_state[1] << 17;
    rng_state[2] ^= rng_state[0];
    rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2];
    rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t;
    rng_state[3] = rotl(rng_state[3], 45);
    return result;
}

// Gaussian noise by table: entry i is the normal quantile of (i + 0.5) /
// 65536 times the RMS, so four table lookups per random word give four
// samples. The tails end at 4.3 sigma, far beyond anything an ADC's own
// noise shows, and it runs at a nanosecond a sample, which 60 Msps needs.
// The quantiles come from bisection on erfc, slow but plainly right.
static int16_t noise_table[65536];

static double normal_quantile(double p) {
    double lo = -12, hi = 12;
    for (int i = 0; i < 100; ++i) {
        double mid = 0.5 * (lo + hi);
        if (0.5 * erfc(-mid / sqrt(2.0)) < p) lo = mid;
        else hi = mid;
    }
    return 0.5 * (lo + hi);
}

static void make_noise_table(double rms) {
    for (int i = 0; i < 65536; ++i) {
        double v = normal_quantile((i + 0.5) / 65536.0) * rms;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        noise_table[i] = (int16_t)lrint(v);
    }
}

static void add_noise_s16(int16_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint64_t r = rng_next();
        for (int k = 0; k < 4 && i < n; ++k, ++i, r >>= 16) {
            int32_t v = (int32_t)s[i] + noise_table[r & 0xffff];
            s[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
}

enum out_kind { OUT_FIFO, OUT_RTLTCP };

struct output {
    enum out_kind kind;
    char path[4096];
    struct sockaddr_in addr;
    int listen_fd;
    int fd;  // -1 while no receiver is attached
    size_t kernel_buffer;
    uint8_t cmd[5];
    size_t cmd_len;
};

static void parse_out(struct output* o, const char* spec) {
    memset(o, 0, sizeof *o);
    o->fd = -1;
    o->listen_fd = -1;
    if (strncmp(spec, "fifo:", 5) == 0) {
        o->kind = OUT_FIFO;
        if (strlen(spec + 5) >= sizeof o->path || spec[5] == 0) die("bad fifo path");
        strcpy(o->path, spec + 5);
        return;
    }
    if (strncmp(spec, "rtltcp:", 7) == 0) {
        o->kind = OUT_RTLTCP;
        char host[64];
        const char* colon = strrchr(spec + 7, ':');
        if (!colon || (size_t)(colon - (spec + 7)) >= sizeof host) die("bad rtltcp address, want rtltcp:ADDR:PORT");
        memcpy(host, spec + 7, (size_t)(colon - (spec + 7)));
        host[colon - (spec + 7)] = 0;
        char* end;
        long port = strtol(colon + 1, &end, 10);
        if (*end || port <= 0 || port > 65535) die("bad rtltcp port");
        o->addr.sin_family = AF_INET;
        o->addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &o->addr.sin_addr) != 1) die("bad rtltcp address %s", host);
        return;
    }
    die("--out must be fifo:PATH or rtltcp:ADDR:PORT");
}

// The log of a normal run grows by a few hundred bytes a second. A bug
// that logs in a loop once filled this machine's disk in two minutes and
// broke other services' writes, so past 64 MiB the log stops the run.
static const long log_limit = 64L << 20;
static long log_bytes;

// Strings from outside (paths, error texts) go into the log escaped.
static const char* json_escape(const char* in) {
    static char bufs[4][1024];
    static int next;
    char* out = bufs[next++ % 4];
    size_t o = 0;
    for (const unsigned char* c = (const unsigned char*)in; *c && o + 7 < sizeof bufs[0]; ++c) {
        if (*c == '"' || *c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*c;
        } else if (*c < 0x20) {
            o += (size_t)snprintf(out + o, 7, "\\u%04x", *c);
        } else {
            out[o++] = (char)*c;
        }
    }
    out[o] = 0;
    return out;
}

static void log_event(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    log_bytes += n + 1;
    if (log_bytes > log_limit) {
        fputs("{\"ev\":\"abort\",\"why\":\"log over 64 MiB\"}\n", log_file);
        fclose(log_file);
        fputs("pace: log over 64 MiB, stopping\n", stderr);
        _exit(3);
    }
}

// Pipe or socket buffer: about 8 ms of samples, at least 64 KiB, at most
// 1 MiB, see the top of the file.
static size_t kernel_buffer_for(double bytes_per_second) {
    double want = bytes_per_second * 0.008;
    if (want < 65536) want = 65536;
    if (want > 1 << 20) want = 1 << 20;
    return (size_t)want;
}

static bool open_fifo(struct output* o, bool block, double bytes_per_second) {
    int fd = open(o->path, O_WRONLY | (block ? 0 : O_NONBLOCK) | O_CLOEXEC);
    if (fd < 0) {
        if (!block && (errno == ENXIO || errno == EINTR)) return false;
        if (block && errno == EINTR) return false;
        die("cannot open %s: %s", o->path, strerror(errno));
    }
    // A regular file where the FIFO should be would take every sample at
    // once and grow without end: a harness bug must not fill the disk.
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) die("%s is not a FIFO", o->path);
    int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int size = fcntl(fd, F_SETPIPE_SZ, (int)kernel_buffer_for(bytes_per_second));
    o->kernel_buffer = size > 0 ? (size_t)size : 0;
    o->fd = fd;
    return true;
}

static void send_rtltcp_header(int fd) {
    uint8_t header[12] = {'R', 'T', 'L', '0'};
    uint32_t tuner = htonl(opt.tuner), gains = htonl(opt.gains);
    memcpy(header + 4, &tuner, 4);
    memcpy(header + 8, &gains, 4);
    size_t sent = 0;
    while (sent < sizeof header) {
        ssize_t n = send(fd, header + sent, sizeof header - sent, MSG_NOSIGNAL);
        if (n > 0) sent += (size_t)n;
        else if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        else return;
    }
}

static bool accept_client(struct output* o, bool block, double bytes_per_second, int64_t t0_mono) {
    if (block) {
        int flags = fcntl(o->listen_fd, F_GETFL);
        fcntl(o->listen_fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int fd = accept4(o->listen_fd, (struct sockaddr*)&peer, &len, SOCK_CLOEXEC);
    if (block) {
        int flags = fcntl(o->listen_fd, F_GETFL);
        fcntl(o->listen_fd, F_SETFL, flags | O_NONBLOCK);
    }
    if (fd < 0) return false;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    // Linux doubles what is asked for, so ask for half. And room for four
    // segments at least: on loopback a segment is 64 KiB, and with room for
    // only one the receiver's delayed acknowledgement (40 ms) paces the
    // stream, so that a receiver catching up after a stall drained at a
    // fifth of real time and pace dropped what it could not send.
    int size = (int)kernel_buffer_for(bytes_per_second) / 2;
    if (size < 128 * 1024) size = 128 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
    socklen_t sl = sizeof size;
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, &sl);
    o->kernel_buffer = (size_t)size;
    send_rtltcp_header(fd);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    o->fd = fd;
    o->cmd_len = 0;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
    if (t0_mono)
        log_event("{\"ev\":\"client\",\"t_ms\":%.3f,\"state\":\"connected\",\"peer\":\"%s:%d\"}",
                  (clock_ns(CLOCK_MONOTONIC) - t0_mono) / 1e6, ip, ntohs(peer.sin_port));
    return true;
}

static void drop_receiver(struct output* o, int64_t t0_mono, const char* why) {
    if (o->fd >= 0) close(o->fd);
    o->fd = -1;
    log_event("{\"ev\":\"client\",\"t_ms\":%.3f,\"state\":\"closed\",\"why\":\"%s\"}",
              (clock_ns(CLOCK_MONOTONIC) - t0_mono) / 1e6, json_escape(why));
}

// rtl_tcp commands are five bytes: a command number and a big-endian
// argument. The scene is fixed, so none of them changes anything, but a
// receiver asking for another sample rate or frequency than the lab gives
// it is misconfigured, and the harness wants to know.
static bool read_commands(struct output* o, int64_t t0_mono) {
    uint8_t buf[512];
    for (;;) {
        ssize_t n = recv(o->fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n == 0) return false;
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        for (ssize_t i = 0; i < n; ++i) {
            o->cmd[o->cmd_len++] = buf[i];
            if (o->cmd_len == 5) {
                uint32_t arg;
                memcpy(&arg, o->cmd + 1, 4);
                log_event("{\"ev\":\"cmd\",\"t_ms\":%.3f,\"cmd\":%u,\"arg\":%" PRIu32 "}",
                          (clock_ns(CLOCK_MONOTONIC) - t0_mono) / 1e6, o->cmd[0], ntohl(arg));
                o->cmd_len = 0;
            }
        }
    }
}

// Transfers waiting for the receiver.
// The byte stream towards receivers is the pushed transfers back to back;
// transfer p is bytes [p * chunk_bytes, (p + 1) * chunk_bytes). `first`
// remembers each recent transfer's first sample index, so that bytes lost
// at a disconnect can be named by sample index.
struct queue {
    uint8_t* buf;
    size_t cap;
    size_t head;  // bytes enqueued since the start
    size_t tail;  // bytes written since the start
    uint64_t* first;
    size_t ring;
};

static void queue_push(struct queue* q, const uint8_t* src, size_t n, uint64_t first_sample) {
    q->first[(q->head / n) % q->ring] = first_sample;
    size_t off = q->head % q->cap;
    size_t first = n < q->cap - off ? n : q->cap - off;
    memcpy(q->buf + off, src, first);
    memcpy(q->buf, src + first, n - first);
    q->head += n;
}


// Consecutive dropped transfers are logged as one range, so that a long
// overload costs a line, not 250 a second.
struct drop_range {
    uint64_t first;
    uint64_t samples;
    const char* why;
};

static void flush_drops(struct drop_range* d) {
    if (d->samples)
        log_event("{\"ev\":\"drop\",\"first\":%" PRIu64 ",\"samples\":%" PRIu64 ",\"why\":\"%s\"}", d->first,
                  d->samples, d->why);
    d->samples = 0;
}

static void note_drop(struct drop_range* d, uint64_t first, uint64_t samples, const char* why) {
    if (d->samples && (d->first + d->samples != first || strcmp(d->why, why) != 0)) flush_drops(d);
    if (!d->samples) {
        d->first = first;
        d->why = why;
    }
    d->samples += samples;
}

// Bytes still in the pipe or socket, which a receiver that went away
// never reads.
static size_t kernel_backlog(const struct output* o) {
    int n = 0;
    if (o->fd < 0) return 0;
    if (ioctl(o->fd, o->kind == OUT_FIFO ? FIONREAD : SIOCOUTQ, &n) != 0 || n < 0) return 0;
    return (size_t)n;
}

// A receiver went away: everything from the first sample it did not get
// whole to the end of the queue is lost, logged by sample index.
static uint64_t discard_rest(struct queue* q, size_t in_kernel, size_t frame, size_t chunk_samples,
                             struct drop_range* d) {
    if (in_kernel > q->tail) in_kernel = q->tail;
    uint64_t from = (q->tail - in_kernel) / frame;  // a sample sent in part is lost
    uint64_t to = q->head / frame;
    uint64_t lost = 0;
    while (from < to) {
        uint64_t p = from / chunk_samples, within = from % chunk_samples;
        uint64_t n = chunk_samples - within < to - from ? chunk_samples - within : to - from;
        note_drop(d, q->first[p % q->ring] + within, n, "discarded");
        lost += n;
        from += n;
    }
    flush_drops(d);
    q->tail = q->head;
    return lost;
}

static int cmp_i64(const void* a, const void* b) {
    int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
    return (x > y) - (x < y);
}

struct second_stats {
    uint64_t written;
    uint64_t dropped_full;
    uint64_t dropped_absent;
    size_t queue_max;
    int64_t input_wait_ns;
    int64_t late[4096];
    size_t n_late;
};

static void usage(void) {
    fputs("usage: pace --rate SPS --format cu8|cs16|cf32|s16 --out fifo:PATH|rtltcp:ADDR:PORT\n"
          "            --log FILE [--ppm P] [--chunk-ms 4] [--buffer-ms 1000] [--duration S]\n"
          "            [--loop FILE [--noise-rms R] [--seed N]] [--tuner N] [--gains N]\n"
          "            [--no-realtime]\n"
          "Samples come from stdin, already in the output format, unless --loop names a\n"
          "file to repeat; --noise-rms adds fresh Gaussian noise to s16 samples.\n",
          stderr);
    exit(2);
}

static double parse_double(const char* s, const char* what) {
    char* end;
    double v = strtod(s, &end);
    if (*s == 0 || *end || !isfinite(v)) die("bad %s: %s", what, s);
    return v;
}

int main(int argc, char** argv) {
    static const struct option long_opts[] = {
        {"rate", required_argument, 0, 'r'},     {"ppm", required_argument, 0, 'p'},
        {"format", required_argument, 0, 'f'},   {"out", required_argument, 0, 'o'},
        {"log", required_argument, 0, 'l'},      {"chunk-ms", required_argument, 0, 'c'},
        {"buffer-ms", required_argument, 0, 'b'}, {"duration", required_argument, 0, 'd'},
        {"loop", required_argument, 0, 'L'},     {"noise-rms", required_argument, 0, 'n'},
        {"seed", required_argument, 0, 's'},     {"tuner", required_argument, 0, 't'},
        {"gains", required_argument, 0, 'g'},    {"no-realtime", no_argument, 0, 'R'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
        case 'r': opt.rate = parse_double(optarg, "rate"); break;
        case 'p': opt.ppm = parse_double(optarg, "ppm"); break;
        case 'f':
            for (size_t i = 0; i < sizeof formats / sizeof formats[0]; ++i)
                if (strcmp(optarg, formats[i].name) == 0) opt.format = &formats[i];
            if (!opt.format) die("unknown format %s", optarg);
            break;
        case 'o': opt.out = optarg; break;
        case 'l': opt.log = optarg; break;
        case 'c': opt.chunk_ms = parse_double(optarg, "chunk-ms"); break;
        case 'b': opt.buffer_ms = parse_double(optarg, "buffer-ms"); break;
        case 'd': opt.duration = parse_double(optarg, "duration"); break;
        case 'L': opt.loop = optarg; break;
        case 'n': opt.noise_rms = parse_double(optarg, "noise-rms"); break;
        case 's': opt.seed = strtoull(optarg, NULL, 10); break;
        case 't': opt.tuner = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'g': opt.gains = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'R': opt.no_realtime = true; break;
        default: usage();
        }
    }
    if (optind != argc || opt.rate <= 0 || !opt.format || !opt.out || !opt.log) usage();
    if (opt.chunk_ms <= 0 || opt.buffer_ms < opt.chunk_ms) die("need 0 < chunk-ms <= buffer-ms");
    if (opt.noise_rms > 0 && (strcmp(opt.format->name, "s16") != 0 || !opt.loop))
        die("--noise-rms is for --loop with s16 samples");

    const double clock = opt.rate * (1 + opt.ppm / 1e6);
    const size_t chunk_samples = (size_t)llround(opt.rate * opt.chunk_ms / 1000);
    const size_t chunk_bytes = chunk_samples * opt.format->frame;
    const size_t queue_chunks = (size_t)ceil(opt.buffer_ms / opt.chunk_ms);
    const double bytes_per_second = clock * (double)opt.format->frame;
    if (chunk_samples == 0) die("chunk too small for this rate");

    log_file = fopen(opt.log, "w");
    if (!log_file) die("cannot write %s: %s", opt.log, strerror(errno));

    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    // The source: stdin read ahead up to 2 s (at most 256 MiB), or a file
    // repeated from memory.
    struct input_ring in = {0};
    uint8_t* loop = NULL;
    size_t loop_len = 0, loop_pos = 0;
    pthread_t reader;
    if (opt.loop) {
        FILE* f = fopen(opt.loop, "rb");
        if (!f) die("cannot open %s: %s", opt.loop, strerror(errno));
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 0 || (size_t)len % opt.format->frame) die("%s: empty or not whole samples", opt.loop);
        loop_len = (size_t)len;
        loop = malloc(loop_len);
        if (!loop || fread(loop, 1, loop_len, f) != loop_len) die("cannot read %s", opt.loop);
        fclose(f);
        if (opt.noise_rms > 0) {
            uint64_t x = opt.seed;
            for (int i = 0; i < 4; ++i) rng_state[i] = splitmix64(&x);
            make_noise_table(opt.noise_rms);
        }
    } else {
        double want = bytes_per_second * 2;
        if (want > 256.0 * 1024 * 1024) want = 256.0 * 1024 * 1024;
        in.cap = ((size_t)want / chunk_bytes + 2) * chunk_bytes;
        in.buf = malloc(in.cap);
        if (!in.buf) die("out of memory");
        if (pthread_create(&reader, NULL, input_main, &in) != 0) die("cannot start the input thread");
    }

    // The record of transfers reaches back over the queue and whatever the
    // pipe or socket may still hold (at most 2 MiB).
    struct queue q = {.cap = queue_chunks * chunk_bytes};
    q.ring = queue_chunks + (2u << 20) / chunk_bytes + 4;
    q.buf = malloc(q.cap);
    q.first = calloc(q.ring, sizeof q.first[0]);
    uint8_t* next = malloc(chunk_bytes);
    if (!q.buf || !q.first || !next) die("out of memory");

    // Only this thread delivers, so only it runs in real time; the reader of
    // stdin was started before and keeps the normal policy.
    char sched[64] = "other";
    if (!opt.no_realtime) {
        prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
        bool locked = mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
        struct sched_param sp = {.sched_priority = 10};
        if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
            snprintf(sched, sizeof sched, "fifo:10%s", locked ? ",locked" : "");
        else
            snprintf(sched, sizeof sched, "other%s", locked ? ",locked" : "");
    }

    // Some samples in hand before the receiver's clock starts, so that the
    // first transfers do not wait for the renderer.
    if (!opt.loop) {
        size_t want = (size_t)(bytes_per_second * 0.5);
        if (want > in.cap / 2) want = in.cap / 2;
        while (!stop_requested && input_available(&in) < want && !atomic_load_explicit(&in.ended, memory_order_acquire)) {
            struct timespec ms10 = {0, 10000000};
            nanosleep(&ms10, NULL);
        }
    }

    // Wait for the receiver; its arrival is t0.
    struct output out;
    parse_out(&out, opt.out);
    if (out.kind == OUT_FIFO) {
        if (mkfifo(out.path, 0600) != 0 && errno != EEXIST) die("cannot make %s: %s", out.path, strerror(errno));
        while (!open_fifo(&out, true, bytes_per_second))
            if (stop_requested) return 0;
    } else {
        out.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        int one = 1;
        setsockopt(out.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(out.listen_fd, (struct sockaddr*)&out.addr, sizeof out.addr) != 0) die("cannot bind: %s", strerror(errno));
        if (listen(out.listen_fd, 4) != 0) die("cannot listen: %s", strerror(errno));
        while (!accept_client(&out, true, bytes_per_second, 0))
            if (stop_requested) return 0;
    }
    const int64_t t0_mono = clock_ns(CLOCK_MONOTONIC);
    const int64_t t0_real = clock_ns(CLOCK_REALTIME);
    log_event("{\"ev\":\"start\",\"t0_mono_ns\":%" PRId64 ",\"t0_real_ns\":%" PRId64
              ",\"rate\":%.6f,\"ppm\":%.6f,\"clock\":%.9f,\"format\":\"%s\",\"frame\":%zu"
              ",\"chunk_samples\":%zu,\"queue_chunks\":%zu,\"kernel_buffer\":%zu,\"out\":\"%s\""
              ",\"loop\":%s,\"noise_rms\":%.3f,\"seed\":%" PRIu64 ",\"sched\":\"%s\"}",
              t0_mono, t0_real, opt.rate, opt.ppm, clock, opt.format->name, opt.format->frame, chunk_samples,
              queue_chunks, out.kernel_buffer, json_escape(opt.out), opt.loop ? "true" : "false", opt.noise_rms, opt.seed,
              sched);
    fflush(log_file);

    // Chunk k is due when its last sample has been taken.
    const double ns_per_chunk = (double)chunk_samples / clock * 1e9;
    uint64_t k = 0;
    bool next_ready = false;
    bool input_ended = false;
    struct second_stats sec = {0};
    int64_t sec_index = 0;
    uint64_t total_written = 0, total_dropped_full = 0, total_dropped_absent = 0, total_discarded = 0;
    struct drop_range drops = {0};
    int64_t worst_late = 0, total_input_wait = 0;
    const char* end_reason = "signal";

    // Fills `next` with chunk k's samples if the source has them.
    #define PREPARE_NEXT()                                                                          \
        do {                                                                                        \
            if (next_ready) break;                                                                  \
            if (loop) {                                                                             \
                size_t filled = 0;                                                                  \
                while (filled < chunk_bytes) {                                                      \
                    size_t n = chunk_bytes - filled;                                                \
                    if (n > loop_len - loop_pos) n = loop_len - loop_pos;                           \
                    memcpy(next + filled, loop + loop_pos, n);                                      \
                    filled += n;                                                                    \
                    loop_pos = (loop_pos + n) % loop_len;                                           \
                }                                                                                   \
                if (opt.noise_rms > 0) add_noise_s16((int16_t*)next, chunk_samples);                \
                next_ready = true;                                                                  \
            } else if (input_available(&in) >= chunk_bytes) {                                       \
                input_take(&in, next, chunk_bytes);                                                 \
                next_ready = true;                                                                  \
            }                                                                                       \
        } while (0)

    PREPARE_NEXT();
    while (!stop_requested) {
        int64_t now = clock_ns(CLOCK_MONOTONIC);
        const int64_t due = t0_mono + (int64_t)llround((double)(k + 1) * ns_per_chunk);

        // Once a second, what happened in it.
        while (now - t0_mono >= (sec_index + 1) * 1000000000LL) {
            qsort(sec.late, sec.n_late, sizeof sec.late[0], cmp_i64);
            int64_t p50 = sec.n_late ? sec.late[sec.n_late / 2] : 0;
            int64_t p99 = sec.n_late ? sec.late[(sec.n_late * 99) / 100] : 0;
            int64_t mx = sec.n_late ? sec.late[sec.n_late - 1] : 0;
            log_event("{\"ev\":\"sec\",\"s\":%" PRId64 ",\"written\":%" PRIu64 ",\"dropped_full\":%" PRIu64
                      ",\"dropped_absent\":%" PRIu64 ",\"queue_max_ms\":%.3f,\"late_p50_us\":%.1f"
                      ",\"late_p99_us\":%.1f,\"late_max_us\":%.1f,\"input_wait_us\":%.1f}",
                      sec_index, sec.written / opt.format->frame, sec.dropped_full, sec.dropped_absent,
                      (double)sec.queue_max / bytes_per_second * 1000, p50 / 1e3, p99 / 1e3, mx / 1e3,
                      sec.input_wait_ns / 1e3);
            fflush(log_file);
            memset(&sec, 0, sizeof sec);
            ++sec_index;
        }
        if (opt.duration > 0 && (double)(due - t0_mono) > opt.duration * 1e9) {
            end_reason = "duration";
            break;
        }

        if (now >= due) {
            // The source must have chunk k by now; if it does not, the
            // renderer is too slow and the run says so.
            if (!next_ready) {
                int64_t waited_from = now;
                while (!next_ready && !stop_requested) {
                    PREPARE_NEXT();
                    if (next_ready) break;
                    if (atomic_load_explicit(&in.ended, memory_order_acquire) && input_available(&in) < chunk_bytes) {
                        input_ended = true;
                        break;
                    }
                    struct timespec tick = {0, 200000};
                    nanosleep(&tick, NULL);
                }
                now = clock_ns(CLOCK_MONOTONIC);
                sec.input_wait_ns += now - waited_from;
                total_input_wait += now - waited_from;
                if (input_ended) {
                    end_reason = "input ended";
                    break;
                }
                if (!next_ready) break;
            }
            const int64_t late = now - due;
            if (sec.n_late < sizeof sec.late / sizeof sec.late[0]) sec.late[sec.n_late++] = late;
            if (late > worst_late) worst_late = late;

            if (out.fd < 0) {
                sec.dropped_absent += chunk_samples;
                total_dropped_absent += chunk_samples;
                note_drop(&drops, k * chunk_samples, chunk_samples, "absent");
            } else if (q.cap - (q.head - q.tail) < chunk_bytes) {
                sec.dropped_full += chunk_samples;
                total_dropped_full += chunk_samples;
                note_drop(&drops, k * chunk_samples, chunk_samples, "full");
            } else {
                flush_drops(&drops);
                queue_push(&q, next, chunk_bytes, k * chunk_samples);
            }
            next_ready = false;
            ++k;
            PREPARE_NEXT();
            continue;
        }

        // Until the next chunk is due: write what waits, take commands,
        // and look for a receiver that went away or came back.
        if (out.fd < 0) {
            if (out.kind == OUT_FIFO) {
                if (open_fifo(&out, false, bytes_per_second))
                    log_event("{\"ev\":\"client\",\"t_ms\":%.3f,\"state\":\"connected\"}", (now - t0_mono) / 1e6);
                else {
                    struct timespec ts = ns_to_timespec(due - now);
                    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
                }
            } else {
                struct pollfd p = {.fd = out.listen_fd, .events = POLLIN};
                struct timespec ts = ns_to_timespec(due - now);
                if (ppoll(&p, 1, &ts, NULL) > 0) accept_client(&out, false, bytes_per_second, t0_mono);
            }
            continue;
        }
        struct pollfd p = {.fd = out.fd, .events = 0};
        if (q.head != q.tail) p.events |= POLLOUT;
        if (out.kind == OUT_RTLTCP) p.events |= POLLIN;
        struct timespec ts = ns_to_timespec(due - now);
        int ready = ppoll(&p, 1, &ts, NULL);
        if (ready <= 0) continue;
        // A closing rtl_tcp client shows as readable with nothing to read;
        // a FIFO whose reader left shows as an error.
        bool gone = (p.revents & (POLLERR | POLLNVAL)) || ((p.revents & POLLHUP) && !(p.revents & POLLIN));
        if (!gone && (p.revents & POLLIN)) gone = !read_commands(&out, t0_mono);
        if (gone) {
            flush_drops(&drops);
            total_discarded += discard_rest(&q, kernel_backlog(&out), opt.format->frame, chunk_samples, &drops);
            drop_receiver(&out, t0_mono, "closed");
            continue;
        }
        if (p.revents & POLLOUT) {
            size_t off = q.tail % q.cap;
            size_t n = q.head - q.tail;
            if (n > q.cap - off) n = q.cap - off;
            ssize_t w = out.kind == OUT_RTLTCP ? send(out.fd, q.buf + off, n, MSG_NOSIGNAL | MSG_DONTWAIT)
                                               : write(out.fd, q.buf + off, n);
            if (w > 0) {
                q.tail += (size_t)w;
                sec.written += (uint64_t)w;
                total_written += (uint64_t)w;
            } else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                const char* why = strerror(errno);
                flush_drops(&drops);
                total_discarded += discard_rest(&q, kernel_backlog(&out), opt.format->frame, chunk_samples, &drops);
                drop_receiver(&out, t0_mono, why);
                continue;
            }
        }
        size_t queued = q.head - q.tail;
        if (queued > sec.queue_max) sec.queue_max = queued;
    }
    #undef PREPARE_NEXT

    flush_drops(&drops);
    log_event("{\"ev\":\"end\",\"why\":\"%s\",\"chunks\":%" PRIu64 ",\"written\":%" PRIu64
              ",\"dropped_full\":%" PRIu64 ",\"dropped_absent\":%" PRIu64 ",\"discarded\":%" PRIu64
              ",\"queued_at_end\":%" PRIu64 ",\"late_max_us\":%.1f,\"input_wait_us\":%.1f}",
              end_reason, k, total_written / opt.format->frame, total_dropped_full, total_dropped_absent,
              total_discarded, (uint64_t)((q.head - q.tail) / opt.format->frame), worst_late / 1e3,
              total_input_wait / 1e3);
    fclose(log_file);
    if (out.fd >= 0) close(out.fd);
    return 0;
}
