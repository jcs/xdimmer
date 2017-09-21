/*
 * xdimmer
 * Copyright (c) 2013-2016 joshua stein <jcs@jcs.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <err.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __OpenBSD__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <dev/wscons/wsconsio.h>
#include <sys/sysctl.h>
#include <sys/sensors.h>
#include <errno.h>
#endif

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/Xrandr.h>

#define DEFAULT_DIM_TIMEOUT	120
#define DEFAULT_DIM_PERCENTAGE	10

#define DIM_STEPS		20
#define BRIGHTEN_STEPS		5

enum {
	OP_GET,
	OP_SET,
};

static const struct als_setting {
	char *label;
	int min_lux;
	int backlight;
	int kbd_backlight;
} als_settings[] = {
	/* scene	      min lux  screen  kbd */
	{ "pitch black",	   0,	 20,	80 },
	{ "very dark",		  11,	 30,	70 },
	{ "dark indoors",	  51,	 40,	60 },
	{ "dim indoors",	 201,	 50,	50 },
	{ "normal indoors",	 401,	 60,	40 },
	{ "bright indoors",	1001,	 70,	30 },
	{ "dim outdoors",	5001,	 80,	20 },
	{ "cloudy outdoors",   10001,	 90,	10 },
	{ "sunlight",	       30001,	100,	 0 },
};

void xloop(void);
void set_alarm(XSyncAlarm *, XSyncCounter, XSyncTestType, XSyncValue);
void dim(void);
void brighten(void);
void bail(int);
void stepper(double, double, int);
double backlight_op(int, double);
double kbd_backlight_op(int, double);
int als_find_sensor(void);
void als_fetch(void);
void usage(void);
Bool XNextEventOrTimeout(Display *, XEvent *, unsigned int);

extern char *__progname;

static double als = -1;
static double backlight = -1;
static Atom backlight_a = 0;
static int debug = 0;
static int dimkbd = 0;
static int dimmed = 0;
static int dimscreen = 1;
static int useals = 0;
static int exiting = 0;
static double kbd_backlight = -1;

static int dim_timeout = DEFAULT_DIM_TIMEOUT;
static int dim_pct = DEFAULT_DIM_PERCENTAGE;
static int dim_steps = DIM_STEPS;

#ifdef __OpenBSD__
static int wsconsdfd = 0;
static int wsconskfd = 0;
int alsmib[5] = { CTL_HW, HW_SENSORS, 0, 0, 0 };
#endif

static Display *dpy;

int
main(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "adknp:s:t:")) != -1) {
		const char *errstr;

		switch (ch) {
		case 'a':
#ifndef __OpenBSD__
			errx(1, "ambient light sensors not supported on this "
			    "platform");
#endif
			useals = 1;
			break;
		case 'd':
			debug = 1;
			break;
		case 'k':
#ifndef __OpenBSD__
			errx(1, "keyboard backlight not supported on this "
			    "platform");
#endif
			dimkbd = 1;
			break;
		case 'n':
			dimscreen = 0;
			break;
		case 'p':
			dim_pct = strtonum(optarg, 1, 100, &errstr);
			if (errstr)
				errx(2, "dim percentage: %s", errstr);
			break;
		case 's':
			dim_steps = strtonum(optarg, 1, 100, &errstr);
			if (errstr)
				errx(2, "dim steps: %s", errstr);
			break;
		case 't':
			dim_timeout = strtonum(optarg, 1, INT_MAX, &errstr);
			if (errstr)
				errx(2, "dim timeout: %s", errstr);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (!dimscreen && !dimkbd)
		errx(1, "not dimming screen or keyboard, nothing to do");

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

	if (dimscreen) {
		backlight_a = XInternAtom(dpy, RR_PROPERTY_BACKLIGHT, True);
		if (backlight_a == None) {
#ifdef __OpenBSD__
			/* see if wscons display.brightness is available */
			if (!(wsconsdfd = open("/dev/ttyC0", O_WRONLY)) ||
			    backlight_op(OP_GET, 0) < 0)
#endif
				errx(1, "no backlight control");
		}
	}

#ifdef __OpenBSD__
	if (dimkbd)
		if (!(wsconskfd = open("/dev/wskbd0", O_WRONLY)) ||
		    kbd_backlight_op(OP_GET, 0) < 0)
			errx(1, "no keyboard backlight control");

	if (useals && !als_find_sensor())
		errx(1, "can't find ambient light sensor");
#endif

	if (debug) {
		if (dimscreen)
			printf("dimming screen to %d%% in %d secs\n", dim_pct,
			    dim_timeout);

		if (dimkbd)
			printf("dimming keyboard backlight in %d secs\n",
			    dim_timeout);
	}

	signal(SIGINT, bail);
	signal(SIGTERM, bail);

	xloop();

	return (0);
}

void
xloop(void)
{
	XSyncSystemCounter *counters;
	XSyncAlarm idle_alarm = None;
	XSyncAlarm reset_alarm = None;
	XSyncValue val;
	int sync_event, error;
	int sync_major, sync_minor, ncounters, idler = 0;
	int i;

	if (XSyncQueryExtension(dpy, &sync_event, &error) != True)
		errx(1, "no sync extension available");

	XSyncInitialize(dpy, &sync_major, &sync_minor);

	counters = XSyncListSystemCounters(dpy, &ncounters);
	for (i = 0; i < ncounters; i++)
		if (!strcmp(counters[i].name, "IDLETIME")) {
			idler = counters[i].counter;
			break;
		}
	XSyncFreeSystemCounterList(counters);

	if (!idler)
		errx(1, "no idle counter");

	/* fire an XSyncAlarmNotifyEvent when idletime counter reaches our
	 * dim_timeout seconds */
	XSyncIntToValue(&val, dim_timeout * 1000);
	set_alarm(&idle_alarm, idler, XSyncPositiveComparison, val);

	if (dimscreen)
		backlight = backlight_op(OP_GET, 0);
	if (dimkbd)
		kbd_backlight = kbd_backlight_op(OP_GET, 0);

	for (;;) {
		XEvent e;
		XSyncAlarmNotifyEvent *alarm_e;
		int overflow;
		XSyncValue add, plusone;

		if (debug)
			printf("waiting for next event\n");

		/* if we're checking an als, only wait 1 second for x event */
		XNextEventOrTimeout(dpy, &e, (useals ? 1000 : 0));
		if (e.type != 0 && debug)
			printf("got event of type %d\n", e.type);

		if (exiting) {
			brighten();
			exit(0);
		}

		if (e.type == 0 && useals) {
			als_fetch();
			continue;
		}

		if (e.type != sync_event + XSyncAlarmNotify)
			continue;

		alarm_e = (XSyncAlarmNotifyEvent *)&e;

		if (alarm_e->alarm == idle_alarm) {
			if (debug)
				printf("idle counter reached %dms\n",
					XSyncValueLow32(alarm_e->counter_value));

			XSyncDestroyAlarm(dpy, idle_alarm);
			idle_alarm = None;

			dim();

			/* fire reset_alarm when idletime counter resets */
			XSyncIntToValue(&add, -1);
			XSyncValueAdd(&plusone, alarm_e->counter_value, add,
			    &overflow);
			set_alarm(&reset_alarm, idler, XSyncNegativeComparison,
			    plusone);
		}
		else if (alarm_e->alarm == reset_alarm) {
			if (debug)
				printf("idle counter reset\n");

			XSyncDestroyAlarm(dpy, reset_alarm);
			reset_alarm = None;

			brighten();

			XSyncIntToValue(&val, dim_timeout * 1000);
			set_alarm(&idle_alarm, idler, XSyncPositiveComparison,
			    val);
		}
	}
}

void
set_alarm(XSyncAlarm *alarm, XSyncCounter counter, XSyncTestType test,
    XSyncValue value)
{
	XSyncAlarmAttributes attr;
	XSyncValue delta;
	unsigned int flags;

	XSyncIntToValue (&delta, 0);

	attr.trigger.counter = counter;
	attr.trigger.value_type = XSyncAbsolute;
	attr.trigger.test_type = test;
	attr.trigger.wait_value = value;
	attr.delta = delta;

	flags = XSyncCACounter | XSyncCAValueType | XSyncCATestType |
	    XSyncCAValue | XSyncCADelta;

	if (*alarm)
		XSyncChangeAlarm(dpy, *alarm, flags, &attr);
	else
		*alarm = XSyncCreateAlarm(dpy, flags, &attr);
}

void
dim(void)
{
	if (dimscreen)
		backlight = backlight_op(OP_GET, 0);
	if (dimkbd)
		kbd_backlight = kbd_backlight_op(OP_GET, 0);

	if ((dimscreen && (backlight > dim_pct)) ||
	    (dimkbd && (kbd_backlight > 0))) {
		if (debug) {
			if (dimscreen)
				printf("dimming screen to %d\n", dim_pct);
			if (dimkbd)
				printf("dimming keyboard\n");
		}

		stepper(dim_pct, 0, dim_steps);
		dimmed = 1;
	}
	else if (dimscreen && debug)
		printf("backlight already at %f, not dimming to %d\n",
		    backlight, dim_pct);
}

void
brighten(void)
{
	if (dimmed) {
		if (debug) {
			if (dimscreen)
				printf("brightening screen back to %f\n",
				    backlight);
			if (dimkbd)
				printf("brightening keyboard\n");
		}

		stepper(backlight, kbd_backlight, BRIGHTEN_STEPS);
	}
	else if (debug)
		printf("no previous backlight setting, not brightening\n");

	dimmed = 0;
}

void
stepper(double new_backlight, double new_kbd_backlight, int steps)
{
	double tbacklight = 0;
	double tkbdbacklight = 0;
	double step_inc = 0, kbd_step_inc = 0;
	int j;

	if (dimscreen) {
		tbacklight = backlight_op(OP_GET, 0);

		if ((int)new_backlight != (int)tbacklight)
			step_inc = (new_backlight - tbacklight) / steps;
	}
	if (dimkbd) {
		tkbdbacklight = kbd_backlight_op(OP_GET, 0);

		if ((int)new_kbd_backlight != (int)tkbdbacklight)
			kbd_step_inc = (new_kbd_backlight - tkbdbacklight) /
			    steps;
	}

	if (!(step_inc || kbd_step_inc))
		return;

	if (debug) {
		if (dimscreen)
			printf("stepping from %0.2f to %0.2f in increments "
			    "of %f (%d step%s)\n",
			    tbacklight, new_backlight, step_inc, steps,
			    (steps == 1 ? "" : "s"));
		if (dimkbd)
			printf("stepping keyboard from %0.2f to %0.2f in "
			    "increments of %f (%d step%s)\n",
			    tkbdbacklight, new_kbd_backlight, kbd_step_inc,
			    steps, (steps == 1 ? "" : "s"));
	}

	for (j = 1; j <= steps; j++) {
		if (dimscreen)
			tbacklight += step_inc;
		if (dimkbd)
			tkbdbacklight += kbd_step_inc;

		if (j == steps) {
			if (dimscreen)
				tbacklight = new_backlight;
			if (dimkbd)
				tkbdbacklight = new_kbd_backlight;
		}

		if (dimscreen)
			backlight_op(OP_SET, tbacklight);
		if (dimkbd)
			kbd_backlight_op(OP_SET, tkbdbacklight);

		/* only slow down steps if we're dimming */
		if (j < steps && ((dimscreen && (step_inc < 0)) ||
		    (dimkbd && (kbd_step_inc < 0))))
			usleep(25000);
	}
}

double
backlight_op(int op, double new_backlight)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems;
	unsigned long bytes_after;
	unsigned char *prop;
	XRRPropertyInfo *info;
	long value, to;
	double min, max;
	int i;

	double cur_backlight = -1.0;

	if (backlight_a == None) {
#ifdef __OpenBSD__
		struct wsdisplay_param param;

		if (debug)
			printf("%s (wscons): %f\n", __func__, new_backlight);

		param.param = WSDISPLAYIO_PARAM_BRIGHTNESS;
		if (ioctl(wsconsdfd, WSDISPLAYIO_GETPARAM, &param) < 0)
			err(1, "WSDISPLAYIO_GETPARAM failed");

		if (op == OP_SET) {
			param.curval = (double)(param.max - param.min) *
				(new_backlight / 100.0);

			if (param.curval > param.max)
				param.curval = param.max;
			else if (param.curval < param.min)
				param.curval = param.min;

			if (ioctl(wsconsdfd, WSDISPLAYIO_SETPARAM, &param) < 0)
				err(1, "WSDISPLAYIO_SETPARAM failed");
		}

		cur_backlight = ((double)param.curval /
		    (double)(param.max - param.min)) * 100;
#endif
	} else {
		if (debug)
			printf("%s (xrandr): %f\n", __func__, new_backlight);

		XRRScreenResources *screen_res = XRRGetScreenResources(dpy,
		    DefaultRootWindow(dpy));
		if (!screen_res)
			errx(1, "no screen resources");

		for (i = 0; i < screen_res->noutput; i++) {
			RROutput output = screen_res->outputs[i];

			/* yay magic numbers */

			if (XRRGetOutputProperty(dpy, output, backlight_a,
			    0, 4, False, False, None, &actual_type,
			    &actual_format, &nitems, &bytes_after,
			    &prop) != Success)
				continue;

			if (actual_type != XA_INTEGER || nitems != 1 ||
			    actual_format != 32) {
				XFree (prop);
				continue;
			}

			value = *((long *) prop);
			XFree(prop);

			info = XRRQueryOutputProperty(dpy, output, backlight_a);
			if (!info)
				continue;

			if (!info->range || info->num_values != 2) {
				XFree(info);
				continue;
			}

			min = info->values[0];
			max = info->values[1];
			XFree(info);

			/* convert into a percentage */
			cur_backlight = ((value - min) * 100) / (max - min);

			if (op == OP_SET) {
				to = min + ((new_backlight *
				    (max - min)) / 100);
				if (to < min)
					to = min;
				if (to > max)
					to = max;

				XRRChangeOutputProperty(dpy, output,
				    backlight_a, XA_INTEGER, 32,
				    PropModeReplace,
				    (unsigned char *)&to, 1);
				XSync(dpy, True);
			}
			else
				/* just return the first screen's backlight */
				break;
		}

		XRRFreeScreenResources(screen_res);
	}

	return cur_backlight;
}

double
kbd_backlight_op(int op, double new_backlight)
{
#ifdef __OpenBSD__
	struct wskbd_backlight param;

	if (ioctl(wsconskfd, WSKBDIO_GETBACKLIGHT, &param) < 0)
		err(1, "WSKBDIO_GETBACKLIGHT failed");

	if (op == OP_SET) {
		if (debug)
			printf("%s: %f\n", __func__, new_backlight);

		param.curval = (double)(param.max - param.min) *
			(new_backlight / 100.0);

		if (param.curval > param.max)
			param.curval = param.max;
		else if (param.curval < param.min)
			param.curval = param.min;

		if (ioctl(wsconskfd, WSKBDIO_SETBACKLIGHT, &param) < 0)
			err(1, "WSKBDIO_SETBACKLIGHT failed");
	}

	return ((double)param.curval / (double)(param.max - param.min)) * 100;
#else
	return 0;
#endif
}

int
als_find_sensor(void)
{
#ifdef __OpenBSD__
	struct sensordev sensordev;
	struct sensor sensor;
	size_t sdlen, slen;
	int dev, numt;

	sdlen = sizeof(sensordev);
	slen = sizeof(sensor);

	for (dev = 0; ; dev++) {
		alsmib[2] = dev;

		if (sysctl(alsmib, 3, &sensordev, &sdlen, NULL, 0) == -1) {
			if (errno == ENXIO)
				continue;
			else if (errno == ENOENT)
				break;

			return 0;
		}

		if (strstr(sensordev.xname, "acpials") == NULL &&
		   strstr(sensordev.xname, "asmc") == NULL)
			continue;

		alsmib[3] = SENSOR_LUX;

		for (numt = 0; numt < 1; numt++) {
			alsmib[4] = numt;
			if (sysctl(alsmib, 5, &sensor, &slen, NULL, 0) == -1) {
				if (errno != ENOENT) {
					warn("sysctl");
					continue;
				}
			}

			if (debug)
				printf("using als sensor %s\n",
				    sensordev.xname);

			return 1;
		}
	}
#endif

	return 0;
}

void
als_fetch(void)
{
	struct sensordev sensordev;
	struct sensor sensor;
	size_t sdlen, slen;
	double lux, tbacklight = backlight, tkbd_backlight = kbd_backlight;
	int i;

	sdlen = sizeof(sensordev);
	slen = sizeof(sensor);

	if (sysctl(alsmib, 5, &sensor, &sdlen, NULL, 0) == -1) {
		warn("sysctl");
		return;
	}

	lux = sensor.value / 1000000.0;

	if ((int)als < 0) {
		als = lux;
		return;
	}

	if (abs((int)lux - (int)als) < 10) {
		als = lux;
		return;
	}

	if (debug)
		printf("als lux change %f -> %f, screen: %f, kbd: %f\n",
		    als, lux, backlight, kbd_backlight);

	for (i = (sizeof(als_settings) / sizeof(struct als_setting)) - 1;
	    i >= 0; i--) {
		struct als_setting as = als_settings[i];

		if (lux < as.min_lux)
			continue;

		if (debug)
			printf("using lux profile %s\n", as.label);

		if (dimkbd && (round(kbd_backlight) != as.kbd_backlight)) {
			if (debug)
				printf("adjusting keyboard backlight to %d%%\n",
				    as.kbd_backlight);

			tkbd_backlight = as.kbd_backlight;
		}

		if (round(backlight) != as.backlight) {
			if (debug)
				printf("adjusting screen backlight to %d%%\n",
				    as.backlight);

			tbacklight = as.backlight;
		}

		stepper(tbacklight, tkbd_backlight, dim_steps);

		break;
	}

	als = lux;
}

void
usage(void)
{
	fprintf(stderr, "usage: %s [-adkn] [-p dim pct] [-s dim steps] "
	    "[-t timeout secs]\n", __progname);
	exit(1);
}

void
bail(int sig)
{
	if (debug)
		printf("got signal %d, trying to exit\n", sig);

	/* XXX: doing X ops inside a signal handler causes an infinite loop in
	 * _XReply/xcb, so we can't properly brighten() and exit, so we just
	 * set a flag and wait for the next time our idle counter stuff happens
	 * so we can exit there */
	if (dimmed)
		exiting = 1;
	else
		exit(0);
}

Bool
XNextEventOrTimeout(Display *dpy, XEvent *e, unsigned int msecs)
{
	int fd;
	fd_set fdsr;
	struct timeval tv;

	if (msecs == 0)
		return XNextEvent(dpy, e);

	if (XPending(dpy) == 0) {
		fd = ConnectionNumber(dpy);
		FD_ZERO(&fdsr);
		FD_SET(fd, &fdsr);
		tv.tv_sec = msecs / 1000;
		tv.tv_usec = (msecs % 1000) * 1000;
		if (select(fd + 1, &fdsr, NULL, NULL, &tv) >= 0) {
			e->type = 0;
			return False;
		}
	}

	return XNextEvent(dpy, e);
}
