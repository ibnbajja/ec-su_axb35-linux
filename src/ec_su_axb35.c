// ec_su_axb35.c

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

extern int ec_read(u8 addr, u8 *val);
extern int ec_write(u8 addr, u8 val);

enum fan_mode { AUTO, FIXED, CURVE };

struct ec_fan {
    const char    *name;
    u8             speed_reg_high;
    u8             speed_reg_low;
    u8             mode_reg;
    u8             rampup_curve[6];
    u8             rampdown_curve[6];
    enum fan_mode  mode;
    struct device *dev;
};

struct ec_temp {
    const char    *name;
    u8             reg;
    u8             temp_min;
    u8             temp_max;
    struct device *dev;
};

struct ec_apu {
    const char    *name;
    u8             power_mode_reg;
    struct device *dev;
};

static struct class *ec_class;

static struct ec_fan ec_fans[] = {
    { .name           = "fan1",
      .speed_reg_high = 0x35,
      .speed_reg_low  = 0x36,
      .mode_reg       = 0x21,
      .rampup_curve   = { 0, 60, 70, 83, 95, 97 },
      .rampdown_curve = { 0, 40, 50, 80, 94, 96 } },
    { .name           = "fan2",
      .speed_reg_high = 0x37,
      .speed_reg_low  = 0x38,
      .mode_reg       = 0x23,
      .rampup_curve   = { 0, 60, 70, 83, 95, 97 },
      .rampdown_curve = { 0, 40, 50, 80, 94, 96 } },
    { .name           = "fan3",
      .speed_reg_high = 0x28,
      .speed_reg_low  = 0x29,
      .mode_reg       = 0x25,
      .rampup_curve   = { 0, 20, 60, 83, 95, 97 },
      .rampdown_curve = { 0, 0, 50, 80, 94, 96 } },
};

static struct ec_temp ec_temp = {
    .name = "temp1",
    .reg  = 0x70,
};

static struct ec_apu ec_apu = {
    .name           = "apu",
    .power_mode_reg = 0x31,
};

/* Serializes curve updates with each other and with the curve readers */
static DEFINE_MUTEX(ec_curve_lock);

/*
 * Load guard. The driver writes EC registers, so it only loads on a board
 * and EC firmware whose register map has been checked. Anything else gets
 * -ENODEV, unless the admin names it with the allow_board parameter.
 */
#define EC_REG_FW_MAJOR 0x00
#define EC_REG_FW_MINOR 0x01

struct ec_board {
    const char *sys_vendor;
    const char *product_name;
    const char *chassis_vendor;
    u8          ec_major;
    u8          ec_minor;
};

static const struct ec_board ec_known_boards[] = {
    /*
     * GMKtec EVO-X2 (Sixunited AXB35-02), BIOS 1.05, EC firmware 1.6: map
     * checked against the DSDT and a read-only EC RAM peek.
     */
    { "GMKtec", "NucBox_EVO-X2", "SU", 1, 6 },
};

static char *allow_board;
module_param(allow_board, charp, 0444);
MODULE_PARM_DESC(allow_board,
                 "Also load on this known-good board: "
                 "\"sys_vendor:product_name:chassis_vendor:EC_MAJOR.EC_MINOR\"");

static bool ec_board_matches(const struct ec_board *board, u8 ec_major,
                             u8 ec_minor)
{
    return dmi_match(DMI_SYS_VENDOR, board->sys_vendor) &&
           dmi_match(DMI_PRODUCT_NAME, board->product_name) &&
           dmi_match(DMI_CHASSIS_VENDOR, board->chassis_vendor) &&
           board->ec_major == ec_major && board->ec_minor == ec_minor;
}

/* Parses allow_board in place: "vendor:product:chassis_vendor:MAJOR.MINOR" */
static int ec_parse_board(char *s, struct ec_board *board)
{
    char *ver;
    char *major;

    board->sys_vendor     = strsep(&s, ":");
    board->product_name   = strsep(&s, ":");
    board->chassis_vendor = strsep(&s, ":");
    ver                   = strsep(&s, ":");
    if (!ver || s || !*board->sys_vendor || !*board->product_name ||
        !*board->chassis_vendor)
        return -EINVAL;

    major = strsep(&ver, ".");
    if (!ver || kstrtou8(major, 10, &board->ec_major) ||
        kstrtou8(ver, 10, &board->ec_minor))
        return -EINVAL;

    return 0;
}

static int ec_check_board(void)
{
    const char     *smbios_ec;
    char            ec_ver[8];
    struct ec_board extra;
    char           *buf;
    u8              major;
    u8              minor;
    int             ret;
    int             i;

    ret = ec_read(EC_REG_FW_MAJOR, &major);
    if (!ret)
        ret = ec_read(EC_REG_FW_MINOR, &minor);
    if (ret) {
        pr_err("ec_su_axb35: cannot read the EC firmware version (%d), "
               "refusing to load\n",
               ret);
        return -ENODEV;
    }
    snprintf(ec_ver, sizeof(ec_ver), "%u.%u", major, minor);

    /* SMBIOS type 0 also reports the EC release; when present it must agree */
    smbios_ec = dmi_get_system_info(DMI_EC_FIRMWARE_RELEASE);
    if (smbios_ec && strcmp(smbios_ec, ec_ver)) {
        pr_err("ec_su_axb35: EC RAM says firmware %s but SMBIOS says %s, "
               "refusing to load\n",
               ec_ver, smbios_ec);
        return -ENODEV;
    }

    for (i = 0; i < ARRAY_SIZE(ec_known_boards); i++)
        if (ec_board_matches(&ec_known_boards[i], major, minor))
            return 0;

    if (allow_board) {
        /* extra's strings point into buf, so match before freeing it */
        buf = kstrdup(allow_board, GFP_KERNEL);
        if (!buf)
            return -ENOMEM;

        ret = ec_parse_board(buf, &extra);
        if (!ret && ec_board_matches(&extra, major, minor)) {
            pr_warn("ec_su_axb35: board allowed by allow_board=\"%s\"\n",
                    allow_board);
            kfree(buf);
            return 0;
        }
        kfree(buf);
        if (ret) {
            pr_err("ec_su_axb35: cannot parse allow_board=\"%s\"\n",
                   allow_board);
            return ret;
        }
    }

    pr_err("ec_su_axb35: unsupported board \"%s\" \"%s\" chassis vendor "
           "\"%s\" EC firmware %s, refusing to load (see allow_board)\n",
           dmi_get_system_info(DMI_SYS_VENDOR) ?: "",
           dmi_get_system_info(DMI_PRODUCT_NAME) ?: "",
           dmi_get_system_info(DMI_CHASSIS_VENDOR) ?: "", ec_ver);
    return -ENODEV;
}

static int ec_fan_read_rpm(const struct ec_fan *fan, u16 *rpm)
{
    u8  hi;
    u8  lo;
    int ret;

    ret = ec_read(fan->speed_reg_high, &hi);
    if (ret)
        return ret;

    ret = ec_read(fan->speed_reg_low, &lo);
    if (ret)
        return ret;

    *rpm = (hi << 8) | lo;

    // wired fan3 behavior, displaying 8000 before turning to 0
    if (strcmp(fan->name, "fan3") == 0 && *rpm == 8000)
        *rpm = 0;

    return 0;
}

static ssize_t fan_rpm_show(struct device *dev, struct device_attribute *attr,
                            char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    u16            rpm;
    int            ret;

    ret = ec_fan_read_rpm(fan, &rpm);
    if (ret)
        return ret;

    return sprintf(buf, "%u\n", rpm);
}

static struct device_attribute dev_attr_fan_rpm =
    __ATTR(rpm, 0444, fan_rpm_show, NULL);

static int update_fan_mode(struct ec_fan *fan)
{
    u8  val;
    int ret;

    ret = ec_read(fan->mode_reg, &val);
    if (ret)
        return ret;

    switch (val) {
    case 0x10:
    case 0x20:
    case 0x30:
        fan->mode = AUTO;
        break;
    case 0x11:
    case 0x21:
    case 0x31:
        // curve and fixed use the value in the EC register
        // so fixed is only allowed if it was already know as
        // FIXED to the driver
        if (fan->mode != FIXED) {
            fan->mode = CURVE;
        } else {
            fan->mode = FIXED;
        }
        break;
    }

    return 0;
}

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr,
                             char *buf)
{
    struct ec_fan *fan  = dev_get_drvdata(dev);
    const char    *mode = "unknown";
    int            ret;

    ret = update_fan_mode(fan);
    if (ret)
        return ret;

    switch (fan->mode) {
    case AUTO:
        mode = "auto";
        break;
    case FIXED:
        mode = "fixed";
        break;
    case CURVE:
        mode = "curve";
        break;
    }

    return sprintf(buf, "%s\n", mode);
}

static int read_fan_level(struct ec_fan *fan, u8 *level)
{
    u8  val;
    int ret;

    ret = ec_read(fan->mode_reg + 1, &val);
    if (ret)
        return ret;

    switch (val & 0xF) {
    case 0x2: // 20%
        *level = 1;
        break;
    case 0x3: // 40%
        *level = 2;
        break;
    case 0x4: // 60%
        *level = 3;
        break;
    case 0x5: // 80%
        *level = 4;
        break;
    case 0x6: // 100%
        *level = 5;
        break;
    case 0x7:
    default: // off
        *level = 0;
    }

    return 0;
}

static ssize_t fan_level_show(struct device *dev, struct device_attribute *attr,
                              char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    u8             level;
    int            ret;

    ret = read_fan_level(fan, &level);
    if (ret)
        return ret;

    return sprintf(buf, "%u\n", level);
}

static int write_fan_level(struct ec_fan *fan, u8 level)
{
    u8 val;

    switch (fan->mode_reg) {
    case 0x21:
        val = 0x10;
        break;
    case 0x23:
        val = 0x20;
        break;
    case 0x25:
        val = 0x30;
        break;
    default:
        return -EINVAL;
    }

    switch (level) {
    case 0:
        val += 0x7;
        break;
    case 1:
        val += 0x2;
        break;
    case 2:
        val += 0x3;
        break;
    case 3:
        val += 0x4;
        break;
    case 4:
        val += 0x5;
        break;
    case 5:
    default:
        val += 0x6;
    }

    return ec_write(fan->mode_reg + 1, val);
}

static ssize_t fan_level_store(struct device           *dev,
                               struct device_attribute *attr, const char *buf,
                               size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    u8             val;
    int            ret;

    if (kstrtou8(buf, 10, &val) || val > 5)
        return -EINVAL;

    ret = write_fan_level(fan, val);
    if (ret)
        return ret;

    return count;
}

static struct device_attribute dev_attr_fan_level =
    __ATTR(level, 0644, fan_level_show, fan_level_store);

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
                              const char *buf, size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    enum fan_mode  mode;
    u8             val;
    int            ret;

    if (sysfs_streq(buf, "auto")) {
        mode = AUTO;
    } else if (sysfs_streq(buf, "fixed")) {
        mode = FIXED;
    } else if (sysfs_streq(buf, "curve")) {
        mode = CURVE;
    } else {
        return -EINVAL;
    }

    switch (mode) {
    case AUTO:
        switch (fan->mode_reg) {
        case 0x21:
            val = 0x10;
            break;
        case 0x23:
            val = 0x20;
            break;
        case 0x25:
            val = 0x30;
            break;
        default:
            return -EINVAL;
        }
        break;
    case FIXED:
    case CURVE:
    default:
        switch (fan->mode_reg) {
        case 0x21:
            val = 0x11;
            break;
        case 0x23:
            val = 0x21;
            break;
        case 0x25:
            val = 0x31;
            break;
        default:
            return -EINVAL;
        }
        break;
    }

    ret = ec_write(fan->mode_reg, val);
    if (ret)
        return ret;
    fan->mode = mode;

    // When switching to CURVE mode, set initial fan level based on current temperature
    // to prevent RPM burst from inappropriate starting level.
    // If the temperature cannot be read, the worker positions the fan instead.
    if (fan->mode == CURVE) {
        u8 temp;

        if (ec_read(ec_temp.reg, &temp) == 0) {
            u8  initial_level = 0;
            int i;

            // Find the appropriate level based on current temperature
            // Use rampup curve for initial positioning to be more responsive
            mutex_lock(&ec_curve_lock);
            for (i = 5; i > 0; i--) {
                if (temp >= fan->rampup_curve[i]) {
                    initial_level = i;
                    break;
                }
            }
            mutex_unlock(&ec_curve_lock);

            ret = write_fan_level(fan, initial_level);
            if (ret)
                return ret;
        }
    }

    return count;
}

static struct device_attribute dev_attr_fan_mode =
    __ATTR(mode, 0644, fan_mode_show, fan_mode_store);

/*
 * A curve pair is valid when the rampup thresholds strictly increase and
 * every rampdown threshold is below the rampup threshold of the same level,
 * so the worker can never step a fan up and straight back down at one
 * temperature.
 */
static bool fan_curves_valid(const u8 *rampup, const u8 *rampdown)
{
    int i;

    for (i = 1; i < 6; i++) {
        if (i > 1 && rampup[i] <= rampup[i - 1])
            return false;
        if (rampdown[i] >= rampup[i])
            return false;
    }
    return true;
}

static ssize_t fan_curve_show(u8 *curve, char *buf)
{
    int   i;
    char *p = buf;

    mutex_lock(&ec_curve_lock);
    for (i = 1; i < 6; i++) {
        p += sprintf(p, "%d", curve[i]);
        if (i < 5) {
            *p++ = ',';
        }
    }
    mutex_unlock(&ec_curve_lock);
    *p++ = '\n';
    *p   = '\0';

    return p - buf;
}

/*
 * Replaces one of the fan's two curves. The new curve is checked together
 * with the fan's other curve, so the order of writes matters: writing
 * rampdown "0,0,0,0,0" first always leaves room for any valid rampup.
 */
static ssize_t fan_curve_store(struct ec_fan *fan, bool is_rampup,
                               const char *buf, size_t count)
{
    char *orig = kstrndup(buf, count, GFP_KERNEL);
    char *str  = orig;
    char *token;
    int   values[5];
    u8    rampup[6];
    u8    rampdown[6];
    u8   *curve = is_rampup ? rampup : rampdown;
    int   i     = 0;
    int   ret   = 0;

    if (!orig)
        return -ENOMEM;

    token = strsep(&str, ",");
    while (token && i < 5) {
        if (kstrtoint(token, 10, &values[i]) < 0) {
            ret = -EINVAL;
            break;
        }
        if (values[i] < 0 || values[i] > 100) {
            ret = -EINVAL;
            break;
        }
        i++;
        token = strsep(&str, ",");
    }

    // exactly five values
    if (i != 5 || token)
        ret = -EINVAL;

    if (ret == 0) {
        mutex_lock(&ec_curve_lock);
        memcpy(rampup, fan->rampup_curve, sizeof(rampup));
        memcpy(rampdown, fan->rampdown_curve, sizeof(rampdown));
        for (i = 0; i < 5; i++) {
            curve[i + 1] = values[i];
        }
        if (fan_curves_valid(rampup, rampdown)) {
            memcpy(fan->rampup_curve, rampup, sizeof(rampup));
            memcpy(fan->rampdown_curve, rampdown, sizeof(rampdown));
        } else {
            ret = -EINVAL;
        }
        mutex_unlock(&ec_curve_lock);
    }

    kfree(orig);
    return ret ? ret : count;
}

static ssize_t fan_rampup_curve_show(struct device           *dev,
                                     struct device_attribute *attr, char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_show(fan->rampup_curve, buf);
}

static ssize_t fan_rampup_curve_store(struct device           *dev,
                                      struct device_attribute *attr,
                                      const char *buf, size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_store(fan, true, buf, count);
}

static struct device_attribute dev_attr_fan_rampup_curve =
    __ATTR(rampup_curve, 0644, fan_rampup_curve_show, fan_rampup_curve_store);

static ssize_t fan_rampdown_curve_show(struct device           *dev,
                                       struct device_attribute *attr, char *buf)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_show(fan->rampdown_curve, buf);
}

static ssize_t fan_rampdown_curve_store(struct device           *dev,
                                        struct device_attribute *attr,
                                        const char *buf, size_t count)
{
    struct ec_fan *fan = dev_get_drvdata(dev);
    return fan_curve_store(fan, false, buf, count);
}

static struct device_attribute dev_attr_fan_rampdown_curve = __ATTR(
    rampdown_curve, 0644, fan_rampdown_curve_show, fan_rampdown_curve_store);

static ssize_t temp_current_show(struct device           *dev,
                                 struct device_attribute *attr, char *buf)
{
    struct ec_temp *temp = dev_get_drvdata(dev);
    u8              val;
    int             ret;

    ret = ec_read(temp->reg, &val);
    if (ret)
        return ret;

    return sprintf(buf, "%u\n", val);
}

static struct device_attribute dev_attr_temp_cur =
    __ATTR(temp, 0444, temp_current_show, NULL);

static ssize_t temp_min_show(struct device *dev, struct device_attribute *attr,
                             char *buf)
{
    struct ec_temp *temp = dev_get_drvdata(dev);
    return sprintf(buf, "%u\n", temp->temp_min);
}

static struct device_attribute dev_attr_temp_min =
    __ATTR(min, 0444, temp_min_show, NULL);

static ssize_t temp_max_show(struct device *dev, struct device_attribute *attr,
                             char *buf)
{
    struct ec_temp *temp = dev_get_drvdata(dev);
    return sprintf(buf, "%u\n", temp->temp_max);
}

static struct device_attribute dev_attr_temp_max =
    __ATTR(max, 0444, temp_max_show, NULL);

static ssize_t apu_power_mode_show(struct device           *dev,
                                   struct device_attribute *attr, char *buf)
{
    struct ec_apu *apu  = dev_get_drvdata(dev);
    const char    *mode = "unknown";
    u8             val;
    int            ret;

    ret = ec_read(apu->power_mode_reg, &val);
    if (ret)
        return ret;

    switch (val) {
    case 0x00:
        mode = "balanced";
        break;
    case 0x01:
        mode = "performance";
        break;
    case 0x02:
        mode = "quiet";
        break;
    default:
        return -EINVAL;
    }

    return sprintf(buf, "%s\n", mode);
}

static ssize_t apu_power_mode_store(struct device           *dev,
                                    struct device_attribute *attr,
                                    const char *buf, size_t count)
{
    struct ec_apu *apu = dev_get_drvdata(dev);
    u8             val;
    int            ret;

    if (sysfs_streq(buf, "balanced")) {
        val = 0x00;
    } else if (sysfs_streq(buf, "performance")) {
        val = 0x01;
    } else if (sysfs_streq(buf, "quiet")) {
        val = 0x02;
    } else {
        return -EINVAL;
    }

    ret = ec_write(apu->power_mode_reg, val);
    if (ret)
        return ret;

    return count;
}

static struct device_attribute dev_attr_apu_power_mode =
    __ATTR(power_mode, 0644, apu_power_mode_show, apu_power_mode_store);

/*
 * hwmon interface.
 *
 * Exposes the same EC sensors through the standard hwmon API, so that they
 * show up in 'sensors' and any other hwmon consumer.
 */
static struct device *ec_hwmon_parent;
static struct device *ec_hwmon_dev;

static umode_t ec_hwmon_is_visible(const void *data,
                                   enum hwmon_sensor_types type, u32 attr,
                                   int channel)
{
    switch (type) {
    case hwmon_temp:
        if (attr == hwmon_temp_input)
            return 0444;
        break;
    case hwmon_fan:
        if (attr == hwmon_fan_input && channel < (int) ARRAY_SIZE(ec_fans))
            return 0444;
        break;
    default:
        break;
    }
    return 0;
}

static int ec_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
                         u32 attr, int channel, long *val)
{
    switch (type) {
    case hwmon_temp:
        if (attr == hwmon_temp_input) {
            u8  temp;
            int ret;

            ret = ec_read(ec_temp.reg, &temp);
            if (ret)
                return ret;

            *val = temp * 1000; // hwmon reports millidegrees
            return 0;
        }
        break;
    case hwmon_fan:
        if (attr == hwmon_fan_input && channel < (int) ARRAY_SIZE(ec_fans)) {
            u16 rpm;
            int ret;

            ret = ec_fan_read_rpm(&ec_fans[channel], &rpm);
            if (ret)
                return ret;

            *val = rpm;
            return 0;
        }
        break;
    default:
        break;
    }
    return -EOPNOTSUPP;
}

static const struct hwmon_ops ec_hwmon_ops = {
    .is_visible = ec_hwmon_is_visible,
    .read       = ec_hwmon_read,
};

static const u32 ec_hwmon_temp_config[] = {
    HWMON_T_INPUT,
    0,
};

static const u32 ec_hwmon_fan_config[] = {
    HWMON_F_INPUT,
    HWMON_F_INPUT,
    HWMON_F_INPUT,
    0,
};

static const struct hwmon_channel_info ec_hwmon_temp_info = {
    .type   = hwmon_temp,
    .config = ec_hwmon_temp_config,
};

static const struct hwmon_channel_info ec_hwmon_fan_info = {
    .type   = hwmon_fan,
    .config = ec_hwmon_fan_config,
};

static const struct hwmon_channel_info *ec_hwmon_info[] = {
    &ec_hwmon_temp_info,
    &ec_hwmon_fan_info,
    NULL,
};

static const struct hwmon_chip_info ec_hwmon_chip_info = {
    .ops  = &ec_hwmon_ops,
    .info = ec_hwmon_info,
};

static struct delayed_work ec_update_work;

static void ec_update_worker(struct work_struct *work)
{
    u8  temp;
    int i;
    int ret;

    ret = ec_read(ec_temp.reg, &temp);
    if (ret) {
        // Without a temperature the curve cannot be evaluated: keep the
        // current fan levels and try again on the next tick.
        pr_warn_ratelimited("ec_su_axb35: temperature read failed (%d), "
                            "fan levels unchanged\n",
                            ret);
        goto requeue;
    }

    // Update min/max
    if (ec_temp.temp_min == 0 || temp < ec_temp.temp_min)
        ec_temp.temp_min = temp;

    if (temp > ec_temp.temp_max)
        ec_temp.temp_max = temp;

    // update fan level if curve mode is active
    mutex_lock(&ec_curve_lock);
    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        struct ec_fan *fan = &ec_fans[i];

        if (fan->mode == CURVE) {
            u8 level;

            if (read_fan_level(fan, &level))
                continue;

            ret = 0;

            if (level < 5 && temp >= fan->rampup_curve[level + 1]) {
                ret = write_fan_level(fan, level + 1);
            } else if (level > 0 && temp <= fan->rampdown_curve[level]) {
                ret = write_fan_level(fan, level - 1);
            }
            if (ret)
                pr_warn_ratelimited("ec_su_axb35: %s level write failed (%d)\n",
                                    fan->name, ret);
        }
    }
    mutex_unlock(&ec_curve_lock);

requeue:
    // Requeue the work
    schedule_delayed_work(&ec_update_work,
                          msecs_to_jiffies(1000)); // every 1 sec
}

static dev_t ec_su_axb35_dev;

/* Minor numbers of the devices created in ec_class */
#define EC_MINOR_TEMP  (ARRAY_SIZE(ec_fans))
#define EC_MINOR_APU   (ARRAY_SIZE(ec_fans) + 1)
#define EC_MINOR_HWMON (ARRAY_SIZE(ec_fans) + 2)
#define EC_NUM_DEVICES (ARRAY_SIZE(ec_fans) + 3)

static int __init ec_su_axb35_init(void)
{
    int i;
    int ret;

    BUILD_BUG_ON(ARRAY_SIZE(ec_hwmon_fan_config) != ARRAY_SIZE(ec_fans) + 1);

    ret = ec_check_board();
    if (ret)
        return ret;

    ret = alloc_chrdev_region(&ec_su_axb35_dev, 0, EC_NUM_DEVICES,
                              "ec_su_axb35");
    if (ret < 0) {
        pr_err("ec_su_axb35: Failed to allocation major number\n");
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    ec_class = class_create("ec_su_axb35");
#else
    ec_class = class_create(THIS_MODULE, "ec_su_axb35");
#endif

    if (IS_ERR(ec_class)) {
        unregister_chrdev_region(ec_su_axb35_dev, EC_NUM_DEVICES);
        return PTR_ERR(ec_class);
    }

    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        struct ec_fan *fan = &ec_fans[i];

        fan->dev = device_create(
            ec_class, NULL, MKDEV(MAJOR(ec_su_axb35_dev), i), fan, fan->name);
        if (IS_ERR(fan->dev))
            continue;

        dev_set_drvdata(fan->dev, fan);
        device_create_file(fan->dev, &dev_attr_fan_rpm);
        device_create_file(fan->dev, &dev_attr_fan_mode);
        device_create_file(fan->dev, &dev_attr_fan_level);
        device_create_file(fan->dev, &dev_attr_fan_rampup_curve);
        device_create_file(fan->dev, &dev_attr_fan_rampdown_curve);
        update_fan_mode(fan);
    }

    ec_temp.dev = device_create(
        ec_class, NULL, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_TEMP), &ec_temp,
        ec_temp.name);
    if (!IS_ERR(ec_temp.dev)) {
        dev_set_drvdata(ec_temp.dev, &ec_temp);
        device_create_file(ec_temp.dev, &dev_attr_temp_cur);
        device_create_file(ec_temp.dev, &dev_attr_temp_min);
        device_create_file(ec_temp.dev, &dev_attr_temp_max);
    }

    ec_apu.dev = device_create(ec_class, NULL,
                               MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_APU),
                               &ec_apu, ec_apu.name);
    if (!IS_ERR(ec_apu.dev)) {
        dev_set_drvdata(ec_apu.dev, &ec_apu);
        device_create_file(ec_apu.dev, &dev_attr_apu_power_mode);
    }

    /*
     * The hwmon core requires a parent device, so create a plain class
     * device to hang the hwmon device off.
     */
    ec_hwmon_parent = device_create(ec_class, NULL,
                                    MKDEV(MAJOR(ec_su_axb35_dev),
                                          EC_MINOR_HWMON),
                                    NULL, "ec_hwmon");
    if (IS_ERR(ec_hwmon_parent)) {
        ec_hwmon_parent = NULL;
    } else {
        ec_hwmon_dev = hwmon_device_register_with_info(
            ec_hwmon_parent, "ec_su_axb35", NULL, &ec_hwmon_chip_info, NULL);
        if (IS_ERR(ec_hwmon_dev)) {
            pr_warn("ec_su_axb35: Failed to register hwmon device: %ld\n",
                    PTR_ERR(ec_hwmon_dev));
            ec_hwmon_dev = NULL;
        }
    }

    INIT_DELAYED_WORK(&ec_update_work, ec_update_worker);
    schedule_delayed_work(&ec_update_work, msecs_to_jiffies(1000));

    pr_info("ec_su_axb35: Sixunited AXB35-02 EC driver loaded\n");
    return 0;
}

static void __exit ec_su_axb35_exit(void)
{
    int i;

    if (ec_hwmon_dev) {
        hwmon_device_unregister(ec_hwmon_dev);
        ec_hwmon_dev = NULL;
    }
    if (ec_hwmon_parent) {
        device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_HWMON));
        ec_hwmon_parent = NULL;
    }

    /* Reset all fans to AUTO mode before unloading */
    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        struct ec_fan *fan = &ec_fans[i];
        u8 val;

        fan->mode = AUTO;

        switch (fan->mode_reg) {
        case 0x21: // Fan 1
            val = 0x10;
            break;
        case 0x23: // Fan 2
            val = 0x20;
            break;
        case 0x25: // Fan 3
            val = 0x30;
            break;
        default:
            continue;
        }
        if (ec_write(fan->mode_reg, val))
            pr_err("ec_su_axb35: %s could not be reset to AUTO mode\n",
                   fan->name);
        else
            pr_info("ec_su_axb35: %s reset to AUTO mode\n", fan->name);
    }

    for (i = 0; i < ARRAY_SIZE(ec_fans); i++) {
        if (!IS_ERR(ec_fans[i].dev)) {
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_rpm);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_mode);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_level);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_rampup_curve);
            device_remove_file(ec_fans[i].dev, &dev_attr_fan_rampdown_curve);
            device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), i));
        }
    }

    if (!IS_ERR(ec_temp.dev)) {
        device_remove_file(ec_temp.dev, &dev_attr_temp_cur);
        device_remove_file(ec_temp.dev, &dev_attr_temp_min);
        device_remove_file(ec_temp.dev, &dev_attr_temp_max);
        device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_TEMP));
    }

    if (!IS_ERR(ec_apu.dev)) {
        device_remove_file(ec_apu.dev, &dev_attr_apu_power_mode);
        device_destroy(ec_class, MKDEV(MAJOR(ec_su_axb35_dev), EC_MINOR_APU));
    }

    cancel_delayed_work_sync(&ec_update_work);

    class_destroy(ec_class);
    unregister_chrdev_region(ec_su_axb35_dev, EC_NUM_DEVICES);
    pr_info("ec_su_axb35: Module unloaded\n");
}

module_init(ec_su_axb35_init);
module_exit(ec_su_axb35_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("loom@mopper.de");
MODULE_DESCRIPTION("Sixunited AXB35-02 Embedded Controller driver");
