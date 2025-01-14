// SPDX-License-Identifier: GPL-2.0+
/*
 * hwmon driver for NZXT Kraken X53/X63/X73 and Z53/Z63/Z73 all in one coolers.
 * X53 and Z53 in code refer to all models in their respective series (shortened
 * for brevity).
 *
 * Copyright 2021  Jonas Malaco <jonas@protocubo.io>
 * Copyright 2022  Aleksa Savic <savicaleksa83@gmail.com>
 */

#include <linux/debugfs.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <asm/unaligned.h>

#define USB_VENDOR_ID_NZXT		0x1e71
#define USB_PRODUCT_ID_X53		0x2007
#define USB_PRODUCT_ID_X53_SECOND	0x2014
#define USB_PRODUCT_ID_Z53		0x3008

enum kinds { X53, Z53 } __packed;
enum pwm_enable { off, manual, curve } __packed;

static const char *const kraken3_device_names[] = {
	[X53] = "x53",
	[Z53] = "z53",
};

#define DRIVER_NAME		"nzxt_kraken3"
#define STATUS_REPORT_ID	0x75
#define FIRMWARE_REPORT_ID	0x11
#define STATUS_VALIDITY		2000	/* In ms, equivalent to period of four status reports */
#define CUSTOM_CURVE_POINTS	40	/* For temps from 20C to 59C (critical temp) */
#define PUMP_DUTY_MIN		20	/* In percent */

/* Sensor report offsets for Kraken X53 and Z53 */
#define TEMP_SENSOR_START_OFFSET	15
#define TEMP_SENSOR_END_OFFSET		16
#define PUMP_SPEED_OFFSET		17
#define PUMP_DUTY_OFFSET		19

/* Firmware version report offset for Kraken X53 and Z53 */
#define FIRMWARE_VERSION_OFFSET		17

/* Sensor report offsets for Kraken Z53 */
#define Z53_FAN_SPEED_OFFSET		23
#define Z53_FAN_DUTY_OFFSET		25

/* Report offsets for control commands for Kraken X53 and Z53 */
#define SET_DUTY_ID_OFFSET		1

/* Control commands and their lengths for Kraken X53 and Z53 */

/* Last byte sets the report interval at 0.5s */
static const u8 set_interval_cmd[] = { 0x70, 0x02, 0x01, 0xB8, 1 };
static const u8 finish_init_cmd[] = { 0x70, 0x01 };
static const u8 __maybe_unused get_fw_version_cmd[] = { 0x10, 0x01 };
static const u8 set_pump_duty_cmd_header[] = { 0x72, 0x00, 0x00, 0x00 };
static const u8 z53_get_status_cmd[] = { 0x74, 0x01 };

#define SET_INTERVAL_CMD_LENGTH			5
#define FINISH_INIT_CMD_LENGTH			2
#define GET_FW_VERSION_CMD_LENGTH		2
#define MAX_REPORT_LENGTH			64
#define MIN_REPORT_LENGTH			20
#define SET_CURVE_DUTY_CMD_HEADER_LENGTH	4
/* 4 byte header and 40 duty offsets */
#define SET_CURVE_DUTY_CMD_LENGTH		(4 + 40)
#define Z53_GET_STATUS_CMD_LENGTH		2

static const char *const kraken3_temp_label[] = {
	"Coolant temp",
};

static const char *const kraken3_fan_label[] = {
	"Pump speed",
	"Fan speed"
};

struct kraken3_channel_info {
	enum pwm_enable mode;

	/* Both values are PWM */
	u16 reported_duty;
	u16 fixed_duty;		/* Manually set fixed duty */

	u8 pwm_points[CUSTOM_CURVE_POINTS];
};

struct kraken3_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct dentry *debugfs;
	struct mutex buffer_lock;	/* For locking access to buffer */
	/* Used to ensure that only one reader requests a Z53 status report */
	struct mutex z53_status_request_lock;
	/*
	 * For retaining returned error code in case of failure with
	 * multiple readers present during status report refresh.
	 */
	int z53_status_request_ret;
	/* For delaying multiple readers until a Z53 status report is received */
	wait_queue_head_t z53_reader_wq;
	bool z53_status_processed;
	struct completion fw_version_processed;
	struct completion status_report_processed;

	u8 *buffer;
	struct kraken3_channel_info channel_info[2];	/* Pump and fan */

	/* Sensor values */
	s32 temp_input[1];
	u16 fan_input[2];

	enum kinds kind;
	u8 firmware_version[3];

	unsigned long updated;	/* jiffies */
};

static umode_t kraken3_is_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				  int channel)
{
	const struct kraken3_data *priv = data;

	switch (type) {
	case hwmon_temp:
		if (channel < 1)
			return 0444;
		break;
	case hwmon_fan:
		switch (priv->kind) {
		case X53:
			/* Just the pump */
			if (channel < 1)
				return 0444;
			break;
		case Z53:
			/* Pump and fan */
			if (channel < 2)
				return 0444;
			break;
		default:
			break;
		}
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
		case hwmon_pwm_input:
			switch (priv->kind) {
			case X53:
				/* Just the pump */
				if (channel < 1)
					return 0644;
				break;
			case Z53:
				/* Pump and fan */
				if (channel < 2)
					return 0644;
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}

	return 0;
}

/*
 * Writes the command to the device with the rest of the report (up to 64 bytes) filled
 * with zeroes.
 */
static int kraken3_write_expanded(struct kraken3_data *priv, const u8 *cmd, int cmd_length)
{
	int ret;

	mutex_lock(&priv->buffer_lock);

	memset(priv->buffer, 0x00, MAX_REPORT_LENGTH);
	memcpy(priv->buffer, cmd, cmd_length);
	ret = hid_hw_output_report(priv->hdev, priv->buffer, MAX_REPORT_LENGTH);

	mutex_unlock(&priv->buffer_lock);
	return ret;
}

static int kraken3_percent_to_pwm(long val)
{
	return DIV_ROUND_CLOSEST(val * 255, 100);
}

static int kraken3_pwm_to_percent(long val, int channel)
{
	int percent_value;

	if (val < 0 || val > 255)
		return -EINVAL;

	percent_value = DIV_ROUND_CLOSEST(val * 100, 255);

	/* Bring up pump duty to min value if needed */
	if (channel == 0 && percent_value < PUMP_DUTY_MIN)
		percent_value = PUMP_DUTY_MIN;

	return percent_value;
}

static int kraken3_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			long *val)
{
	int ret;
	ulong remaining_time;	/* Used for priv->status_report_processed completion with timeout */
	struct kraken3_data *priv = dev_get_drvdata(dev);

	if (time_after(jiffies, priv->updated + msecs_to_jiffies(STATUS_VALIDITY))) {
		if (priv->kind == X53 && !completion_done(&priv->status_report_processed)) {
			/*
			 * fancontrol exits if it reads an error code, so instead of returning one
			 * if we don't yet have a report, wait for the first report to be parsed
			 * (but up to STATUS_VALIDITY seconds). This does not concern the Z series
			 * devices, because they send a sensor report only when requested.
			 */
			if (wait_for_completion_interruptible_timeout
			    (&priv->status_report_processed,
			     msecs_to_jiffies(STATUS_VALIDITY)) <= 0)
				return -ENODATA;
		} else if (priv->kind == Z53) {
			/* Ensure that only one of the readers requests status */
			if (mutex_trylock(&priv->z53_status_request_lock)) {
				/* Reset ret value for other readers */
				priv->z53_status_request_ret = 0;
				priv->z53_status_processed = false;

				/* Send command for getting status */
				ret = kraken3_write_expanded(priv, z53_get_status_cmd,
							     Z53_GET_STATUS_CMD_LENGTH);
				if (ret < 0) {
					priv->z53_status_request_ret = ret;
					goto signal_other_readers;
				}

				/* Wait for completion from kraken3_raw_event() */
				remaining_time =
				    wait_for_completion_interruptible_timeout
					(&priv->status_report_processed,
					 msecs_to_jiffies(STATUS_VALIDITY));

				/* Didn't receive status report in time or was interrupted? */
				if (remaining_time == 0 || remaining_time == -ERESTARTSYS) {
					ret = -ENODATA;
					priv->z53_status_request_ret = ret;
				}
signal_other_readers:
				/* Let other readers proceed */
				priv->z53_status_processed = true;
				wake_up_interruptible_all(&priv->z53_reader_wq);

				mutex_unlock(&priv->z53_status_request_lock);
				if (ret < 0)
					return ret;
			} else {
				/* Wait for the main reader with a timeout of 2 * STATUS_VALIDITY */
				if (wait_event_interruptible_timeout
				    (priv->z53_reader_wq, priv->z53_status_processed,
				     msecs_to_jiffies(2 * STATUS_VALIDITY)) <= 0)
					return -ENODATA;

				/* Return saved error code if reader failed in some way */
				if (priv->z53_status_request_ret < 0)
					return priv->z53_status_request_ret;
			}
		} else {
			return -ENODATA;
		}
	}

	switch (type) {
	case hwmon_temp:
		*val = priv->temp_input[channel];
		break;
	case hwmon_fan:
		*val = priv->fan_input[channel];
		break;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_enable:
			*val = priv->channel_info[channel].mode;
			break;
		case hwmon_pwm_input:
			*val = priv->channel_info[channel].reported_duty;
			break;
		default:
			break;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int kraken3_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			       int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = kraken3_temp_label[channel];
		break;
	case hwmon_fan:
		*str = kraken3_fan_label[channel];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/* Writes custom curve to device */
static int kraken3_write_curve(struct kraken3_data *priv, u8 *curve_array, int channel)
{
	int ret;
	u8 fixed_duty_cmd[SET_CURVE_DUTY_CMD_LENGTH];

	/* Copy command header */
	memcpy(fixed_duty_cmd, set_pump_duty_cmd_header, SET_CURVE_DUTY_CMD_HEADER_LENGTH);

	/* Set the correct ID for writing pump/fan duty (0x01 or 0x02, respectively) */
	fixed_duty_cmd[SET_DUTY_ID_OFFSET] = channel + 1;

	/* Copy curve to command */
	memcpy(fixed_duty_cmd + SET_CURVE_DUTY_CMD_HEADER_LENGTH, curve_array, CUSTOM_CURVE_POINTS);

	ret = kraken3_write_expanded(priv, fixed_duty_cmd, SET_CURVE_DUTY_CMD_LENGTH);
	return ret;
}

static int kraken3_write_fixed_duty(struct kraken3_data *priv, long val, int channel)
{
	int ret, percent_val, i;
	u8 fixed_curve_points[CUSTOM_CURVE_POINTS];

	percent_val = kraken3_pwm_to_percent(val, channel);
	if (percent_val < 0)
		return percent_val;

	/*
	 * The devices can only control the duty through a curve.
	 * Since we're setting a fixed duty here, fill the whole curve
	 * (ranging from 20C to 59C) with the same duty, except for
	 * the last point, the critical temperature, where it's maxed
	 * out for safety.
	 */

	/* Fill the custom curve with the fixed value we're setting */
	for (i = 0; i < CUSTOM_CURVE_POINTS - 1; i++)
		fixed_curve_points[i] = percent_val;

	/* Force duty to 100% at critical temp */
	fixed_curve_points[CUSTOM_CURVE_POINTS - 1] = 100;

	/* Write the fixed duty curve to the device */
	ret = kraken3_write_curve(priv, fixed_curve_points, channel);
	return ret;
}

static int kraken3_write(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			 long val)
{
	int ret;
	struct kraken3_data *priv = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			/* Remember the last set fixed duty for channel */
			priv->channel_info[channel].fixed_duty = val;

			if (priv->channel_info[channel].mode == manual) {
				ret = kraken3_write_fixed_duty(priv, val, channel);
				if (ret < 0)
					return ret;

				/*
				 * Lock onto this value and report it until next interrupt status
				 * report is received, so userspace tools can continue to work.
				 */
				priv->channel_info[channel].reported_duty = val;
			}
			break;
		case hwmon_pwm_enable:
			if (val < 0 || val > 2)
				return -EINVAL;

			switch (val) {
			case 0:
				/* Set channel to 100%, direct duty value */
				ret = kraken3_write_fixed_duty(priv, 255, channel);
				if (ret < 0)
					return ret;

				/* We don't control anything anymore */
				priv->channel_info[channel].mode = off;
				break;
			case 1:
				/* Apply the last known direct duty value */
				ret =
				    kraken3_write_fixed_duty(priv,
							     priv->channel_info[channel].fixed_duty,
							     channel);
				if (ret < 0)
					return ret;

				priv->channel_info[channel].mode = manual;
				break;
			case 2:
				/* Apply the curve and note as enabled */
				ret =
				    kraken3_write_curve(priv,
							priv->channel_info[channel].pwm_points,
							channel);
				if (ret < 0)
					return ret;

				priv->channel_info[channel].mode = curve;
				break;
			default:
				break;
			}
			break;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static ssize_t kraken3_fan_curve_pwm_store(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int ret;
	long val;
	struct sensor_device_attribute_2 *dev_attr = to_sensor_dev_attr_2(attr);
	struct kraken3_data *priv = dev_get_drvdata(dev);

	if (kstrtol(buf, 10, &val) < 0)
		return -EINVAL;

	val = kraken3_pwm_to_percent(val, dev_attr->nr);
	if (val < 0)
		return val;

	priv->channel_info[dev_attr->nr].pwm_points[dev_attr->index] = val;

	if (priv->channel_info[dev_attr->nr].mode == curve) {
		/* Apply the curve */
		ret =
		    kraken3_write_curve(priv,
					priv->channel_info[dev_attr->nr].pwm_points, dev_attr->nr);
		if (ret < 0)
			return ret;
	}

	return count;
}

static umode_t kraken3_curve_props_are_visible(struct kobject *kobj, struct attribute *attr,
					       int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct kraken3_data *priv = dev_get_drvdata(dev);

	/* Only Z53 has the fan curve */
	if (index >= CUSTOM_CURVE_POINTS && priv->kind != Z53)
		return 0;

	return attr->mode;
}

/* Custom pump curve from 20C to 59C (critical temp) */
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point1_pwm, kraken3_fan_curve_pwm, 0, 0);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point2_pwm, kraken3_fan_curve_pwm, 0, 1);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point3_pwm, kraken3_fan_curve_pwm, 0, 2);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point4_pwm, kraken3_fan_curve_pwm, 0, 3);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point5_pwm, kraken3_fan_curve_pwm, 0, 4);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point6_pwm, kraken3_fan_curve_pwm, 0, 5);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point7_pwm, kraken3_fan_curve_pwm, 0, 6);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point8_pwm, kraken3_fan_curve_pwm, 0, 7);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point9_pwm, kraken3_fan_curve_pwm, 0, 8);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point10_pwm, kraken3_fan_curve_pwm, 0, 9);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point11_pwm, kraken3_fan_curve_pwm, 0, 10);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point12_pwm, kraken3_fan_curve_pwm, 0, 11);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point13_pwm, kraken3_fan_curve_pwm, 0, 12);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point14_pwm, kraken3_fan_curve_pwm, 0, 13);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point15_pwm, kraken3_fan_curve_pwm, 0, 14);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point16_pwm, kraken3_fan_curve_pwm, 0, 15);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point17_pwm, kraken3_fan_curve_pwm, 0, 16);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point18_pwm, kraken3_fan_curve_pwm, 0, 17);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point19_pwm, kraken3_fan_curve_pwm, 0, 18);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point20_pwm, kraken3_fan_curve_pwm, 0, 19);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point21_pwm, kraken3_fan_curve_pwm, 0, 20);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point22_pwm, kraken3_fan_curve_pwm, 0, 21);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point23_pwm, kraken3_fan_curve_pwm, 0, 22);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point24_pwm, kraken3_fan_curve_pwm, 0, 23);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point25_pwm, kraken3_fan_curve_pwm, 0, 24);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point26_pwm, kraken3_fan_curve_pwm, 0, 25);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point27_pwm, kraken3_fan_curve_pwm, 0, 26);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point28_pwm, kraken3_fan_curve_pwm, 0, 27);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point29_pwm, kraken3_fan_curve_pwm, 0, 28);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point30_pwm, kraken3_fan_curve_pwm, 0, 29);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point31_pwm, kraken3_fan_curve_pwm, 0, 30);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point32_pwm, kraken3_fan_curve_pwm, 0, 31);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point33_pwm, kraken3_fan_curve_pwm, 0, 32);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point34_pwm, kraken3_fan_curve_pwm, 0, 33);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point35_pwm, kraken3_fan_curve_pwm, 0, 34);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point36_pwm, kraken3_fan_curve_pwm, 0, 35);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point37_pwm, kraken3_fan_curve_pwm, 0, 36);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point38_pwm, kraken3_fan_curve_pwm, 0, 37);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point39_pwm, kraken3_fan_curve_pwm, 0, 38);
static SENSOR_DEVICE_ATTR_2_WO(temp1_auto_point40_pwm, kraken3_fan_curve_pwm, 0, 39);

/* Custom fan curve from 20C to 59C (critical temp) */
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point1_pwm, kraken3_fan_curve_pwm, 1, 0);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point2_pwm, kraken3_fan_curve_pwm, 1, 1);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point3_pwm, kraken3_fan_curve_pwm, 1, 2);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point4_pwm, kraken3_fan_curve_pwm, 1, 3);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point5_pwm, kraken3_fan_curve_pwm, 1, 4);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point6_pwm, kraken3_fan_curve_pwm, 1, 5);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point7_pwm, kraken3_fan_curve_pwm, 1, 6);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point8_pwm, kraken3_fan_curve_pwm, 1, 7);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point9_pwm, kraken3_fan_curve_pwm, 1, 8);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point10_pwm, kraken3_fan_curve_pwm, 1, 9);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point11_pwm, kraken3_fan_curve_pwm, 1, 10);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point12_pwm, kraken3_fan_curve_pwm, 1, 11);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point13_pwm, kraken3_fan_curve_pwm, 1, 12);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point14_pwm, kraken3_fan_curve_pwm, 1, 13);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point15_pwm, kraken3_fan_curve_pwm, 1, 14);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point16_pwm, kraken3_fan_curve_pwm, 1, 15);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point17_pwm, kraken3_fan_curve_pwm, 1, 16);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point18_pwm, kraken3_fan_curve_pwm, 1, 17);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point19_pwm, kraken3_fan_curve_pwm, 1, 18);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point20_pwm, kraken3_fan_curve_pwm, 1, 19);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point21_pwm, kraken3_fan_curve_pwm, 1, 20);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point22_pwm, kraken3_fan_curve_pwm, 1, 21);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point23_pwm, kraken3_fan_curve_pwm, 1, 22);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point24_pwm, kraken3_fan_curve_pwm, 1, 23);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point25_pwm, kraken3_fan_curve_pwm, 1, 24);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point26_pwm, kraken3_fan_curve_pwm, 1, 25);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point27_pwm, kraken3_fan_curve_pwm, 1, 26);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point28_pwm, kraken3_fan_curve_pwm, 1, 27);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point29_pwm, kraken3_fan_curve_pwm, 1, 28);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point30_pwm, kraken3_fan_curve_pwm, 1, 29);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point31_pwm, kraken3_fan_curve_pwm, 1, 30);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point32_pwm, kraken3_fan_curve_pwm, 1, 31);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point33_pwm, kraken3_fan_curve_pwm, 1, 32);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point34_pwm, kraken3_fan_curve_pwm, 1, 33);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point35_pwm, kraken3_fan_curve_pwm, 1, 34);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point36_pwm, kraken3_fan_curve_pwm, 1, 35);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point37_pwm, kraken3_fan_curve_pwm, 1, 36);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point38_pwm, kraken3_fan_curve_pwm, 1, 37);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point39_pwm, kraken3_fan_curve_pwm, 1, 38);
static SENSOR_DEVICE_ATTR_2_WO(temp2_auto_point40_pwm, kraken3_fan_curve_pwm, 1, 39);

static struct attribute *kraken3_curve_attrs[] = {
	/* Pump control curve */
	&sensor_dev_attr_temp1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point9_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point10_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point11_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point12_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point13_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point14_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point15_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point16_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point17_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point18_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point19_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point20_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point21_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point22_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point23_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point24_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point25_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point26_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point27_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point28_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point29_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point30_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point31_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point32_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point33_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point34_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point35_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point36_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point37_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point38_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point39_pwm.dev_attr.attr,
	&sensor_dev_attr_temp1_auto_point40_pwm.dev_attr.attr,
	/* Fan control curve (Z53 only) */
	&sensor_dev_attr_temp2_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point9_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point10_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point11_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point12_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point13_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point14_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point15_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point16_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point17_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point18_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point19_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point20_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point21_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point22_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point23_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point24_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point25_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point26_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point27_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point28_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point29_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point30_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point31_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point32_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point33_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point34_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point35_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point36_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point37_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point38_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point39_pwm.dev_attr.attr,
	&sensor_dev_attr_temp2_auto_point40_pwm.dev_attr.attr,
	NULL
};

static const struct attribute_group kraken3_curves_group = {
	.attrs = kraken3_curve_attrs,
	.is_visible = kraken3_curve_props_are_visible
};

static const struct attribute_group *kraken3_groups[] = {
	&kraken3_curves_group,
	NULL
};

static const struct hwmon_ops kraken3_hwmon_ops = {
	.is_visible = kraken3_is_visible,
	.read = kraken3_read,
	.read_string = kraken3_read_string,
	.write = kraken3_write
};

static const struct hwmon_channel_info *kraken3_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info kraken3_chip_info = {
	.ops = &kraken3_hwmon_ops,
	.info = kraken3_info,
};

static int kraken3_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data,
			     int size)
{
	int i;
	struct kraken3_data *priv = hid_get_drvdata(hdev);

	if (size < MIN_REPORT_LENGTH)
		return 0;

	if (report->id == FIRMWARE_REPORT_ID) {
		/* Read firmware version */
		for (i = 0; i < 3; i++)
			priv->firmware_version[i] = data[FIRMWARE_VERSION_OFFSET + i];
		complete(&priv->fw_version_processed);
		return 0;
	}

	if (report->id != STATUS_REPORT_ID)
		return 0;

	if (data[TEMP_SENSOR_START_OFFSET] == 0xff && data[TEMP_SENSOR_END_OFFSET] == 0xff) {
		hid_err_once(hdev, "firmware or device is possibly damaged, not parsing reports\n");

		/*
		 * Mark first X-series device report as received,
		 * as well as all for Z-series, if faulty.
		 */
		if (priv->kind != X53 || !completion_done(&priv->status_report_processed))
			complete_all(&priv->status_report_processed);

		return 0;
	}

	/* Temperature and fan sensor readings */
	priv->temp_input[0] =
	    data[TEMP_SENSOR_START_OFFSET] * 1000 + data[TEMP_SENSOR_END_OFFSET] * 100;

	priv->fan_input[0] = get_unaligned_le16(data + PUMP_SPEED_OFFSET);
	priv->channel_info[0].reported_duty = kraken3_percent_to_pwm(data[PUMP_DUTY_OFFSET]);

	/* Mark first X-series device report as received */
	if (priv->kind == X53 && !completion_done(&priv->status_report_processed))
		complete_all(&priv->status_report_processed);

	/* Additional readings for Z53 */
	if (priv->kind == Z53) {
		priv->fan_input[1] = get_unaligned_le16(data + Z53_FAN_SPEED_OFFSET);
		priv->channel_info[1].reported_duty =
		    kraken3_percent_to_pwm(data[Z53_FAN_DUTY_OFFSET]);

		complete(&priv->status_report_processed);
	}

	priv->updated = jiffies;

	return 0;
}

static int kraken3_init_device(struct hid_device *hdev)
{
	int ret;
	struct kraken3_data *priv = hid_get_drvdata(hdev);

	/* Set the polling interval */
	ret = kraken3_write_expanded(priv, set_interval_cmd, SET_INTERVAL_CMD_LENGTH);
	if (ret < 0)
		return ret;

	/* Finalize the init process */
	ret = kraken3_write_expanded(priv, finish_init_cmd, FINISH_INIT_CMD_LENGTH);
	if (ret < 0)
		return ret;

	return 0;
}

static int __maybe_unused kraken3_reset_resume(struct hid_device *hdev)
{
	int ret;

	ret = kraken3_init_device(hdev);
	if (ret)
		hid_err(hdev, "req init (reset_resume) failed with %d\n", ret);

	return ret;
}

#ifdef CONFIG_DEBUG_FS

static int firmware_version_show(struct seq_file *seqf, void *unused)
{
	int ret;
	struct kraken3_data *priv = seqf->private;

	ret = kraken3_write_expanded(priv, get_fw_version_cmd, GET_FW_VERSION_CMD_LENGTH);
	if (ret < 0)
		return -ENODATA;

	/*
	 * The response to this request that the device sends is only catchable in
	 * kraken3_raw_event(), so we have to wait until it's processed there.
	 */
	if (wait_for_completion_interruptible(&priv->fw_version_processed))
		return -ENODATA;

	seq_printf(seqf, "%u.%u.%u\n", priv->firmware_version[0], priv->firmware_version[1],
		   priv->firmware_version[2]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(firmware_version);

static void kraken3_debugfs_init(struct kraken3_data *priv)
{
	char name[64];

	scnprintf(name, sizeof(name), "%s_%s-%s", DRIVER_NAME, kraken3_device_names[priv->kind],
		  dev_name(&priv->hdev->dev));

	priv->debugfs = debugfs_create_dir(name, NULL);
	debugfs_create_file("firmware_version", 0444, priv->debugfs, priv, &firmware_version_fops);
}

#else

static void kraken3_debugfs_init(struct kraken3_data *priv)
{
}

#endif

static int kraken3_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct kraken3_data *priv;
	int ret;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);

	/*
	 * Initialize ->updated to STATUS_VALIDITY seconds in the past, making
	 * the initial empty data invalid for kraken3_read without the need for
	 * a special case there.
	 */
	priv->updated = jiffies - msecs_to_jiffies(STATUS_VALIDITY);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parse failed with %d\n", ret);
		return ret;
	}

	/* Enable hidraw so existing user-space tools can continue to work */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid hw start failed with %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid hw open failed with %d\n", ret);
		goto fail_and_stop;
	}

	switch (hdev->product) {
	case USB_PRODUCT_ID_X53:
	case USB_PRODUCT_ID_X53_SECOND:
		priv->kind = X53;
		break;
	case USB_PRODUCT_ID_Z53:
		priv->kind = Z53;
		break;
	default:
		break;
	}

	priv->buffer = devm_kzalloc(&hdev->dev, MAX_REPORT_LENGTH, GFP_KERNEL);
	if (!priv->buffer) {
		ret = -ENOMEM;
		goto fail_and_close;
	}

	mutex_init(&priv->buffer_lock);
	mutex_init(&priv->z53_status_request_lock);
	init_waitqueue_head(&priv->z53_reader_wq);
	init_completion(&priv->fw_version_processed);
	init_completion(&priv->status_report_processed);

	ret = kraken3_init_device(hdev);
	if (ret) {
		hid_err(hdev, "device init failed with %d\n", ret);
		goto fail_and_close;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(&hdev->dev,
							  kraken3_device_names[priv->kind], priv,
							  &kraken3_chip_info, kraken3_groups);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon registration failed with %d\n", ret);
		goto fail_and_close;
	}

	kraken3_debugfs_init(priv);

	return 0;

fail_and_close:
	hid_hw_close(hdev);
fail_and_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void kraken3_remove(struct hid_device *hdev)
{
	struct kraken3_data *priv = hid_get_drvdata(hdev);

	debugfs_remove_recursive(priv->debugfs);
	hwmon_device_unregister(priv->hwmon_dev);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static const struct hid_device_id kraken3_table[] = {
	/* NZXT Kraken X53/X63/X73 have two possible product IDs */
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_PRODUCT_ID_X53) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_PRODUCT_ID_X53_SECOND) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_PRODUCT_ID_Z53) },
	{ }
};

MODULE_DEVICE_TABLE(hid, kraken3_table);

static struct hid_driver kraken3_driver = {
	.name = DRIVER_NAME,
	.id_table = kraken3_table,
	.probe = kraken3_probe,
	.remove = kraken3_remove,
	.raw_event = kraken3_raw_event,
#ifdef CONFIG_PM
	.reset_resume = kraken3_reset_resume,
#endif
};

static int __init kraken3_init(void)
{
	return hid_register_driver(&kraken3_driver);
}

static void __exit kraken3_exit(void)
{
	hid_unregister_driver(&kraken3_driver);
}

/* When compiled into the kernel, initialize after the HID bus */
late_initcall(kraken3_init);
module_exit(kraken3_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Malaco <jonas@protocubo.io>");
MODULE_AUTHOR("Aleksa Savic <savicaleksa83@gmail.com>");
MODULE_DESCRIPTION("Hwmon driver for NZXT Kraken X53/X63/X73, Z53/Z63/Z73 coolers");
