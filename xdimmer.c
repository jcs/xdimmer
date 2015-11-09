/*
 * xdimmer
 * Copyright (c) 2013, 2015 joshua stein <jcs@jcs.org>
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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/Xrandr.h>

#define DEFAULT_DIM_TIMEOUT	120
#define DEFAULT_DIM_PERCENTAGE	10

#define DIM_STEPS		20
#define BRIGHTEN_STEPS		5

void xloop(void);
void set_alarm(XSyncAlarm *, XSyncCounter, XSyncTestType, XSyncValue);
void dim(void);
void brighten(void);
void bail(int);
double backlight_op(int, double, int);
void usage(void);

extern char *__progname;

static Atom backlight_a = 0;
static double backlight = -1;
static int dimmed = 0;
static int debug = 0;
static int exiting = 0;

static int dim_timeout = DEFAULT_DIM_TIMEOUT;
static int dim_pct = DEFAULT_DIM_PERCENTAGE;

static Display *dpy;

int
main(int argc, char *argv[])
{
	int ch;

	while ((ch = getopt(argc, argv, "dp:t:")) != -1) {
		const char *errstr;

		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'p':
			dim_pct = strtonum(optarg, 1, 100, &errstr);
			if (errstr)
				errx(2, "dim percentage: %s", errstr);
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

	if (!(dpy = XOpenDisplay(NULL)))
		errx(1, "can't open display %s", XDisplayName(NULL));

#ifdef __OpenBSD__
	if (pledge("stdio", NULL) == -1)
		err(1, "pledge");
#endif

	backlight_a = XInternAtom(dpy, RR_PROPERTY_BACKLIGHT, True);
	if (backlight_a == None)
		errx(1, "no backlight control");

	if (debug)
		printf("dimming to %d%% in %d secs\n", dim_pct, dim_timeout);

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

	for (;;) {
		XEvent e;
		XSyncAlarmNotifyEvent *alarm_e;
		int overflow;
		XSyncValue add, plusone;

		if (debug)
			printf("waiting for next event\n");

		XNextEvent(dpy, &e);

		if (debug)
			printf("got event of type %d\n", e.type);

		if (exiting) {
			brighten();
			exit(0);
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
	backlight = backlight_op(0, 0, 0);
	if (backlight > dim_pct) {
		if (debug)
			printf("dimming to %d\n", dim_pct);

		backlight = backlight_op(1, dim_pct, DIM_STEPS);
		dimmed = 1;
	}
	else if (debug)
		printf("backlight already at %f, not dimming to %d\n",
		    backlight, dim_pct);
}

void
brighten(void)
{
	if (backlight == -1)
		backlight = backlight_op(0, 0, 0);

	if (backlight > 0) {
		if (debug)
			printf("brightening back to %f\n", backlight);

		backlight_op(1, backlight, BRIGHTEN_STEPS);
	}
	else if (debug)
		printf("no previous backlight setting, not brightening\n");

	dimmed = 0;
}

double
backlight_op(int set, double new_backlight, int steps)
{
	Atom actual_type;
	int actual_format;
	unsigned long nitems;
	unsigned long bytes_after;
	unsigned char *prop;
	XRRPropertyInfo *info;
	long value, to;
	double min, max;
	int step_inc, i;

	double cur_backlight = -1.0;

	XRRScreenResources *screen_res = XRRGetScreenResources(dpy,
	    DefaultRootWindow(dpy));
	if (!screen_res)
		errx(1, "no screen resources");

	for (i = 0; i < screen_res->noutput; i++) {
		RROutput output = screen_res->outputs[i];

		/* yay magic numbers */

		if (XRRGetOutputProperty(dpy, output, backlight_a,
		    0, 4, False, False, None, &actual_type, &actual_format,
		    &nitems, &bytes_after, &prop) != Success)
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

		if (set) {
			int j;

			to = min + ((new_backlight * (max - min)) / 100);
			if (to < min)
				to = min;
			if (to > max)
				to = max;

			if (to == value)
				steps = 0;
			else
				step_inc = (to - value) / steps;

			if (debug)
				printf("stepping from %ld to %ld in "
				    "increments of %d (%d step%s)... ",
				    value, to, step_inc, steps,
				    (steps == 1 ? "" : "s"));

			for (j = 1; j <= steps; j++) {
				value += step_inc;

				if (j == steps)
					value = to;

				if (debug)
					printf(" %ld", value);

				XRRChangeOutputProperty(dpy, output,
				    backlight_a, XA_INTEGER, 32,
				    PropModeReplace,
				    (unsigned char *)&value, 1);
				XSync(dpy, True);

				if (j < steps && step_inc < 0)
					usleep(15000);
			}

			if (debug)
				printf("\n");
		}
		else
			/* just return the first screen's backlight */
			break;
	}

	XRRFreeScreenResources(screen_res);

	return cur_backlight;
}

void
usage(void)
{
	fprintf(stderr, "usage: %s [-d] [-p dim pct] [-t timeout secs]\n",
	    __progname);
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
