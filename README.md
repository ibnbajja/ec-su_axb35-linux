# ec-su_axb35-linux
Linux driver for the embedded controller on the Sixunited AXB35-02 board.

Vendors using that board:
  - GMKtec EVO-X2
  - Bosgame M5
  - FEVM FA-EX9
  - Peladn YO1
  - NIMO AI MiniPC
  - Corsair AI Workstation 300

An update to date list can be found [here](https://strixhalo.wiki/Hardware/Boards/Sixunited_AXB35/)

For more details, please have a look at the desevens wiki: https://strixhalo.wiki/Guides/Sixunited_AXB35/Power_Mode_and_Fan_Control/

# Build instructions
```
$ make
$ sudo make install
$ sudo insmod ec_su_axb35
```

# Devices
```
# Fan devices
/sys/class/ec_su_axb35/fan1/                    - CPU fan 1
/sys/class/ec_su_axb35/fan2/                    - CPU fan 2
/sys/class/ec_su_axb35/fan3/                    - System fan
/sys/class/ec_su_axb35/fanX/rpm            (RO) - current speed in rpm
/sys/class/ec_su_axb35/fanX/mode           (RW) - [auto, fixed, curve]
/sys/class/ec_su_axb35/fanX/level          (RW) - [0-5] (0=0%, 1=20%, ..., 5=100%)
/sys/class/ec_su_axb35/fanX/rampup_curve   (RW) - 5 values (°C thresholds for level 1-5)
/sys/class/ec_su_axb35/fanX/rampdown_curve (RW) - 5 values (°C thresholds for level 1-5)

# Temperature device
/sys/class/ec_su_axb35/temp1/                   - CPU temperature in °C
/sys/class/ec_su_axb35/temp1/temp          (RO) - current
/sys/class/ec_su_axb35/temp1/min           (RO) - min temp measured since dirver load
/sys/class/ec_su_axb35/temp1/max           (RO) - amx temp measured since driver load

# APU device
/sys/class/ec_su_axb35/apu/power_mode      (RW) - [quiet, balanced, performance]
```

# hwmon
In addition to the sysfs interface above, the driver registers a standard hwmon
device (name `ec_su_axb35`), so the sensors are visible to `sensors` and any
other hwmon consumer:

```
/sys/class/hwmon/hwmonX/temp1_input    (RO) - CPU temperature in millidegrees C
/sys/class/hwmon/hwmonX/fan1_input     (RO) - fan 1 speed in rpm
/sys/class/hwmon/hwmonX/fan2_input     (RO) - fan 2 speed in rpm
/sys/class/hwmon/hwmonX/fan3_input     (RO) - fan 3 speed in rpm
```

Use `$ sensors` to list them.

# Python GUI app (needs root to write to /sys/class/ec_su_axb35/*)
to test:
python ./ec-su_axb35-linux-gui.py

to install:
sudo install -m 755 ec-su_axb35-linux-gui.py /usr/local/bin/ec-su_axb35-linux-gui
cp ec-fan-control.desktop ~/.local/share/applications/

# EVO-X2 fork

This fork runs the driver on one GMKtec EVO-X2 (Fedora 44) that serves a
production workload. It is [eschaubert/ec-su_axb35-linux](https://github.com/eschaubert/ec-su_axb35-linux)
at `edc429d9fa23ff7d2365c02adea421684d001d0d`, which is upstream
[cmetz/ec-su_axb35-linux](https://github.com/cmetz/ec-su_axb35-linux) `f62c2c2` plus a read-only
hwmon device folded into the main module. On top of that:

| Change | Why |
|---|---|
| Load guard: DMI `GMKtec` / `NucBox_EVO-X2` / chassis vendor `SU`, and EC firmware 1.6 in EC RAM 0x00/0x01 (and in SMBIOS, when it reports one); anything else gets `-ENODEV` | The driver writes EC registers; the register map was checked on this board and firmware only |
| `allow_board="vendor:product:chassis_vendor:MAJOR.MINOR"` parameter | Add another verified board without rebuilding |
| Every `ec_read`/`ec_write` result is checked; the worker keeps the fan levels when the temperature read fails | Upstream ran the curve on an uninitialized byte |
| Curve writes must keep rampup strictly increasing and `rampdown[i] < rampup[i]` | Otherwise a fan can step up and back down at one temperature |
| Free the curve buffer, not the `strsep` cursor | Malformed curve writes freed an interior pointer |
| `curve_min_level` parameter (default 1 = 20%) | The curve never stops a fan: not on the switch to curve mode, not when cooling down |
| Loading leaves fans in their firmware state (manual mode shows as `fixed`) | `modprobe` alone changes nothing; only `mode=curve` hands a fan to the driver |
| Unload stops the curve worker before resetting fans to AUTO | No level write after the reset |
| `dkms.conf`, `contrib/evo-x2/` | DKMS install, systemd unit, A/B check script |

Module parameters (read-only after load):

```
allow_board      extra known-good board, "sys_vendor:product_name:chassis_vendor:EC_MAJOR.EC_MINOR"
curve_min_level  lowest level in curve mode, 0-5 (default 1; 0 lets the curve stop the fans)
```

## contrib/evo-x2

| File | Installed as |
|---|---|
| `ec-su_axb35-fan-curve.service` | `/etc/systemd/system/` - oneshot: `modprobe`, curves + `mode=curve` for fan1-3; stop = `mode=auto` |
| `ec-su_axb35-fan-curve` | `/usr/local/libexec/` - `start`, `stop`, `status`; never writes `apu/power_mode` |
| `fan-curve.conf` | `/etc/ec-su_axb35/fan-curve.conf` - fan1/fan2 100% at 88 °C, fan3 at 85 °C, never off |
| `ec-su_axb35-check` | `/usr/local/bin/` - samples gpu_metrics + hwmon for N minutes, prints averages + a JSON line |
| `dkms-framework.conf` | `/etc/dkms/framework.conf.d/ec_su_axb35.conf` - skips Fedora's `dracut --regenerate-all` after DKMS transactions (this module is not needed in the initramfs) |

There is deliberately no `/etc/modules-load.d` entry: the unit loads the module, so disabling the
unit also stops it loading at boot.

## Install (Fedora 44)

Secure Boot is off on the target, so module signing is not needed. DKMS still creates
`/var/lib/dkms/mok.{key,pub}` and signs the module with it; that is harmless.
`kernel-devel` for the running kernel must be installed.

```
git clone https://github.com/OWNER/ec-su_axb35-linux ~/Dev/ec-su_axb35-linux
cd ~/Dev/ec-su_axb35-linux && git checkout evo-x2-2026.10.10   # the reviewed tag
sudo dnf install dkms
sudo install -D -m0644 contrib/evo-x2/dkms-framework.conf /etc/dkms/framework.conf.d/ec_su_axb35.conf
git archive --prefix=ec_su_axb35-2026.10.10/ HEAD | sudo tar -x -C /usr/src
sudo dkms install ec_su_axb35/2026.10.10       # add + build + install for the running kernel
sudo modprobe ec_su_axb35                      # read-only: fans keep their firmware state
sudo install -D -m0644 contrib/evo-x2/fan-curve.conf /etc/ec-su_axb35/fan-curve.conf
sudo install -D -m0755 contrib/evo-x2/ec-su_axb35-fan-curve /usr/local/libexec/ec-su_axb35-fan-curve
sudo install -D -m0755 contrib/evo-x2/ec-su_axb35-check /usr/local/bin/ec-su_axb35-check
sudo install -D -m0644 contrib/evo-x2/ec-su_axb35-fan-curve.service /etc/systemd/system/ec-su_axb35-fan-curve.service
sudo systemctl daemon-reload
# verify (below), then hand the fans to the curve:
sudo systemctl enable --now ec-su_axb35-fan-curve.service
```

## Verify

After `modprobe`, before enabling the unit:

```
sudo dmesg | grep ec_su_axb35        # "driver loaded"; no "refusing to load"
sudo /usr/local/libexec/ec-su_axb35-fan-curve status
sensors 'ec_su_axb35-*'
```

With the BIOS smart-fan setting, the EC holds every fan in manual mode at 80% (EC RAM
0x21/0x22 = 11/15, 0x23/0x24 = 21/25, 0x25/0x26 = 31/35). The driver must show
`mode fixed level 4` for fan1-3. Under load, expect about 3,870 / 3,990 / 1,600 rpm and an EC
temperature of 98-99 °C. Those were the read-only EC RAM readings on 2026-10-10.

After `systemctl enable --now`:

```
systemctl status ec-su_axb35-fan-curve.service
sudo /usr/local/libexec/ec-su_axb35-fan-curve status   # mode curve, the configured curves; level 5 above 88 °C
```

## A/B check

Run both arms under the same load, 10 minutes each:

```
ec-su_axb35-check --minutes 10 --label firmware-80 --csv /tmp/fan-a.csv   # module loaded, unit not started
sudo systemctl enable --now ec-su_axb35-fan-curve.service
ec-su_axb35-check --minutes 10 --label curve --csv /tmp/fan-b.csv
```

Compare: average gfxclk, gfx temperature, thm_gfx residency % (100% = the iGPU is clocked down
for heat the whole time), socket power, and fan RPMs.

## Rollback

Do the steps in order. After step 1 the fans are back under EC control. After step 4 nothing
from this install is left. Step 5 restores the exact firmware fan state:

1. `sudo systemctl disable --now ec-su_axb35-fan-curve.service`: ExecStop sets
   `mode=auto` on fan1-3, so the EC's own control takes over, and the module no longer loads at
   boot. EC auto is not the BIOS smart-fan state (manual, 80%); step 5 restores that.
2. `sudo modprobe -r ec_su_axb35`: the driver stops its worker and resets fans to AUTO again.
3. `sudo dkms remove ec_su_axb35/2026.10.10 --all && sudo rm -rf /usr/src/ec_su_axb35-2026.10.10`.
4. Remove the installed files and reload:
   `sudo rm -f /etc/systemd/system/ec-su_axb35-fan-curve.service /usr/local/libexec/ec-su_axb35-fan-curve /usr/local/bin/ec-su_axb35-check /etc/dkms/framework.conf.d/ec_su_axb35.conf && sudo rm -rf /etc/ec-su_axb35 && sudo systemctl daemon-reload`.
   There is no `/etc/modules-load.d` entry to remove (`ls /etc/modules-load.d` to confirm).
   Optionally `sudo dnf remove dkms` and `sudo rm -f /var/lib/dkms/mok.key /var/lib/dkms/mok.pub`.
5. Reboot when convenient: the BIOS re-applies its smart-fan setting (the exact pre-install state).

## Source vetting (2026-10-10)

| | Upstream | Base fork |
|---|---|---|
| Repo | [cmetz/ec-su_axb35-linux](https://github.com/cmetz/ec-su_axb35-linux) | [eschaubert/ec-su_axb35-linux](https://github.com/eschaubert/ec-su_axb35-linux) |
| Maintainer | Christoph Metz (cmetz) | Eugen Schaubert (eschaubert), single author |
| Stars / forks | 78 / 19, 6 contributors | 1 / 0, created 2026-10-04 |
| Last release | none (no releases) | none |
| Last commit | `f62c2c2` 2026-08-20 | `edc429d` 2026-10-05 (2 commits on upstream: hwmon merge + fix) |
| License | GPL-2.0 | GPL-2.0 |

Evidence:
- The [Strix Halo wiki fan-control guide](https://strixhalo.wiki/Guides/Sixunited_AXB35/Power_Mode_and_Fan_Control/)
  is built on this driver, Fedora steps included.
  Its [AXB35 board page](https://strixhalo.wiki/Hardware/Boards/Sixunited_AXB35/) lists the EVO-X2.
- Upstream [PR #33](https://github.com/cmetz/ec-su_axb35-linux/pull/33) (DKMS + power button, open)
  was tested on an EVO-X2.
- Downstream users: [laurentpayot/gnome-shell-extension-axb35-pmode](https://github.com/laurentpayot/gnome-shell-extension-axb35-pmode),
  [DAE51D/strix-halo-power](https://github.com/DAE51D/strix-halo-power). 24 GitHub repositories mention
  `ec_su_axb35` in their README.
- On the target (BIOS 1.05, EC 1.6), 2026-10-10:
  - The register map was checked against the decompiled DSDT: EC0 `PNP0C09` and the H2RAM field names.
  - A read-only EC RAM peek confirmed it: fan mode/level 0x21-0x26, fan3 RPM 0x28/0x29,
    temperature 0x70, RPM high byte first.
- eschaubert's two commits were reviewed line by line. They add a read-only hwmon device and no
  new EC writes.
- Every commit in this fork builds warning-free (`W=1`) against `kernel-devel-7.2.8-200.fc44`.
  The DKMS flow and the unit were also tested in a Fedora 44 container.

The driver is below the "widely adopted" bar for production sources (no releases, about 80 stars),
so using it needs the owner's explicit exception. That exception is recorded with the install.
