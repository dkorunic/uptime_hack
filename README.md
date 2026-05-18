# uptime_hack

A Linux kernel module that fakes the system uptime reported by `/proc/uptime`
(and therefore by `uptime(1)`, `w`, `who -b`, monitoring agents, etc.).

The module installs an **ftrace function hook** on the kernel's
`uptime_proc_show`, so every read of `/proc/uptime` runs a replacement that
either substitutes an absolute value for the uptime or adds a configured
offset on top of the real one, depending on how the parameter was set.
No `/proc` entries are replaced, no kernel rodata is patched, and the hook
survives across all 6.x kernels.

Tested against Linux 6.12. Requires the running kernel to be built with:

- `CONFIG_FUNCTION_TRACER=y`
- `CONFIG_DYNAMIC_FTRACE_WITH_REGS=y`
- `CONFIG_KPROBES=y`

All three are default-on in mainline distros (Debian, Ubuntu, Fedora, Arch,
NixOS).

# Parameters

| Parameter  | Type                        | Meaning                                                                                  |
| ---------- | --------------------------- | ---------------------------------------------------------------------------------------- |
| `uptime`   | duration string (see below) | Absolute uptime to report, or an offset added to the real uptime when prefixed with `+`. |
| `idletime` | unsigned long long (u64)    | Seconds added to the idle-time column of `/proc/uptime`                                  |
| `hideme`   | bool (`y`/`n`)              | Hide the module from `lsmod`/`/sys/module`                                               |

The `uptime` parameter has two modes:

- **Absolute** (bare value, no prefix). The reported uptime is set to
  exactly the parsed duration. Example: `uptime=1d` makes `/proc/uptime`
  report ~1 day regardless of how long the system has actually been
  running. The seconds column stays frozen at the configured value
  between writes — only the fractional part keeps ticking.
- **Additive** (`+` prefix). The parsed duration is added to the real
  boot-time uptime, and the result keeps ticking forward normally.
  Example: `uptime=+1d` makes `/proc/uptime` report "real uptime + 1
  day" and continues to advance.

The value `0` is treated specially: with or without the `+` prefix, it
always means "show the real boot-time uptime." This preserves the
historical "reset" idiom (`echo 0 > …/uptime`) and means that a freshly
loaded module — whose default state is `uptime=0` — reports the unaltered
real uptime until you write a new value. Reading the parameter back via
sysfs after a reset returns the bare form (`0`, not `+0`).

The duration syntax is the same in both modes: a plain integer (seconds,
the historical form) or a sequence of `<number><unit>` tokens where
`<unit>` is one of `y`, `d`, `h`, `m`, `s` (case-insensitive). One year is
treated as 365 days. Whitespace between components is allowed. Examples:

| Input          | Resolved seconds | Mode     |
| -------------- | ---------------- | -------- |
| `12345`        | 12345            | absolute |
| `30m`          | 1800             | absolute |
| `1d2h30m`      | 95400            | absolute |
| `1y 30d`       | 34128000         | absolute |
| `+12345`       | 12345            | additive |
| `+1d`          | 86400            | additive |
| `+5d 12h`      | 475200           | additive |
| `+1d 2h 3m 4s` | 93784            | additive |

Reading the parameter back via sysfs preserves the mode: an absolute value
reads as a bare number, an additive value reads with a leading `+`.

Malformed input is rejected with `-EINVAL`; arithmetic that would overflow
a 64-bit unsigned value is rejected with `-ERANGE`. The previous parameter
value and mode are preserved on rejection.

# Usage

```
root@vampirella:~# uptime
 18:57:15 up 13 days,  4:19,  1 user,  load average: 0.21, 0.18, 0.17

# Additive: pretend the box has been up an extra day on top of the real uptime
root@vampirella:~# insmod uptime_hack.ko uptime=+1d
root@vampirella:~# uptime
 18:57:46 up 14 days,  4:20,  1 user,  load average: 0.14, 0.23, 0.27

# Absolute: pin the reported uptime to exactly 5 days, 12h30m (no '+' prefix)
root@vampirella:~# echo 5d12h30m > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 18:58:30 up 5 days, 12:30,  1 user,  load average: 0.15, 0.22, 0.27

# Absolute: years work too (1y = 365 days, so 2y = 730 days exactly)
root@vampirella:~# echo 2y > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 18:58:35 up 730 days, 0 min,  1 user,  load average: 0.15, 0.22, 0.27

# Plain integer is still accepted in both modes — bare = absolute seconds
root@vampirella:~# echo 102021 > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 18:59:25 up 1 day,  4:20,  1 user,  load average: 0.15, 0.22, 0.27

# …and '+'-prefixed = additive seconds on top of real uptime
root@vampirella:~# echo +102021 > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 18:59:38 up 14 days,  8:41,  1 user,  load average: 0.15, 0.22, 0.27

# Whitespace between components is fine (remember to quote in the shell)
root@vampirella:~# echo "+1d 2h 3m 4s" > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 19:00:04 up 14 days,  6:25,  1 user,  load average: 0.15, 0.22, 0.27

# Read the current value back — leading '+' indicates additive mode
root@vampirella:~# cat /sys/module/uptime_hack/parameters/uptime
+93784

# Reset to true uptime — '0' (with or without '+') is special-cased
root@vampirella:~# echo 0 > /sys/module/uptime_hack/parameters/uptime
root@vampirella:~# uptime
 19:00:30 up 13 days,  4:22,  1 user,  load average: 0.15, 0.22, 0.27
```

Idle time can be offset independently (plain seconds only, always additive).
The effect is only visible in `/proc/uptime` directly — `uptime(1)` ignores the
idle column:

```
root@vampirella:~# cat /proc/uptime
1140271.42 4523890.11
root@vampirella:~# insmod uptime_hack.ko uptime=+1d idletime=43200
root@vampirella:~# cat /proc/uptime
1226672.84 4567092.37
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

> **Warning — hiding is one-way at runtime.** `module_hide()` delists the
> module from `THIS_MODULE->list` and removes its kobject from
> `/sys/module/`. After that point `/sys/module/uptime_hack/parameters/hideme`
> no longer exists (so you cannot write `n` to it), and `rmmod uptime_hack`
> fails because `find_module()` only walks the public list. The hook keeps
> working. To unload again, use the bundled `unhide.ko` helper (see below)
> — otherwise the module sticks until reboot.

# Recovering a hidden module

The repo ships a companion module, `unhide.ko`, that resurrects a hidden
`uptime_hack` so it can be `rmmod`'d normally. It needs any kernel-text
address inside the hidden module (e.g. one of its still-listed symbols in
`/proc/kallsyms`), passed as the `target=` parameter:

```
root@vampirella:~# target=$(awk '/\[uptime_hack\]/{print "0x"$1; exit}' /proc/kallsyms)
root@vampirella:~# sudo insmod ./unhide.ko target=$target
root@vampirella:~# sudo rmmod uptime_hack
root@vampirella:~# sudo rmmod unhide
```

`unhide` looks up `__module_address` via the same kprobe trick used by
`uptime_hack` itself, calls it on `target` to get the hidden `struct
module *`, then re-adds it to the module list and `/sys/module/` if
needed. It is not `module_mutex`-safe, so do not race it against concurrent
`insmod`/`rmmod`.

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
   `uptime_proc_show` enters the module's `fh_callback`, which calls
   `ftrace_regs_set_instruction_pointer()` on the saved `struct
ftrace_regs` to point at the replacement `hooked_uptime_proc_show`.
   When the callback returns, control resumes at the replacement. The
   helper is used instead of poking `pt_regs->ip` directly so the hook
   stays arch-portable.

   A `within_module(parent_ip, THIS_MODULE)` guard at the top of
   `fh_callback` short-circuits any call that originates from inside this
   module's own text. If a future change to the replacement ever calls a
   filtered function back, the guard keeps that call from being
   redirected to itself.

The replacement mirrors the upstream `uptime_proc_show` body. For the
uptime column it either uses the configured value directly (absolute mode)
or adds it to the real boot-time uptime (additive mode, selected by the
`+` prefix). The idle-time column is always additive. Because the value
and mode flag are read on every call, sysfs writes take effect immediately
on the next read of `/proc/uptime`.

On unload, `unregister_ftrace_function()` + `ftrace_set_filter_ip(remove=1)`
detaches the hook. ftrace's internal synchronization ensures in-flight
callers drain before the module text is freed.

Earlier versions (≤ 1.7) patched the read-only `proc_ops` table of
`/proc/uptime` directly. That approach silently stopped working on kernels
that placed `proc_ops` behind strict-rwx large-page protections. The ftrace
hook avoids the problem entirely by redirecting at the call site.
