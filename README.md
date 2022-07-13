# shutdd
A shutdown daemon for FreeBSD lurking for push button events on a GPIO port

Compile:

    clang -g0 -O3 -fsigned-char -Wno-empty-body -Wno-parentheses shutdd.c -lgpio -s -o /usr/local/bin/shutdd

Usage:

    shutdd [-p file] [-f] [-n] [-b] [-g] [-h]  
     -p file    the path to the pid file [default: /var/run/shutdd.pid]  
     -f         foreground mode, don't fork off as a daemon.  
     -n         no console, don't fork off as a daemon.  
     -b         GPIO bank id [default: 0].  
     -g         GPIO line id [default: 27].  
     -h         shows these usage instructions.  

`shutdd` does not poll the state of the GPIO port, but instead utilizes FreeBSD's user space interface for GPIO interrupts for lurking on state changes of the GPIO line - default GPIO0.27. Therefore, no significant load is imposed on the CPU's.
