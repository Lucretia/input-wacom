/*
 * Wacom W8001 penabled serial touchscreen driver
 *
 * Copyright (c) 2008 Jaya Kumar
 * Copyright (c) 2010 Red Hat, Inc.
 * Copyright (c) 2010 Ping Cheng, Wacom. <pingc@wacom.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file COPYING in the main directory of this archive for
 * more details.
 *
 * Layout based on Elo serial touchscreen driver by Vojtech Pavlik
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/serio.h>
#include <linux/init.h>
#include <linux/ctype.h>

#define DRIVER_DESC	"Wacom W8001 serial touchscreen driver"

MODULE_AUTHOR("Jaya Kumar <jayakumar.lkml@gmail.com>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

#define W8001_MAX_LENGTH	11
#define W8001_LEAD_MASK		0x80
#define W8001_LEAD_BYTE		0x80
#define W8001_TAB_MASK		0x40
#define W8001_TAB_BYTE		0x40
/* set in first byte of touch data packets */
#define W8001_TOUCH_MASK	(0x10 | W8001_LEAD_MASK)
#define W8001_TOUCH_BYTE	(0x10 | W8001_LEAD_BYTE)

#define W8001_QUERY_PACKET	0x20

#define W8001_CMD_STOP		'0'
#define W8001_CMD_START		'1'
#define W8001_CMD_QUERY		'*'
#define W8001_CMD_TOUCHQUERY	'%'

/* length of data packets in bytes, depends on device. */
#define W8001_PKTLEN_TOUCH93	5
#define W8001_PKTLEN_TOUCH9A	7
#define W8001_PKTLEN_TPCPEN	9
#define W8001_PKTLEN_TPCCTL	11	/* control packet */

struct w8001_coord {
	u8 rdy;
	u8 tsw;
	u8 f1;
	u8 f2;
	u16 x;
	u16 y;
	u16 pen_pressure;
	u8 tilt_x;
	u8 tilt_y;
};

/* touch query reply packet */
struct w8001_touch_query {
	u8 panel_res;
	u8 capacity_res;
	u8 sensor_id;
	u16 x;
	u16 y;
};

/*
 * Per-touchscreen data.
 */

struct w8001 {
	struct input_dev *dev;
	struct serio *serio;
	struct completion cmd_done;
	int id;
	int idx;
	unsigned char response_type;
	unsigned char response[W8001_MAX_LENGTH];
	unsigned char data[W8001_MAX_LENGTH];
	char phys[32];
	int type;
	unsigned int pktlen;
	int trkid[2];
	int trkid_idx;
	bool pen_in_prox;
	bool has_touch;
	int max_touch_x;
	int max_touch_y;
	int max_pen_x;
	int max_pen_y;
};

static void parse_data(u8 *data, struct w8001_coord *coord)
{
	memset(coord, 0, sizeof(*coord));

	coord->rdy = data[0] & 0x20;
	coord->tsw = data[0] & 0x01;
	coord->f1 = data[0] & 0x02;
	coord->f2 = data[0] & 0x04;

	coord->x = (data[1] & 0x7F) << 9;
	coord->x |= (data[2] & 0x7F) << 2;
	coord->x |= (data[6] & 0x60) >> 5;

	coord->y = (data[3] & 0x7F) << 9;
	coord->y |= (data[4] & 0x7F) << 2;
	coord->y |= (data[6] & 0x18) >> 3;

	coord->pen_pressure = data[5] & 0x7F;
	coord->pen_pressure |= (data[6] & 0x07) << 7 ;

	coord->tilt_x = data[7] & 0x7F;
	coord->tilt_y = data[8] & 0x7F;
}

static void parse_single_touch(struct w8001 *w8001)
{
	struct input_dev *dev = w8001->dev;
	unsigned char *data = w8001->data;

	/* We send single touch events for MT (non-signle touch) devices
	 * even a stylus is in proximity. Apps or userland clients have
	 * the opportunity to arbitrate these events if they don't need
	 * the touch data when MT events are received.
	 * Wacom X server driver arbitrates the events for all apps that
	 * are based on X server.
	 */
	int x = (data[1] << 7) | data[2];
	int y = (data[3] << 7) | data[4];
	w8001->has_touch = data[0] & 0x1;

	/* scale to pen maximum */
	if (w8001->max_pen_x && w8001->max_pen_y && w8001->max_touch_x) {
		x = x * w8001->max_pen_x / w8001->max_touch_x;
		y = y * w8001->max_pen_y / w8001->max_touch_y;
	}
	input_report_abs(dev, ABS_X, x);
	input_report_abs(dev, ABS_Y, y);
	input_report_key(dev, BTN_TOUCH, w8001->has_touch);
	input_report_key(dev, BTN_TOOL_FINGER, w8001->has_touch);

	input_sync(dev);
}

static void parse_touchquery(u8 *data, struct w8001_touch_query *query)
{
	memset(query, 0, sizeof(*query));

	query->panel_res = data[1];
	query->sensor_id = data[2] & 0x7;
	query->capacity_res = data[7];

	query->x = data[3] << 9;
	query->x |= data[4] << 2;
	query->x |= (data[2] >> 5) & 0x3;

	query->y = data[5] << 9;
	query->y |= data[6] << 2;
	query->y |= (data[2] >> 3) & 0x3;

	/* Early days' one finger touch models need the following defaults */
	if (!query->x && !query->y) {
		query->x = 1024;
		query->y = 1024;
		query->panel_res = 10;
		query->panel_res = 10;
		if (data[1]) {
			query->x = (1 << data[1]);
			query->y = (1 << data[1]);
		}
	}
}

static void report_pen_events(struct w8001 *w8001, struct w8001_coord *coord)
{
	struct input_dev *dev = w8001->dev;

	/*
	 * We have 1 bit for proximity (rdy) and 3 bits for tip, side,
	 * side2/eraser. If rdy && f2 are set, this can be either pen + side2,
	 * or eraser. assume
	 * - if dev is already in proximity and f2 is toggled → pen + side2
	 * - if dev comes into proximity with f2 set → eraser
	 * If f2 disappears after assuming eraser, fake proximity out for
	 * eraser and in for pen.
	 */

	if (!w8001->type) {
		w8001->type = coord->f2 ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
	} else if (w8001->type == BTN_TOOL_RUBBER) {
		if (!coord->f2) {
			input_report_abs(dev, ABS_PRESSURE, 0);
			input_report_key(dev, BTN_TOUCH, 0);
			input_report_key(dev, BTN_STYLUS, 0);
			input_report_key(dev, BTN_STYLUS2, 0);
			input_report_key(dev, BTN_TOOL_RUBBER, 0);
			input_sync(dev);
			w8001->type = BTN_TOOL_PEN;
		}
	} else {
		input_report_key(dev, BTN_STYLUS2, coord->f2);
	}

	input_report_abs(dev, ABS_X, coord->x);
	input_report_abs(dev, ABS_Y, coord->y);
	input_report_abs(dev, ABS_PRESSURE, coord->pen_pressure);
	input_report_key(dev, BTN_TOUCH, coord->tsw);
	input_report_key(dev, BTN_STYLUS, coord->f1);
	input_report_key(dev, w8001->type, coord->rdy);
	input_sync(dev);

	if (!coord->rdy)
		w8001->type = 0;
}

static irqreturn_t w8001_interrupt(struct serio *serio,
				   unsigned char data, unsigned int flags)
{
	struct w8001 *w8001 = serio_get_drvdata(serio);
	struct input_dev *dev = w8001->dev;
	struct w8001_coord coord;
	unsigned char tmp;

	w8001->data[w8001->idx] = data;
	switch (w8001->idx++) {
	case 0:
		if ((data & W8001_LEAD_MASK) != W8001_LEAD_BYTE) {
			pr_debug("w8001: unsynchronized data: 0x%02x\n", data);
			w8001->idx = 0;
		}
		break;

	case W8001_PKTLEN_TOUCH93 - 1:
	case W8001_PKTLEN_TOUCH9A - 1:
		tmp = (w8001->data[0] & W8001_TOUCH_BYTE);
		if (tmp != W8001_TOUCH_BYTE)
			break;

		if (w8001->pktlen == w8001->idx) {
			w8001->idx = 0;
			if (!w8001->pen_in_prox)
				parse_single_touch(w8001);
		}
		break;

	/* Pen coordinates packet */
	case W8001_PKTLEN_TPCPEN - 1:
		tmp = w8001->data[0] & W8001_TAB_MASK;
		if (unlikely(tmp == W8001_TAB_BYTE))
			break;

		tmp = (w8001->data[0] & W8001_TOUCH_BYTE);
		if (tmp == W8001_TOUCH_BYTE)
			break;

		w8001->idx = 0;
		if (w8001->has_touch) {
			/* send touch data out */
			w8001->has_touch = 0;
			input_report_key(dev, BTN_TOUCH, 0);
			input_report_key(dev, BTN_TOOL_FINGER, 0);
		}
		parse_data(w8001->data, &coord);
		report_pen_events(w8001, &coord);
		w8001->pen_in_prox = coord.rdy ? true : false;
		break;

	/* control packet */
	case W8001_PKTLEN_TPCCTL - 1:
		tmp = (w8001->data[0] & W8001_TOUCH_MASK);
		if (tmp == W8001_TOUCH_BYTE)
			break;

		w8001->idx = 0;
		memcpy(w8001->response, w8001->data, W8001_MAX_LENGTH);
		w8001->response_type = W8001_QUERY_PACKET;
		complete(&w8001->cmd_done);
		break;
	}

	return IRQ_HANDLED;
}

static int w8001_command(struct w8001 *w8001, unsigned char command,
			 bool wait_response)
{
	int rc;

	w8001->response_type = 0;
	init_completion(&w8001->cmd_done);

	rc = serio_write(w8001->serio, command);
	if (rc == 0 && wait_response) {
		wait_for_completion_timeout(&w8001->cmd_done, HZ);
		if (w8001->response_type != W8001_QUERY_PACKET)
			rc = -EIO;
	}

	return rc;
}

static int w8001_setup(struct w8001 *w8001)
{
	struct input_dev *dev = w8001->dev;
	struct w8001_coord coord;
	int error;

	error = w8001_command(w8001, W8001_CMD_STOP, false);
	if (error)
		return error;

	mdelay(250);	/* wait 250ms before querying the device */

	/* penabled? */
	error = w8001_command(w8001, W8001_CMD_QUERY, true);
	if (!error) {
		dev->keybit[BIT_WORD(BTN_TOOL_PEN)] |= BIT_MASK(BTN_TOOL_PEN);
		dev->keybit[BIT_WORD(BTN_TOOL_RUBBER)] |= BIT_MASK(BTN_TOOL_RUBBER);
		dev->keybit[BIT_WORD(BTN_STYLUS)] |= BIT_MASK(BTN_STYLUS);
		dev->keybit[BIT_WORD(BTN_STYLUS2)] |= BIT_MASK(BTN_STYLUS2);
		parse_data(w8001->response, &coord);

		input_set_abs_params(dev, ABS_X, 0, coord.x, 0, 0);
		input_set_abs_params(dev, ABS_Y, 0, coord.y, 0, 0);
		input_set_abs_params(dev, ABS_PRESSURE, 0, coord.pen_pressure, 0, 0);
		if (coord.tilt_x && coord.tilt_y)
		{
			input_set_abs_params(dev, ABS_TILT_X, 0, coord.tilt_x, 0, 0);
			input_set_abs_params(dev, ABS_TILT_Y, 0, coord.tilt_y, 0, 0);
		}
	}

	/* touch enabled? */
	error = w8001_command(w8001, W8001_CMD_TOUCHQUERY, true);

	/* Some non-touch devices may reply to the touch query. But their
	 * second byte is empty, which indicates touch is not supported.
	 */
	if (!error && w8001->response[1]) {
		struct w8001_touch_query touch;
		int px, py;

		parse_touchquery(w8001->response, &touch);

		px = w8001->max_touch_x = touch.x;
		py = w8001->max_touch_x = touch.y;

		/* scale to pen maximum */
		if (coord.x && coord.y) {
			px = w8001->max_pen_x = coord.x;
			py = w8001->max_pen_y = coord.y;
		}
		input_set_abs_params(dev, ABS_X, 0, px, 0, 0);
		input_set_abs_params(dev, ABS_Y, 0, py, 0, 0);
		dev->keybit[BIT_WORD(BTN_TOOL_FINGER)] |= BIT_MASK(BTN_TOOL_FINGER);

		switch (touch.sensor_id) {
		case 0:
		case 2:
			w8001->pktlen = W8001_PKTLEN_TOUCH93;
			break;
		case 1:
		case 3:
		case 4:
			w8001->pktlen = W8001_PKTLEN_TOUCH9A;
			break;
		case 5:
			/* 2FGT is not suppoted for kernels <= 2.6.36 */
			break;
		}
	}

	return w8001_command(w8001, W8001_CMD_START, false);
}

/*
 * w8001_disconnect() is the opposite of w8001_connect()
 */

static void w8001_disconnect(struct serio *serio)
{
	struct w8001 *w8001 = serio_get_drvdata(serio);

	input_get_device(w8001->dev);
	input_unregister_device(w8001->dev);
	serio_close(serio);
	serio_set_drvdata(serio, NULL);
	input_put_device(w8001->dev);
	kfree(w8001);
}

/*
 * w8001_connect() is the routine that is called when someone adds a
 * new serio device that supports the w8001 protocol and registers it as
 * an input device.
 */

static int w8001_connect(struct serio *serio, struct serio_driver *drv)
{
	struct w8001 *w8001;
	struct input_dev *input_dev;
	int err;

	w8001 = kzalloc(sizeof(struct w8001), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!w8001 || !input_dev) {
		err = -ENOMEM;
		goto fail1;
	}

	w8001->serio = serio;
	w8001->id = serio->id.id;
	w8001->dev = input_dev;
	w8001->trkid[0] = w8001->trkid[1] = -1;
	init_completion(&w8001->cmd_done);
	snprintf(w8001->phys, sizeof(w8001->phys), "%s/input0", serio->phys);

	input_dev->name = "Wacom W8001 Penabled Serial TouchScreen";
	input_dev->phys = w8001->phys;
	input_dev->id.bustype = BUS_RS232;
	input_dev->id.vendor = SERIO_W8001;
	input_dev->id.product = w8001->id;
	input_dev->id.version = 0x0100;
	input_dev->dev.parent = &serio->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	serio_set_drvdata(serio, w8001);
	err = serio_open(serio, drv);
	if (err)
		goto fail2;

	err = w8001_setup(w8001);
	if (err)
		goto fail3;

	err = input_register_device(w8001->dev);
	if (err)
		goto fail3;

	return 0;

fail3:
	serio_close(serio);
fail2:
	serio_set_drvdata(serio, NULL);
fail1:
	input_free_device(input_dev);
	kfree(w8001);
	return err;
}

static struct serio_device_id w8001_serio_ids[] = {
	{
		.type	= SERIO_RS232,
		.proto	= SERIO_W8001,
		.id	= SERIO_ANY,
		.extra	= SERIO_ANY,
	},
	{ 0 }
};

MODULE_DEVICE_TABLE(serio, w8001_serio_ids);

static struct serio_driver w8001_drv = {
	.driver		= {
		.name	= "w8001",
	},
	.description	= DRIVER_DESC,
	.id_table	= w8001_serio_ids,
	.interrupt	= w8001_interrupt,
	.connect	= w8001_connect,
	.disconnect	= w8001_disconnect,
};

static int __init w8001_init(void)
{
	return serio_register_driver(&w8001_drv);
}

static void __exit w8001_exit(void)
{
	serio_unregister_driver(&w8001_drv);
}

module_init(w8001_init);
module_exit(w8001_exit);
