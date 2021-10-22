/*
 * xdimmer
 * Copyright (c) 2013-2019 joshua stein <jcs@jcs.org>
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
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/poll.h>

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

#define DEFAULT_DIM_STEPS	20
#define DEFAULT_BRIGHTEN_STEPS	5

enum {
	OP_GET,
	OP_SET,
};

enum {
	MSG_EXIT = 1,
	MSG_DIM,
	MSG_BRIGHTEN,
};

static const struct als_setting {
	char *label;
	int min_lux;
	int backlight;
	int kbd_backlight;
} als_settings[] = {
	/* scene	     min lux screen    kbd */
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
void set_alarm(XSyncAlarm *, XSyncTestType);
void bail(int);
void sigusr1(int);
void sigusr2(int);
void stepper(float, float, int, int);
float backlight_op(int, float);
float kbd_backlight_op(int, float);
int als_find_sensor(void);
void als_fetch(void);
void usage(void);
int XPeekEventOrTimeout(Display *, XEvent *, unsigned int);
int pipemsg[2];

extern char *__progname;

/* options */
static int dim_kbd = 0;
static int dimmed = 0;
static int dim_screen = 1;
static int use_als = 0;

/* ALS reading */
static float als = -1;

/* backlight reading and target while dimming/undimming */
static float backlight = -1;
static float kbd_backlight = -1;

static int dim_timeout = DEFAULT_DIM_TIMEOUT;
static int dim_pct = DEFAULT_DIM_PERCENTAGE;
static int dim_steps = DEFAULT_DIM_STEPS;
static int brighten_steps = DEFAULT_BRIGHTEN_STEPS;

static Atom backlight_a = 0;
static XSyncCounter idler_counter = 0;
static int exiting = 0;
static int force_dim = 0;
static int force_brighten = 0;
static int debug = 0;
#define DPRINTF(x) { if (debug) { printf x; } };

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

	while ((ch = getopt(argc, argv, "ab:dknp:s:t:")) != -1) {
		const char *errstr;

		switch (ch) {
		case 'a':
#ifndef __OpenBSD__
			errx(1, "ambient light sensors not supported on this "
			    "platform");
#endif
			use_als = 1;
			break;
		case 'b':
			brighten_steps = strtonum(optarg, 1, 100, &errstr);
			if (errstr)
				errx(2, "brighten steps: %s", errstr);
			break;
		case 'd':
			debug = 1;
			break;
		case 'k':
#ifndef __OpenBSD__
			errx(1, "keyboard backlight not supported on this "
			    "platform");
#endif
			dim_kbd = 1;
			break;
		case 'n':
			dim_screen = 0;
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

	if (!dim_screen && !dim_kbd && !use_als)
		errx(1, "not dimming screen or keyboard, nothing to do");

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

	if (dim_screen || use_als) {
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
	if (dim_kbd)
		if (!(wsconskfd = open("/dev/wskbd0", O_WRONLY)) ||
		    kbd_backlight_op(OP_GET, 0) < 0)
			errx(1, "no keyboard backlight control");

	if (use_als && !als_find_sensor())
		errx(1, "can't find ambient light sensor");
#endif

	if (dim_screen)
		DPRINTF(("dimming screen to %d%% in %d secs\n", dim_pct,
		    dim_timeout));
	if (dim_kbd)
		DPRINTF(("dimming keyboard backlight in %d secs\n",
		    dim_timeout));
	if (use_als)
		DPRINTF(("automatically updating brightness from ALS\n"));

	signal(SIGINT, bail);
	signal(SIGTERM, bail);
	signal(SIGUSR1, sigusr1);
	signal(SIGUSR2, sigusr2);

	/* setup a pipe to wait for messages from signal handlers */
	pipe(pipemsg);

	xloop();

	return 0;
}

void
xloop(void)
{
	XSyncSystemCounter *counters;
	XSyncAlarm idle_alarm = None;
	XSyncAlarm reset_alarm = None;
	int sync_event, error;
	int major, minor, ncounters;
	int i;

	if (XSyncQueryExtension(dpy, &sync_event, &error) != True)
		errx(1, "no sync extension available");

	XSyncInitialize(dpy, &major, &minor);

	counters = XSyncListSystemCounters(dpy, &ncounters);
	for (i = 0; i < ncounters; i++) {
		if (!strcmp(counters[i].name, "IDLETIME")) {
			idler_counter = counters[i].counter;
			break;
		}
	}
	XSyncFreeSystemCounterList(counters);

	if (!idler_counter)
		errx(1, "no idle counter");

	/*
	 * fire an XSyncAlarmNotifyEvent when IDLETIME counter reaches
	 * dim_timeout seconds
	 */
	set_alarm(&idle_alarm, XSyncPositiveComparison);

	for (;;) {
		XEvent e;
		XSyncAlarmNotifyEvent *alarm_e;
		int do_dim = 0, do_brighten = 0;

		DPRINTF(("waiting for next event\n"));

		/* if we're checking an als, only wait 1 second for x event */
		if (XPeekEventOrTimeout(dpy, &e, (use_als ? 1000 : 0)) == 0) {
			if (use_als && !dimmed)
				als_fetch();
			continue;
		}

		if (exiting)
			break;

		if (force_dim) {
			do_dim = force_dim;
		} else if (force_brighten) {
			do_brighten = force_brighten;
		} else {
			XNextEvent(dpy, &e);

			if (!dim_screen && !dim_kbd)
				continue;

			if (e.type != (sync_event + XSyncAlarmNotify)) {
				DPRINTF(("got event of type %d\n", e.type));
				continue;
			}

			alarm_e = (XSyncAlarmNotifyEvent *)&e;

			if (alarm_e->alarm == idle_alarm) {
				DPRINTF(("idle counter reached %dms, dimming\n",
				    XSyncValueLow32(alarm_e->counter_value)));
				do_dim = 1;
			} else if (alarm_e->alarm == reset_alarm) {
				DPRINTF(("idle counter reset, brightening\n"));
				do_brighten = 1;
			}
		}

		if (do_dim && !dimmed) {
			set_alarm(&reset_alarm, XSyncNegativeTransition);

			if (dim_screen)
				backlight = backlight_op(OP_GET, 0);
			if (dim_kbd)
				kbd_backlight = kbd_backlight_op(OP_GET, 0);

			stepper(dim_pct, 0, force_dim ? 1 : dim_steps, 1);
			dimmed = 1;
		} else if (do_brighten && dimmed) {
			if (use_als)
				als_fetch();

			set_alarm(&idle_alarm, XSyncPositiveComparison);

			stepper(backlight, kbd_backlight,
			    force_brighten ? 1 : brighten_steps, 0);
			dimmed = 0;
		}

		force_dim = force_brighten = 0;
	}

	if (dimmed) {
		DPRINTF(("restoring backlight to %f / %f before exiting\n",
		    backlight, kbd_backlight));
		stepper(backlight, kbd_backlight, brighten_steps, 0);
	}
}

void
set_alarm(XSyncAlarm *alarm, XSyncTestType test)
{
	XSyncAlarmAttributes attr;
	XSyncValue value;
	unsigned int flags;
	int64_t cur_idle;

	XSyncQueryCounter(dpy, idler_counter, &value);
	cur_idle = ((int64_t)XSyncValueHigh32(value) << 32) |
	    XSyncValueLow32(value);
	DPRINTF(("cur idle %lld\n", cur_idle));

	attr.trigger.counter = idler_counter;
	attr.trigger.test_type = test;
	attr.trigger.value_type = XSyncRelative;
	XSyncIntsToValue(&attr.trigger.wait_value, dim_timeout * 1000,
	    (unsigned long)(dim_timeout * 1000) >> 32);
	XSyncIntToValue(&attr.delta, 0);

	flags = XSyncCACounter | XSyncCATestType | XSyncCAValue | XSyncCADelta;

	if (*alarm)
		XSyncDestroyAlarm(dpy, *alarm);

	*alarm = XSyncCreateAlarm(dpy, flags, &attr);
}

void
stepper(float new_backlight, float new_kbd_backlight, int steps, int inter)
{
	float tbacklight, tkbd_backlight;
	float step_inc = 0, kbd_step_inc = 0;
	int j;

	if (dim_screen || use_als) {
		tbacklight = backlight_op(OP_GET, 0);
		if (((int)new_backlight != (int)tbacklight))
			step_inc = (new_backlight - tbacklight) / steps;
	}

	if (dim_kbd) {
		tkbd_backlight = kbd_backlight_op(OP_GET, 0);
		if ((int)new_kbd_backlight != (int)tkbd_backlight)
			kbd_step_inc = (new_kbd_backlight - tkbd_backlight) /
			    steps;
	}

	if (!step_inc && !kbd_step_inc)
		return;

	if (dim_screen || use_als)
		DPRINTF(("stepping from %0.2f to %0.2f in increments of %f "
		    "(%d step%s)\n", tbacklight, new_backlight, step_inc, steps,
		    (steps == 1 ? "" : "s")));

	if (dim_kbd)
		DPRINTF(("stepping keyboard from %0.2f to %0.2f in increments "
		    "of %f (%d step%s)\n", tkbd_backlight, new_kbd_backlight,
		    kbd_step_inc, steps, (steps == 1 ? "" : "s")));

	/* discard any stale alarm events */
	XSync(dpy, True);

	for (j = 1; j <= steps; j++) {
		XEvent e;

		if (dim_screen || use_als) {
			if (j == steps)
				tbacklight = new_backlight;
			else
				tbacklight += step_inc;

			backlight_op(OP_SET, tbacklight);
		}

		if (dim_kbd) {
			if (j == steps)
				tkbd_backlight = new_kbd_backlight;
			else
				tkbd_backlight += kbd_step_inc;

			kbd_backlight_op(OP_SET, tkbd_backlight);
		}

		if (inter && XPeekEventOrTimeout(dpy, &e, 1) != 0) {
			DPRINTF(("%s: X event of type %d while stepping, "
			    "breaking early\n", __func__, e.type));
			return;
		}
	}
}

float
backlight_op(int op, float new_backlight)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems;
	unsigned long bytes_after;
	unsigned char *prop;
	XRRPropertyInfo *info;
	long value, to;
	float min, max;
	int i;

	float cur_backlight = -1.0;

	if (backlight_a == None) {
#ifdef __OpenBSD__
		struct wsdisplay_param param;

		if (op == OP_SET)
			DPRINTF(("%s (wscons): set %f\n", __func__,
			    new_backlight));

		param.param = WSDISPLAYIO_PARAM_BRIGHTNESS;
		if (ioctl(wsconsdfd, WSDISPLAYIO_GETPARAM, &param) < 0)
			err(1, "WSDISPLAYIO_GETPARAM failed");

		if (op == OP_SET) {
			param.curval = (float)(param.max - param.min) *
				(new_backlight / 100.0);

			if (param.curval > param.max)
				param.curval = param.max;
			else if (param.curval < param.min)
				param.curval = param.min;

			if (ioctl(wsconsdfd, WSDISPLAYIO_SETPARAM, &param) < 0)
				err(1, "WSDISPLAYIO_SETPARAM failed");
		}

		cur_backlight = ((float)param.curval /
		    (float)(param.max - param.min)) * 100;
#endif
	} else {
		if (op == OP_SET)
			DPRINTF(("%s (xrandr): set %f\n", __func__,
			    new_backlight));

		XRRScreenResources *screen_res =
		    XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
		if (!screen_res)
			errx(1, "no screen resources");

		for (i = 0; i < screen_res->noutput; i++) {
			RROutput output = screen_res->outputs[i];

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
				XSync(dpy, False);
			}
			else
				/* just return the first screen's backlight */
				break;
		}

		XRRFreeScreenResources(screen_res);
	}

	if (op == OP_GET)
		DPRINTF(("%s (xrandr): %f\n", __func__, cur_backlight));

	return cur_backlight;
}

float
kbd_backlight_op(int op, float new_backlight)
{
#ifdef __OpenBSD__
	struct wskbd_backlight param;

	if (ioctl(wsconskfd, WSKBDIO_GETBACKLIGHT, &param) < 0)
		err(1, "WSKBDIO_GETBACKLIGHT failed");

	if (op == OP_SET) {
		DPRINTF(("%s: %f\n", __func__, new_backlight));

		param.curval = (float)(param.max - param.min) *
		    (new_backlight / 100.0);

		if (param.curval > param.max)
			param.curval = param.max;
		else if (param.curval < param.min)
			param.curval = param.min;

		if (ioctl(wsconskfd, WSKBDIO_SETBACKLIGHT, &param) < 0)
			err(1, "WSKBDIO_SETBACKLIGHT failed");
	}

	return ((float)param.curval / (float)(param.max - param.min)) * 100;
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

			DPRINTF(("using als sensor %s\n", sensordev.xname));

			return 1;
		}
	}
#endif

	return 0;
}

void
als_fetch(void)
{
#ifdef __OpenBSD__
	struct sensordev sensordev;
	struct sensor sensor;
	size_t sdlen;
	float lux, tbacklight = backlight, tkbd_backlight = kbd_backlight;
	int i;

	sdlen = sizeof(sensordev);

	if (sysctl(alsmib, 5, &sensor, &sdlen, NULL, 0) == -1) {
		warn("sysctl");
		return;
	}

	lux = sensor.value / 1000000.0;

	if ((int)als < 0) {
		als = lux;
	} else if (abs((int)lux - (int)als) < 10) {
		als = lux;
		return;
	} else {
		DPRINTF(("als lux change %f -> %f, screen: %f, kbd: %f\n", als,
		    lux, backlight, kbd_backlight));
	}

	for (i = (sizeof(als_settings) / sizeof(struct als_setting)) - 1;
	    i >= 0; i--) {
		struct als_setting as = als_settings[i];

		if (lux < as.min_lux)
			continue;

		DPRINTF(("using lux profile %s\n", as.label));

		if (dim_kbd && ((int)round(kbd_backlight) != as.kbd_backlight)) {
			DPRINTF(("als: adjusting keyboard backlight from %d%% "
			    "to %d%%\n", (int)round(kbd_backlight),
			    as.kbd_backlight));
			tkbd_backlight = as.kbd_backlight;
		}

		if ((int)round(backlight) != as.backlight) {
			DPRINTF(("als: adjusting screen backlight from %d%% "
			    "to %d%%\n", (int)round(backlight), as.backlight));
			tbacklight = as.backlight;
		}

		if ((int)round(kbd_backlight) != tkbd_backlight ||
		    (int)round(backlight) != tbacklight)
			stepper(tbacklight, tkbd_backlight, dim_steps, 0);

		/* become our new normal */
		backlight = tbacklight;
		kbd_backlight = tkbd_backlight;

		setproctitle("%s", as.label);

		break;
	}

	als = lux;
#endif
}

void
usage(void)
{
	fprintf(stderr, "usage: %s [-adkn] [-b brighten steps] [-p dim pct] "
	    "[-s dim steps] [-t timeout secs]\n", __progname);
	exit(1);
}

void
bail(int sig)
{
	int msg = MSG_EXIT;

	if (exiting)
		exit(0);

	/*
	 * Doing X ops inside a signal handler causes an infinite loop in
	 * _XReply/xcb, so we can't properly brighten and exit ourselves.
	 * write to the pipe setup earlier so our event loop will see it upon
	 * polling.
	 */
	DPRINTF(("got signal %d, trying to exit\n", sig));
	write(pipemsg[1], &msg, 1);
	exiting = 1;
}

void
sigusr1(int sig)
{
	int msg = MSG_DIM;

	DPRINTF(("got signal %d, forcing dim\n", sig));
	write(pipemsg[1], &msg, 1);
}

void
sigusr2(int sig)
{
	int msg = MSG_BRIGHTEN;

	DPRINTF(("got signal %d, forcing brighten\n", sig));
	write(pipemsg[1], &msg, 1);
}

int
XPeekEventOrTimeout(Display *dpy, XEvent *e, unsigned int msecs)
{
	struct pollfd pfd[2];
	int msg = 0;

	while (!XPending(dpy)) {
		memset(&pfd, 0, sizeof(pfd));
		pfd[0].fd = ConnectionNumber(dpy);
		pfd[0].events = POLLIN;
		pfd[1].fd = pipemsg[0];
		pfd[1].events = POLLIN;

		switch (poll(pfd, 2, msecs == 0 ? INFTIM : msecs)) {
		case -1:
			/* signal, maybe exit handler, we'll loop again */
			DPRINTF(("poll returned -1 for errno %d\n", errno));
			break;
		case 0:
			/* timed out */
			return 0;
		default:
			if (pfd[1].revents) {
				read(pipemsg[0], &msg, 1);
				switch (msg) {
				case MSG_EXIT:
					DPRINTF(("%s: got pipe message: exit\n",
					    __func__));
					exiting = 1;
					break;
				case MSG_DIM:
					DPRINTF(("%s: got pipe message: dim\n",
					    __func__));
					force_dim = 1;
					break;
				case MSG_BRIGHTEN:
					DPRINTF(("%s: got pipe message: "
					    "brighten\n", __func__));
					force_brighten = 1;
					break;
				default:
					DPRINTF(("%s: junk on msg pipe: 0x%x\n",
					    __func__, msg));
				}
				return 1;
			} else if (pfd[0].revents) {
				DPRINTF(("%s: got X event\n", __func__));
				XPeekEvent(dpy, e);
				return 1;
			}
		}
	}

	XPeekEvent(dpy, e);
	return 1;
}
