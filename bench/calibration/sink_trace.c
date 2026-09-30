// SPDX-License-Identifier: AGPL-3.0-or-later
// Lab-only interposition for PulseAudio 16.1's single float32 stereo null sink.
// Its latency callback is timestamp - pa_rtclock_now(). Bracketing that call
// recovers the render deadline without depending on browser/client estimates.
// PCM copies enter a bounded SPSC ring; the audio thread never performs file IO.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct { void *memblock; size_t index, length; } chunk;
#define SLOTS 16
#define CAP (1024*1024)
static struct event { int kind; int64_t time_ns, bracket_ns, mono_ns, real_ns; size_t bytes; unsigned char pcm[CAP]; } ring[SLOTS];
static _Atomic uint64_t produced, consumed, dropped;
static _Atomic bool stop_writer;
static pthread_t writer;
static bool active;
static FILE *audio, *events;
static char trace_prefix[4000];
static void (*render_original)(void *,size_t,chunk *);
static void (*rewind_original)(void *,size_t);
static int64_t (*latency_original)(void *,bool);
static void *(*acquire_original)(void *);
static void (*release_original)(void *);

static int64_t now_ns(clockid_t clock) {
    struct timespec t;
    if(clock_gettime(clock,&t)) return 0;
    return (int64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static void *write_events(void *unused) {
    (void)unused;
    // PulseAudio closes inherited descriptors during startup. Open only
    // after its IO thread has rendered: constructor-opened FILE descriptors
    // could otherwise be reused for the daemon's shared-memory pool.
    while(!atomic_load(&produced) && !atomic_load(&stop_writer)) usleep(1000);
    char name[4096];
    snprintf(name,sizeof name,"%s.f32",trace_prefix); audio=fopen(name,"wb");
    snprintf(name,sizeof name,"%s.jsonl",trace_prefix); events=fopen(name,"w");
    if(!audio || !events) { atomic_fetch_add(&dropped,1); return NULL; }
    fprintf(events,"{\"ev\":\"start\",\"pulse_version\":\"16.1\",\"rate\":48000,\"channels\":2,\"format\":\"float32le\"}\n");
    uint64_t offset=0;
    while(!atomic_load(&stop_writer) || atomic_load(&consumed)<atomic_load(&produced)) {
        uint64_t n=atomic_load_explicit(&consumed,memory_order_relaxed);
        if(n==atomic_load_explicit(&produced,memory_order_acquire)) { usleep(1000); continue; }
        struct event *e=&ring[n%SLOTS];
        if(e->kind==1 && fwrite(e->pcm,1,e->bytes,audio)!=e->bytes) atomic_fetch_add(&dropped,1);
        if(fprintf(events,"{\"ev\":\"%s\",\"t0_mono_ns\":%"PRId64",\"bracket_ns\":%"PRId64
                   ",\"mono_ns\":%"PRId64",\"real_ns\":%"PRId64",\"bytes\":%zu,\"file_offset\":%"PRIu64"}\n",
                   e->kind==1?"render":"rewind",e->time_ns,e->bracket_ns,e->mono_ns,e->real_ns,e->bytes,offset)<0)
            atomic_fetch_add(&dropped,1);
        if(e->kind==1) offset+=e->bytes;
        atomic_store_explicit(&consumed,n+1,memory_order_release);
    }
    fprintf(events,"{\"ev\":\"end\",\"dropped\":%"PRIu64"}\n",atomic_load(&dropped));
    fclose(audio); fclose(events);
    return NULL;
}
static void publish(int kind, int64_t time, int64_t bracket, chunk *data, size_t bytes) {
    uint64_t n=atomic_load_explicit(&produced,memory_order_relaxed);
    if(bytes>CAP || n-atomic_load_explicit(&consumed,memory_order_acquire)>=SLOTS) {
        atomic_fetch_add(&dropped,1); return;
    }
    struct event *e=&ring[n%SLOTS];
    e->kind=kind; e->time_ns=time; e->bracket_ns=bracket; e->bytes=bytes;
    int64_t before=now_ns(CLOCK_MONOTONIC);
    e->real_ns=now_ns(CLOCK_REALTIME);
    int64_t after=now_ns(CLOCK_MONOTONIC);
    e->mono_ns=(before+after)/2;
    e->bracket_ns+=after-before;
    if(kind==1) {
        void *ptr=acquire_original(data->memblock);
        memcpy(e->pcm,(unsigned char *)ptr+data->index,bytes);
        release_original(data->memblock);
    }
    atomic_store_explicit(&produced,n+1,memory_order_release);
}
__attribute__((constructor)) static void initialise(void) {
    render_original=dlsym(RTLD_NEXT,"pa_sink_render");
    rewind_original=dlsym(RTLD_NEXT,"pa_sink_process_rewind");
    latency_original=dlsym(RTLD_NEXT,"pa_sink_get_latency_within_thread");
    acquire_original=dlsym(RTLD_NEXT,"pa_memblock_acquire");
    release_original=dlsym(RTLD_NEXT,"pa_memblock_release");
    const char *prefix=getenv("FB_PULSE_TRACE_PREFIX");
    if(!prefix || !render_original || !rewind_original || !latency_original || !acquire_original || !release_original) return;
    if(strlen(prefix)>=sizeof trace_prefix) return;
    strcpy(trace_prefix,prefix);
    active=pthread_create(&writer,NULL,write_events,NULL)==0;
}
__attribute__((destructor)) static void finish(void) {
    if(active) { atomic_store(&stop_writer,true); pthread_join(writer,NULL); }
}
void pa_sink_render(void *sink, size_t bytes, chunk *result) {
    if(!render_original) _exit(120);
    int64_t before=0,after=0,latency=0;
    if(active) { before=now_ns(CLOCK_MONOTONIC); latency=latency_original(sink,true); after=now_ns(CLOCK_MONOTONIC); }
    render_original(sink,bytes,result);
    if(active) publish(1,(before+after)/2+latency*1000,after-before,result,result->length);
}
void pa_sink_process_rewind(void *sink, size_t bytes) {
    if(!rewind_original) _exit(121);
    int64_t before=0,after=0,latency=0;
    if(active && bytes) { before=now_ns(CLOCK_MONOTONIC); latency=latency_original(sink,true); after=now_ns(CLOCK_MONOTONIC); }
    rewind_original(sink,bytes);
    if(active && bytes) publish(2,(before+after)/2+latency*1000-(int64_t)(bytes/8)*1000000000/48000,after-before,NULL,bytes);
}
