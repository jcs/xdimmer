XDIMMER(1) - General Commands Manual

# NAME

**xdimmer** - dim the screen and/or the keyboard backlight when idle or when ambient
light changes

# SYNOPSIS

**xdimmer**
\[**-a**]
\[**-d**]
\[**-k**]
\[**-n**]
\[**-p**&nbsp;*percent*]
\[**-s**&nbsp;*steps*]
\[**-t**&nbsp;*timeout*]

# DESCRIPTION

**xdimmer**
waits
*timeout*
number of seconds and if no keyboard or mouse input is detected, the screen
backlight is dimmed to
*percent*
in
*steps*
steps, unless the
*-n*
option was specified.
Once keyboard or mouse input is detected, the screen backlight is restored
to its previous brightness value.

On OpenBSD, if the
*-k*
option is used, the keyboard backlight is also dimmed to zero and restored
upon movement via the
wscons(4)
interface.

Also on OpenBSD, if the
*-a*
option is used and an ambient light sensor device is located via
sysctl(8),
screen backlight (and keyboard backlight if
*-k*
is used) is dimmed or brightened based on lux readings.

# OPTIONS

**-a**

> Change backlights according to ambient light sensor lux readings.
> Currently only supported on OpenBSD.

**-d**

> Print debugging messages to stdout.

**-k**

> Affect the keyboard backlight as well as the screen backlight.
> Currently only supported on OpenBSD.

**-n**

> Do not adjust the screen backlight (can only be used if
> *-k*
> is used).

**-p** *percent*

> Absolute brightness value to which the backlight is dimmed.
> The default is
> `10`
> percent.

**-t** *steps*

> Number of steps to take while decrementing backlight.
> The default is
> `20`
> steps.

**-t** *timeout*

> Number of seconds to wait without receiving input before dimming.
> The default is
> `120`
> seconds.

# AUTHORS

**xdimmer**
was written by
joshua stein &lt;[jcs@jcs.org](mailto:jcs@jcs.org)&gt;.

OpenBSD 6.1 - April 2, 2017
