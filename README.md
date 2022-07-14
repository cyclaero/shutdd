# shutdd
A shutdown daemon for FreeBSD lurking for push button events on a GPIO port

Compile:

    clang -g0 -O3 -fsigned-char shutdd.c -lgpio -lpthread -s -o /usr/local/bin/shutdd

Usage:

    usage: shutdd [-p file] [-f] [-n] [-b bank] [-g line] [-i interval] [-h]
     -p file     the path to the pid file [default: /var/run/shutdd.pid]
     -f          foreground mode, don't fork off as a daemon.
     -n          no console, don't fork off as a daemon.
     -b bank     GPIO bank id [0-4, default: 0].
     -g line     GPIO line id [0-53, default: 27].
     -i interval multiple push interval [0-2000 ms, default: 600 ms].
     -h          shows these usage instructions.

`shutdd` does not poll the state of the GPIO port, but instead utilizes FreeBSD's user space interface for GPIO interrupts for lurking on state changes of the GPIO line - default GPIO0.27. Therefore, no significant load is imposed on the CPU's.

A single push causes the system to shutdown.  
A double push causes the system to restart.  
A triple push causes the system to enter single user mode.
