uptime_hack
===========

A kernel module to change your uptime.
This kernel module has been tested with 4.10, but might also compile on
slightlier older kernel versions.

Usage:
======

```
root@vampirella:~# uptime
 14:59:40 up  2:52,  4 users,  load average: 0.09, 0.15, 0.21
root@vampirella:~# insmod uptime_hack.ko uptime=12345
root@vampirella:~# uptime
 14:59:52 up 35 days, 17:22,  4 users,  load average: 0.07, 0.14, 0.21

root@vampirella:~# echo 102021 > /sys/module/uptime_hack/parameters/uptime 
root@vampirella:~# uptime
 14:58:25 up 295 days,  7:03,  4 users,  load average: 0.08, 0.16, 0.22

root@vampirella:~# insmod uptime_hack.ko
root@vampirella:~# lsmod| grep uptime_hack
uptime_hack            13443  0 

root@vampirella:~# echo y > /sys/module/uptime_hack/parameters/hideme 
root@vampirella:~# lsmod| grep uptime_hack
root@vampirella:~#
```
