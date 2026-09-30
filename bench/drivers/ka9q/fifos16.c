// SPDX-License-Identifier: GPL-3.0-or-later
//
// fifos16: a radiod front end that reads real signed 16-bit little-endian
// samples from a named pipe, as an RX888 delivers them over USB.
//
// radiod has no file or pipe input. This driver lets the lab give it a
// wideband real band (0 to 30 MHz at 60 Msps) that a program on the host
// writes into a FIFO at the sample rate. It presents itself to radiod as
// radiod's own rx888 driver does in HF mode: a real front end at 0 Hz, 16
// bits per sample, the same usable IF span (15 kHz to 0.47 of the sample
// rate), the same conversion and overload count (|x| > 32766), and the
// same 100 ms smoothing of the input power with the DC removed. The sample
// rate and the FIFO's path come from the config.
//
// Buffering. Whatever the pipe holds is read at once, up to one filter
// block (L samples), converted, and handed over; nothing waits here but an
// odd byte between reads. The pipe's own capacity is the writer's choice
// (the lab's pace sets it to about 8 ms of samples); this driver does not
// change it.
//
// The FIFO is opened for reading and writing. On Linux that never blocks
// and never reads end-of-file, so a writer may come, go and come back while
// radiod runs (fifo(7)); the writer sees this driver as its reader from the
// moment the front end starts until it is shut down. radiod starts a front
// end with its first channel and, in current ka9q-radio, shuts it down with
// its last; the madpsy fork starts it once and never stops it.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <iniparser/iniparser.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "misc.h"
#include "config.h"
#include "radio.h"

static char const *Fifos16_keys[] = {
  "device", "library", "description", "samprate", "path", NULL
};

enum state { STOPPED, STARTING, RUNNING, STOPPING };

struct sdrstate {
  struct frontend *frontend;
  char path[4096];
  double scale;       // scale_AD(): full scale to +/-1, +3 dB for a real signal
  pthread_t thread;
  _Atomic enum state state;
};

extern char const *Description;

static double const Power_tc = 0.1;   // seconds, as rx888.c's PTC
static double const Nyquist = 0.47;   // usable top as a fraction of samprate, as rx888.c

int fifos16_setup(struct frontend * const frontend, dictionary const * const dictionary, char const * const section){
  assert(dictionary != NULL);
  {
    char const * const device = config_getstring(dictionary,section,"device",section);
    if(strcasecmp(device,"fifos16") != 0)
      return -1; // not for us
  }
  config_validate_section(stderr,dictionary,section,Fifos16_keys,NULL);

  char const * const samprate = config_getstring(dictionary,section,"samprate",NULL);
  char const * const path = config_getstring(dictionary,section,"path",NULL);
  if(samprate == NULL || path == NULL){
    fprintf(stderr,"fifos16 [%s]: samprate and path are required\n",section);
    return -1;
  }
  struct sdrstate * const sdr = calloc(1,sizeof *sdr);
  if(sdr == NULL)
    return -1;
  sdr->frontend = frontend;
  frontend->context = sdr;
  snprintf(sdr->path,sizeof sdr->path,"%s",path);

  frontend->samprate = lround(parse_frequency(samprate,false));
  if(!(frontend->samprate > 0)){
    fprintf(stderr,"fifos16 [%s]: bad samprate %s\n",section,samprate);
    return -1;
  }
  frontend->isreal = true;
  frontend->bitspersample = 16;
  frontend->frequency = 0;         // direct sampling from 0 Hz
  frontend->lock = true;
  frontend->rf_gain = 0;           // zero, not NAN: see rtltcp.c
  frontend->rf_atten = 0;
  frontend->rf_level_cal = 0;
  frontend->rf_agc = false;
  frontend->min_IF = 15000;
  frontend->max_IF = Nyquist * frontend->samprate;
  {
    char const *p = config_getstring(dictionary,section,"description",NULL);
    snprintf(frontend->description,sizeof frontend->description,"%s",p != NULL ? p : "fifos16");
    Description = frontend->description;
  }
  fprintf(stderr,"fifos16: %s, %.0f Hz real\n",sdr->path,(double)frontend->samprate);
  return 0;
}

static bool still_running(struct sdrstate const * const sdr){
  enum state const s = atomic_load(&sdr->state);
  return s == RUNNING || s == STARTING;
}

static void *proc_fifos16(void *arg){
  struct sdrstate * const sdr = arg;
  struct frontend * const frontend = sdr->frontend;
  pthread_setname("fifos16");

  int fd = -1;
  while(still_running(sdr)){
    fd = open(sdr->path,O_RDWR | O_CLOEXEC);
    if(fd >= 0)
      break;
    fprintf(stderr,"fifos16: %s: %s; trying again in 1 s\n",sdr->path,strerror(errno));
    for(int i = 0; i < 10 && still_running(sdr); i++){
      struct timespec const tick = {0, 100 * 1000000};
      nanosleep(&tick,NULL);
    }
  }
  if(fd < 0)
    return NULL;
  struct stat st;
  if(fstat(fd,&st) != 0 || !S_ISFIFO(st.st_mode)){
    // A plain file would be read as fast as the disk allows, not at the
    // sample rate.
    fprintf(stderr,"fifos16: %s is not a FIFO; front end stays silent\n",sdr->path);
    close(fd);
    return NULL;
  }
  fprintf(stderr,"fifos16: reading %s\n",sdr->path);

  size_t const max_samples = frontend->L > 0 ? (size_t)frontend->L : 1 << 20;
  // Samples always start at buf[0]; an odd byte left over waits there.
  int16_t * const buf = malloc(2 * max_samples + 2);
  if(buf == NULL){
    fprintf(stderr,"fifos16: out of memory\n");
    close(fd);
    return NULL;
  }
  size_t have = 0; // bytes
  float const scale = (float)sdr->scale;
  while(still_running(sdr)){
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int const r = poll(&p,1,100); // wakes for shutdown, which only sets the state
    if(r < 0 && errno != EINTR){
      fprintf(stderr,"fifos16: poll %s: %s\n",sdr->path,strerror(errno));
      break;
    }
    if(r <= 0)
      continue;
    ssize_t const n = read(fd,(uint8_t *)buf + have,2 * max_samples - have);
    if(n < 0){
      if(errno == EINTR || errno == EAGAIN)
        continue;
      fprintf(stderr,"fifos16: read %s: %s\n",sdr->path,strerror(errno));
      break;
    }
    have += n;
    size_t const count = have / 2;
    float * const wptr = frontend->in.input_write_pointer.r;
    uint64_t energy = 0;
    int64_t sum = 0;
    uint64_t overs = 0;
    size_t last_over = SIZE_MAX;
    for(size_t i = 0; i < count; i++){
      int16_t const x = buf[i]; // little-endian host assumed, as radiod's rx888 driver does
      sum += x;
      energy += (uint64_t)((int32_t)x * (int32_t)x);
      if(x > 32766 || x < -32766){
        overs++;
        last_over = i;
      }
      wptr[i] = (float)x * scale;
    }
    if(count > 0){
      int const w = write_rfilter(&frontend->in,NULL,(int)count);
      assert(w != -1);
      (void)w;
      frontend->samples += count;
      if(overs){
        frontend->overranges += overs;
        frontend->samp_since_over = count - 1 - last_over;
      } else
        frontend->samp_since_over += count;
      double const dc = (double)sum / count;
      double const power = (double)energy / count - dc * dc;
      double const alpha = -expm1(-(double)count / frontend->samprate / Power_tc);
      frontend->if_power += alpha * (power - frontend->if_power);
    }
    if(have & 1)
      ((uint8_t *)buf)[0] = ((uint8_t *)buf)[have - 1];
    have &= 1;
  }
  free(buf);
  close(fd); // the writer now sees no reader, as when a dongle is unplugged
  return NULL;
}

int fifos16_startup(struct frontend * const frontend){
  struct sdrstate * const sdr = frontend->context;
  assert(sdr != NULL);
  while(true){
    enum state s = STOPPED;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STARTING))
      break;
    if(s == RUNNING)
      return 0;
    usleep(10000);
  }
  sdr->scale = scale_AD(frontend);
  if(pthread_create(&sdr->thread,NULL,proc_fifos16,sdr) != 0){
    atomic_store(&sdr->state,STOPPED);
    fprintf(stderr,"fifos16: cannot start the reader thread\n");
    return -1;
  }
  atomic_store(&sdr->state,RUNNING);
  fprintf(stderr,"fifos16: running\n");
  return 0;
}

int fifos16_shutdown(struct frontend * const frontend){
  struct sdrstate * const sdr = frontend->context;
  assert(sdr != NULL);
  while(true){
    enum state s = RUNNING;
    if(atomic_compare_exchange_strong(&sdr->state,&s,STOPPING))
      break;
    if(s == STOPPED)
      return 0;
    usleep(10000);
  }
  pthread_join(sdr->thread,NULL); // within about 100 ms (a poll)
  atomic_store(&sdr->state,STOPPED);
  fprintf(stderr,"fifos16: stopped\n");
  return 0;
}

double fifos16_tune(struct frontend * const frontend,double const freq){
  (void)freq;
  return frontend->frequency;
}
