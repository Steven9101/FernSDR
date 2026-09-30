// SPDX-License-Identifier: GPL-3.0-or-later
//
// rtltcp: a radiod front end that takes complex unsigned 8-bit samples from
// an rtl_tcp server.
//
// radiod reads its samples from hardware drivers loaded as shared objects
// and has no network or file input, so on its own it cannot be given the
// same recorded or synthesised band as a receiver that reads rtl_tcp. This
// driver is that input: it connects to an rtl_tcp server, converts the
// bytes exactly as radiod's own rtlsdr driver converts a dongle's (excess
// 128, 8 bits per sample, scale_AD), and writes them into the front end's
// filter as a local dongle would. Sample rate and centre frequency come from
// the config, because the server cannot be tuned; they are also sent to the
// server as rtl_tcp commands so that it can log what the receiver expects.
//
// Buffering. Whatever the socket holds is read at once, up to one filter
// block (L samples), converted, and handed over; nothing waits here but an
// odd byte between reads. The socket's receive buffer is set to `rcvbuf`
// bytes (default 65536; the kernel doubles it for bookkeeping), so when
// radiod falls behind, at most about that much waits in this socket (16 ms
// at 2.048 Msps) before the server's own queue fills. rcvbuf = 0 leaves the
// kernel's autotuned default, up to net.ipv4.tcp_rmem's maximum.
//
// Lifetime. radiod starts a front end when its first channel appears and,
// in ka9q-radio since 2025, shuts it down when the last one goes; the
// thread connects on start, retries every second until the server answers,
// reconnects when the server goes away, and disconnects on shutdown. The
// madpsy fork used by UberSDR starts the front end once and never stops it.
//
// Written against ka9q-radio's front end interface (radio.h: struct
// frontend, write_cfilter, scale_AD); it builds unchanged against the
// madpsy fork, whose struct differs in field types but not in anything used
// here in a way that matters (see README.md).
#include <assert.h>
#include <complex.h>
#include <errno.h>
#include <iniparser/iniparser.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "misc.h"
#include "config.h"
#include "radio.h"

static char const *Rtltcp_keys[] = {
  "device", "library", "description", "samprate", "frequency", "host", "port", "rcvbuf", NULL
};

enum state { STOPPED, STARTING, RUNNING, STOPPING };

struct sdrstate {
  struct frontend *frontend;
  char host[256];
  char port[16];
  int rcvbuf;
  double scale;       // scale_AD(): full-scale input to +/-1, with the (zero) gains folded in
  pthread_t thread;
  _Atomic enum state state;
};

extern char const *Description;

// Power is smoothed over about 100 ms whatever the read sizes are.
static double const Power_tc = 0.1;

int rtltcp_setup(struct frontend * const frontend, dictionary const * const dictionary, char const * const section){
  assert(dictionary != NULL);
  {
    char const * const device = config_getstring(dictionary,section,"device",section);
    if(strcasecmp(device,"rtltcp") != 0)
      return -1; // not for us
  }
  config_validate_section(stderr,dictionary,section,Rtltcp_keys,NULL);

  char const * const samprate = config_getstring(dictionary,section,"samprate",NULL);
  char const * const frequency = config_getstring(dictionary,section,"frequency",NULL);
  if(samprate == NULL || frequency == NULL){
    fprintf(stderr,"rtltcp [%s]: samprate and frequency are required; the server cannot be asked for them\n",section);
    return -1;
  }
  struct sdrstate * const sdr = calloc(1,sizeof *sdr);
  if(sdr == NULL)
    return -1;
  sdr->frontend = frontend;
  frontend->context = sdr;
  snprintf(sdr->host,sizeof sdr->host,"%s",config_getstring(dictionary,section,"host","127.0.0.1"));
  snprintf(sdr->port,sizeof sdr->port,"%d",config_getint(dictionary,section,"port",1234));
  sdr->rcvbuf = config_getint(dictionary,section,"rcvbuf",65536);

  frontend->samprate = lround(parse_frequency(samprate,false));
  frontend->frequency = parse_frequency(frequency,false);
  if(!(frontend->samprate > 0) || !(frontend->frequency > 0)){
    fprintf(stderr,"rtltcp [%s]: bad samprate %s or frequency %s\n",section,samprate,frequency);
    return -1;
  }
  frontend->isreal = false;        // rtl_tcp is always complex
  frontend->bitspersample = 8;     // as radiod's rtlsdr driver
  frontend->lock = true;           // the server cannot be retuned
  // Zero, not NAN: the madpsy fork's scale_AD() adds these without checking.
  frontend->rf_gain = 0;
  frontend->rf_atten = 0;
  frontend->rf_level_cal = 0;
  frontend->rf_agc = false;
  // The usable span radiod's rtlsdr driver declares for a dongle.
  frontend->min_IF = -0.47 * frontend->samprate;
  frontend->max_IF = +0.47 * frontend->samprate;
  {
    char const *p = config_getstring(dictionary,section,"description",NULL);
    if(p != NULL)
      snprintf(frontend->description,sizeof frontend->description,"%s",p);
    else
      snprintf(frontend->description,sizeof frontend->description,"rtl_tcp %.100s:%s",sdr->host,sdr->port);
    Description = frontend->description;
  }
  fprintf(stderr,"rtltcp: %s:%s, %.0f Hz complex at %.0f Hz, receive buffer %d bytes\n",
          sdr->host,sdr->port,(double)frontend->samprate,frontend->frequency,sdr->rcvbuf);
  return 0;
}

static bool still_running(struct sdrstate const * const sdr){
  enum state const s = atomic_load(&sdr->state);
  return s == RUNNING || s == STARTING;
}

// Sleeps up to ms, waking early when the front end is being shut down.
static void pause_ms(struct sdrstate const * const sdr,int ms){
  while(ms > 0 && still_running(sdr)){
    struct timespec const tick = {0, 100 * 1000000};
    nanosleep(&tick,NULL);
    ms -= 100;
  }
}

static bool send_command(int fd,uint8_t cmd,uint32_t arg){
  uint8_t const msg[5] = {cmd, arg >> 24, arg >> 16, arg >> 8, arg};
  return send(fd,msg,sizeof msg,MSG_NOSIGNAL) == (ssize_t)sizeof msg;
}

// Connects and reads the server's 12-byte greeting ("RTL0", tuner type, gain
// count). Returns the socket, or -1 to try again later.
static int connect_server(struct sdrstate * const sdr,bool quiet){
  struct addrinfo const hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV};
  struct addrinfo *results = NULL;
  int const e = getaddrinfo(sdr->host,sdr->port,&hints,&results);
  if(e != 0){
    if(!quiet)
      fprintf(stderr,"rtltcp: %s:%s: %s\n",sdr->host,sdr->port,gai_strerror(e));
    return -1;
  }
  int fd = -1;
  for(struct addrinfo const *ai = results; ai != NULL; ai = ai->ai_next){
    fd = socket(ai->ai_family,ai->ai_socktype | SOCK_CLOEXEC,ai->ai_protocol);
    if(fd < 0)
      continue;
    // Before connect(), so that the window is scaled for it.
    if(sdr->rcvbuf > 0)
      setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&sdr->rcvbuf,sizeof sdr->rcvbuf);
    struct timeval const timeout = {2, 0}; // bounds connect() and the greeting
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
    if(connect(fd,ai->ai_addr,ai->ai_addrlen) == 0)
      break;
    if(!quiet)
      fprintf(stderr,"rtltcp: connect %s:%s: %s\n",sdr->host,sdr->port,strerror(errno));
    close(fd);
    fd = -1;
  }
  freeaddrinfo(results);
  if(fd < 0)
    return -1;

  int const one = 1;
  setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof one);
  uint8_t greeting[12];
  size_t got = 0;
  while(got < sizeof greeting){
    ssize_t const n = recv(fd,greeting + got,sizeof greeting - got,0);
    if(n <= 0){
      fprintf(stderr,"rtltcp: %s:%s closed before its greeting\n",sdr->host,sdr->port);
      close(fd);
      return -1;
    }
    got += n;
  }
  if(memcmp(greeting,"RTL0",4) != 0)
    fprintf(stderr,"rtltcp: %s:%s greeting is not RTL0; reading samples anyway\n",sdr->host,sdr->port);
  struct timeval const none = {0, 0};
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&none,sizeof none);

  // SET_SAMPLE_RATE and SET_FREQUENCY. A real server obeys them; the lab's
  // logs them, so a mismatch with the scene shows.
  struct frontend const * const frontend = sdr->frontend;
  if(!send_command(fd,0x02,(uint32_t)lround(frontend->samprate))
     || !send_command(fd,0x01,(uint32_t)llround(frontend->frequency)))
    fprintf(stderr,"rtltcp: could not send rate and frequency to %s:%s: %s\n",sdr->host,sdr->port,strerror(errno));
  int rcvbuf = 0;
  socklen_t len = sizeof rcvbuf;
  getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rcvbuf,&len);
  fprintf(stderr,"rtltcp: connected to %s:%s, receive buffer %d bytes\n",sdr->host,sdr->port,rcvbuf);
  return fd;
}

static void *proc_rtltcp(void *arg){
  struct sdrstate * const sdr = arg;
  struct frontend * const frontend = sdr->frontend;
  pthread_setname("rtltcp");

  // One filter block at most per read; radiod's input ring always has room
  // for a block beyond the one being filled.
  size_t const max_samples = frontend->L > 0 ? (size_t)frontend->L : 65536;
  uint8_t * const buf = malloc(2 * max_samples + 1);
  if(buf == NULL){
    fprintf(stderr,"rtltcp: out of memory\n");
    return NULL;
  }
  bool quiet = false;
  while(still_running(sdr)){
    int const fd = connect_server(sdr,quiet);
    if(fd < 0){
      quiet = true; // say it once per outage, not every second
      pause_ms(sdr,1000);
      continue;
    }
    quiet = false;
    size_t have = 0; // bytes in buf: at most one odd byte carried between reads
    while(still_running(sdr)){
      // Polling, not a blocking read, so that shutdown only has to change
      // the state and wait: this thread alone owns the socket.
      struct pollfd p = {.fd = fd, .events = POLLIN};
      int const r = poll(&p,1,100);
      if(r < 0 && errno != EINTR)
        break;
      if(r <= 0)
        continue;
      ssize_t const n = recv(fd,buf + have,2 * max_samples - have,0);
      if(n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)){
        fprintf(stderr,"rtltcp: %s:%s went away: %s\n",sdr->host,sdr->port,n == 0 ? "closed" : strerror(errno));
        break;
      }
      if(n < 0)
        continue;
      have += n;
      size_t const count = have / 2;
      float complex * const wptr = frontend->in.input_write_pointer.c;
      double const scale = sdr->scale;
      double energy = 0;
      uint64_t overs = 0;
      size_t last_over = SIZE_MAX;
      for(size_t i = 0; i < count; i++){
        uint8_t const re = buf[2 * i], im = buf[2 * i + 1];
        if(re == 0 || re == 255 || im == 0 || im == 255){
          overs += (re == 0 || re == 255) + (im == 0 || im == 255);
          last_over = i;
        }
        // Excess 128, as radiod's rtlsdr driver has it.
        double const i_ = (int)re - 128, q_ = (int)im - 128;
        energy += i_ * i_ + q_ * q_;
        wptr[i] = CMPLXF((float)(scale * i_),(float)(scale * q_));
      }
      if(count > 0){
        int const w = write_cfilter(&frontend->in,NULL,(int)count);
        assert(w != -1);
        (void)w;
        frontend->samples += count;
        if(overs){
          frontend->overranges += overs;
          frontend->samp_since_over = count - 1 - last_over;
        } else
          frontend->samp_since_over += count;
        double const alpha = -expm1(-(double)count / frontend->samprate / Power_tc);
        frontend->if_power += alpha * (energy / count - frontend->if_power);
      }
      if(have & 1)
        buf[0] = buf[have - 1];
      have &= 1;
    }
    close(fd);
    pause_ms(sdr,1000);
  }
  free(buf);
  return NULL;
}

int rtltcp_startup(struct frontend * const frontend){
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
  if(pthread_create(&sdr->thread,NULL,proc_rtltcp,sdr) != 0){
    atomic_store(&sdr->state,STOPPED);
    fprintf(stderr,"rtltcp: cannot start the reader thread\n");
    return -1;
  }
  atomic_store(&sdr->state,RUNNING);
  fprintf(stderr,"rtltcp: running\n");
  return 0;
}

int rtltcp_shutdown(struct frontend * const frontend){
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
  pthread_join(sdr->thread,NULL); // within about 100 ms (a poll), or 2 s in connect()
  atomic_store(&sdr->state,STOPPED);
  fprintf(stderr,"rtltcp: stopped\n");
  return 0;
}

double rtltcp_tune(struct frontend * const frontend,double const freq){
  (void)freq;
  return frontend->frequency; // fixed by the source
}
