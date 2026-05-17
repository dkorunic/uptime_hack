# uptime_hack

A Linux kernel module that fakes the system uptime reported by `/proc/uptime`
(and therefore by `uptime(1)`, `w`, `who -b`, monitoring agents, etc.).

The module installs an **ftrace function hook** on the kernel's
`uptime_proc_show`, so every read of `/proc/uptime` runs a replacement that
adds the configured offset before emitting the value. No `/proc` entries are
replaced, no kernel rodata is patched, and the hook survives across all
6.x kernels.

Tested against Linux 6.12. Requires the running kernel to be built with:

- `CONFIG_FUNCTION_TRACER=y`
- `CONFIG_DYNAMIC_FTRACE_WITH_REGS=y`
- `CONFIG_KPROBES=y`

All three are default-on in mainline distros (Debian, Ubuntu, Fedora, Arch,
NixOS).

# Parameters

| Parameter  | Type                        | Meaning                                                  |
| ---------- | --------------------------- | -------------------------------------------------------- |
| `uptime`   | duration string (see below) | Seconds (or d/h/m/s combination) added to `/proc/uptime` |
| `idletime` | unsigned long               | Seconds added to the idle-time column of `/proc/uptime`  |
| `hideme`   | bool (`y`/`n`)              | Hide the module from `lsmod`/`/sys/module`               |

The `uptime` parameter accepts either a plain integer (seconds, the
historical form) or a sequence of `<number><unit>` tokens where `<unit>` is
one of `d`, `h`, `m`, `s` (case-insensitive). Whitespace between components
is allowed. Examples:

| Input         | Resolved seconds |
| ------------- | ---------------- |
| `12345`       | 12345            |
| `30m`         | 1800             |
| `2h`          | 7200             |
| `1d`          | 86400            |
| `1d2h30m`     | 95400            |
| `5d 12h`      | 475200           |
| `1d 2h 3m 4s` | 93784            |

Malformed input is rejected with `-EINVAL`; arithmetic that would overflow
`unsigned long` is rejected with `-ERANGE`. The previous parameter value is
preserved on rejection.

# Usage

```
root@vampirella:~# uptime
 18:57:15 up 13 days,  4:19,  1 user,  load average: 0.21, 0.18, 0.17

# Load with a starting offset of 1 day
root@vampirella:~# insmod uptime_hack.ko uptime=1d
root@vampirella:~# uptime
 18:57:46 up 14 days,  4:20,  1 user,  load average: 0.14, 0.23, 0.27

# Adjust at runtime via sysfs — backward-compatible bare seconds still work
root@vampirella:~# echo 102021 > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 18:58:25 up 14 days,  4:39,  1 user,  load average: 0.15, 0.22, 0.27

# …or use the d/h/m/s suffix syntax
root@vampirella:~# echo 5d12h30m > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 18:58:30 up 18 days, 16:51,  1 user,  load average: 0.15, 0.22, 0.27

# Whitespace between components is fine (remember to quote in the shell)
root@vampirella:~# echo "1d 2h 3m 4s" > /sys/module/uptime_hack/parameters/uptime

# Read the current offset back
root@vampirella:~# cat /sys/module/uptime_hack/parameters/uptime
93784

# Reset to true uptime
root@vampirella:~# echo 0 > /sys/module/uptime_hack/parameters/uptime
```

Idle time can be offset independently (plain seconds only):

```
root@vampirella:~# insmod uptime_hack.ko uptime=1d idletime=43200
```

The module can also hide itself from `lsmod` and `/sys/module`:

```
root@vampirella:~# insmod uptime_hack.ko
root@vampirella:~# lsmod | grep uptime_hack
uptime_hack            12288  0

root@vampirella:~# echo y > /sys/module/uptime_hack/parameters/hideme
root@vampirella:~# lsmod | grep uptime_hack
root@vampirella:~#
```

> **Warning — hiding is one-way.** `module_hide()` delists the module from
> `THIS_MODULE->list` and removes its kobject from `/sys/module/`. After
> that point `/sys/module/uptime_hack/parameters/hideme` no longer exists
> (so you cannot write `n` to it), and `rmmod uptime_hack` fails because
> `find_module()` only walks the public list. The hook keeps working, but
> the module cannot be unloaded without a reboot. Only enable `hideme=y`
> if that's acceptable.

# Build

Standard kernel-module build:

```
$ make
$ sudo insmod ./uptime_hack.ko
$ sudo rmmod uptime_hack
```

Build on NixOS:

```
# replace linux in default.nix with the kernel package of your current booted kernel
$ nix-shell --command 'make'
$ sudo insmod ./uptime_hack.ko
```

(If you don't use [NixOS](https://nixos.org/), you should switch.)

# How it works

On load, the module:

1. Resolves the address of `uptime_proc_show` by briefly registering and
   unregistering a kprobe — this is the modern escape hatch since
   `kallsyms_lookup_name` lost its `EXPORT_SYMBOL` in kernel 5.7.
2. Calls `ftrace_set_filter_ip()` to scope an `ftrace_ops` to that single
   instruction-pointer.
3. Calls `register_ftrace_function()` with `FTRACE_OPS_FL_SAVE_REGS |
FTRACE_OPS_FL_IPMODIFY`. From this point, every call to
   `uptime_proc_show` enters the module's `fh_callback`, which rewrites
   `regs->ip` to point at the replacement `hooked_uptime_proc_show`. When
   the callback returns, control resumes at the replacement.

The replacement mirrors the upstream `uptime_proc_show` body, adding the
configured `uptime` / `idletime` offsets before `seq_printf`. Because the
offsets are read on every call, sysfs writes take effect immediately on the
next read of `/proc/uptime`.

On unload, `unregister_ftrace_function()` + `ftrace_set_filter_ip(remove=1)`
detaches the hook. ftrace's internal synchronization ensures in-flight
callers drain before the module text is freed.

Earlier versions (≤ 1.7) patched the read-only `proc_ops` table of
`/proc/uptime` directly. That approach silently stopped working on kernels
that placed `proc_ops` behind strict-rwx large-page protections. The ftrace
hook avoids the problem entirely by redirecting at the call site.
