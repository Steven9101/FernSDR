// SPDX-License-Identifier: AGPL-3.0-or-later
// Independent null-sink observation through libpulse 16.1 timing snapshots.
// Record indices count stereo float32 bytes; the file stores their mono mean.
#include <pulse/pulseaudio.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static FILE *audio, *events;
static uint64_t frames;
static int failed, pending;
static volatile sig_atomic_t stopped;
static int64_t ns(clockid_t clock) {
    struct timespec t;
    if (clock_gettime(clock, &t)) exit(2);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void stop(int signum) { (void)signum; stopped = 1; }
static void read_audio(pa_stream *s, size_t requested, void *userdata) {
    (void)requested; (void)userdata;
    const void *data;
    size_t bytes;
    if (pa_stream_peek(s, &data, &bytes) < 0) { failed = 1; return; }
    if (!bytes) return;
    if (bytes % 8 || bytes > 4*1024*1024 || frames + bytes/8 > 48000*240) { failed = 1; return; }
    if (!data) {
        fprintf(events, "{\"ev\":\"hole\",\"frame\":%"PRIu64",\"frames\":%zu}\n", frames, bytes/8);
        failed = 1;
    }
    const float *samples = data;
    float mono[4096];
    for (size_t offset = 0; offset < bytes/8;) {
        size_t count = bytes/8-offset;
        if (count > 4096) count = 4096;
        for (size_t i=0; i<count; i++) mono[i] = data ? .5f*(samples[2*(offset+i)]+samples[2*(offset+i)+1]) : NAN;
        if (fwrite(mono, sizeof(float), count, audio) != count) failed = 1;
        offset += count;
    }
    frames += bytes/8;
    if (pa_stream_drop(s) < 0) failed = 1;
}
static void timing(pa_stream *s, int success, void *userdata) {
    (void)userdata;
    pending = 0;
    const pa_timing_info *t = pa_stream_get_timing_info(s);
    if (!success || !t) { failed = 1; return; }
    int64_t before = ns(CLOCK_MONOTONIC), real = ns(CLOCK_REALTIME), after = ns(CLOCK_MONOTONIC);
    fprintf(events, "{\"ev\":\"timing\",\"snapshot_us\":%"PRId64",\"mono_before_ns\":%"PRId64
            ",\"real_ns\":%"PRId64",\"mono_after_ns\":%"PRId64",\"write_bytes\":%"PRId64
            ",\"read_bytes\":%"PRId64",\"sink_us\":%"PRIu64",\"source_us\":%"PRIu64
            ",\"transport_us\":%"PRIu64",\"synchronized\":%d,\"write_corrupt\":%d,\"read_corrupt\":%d"
            ",\"captured_frames\":%"PRIu64",\"suspended\":%d}\n",
            (int64_t)t->timestamp.tv_sec*1000000+t->timestamp.tv_usec, before, real, after,
            t->write_index,t->read_index,t->sink_usec,t->source_usec,t->transport_usec,
            t->synchronized_clocks,t->write_index_corrupt,t->read_index_corrupt,frames,pa_stream_is_suspended(s));
    fflush(events);
}
int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr,"usage: monitor SERVER SECONDS AUDIO.f32 EVENTS.jsonl\n"); return 2; }
    char *end;
    double seconds = strtod(argv[2],&end);
    if (*end || !isfinite(seconds) || seconds < 1 || seconds > 240) return 2;
    audio=fopen(argv[3],"wb"); events=fopen(argv[4],"w");
    if (!audio || !events) return 2;
    signal(SIGTERM,stop); signal(SIGINT,stop);
    pa_mainloop *loop=pa_mainloop_new();
    pa_context *context=pa_context_new(pa_mainloop_get_api(loop),"fernbench-independent-monitor");
    if (pa_context_connect(context,argv[1],PA_CONTEXT_NOAUTOSPAWN,NULL)<0) return 2;
    int64_t deadline=ns(CLOCK_MONOTONIC)+5000000000LL;
    while (pa_context_get_state(context)!=PA_CONTEXT_READY) {
        if (pa_context_get_state(context)==PA_CONTEXT_FAILED || ns(CLOCK_MONOTONIC)>deadline) return 2;
        pa_mainloop_iterate(loop,0,NULL); usleep(1000);
    }
    pa_sample_spec spec={.format=PA_SAMPLE_FLOAT32LE,.rate=48000,.channels=2};
    pa_stream *stream=pa_stream_new(context,"independent monitor",&spec,NULL);
    pa_stream_set_read_callback(stream,read_audio,NULL);
    pa_buffer_attr attr={.maxlength=4*1024*1024,.tlength=(uint32_t)-1,.prebuf=(uint32_t)-1,
                         .minreq=(uint32_t)-1,.fragsize=3840};
    if (pa_stream_connect_record(stream,"fb.monitor",&attr,PA_STREAM_DONT_MOVE)<0) return 2;
    while (pa_stream_get_state(stream)!=PA_STREAM_READY) {
        if (pa_stream_get_state(stream)==PA_STREAM_FAILED || ns(CLOCK_MONOTONIC)>deadline) return 2;
        pa_mainloop_iterate(loop,0,NULL); usleep(1000);
    }
    fprintf(events,"{\"ev\":\"start\",\"rate\":48000,\"stream_channels\":2,\"file_channels\":1,\"bytes_per_frame\":8,\"library\":\"%s\",\"server_protocol\":%u}\n",
            pa_get_library_version(),pa_context_get_server_protocol_version(context));
    deadline=ns(CLOCK_MONOTONIC)+(int64_t)(seconds*1e9);
    int64_t next=0;
    while (!failed && !stopped && ns(CLOCK_MONOTONIC)<deadline) {
        if (pa_stream_get_state(stream)!=PA_STREAM_READY) { failed=1; break; }
        if (!pending && ns(CLOCK_MONOTONIC)>=next) {
            pa_operation *op=pa_stream_update_timing_info(stream,timing,NULL);
            if (!op) { failed=1; break; }
            pending=1; pa_operation_unref(op); next=ns(CLOCK_MONOTONIC)+50000000;
        }
        pa_mainloop_iterate(loop,0,NULL); usleep(1000);
    }
    fprintf(events,"{\"ev\":\"end\",\"frames\":%"PRIu64",\"failed\":%d}\n",frames,failed);
    pa_stream_disconnect(stream); pa_stream_unref(stream);
    pa_context_disconnect(context); pa_context_unref(context); pa_mainloop_free(loop);
    if (fclose(audio) || fclose(events)) failed=1;
    return failed ? 1 : 0;
}
