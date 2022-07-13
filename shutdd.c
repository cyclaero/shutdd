//  shutdd.c
//
//  Created by Dr. Rolf Jansen on 2022-07-12.
//  Copyright 2022 Dr. Rolf Jansen. All rights reserved.
//
//  clang -g0 -O3 -Wno-empty-body -Wno-parentheses shutdd.c -s -o /usr/local/bin/shutdd
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
#include <unistd.h>
#include <fcntl.h>
#include <libgpio.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>


#define DAEMON_NAME "shutdd"

const char *pidfname = "/var/run/shutdd.pid";

void usage(const char *executable)
{
   const char *r = executable + strlen(executable);
   while (--r >= executable && r && *r != '/');
   printf("\nusage: %s [-p file] [-f] [-n] [-b] [-g] [-h]\n"
          " -p file    the path to the pid file [default: /var/run/shutdd.pid]\n"
          " -f         foreground mode, don't fork off as a daemon.\n"
          " -n         no console, don't fork off as a daemon.\n"
          " -b         GPIO bank id [default: 0].\n"
          " -g         GPIO line id [default: 27].\n"
          " -h         shows these usage instructions.\n", ++r);
}


int lurkForGPIOStopEvent(int GPIOBankID, int GPIOStopID)
{
   gpio_handle_t gpio;

   if ((gpio = gpio_open(GPIOBankID)) != GPIO_INVALID_HANDLE)
   {
      struct gpio_event_config fifo_config = {GPIO_EVENT_REPORT_DETAIL, 1024};
      ioctl(gpio, GPIOCONFIGEVENTS, &fifo_config);

      gpio_config_t gcfg = {GPIOStopID, {}, 0, GPIO_PIN_INPUT|GPIO_INTR_EDGE_FALLING};
      gpio_pin_set_flags(gpio, &gcfg);

      ssize_t rc, rs = sizeof(struct gpio_event_detail);
      struct  gpio_event_detail buffer[1024];

      if ((rc = read(gpio, buffer, sizeof(buffer))) < 0)
         syslog(LOG_ERR, "Cannot read from GPIO%d", GPIOBankID);

      else if (rc%rs != 0)
      {
         syslog(LOG_ERR, "read() odd count of %zd bytes from GPIO%d", rc, GPIOBankID);
         rc = -1;
      }

      else
      {
         int i, n = (int)(rc/rs);
         for (rc = 0, i = 0; i < n; i++)
            rc += (buffer[i].gp_pin == GPIOStopID) ? 1 : 0;
      }
      
      gpio_close(gpio);
      return rc;
   }

   else
   {
      syslog(LOG_ERR, "Cannot read from GPIO%d", GPIOBankID);
      return -1;
   }
}


void cleanup(void)
{
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
   switch (kind)
   {
      case noDaemon:
         signal(SIGINT, signals);
         openlog(DAEMON_NAME, LOG_NDELAY | LOG_PID | LOG_CONS, LOG_USER);
         break;

      case launchdDaemon:
         signal(SIGTERM, signals);
         openlog(DAEMON_NAME, LOG_NDELAY | LOG_PID, LOG_USER);
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
            exit(1);                // can not open our pid file

         if (lockf(pidfile, F_TLOCK, 0) < 0)
            exit(0);                // can not lock our pid file -- was locked already

         // only first instance continues beyound this
         char s[256];
         int  l = snprintf(s, 256, "%d\n", getpid());
         write(pidfile, s, l);      // record pid to our pid file

         signal(SIGHUP,  signals);
         signal(SIGINT,  signals);
         signal(SIGQUIT, signals);
         signal(SIGTERM, signals);
         signal(SIGCHLD, SIG_IGN);  // ignore child
         signal(SIGTSTP, SIG_IGN);  // ignore tty signals
         signal(SIGTTOU, SIG_IGN);
         signal(SIGTTIN, SIG_IGN);

         openlog(DAEMON_NAME, LOG_NDELAY | LOG_PID, LOG_USER);
         break;
      }
   }
}


int main(int argc, char *argv[])
{
   char        ch;
   const char *cmd   = argv[0];
   DaemonKind  dKind = discreteDaemon;

   int GPIOBankID = 0,
       GPIOStopID = 27;

   while ((ch = getopt(argc, argv, "p:fnb:g:h")) != -1)
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
            if ((GPIOBankID = strtol(optarg, NULL, 10)) < 0 || 4 < GPIOBankID)
            {
               usage(cmd);
               return 1;
            }
            break;

         case 'g':
            if ((GPIOStopID = strtol(optarg, NULL, 10)) < 0 || 53 < GPIOStopID)
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

   if (lurkForGPIOStopEvent(GPIOBankID, GPIOStopID) > 0)
      kill(1, SIGUSR2);

   return 0;
}  
