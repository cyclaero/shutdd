//  shutdd.c
//
//  Created by Dr. Rolf Jansen on 2022-07-12.
//  Copyright 2022 Dr. Rolf Jansen. All rights reserved.
//
//  clang -g0 -O3 -fsigned-char shutdd.c -lgpio -lpthread -s -o /usr/local/bin/shutdd
//
//  Redistribution and use in source and binary forms, with or without modification,
//  are permitted provided that the following conditions are met:
//
//  1. Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//  2. Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
//  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
//  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
//  INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
//  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
//  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
//  OF THE POSSIBILITY OF SUCH DAMAGE.


#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <libgpio.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>


#define DAEMON_NAME "shutdd"

const char   *pidfname   = "/var/run/shutdd.pid";
gpio_handle_t gpioHandle = -1;

void usage(const char *executable)
{
   const char *r = executable + strlen(executable);
   while (--r >= executable && r && *r != '/');
   printf("\nusage: %s [-p file] [-f] [-n] [-b bank] [-g line] [-i interval] [-h]\n"
          " -p file     the path to the pid file [default: /var/run/shutdd.pid]\n"
          " -f          foreground mode, don't fork off as a daemon.\n"
          " -n          no console, don't fork off as a daemon.\n"
          " -b bank     GPIO bank id [0-4, default: 0].\n"
          " -g line     GPIO line id [0-53, default: 27].\n"
          " -i interval multiple push interval [0-2000 ms, default: 600 ms].\n"
          " -h          shows these usage instructions.\n", ++r);
}


typedef struct
{
   int           gpioBank;
   int           gpioLine;
   int           pushInterval;
}
gpioEventThreadSpec;


static inline double nanostamp(int64_t stamp)
{
   uint64_t ns = (1000000000*(stamp & 0xFFFFFFFFu) >> 32);
   return (int32_t)(stamp >> 32) + ns*1e-9;
}


pthread_t       event_thread;
pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  event_cond  = PTHREAD_COND_INITIALIZER;

bool gPushFlag  = false;
int  gPushCount = 0;

void *gpioEventThread(void *thread_param)
{
   gpioEventThreadSpec *spec = (gpioEventThreadSpec *)thread_param;

   ssize_t rc, rs = sizeof(struct gpio_event_detail);
   struct  gpio_event_detail buffer[1024];

   double t0, t, dt;
   for (;;)
   {
      if ((rc = read(gpioHandle, buffer, sizeof(buffer))) < 0)
         syslog(LOG_ERR, "Cannot read from GPIO%d", spec->gpioBank);

      else if (rc%rs != 0)
      {
         syslog(LOG_ERR, "read() odd count of %zd bytes from GPIO%d", rc, spec->gpioBank);
         rc = -1;
      }

      else
      {
         int c, i, n = (int)(rc/rs);
         for (c = 0, i = 0; i < n; i++)
            c += (buffer[i].gp_pin == spec->gpioLine) ? 1 : 0;
         
         if (c)
         {
            pthread_mutex_lock(&event_mutex);

            t = nanostamp(buffer[n-1].gp_time);
            if (gPushCount == 0)
               gPushCount  = 1, t0 = t, gPushFlag = true;

            else if (0.0005*spec->pushInterval <= (dt = t - t0) && dt < 0.0015*spec->pushInterval)
               gPushCount += 1, t0 = t, gPushFlag = true;

            if (gPushFlag)
               pthread_cond_signal(&event_cond);

            pthread_mutex_unlock(&event_mutex);
        }
      }
   }

   return NULL;
}


void cleanup(void)
{
   if (gpioHandle != -1)
      gpio_close(gpioHandle);

   if (pidfname)
      unlink(pidfname);
}


static void signals(int sig)
{
   switch (sig)
   {
      case SIGHUP:
         syslog(LOG_ERR, "Received SIGHUP signal.");
         kill(0, SIGHUP);
         exit(0);
         break;

      case SIGINT:
         syslog(LOG_ERR, "Received SIGINT signal.");
         kill(0, SIGINT);
         exit(0);
         break;

      case SIGQUIT:
         syslog(LOG_ERR, "Received SIGQUIT signal.");
         kill(0, SIGQUIT);
         exit(0);
         break;

      case SIGTERM:
         syslog(LOG_ERR, "Received SIGTERM signal.");
         kill(0, SIGTERM);
         exit(0);
         break;

      default:
         syslog(LOG_ERR, "Unhandled signal (%d) %s", sig, strsignal(sig));
         break;
   }
}


typedef enum
{
   noDaemon,
   launchdDaemon,
   discreteDaemon
} DaemonKind;


void daemonize(DaemonKind kind)
{
   struct sigaction act = {signals, SIGCHLD|SIGTSTP|SIGTTOU|SIGTTIN, SA_RESTART};

   switch (kind)
   {
      case noDaemon:
         sigaction(SIGINT, &act, NULL);
         openlog(DAEMON_NAME, LOG_NDELAY | LOG_PID | LOG_CONS, LOG_USER);
         pidfname = NULL;
         break;

      case launchdDaemon:
         sigaction(SIGTERM, &act, NULL);
         openlog(DAEMON_NAME, LOG_NDELAY | LOG_PID, LOG_USER);
         pidfname = NULL;
         break;

      case discreteDaemon:
      {
         // fork off the parent process
         pid_t pid = fork();

         if (pid < 0)
            exit(EXIT_FAILURE);

         // if we got a good PID, then we can exit the parent process.
         if (pid > 0)
            exit(EXIT_SUCCESS);

         // The child process continues here.
         // first close all open descriptors
         for (int i = getdtablesize(); i >= 0; --i)
            close(i);

         // re-open stdin, stdout, stderr connected to /dev/null
         int inouterr = open("/dev/null", O_RDWR);    // stdin
         dup(inouterr);                               // stdout
         dup(inouterr);                               // stderr

         // Change the file mode mask, 027 = complement of 750
         umask(027);

         pid_t sid = setsid();
         if (sid < 0)
            exit(EXIT_FAILURE);     // should log the failure before exiting?

         // Check and write our pid lock file
         // and mutually exclude other instances from running
         int pidfile = open(pidfname, O_RDWR|O_CREAT, 0640);
         if (pidfile < 0)
         {
            pidfname = NULL;
            exit(1);                // can not open our pid file
         }

         if (lockf(pidfile, F_TLOCK, 0) < 0)
            exit(0);                // can not lock our pid file -- was locked already

         // only first instance continues beyound this
         char s[256];
         int  l = snprintf(s, 256, "%d\n", getpid());
         write(pidfile, s, l);      // record pid to our pid file

         sigaction(SIGHUP,  &act, NULL);
         sigaction(SIGINT,  &act, NULL);
         sigaction(SIGQUIT, &act, NULL);
         sigaction(SIGTERM, &act, NULL);
         openlog(DAEMON_NAME, LOG_NDELAY | LOG_PID, LOG_USER);
         break;
      }
   }
}


void *gpioEventThread(void *spec);

int main(int argc, char *argv[])
{
   char        ch;
   const char *cmd   = argv[0];
   DaemonKind  dKind = discreteDaemon;

   int gpioBank     = 0,
       gpioLine     = 27,
       pushInterval = 600;

   while ((ch = getopt(argc, argv, "p:fnb:g:i:h")) != -1)
   {
      switch (ch)
      {
         case 'p':
            pidfname = optarg;
            break;

         case 'f':
            dKind = noDaemon;
            break;

         case 'n':
            dKind = launchdDaemon;
            break;

         case 'b':
            if ((gpioBank  = strtol(optarg, NULL, 10)) < 0 || 4 < gpioBank)
            {
               usage(cmd);
               return 1;
            }
            break;

         case 'g':
            if ((gpioLine = strtol(optarg, NULL, 10)) < 0 || 53 < gpioLine)
            {
               usage(cmd);
               return 1;
            }
            break;

         case 'i':
            if ((pushInterval = strtol(optarg, NULL, 10)) < 0 || 2000 < pushInterval)
            {
               usage(cmd);
               return 1;
            }
            break;

         case 'h':
            usage(cmd);
            return 0;

         default:
            usage(cmd);
            return 1;
            break;
      }
   }

   argc -= optind;
   argv += optind;
   daemonize(dKind);
   atexit(cleanup);
 
   if ((gpioHandle = gpio_open(gpioBank)) != GPIO_INVALID_HANDLE)
   {
      struct gpio_event_config fifo_config = {GPIO_EVENT_REPORT_DETAIL, 1024};
      ioctl(gpioHandle, GPIOCONFIGEVENTS, &fifo_config);

      gpio_config_t gcfg = {gpioLine, {}, 0, GPIO_PIN_INPUT|GPIO_INTR_EDGE_FALLING};
      gpio_pin_set_flags(gpioHandle, &gcfg);

      gpioEventThreadSpec spec = {gpioBank, gpioLine, pushInterval};
      pthread_mutex_lock(&event_mutex);
      if (pthread_create(&event_thread, NULL, gpioEventThread, &spec))
      {
         syslog(LOG_ERR, "Cannot create thread for reading GPIO interrupts.");
         return -1;
      }

   resume:
      while (gPushFlag == false)
         pthread_cond_wait(&event_cond, &event_mutex);
      gPushFlag = false;
      pthread_mutex_unlock(&event_mutex);

      usleep(4000*pushInterval);

      switch (gPushCount)
      {
         case 1:
            kill(1, SIGUSR2);
            break;

         case 2:
            kill(1, SIGINT);
            break;

         case 3:
            kill(1, SIGTERM);
            break;

         case 4:
            syslog(LOG_ERR, "Quadruple push is not implemented yet -- shutdd resumes the operation.");
         default:
            pthread_mutex_lock(&event_mutex);
            gPushFlag  = false;
            gPushCount = 0;
            goto resume;
      }
   }

   else
   {
      syslog(LOG_ERR, "Cannot open GPIO%d", gpioBank);
      return -1;
   }

   return 0;
}
