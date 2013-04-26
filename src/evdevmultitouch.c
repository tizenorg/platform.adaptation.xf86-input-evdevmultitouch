/*
 *
 * xserver-xorg-input-evdev-multitouch
 *
 * Contact: Sung-Jin Park <sj76.park@samsung.com>
 *          Sangjin LEE <lsj119@samsung.com>
 *
 * Copyright (c) 2000 - 2011 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Copyright © 2004-2008 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *	Kristian Høgsberg (krh@redhat.com)
 *	Adam Jackson (ajax@redhat.com)
 *	Peter Hutterer (peter.hutterer@redhat.com)
 *	Oliver McFadden (oliver.mcfadden@nokia.com)
 *	Benjamin Tissoires (tissoire@cena.fr)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/keysym.h>

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <xf86.h>
#include <xf86Xinput.h>
#include <xorg/input.h>
#include <xorg/inputstr.h>
#include <xorg/optionstr.h>
#include <exevents.h>
#include <xorgVersion.h>
#include <xkbsrv.h>

#include "evdevmultitouch.h"

#ifdef HAVE_PROPERTIES
#include <X11/Xatom.h>
#include <evdevmultitouch-properties.h>
#include <xserver-properties.h>
/* 1.6 has properties, but no labels */
#ifdef AXIS_LABEL_PROP
#define HAVE_LABELS
#else
#undef HAVE_LABELS
#endif

#endif

#ifndef MAXDEVICES
#include <inputstr.h> /* for MAX_DEVICES */
#define MAXDEVICES MAX_DEVICES
#endif

/* 2.4 compatibility */
#ifndef EVIOCGRAB
#define EVIOCGRAB _IOW('E', 0x90, int)
#endif

#ifndef BTN_TASK
#define BTN_TASK 0x117
#endif

#ifndef EV_SYN
#define EV_SYN EV_RST
#endif
/* end compat */

#define ArrayLength(a) (sizeof(a) / (sizeof((a)[0])))

#ifndef True
#define True	TRUE
#endif
#ifndef False
#define False FALSE
#endif

#define MAX_MT	10

#define POLL_DISABLE	'0'
#define POLL_ENABLE		'1'
#define POLL_REQUEST	'2'
#define SYSFS_POLL	"/sys/class/input/event2/device/poll"

#define ABS_MT_SLOT             0x2f
#define ABS_MT_TOUCH_MAJOR	0x30	/* Major axis of touching ellipse */
#define ABS_MT_TOUCH_MINOR	0x31	/* Minor axis (omit if circular) */
#define ABS_MT_WIDTH_MAJOR	0x32	/* Major axis of approaching ellipse */
#define ABS_MT_WIDTH_MINOR	0x33	/* Minor axis (omit if circular) */
#define ABS_MT_ORIENTATION	0x34	/* Ellipse orientation */
#define ABS_MT_POSITION_X	0x35	/* Center X ellipse position */
#define ABS_MT_POSITION_Y	0x36	/* Center Y ellipse position */
#define ABS_MT_TOOL_TYPE	0x37	/* Type of touching device */
#define ABS_MT_BLOB_ID		0x38	/* Group a set of packets as a blob */
#define ABS_MT_TRACKING_ID		0x39	/* Unique ID of initiated contact */

#define SYN_REPORT		0
#define SYN_CONFIG		1
#define SYN_MT_REPORT		2

/* evdevmultitouch flags */
#define EVDEVMULTITOUCH_KEYBOARD_EVENTS	(1 << 0)
#define EVDEVMULTITOUCH_BUTTON_EVENTS	(1 << 1)
#define EVDEVMULTITOUCH_RELATIVE_EVENTS	(1 << 2)
#define EVDEVMULTITOUCH_ABSOLUTE_EVENTS	(1 << 3)
#define EVDEVMULTITOUCH_TOUCHPAD		(1 << 4)
#define EVDEVMULTITOUCH_INITIALIZED	(1 << 5) /* WheelInit etc. called already? */
#define EVDEVMULTITOUCH_TOUCHSCREEN	(1 << 6)
#define EVDEVMULTITOUCH_CALIBRATED	(1 << 7) /* run-time calibrated? */
#define EVDEVMULTITOUCH_TABLET		(1 << 8) /* device looks like a tablet? */
#define EVDEVMULTITOUCH_UNIGNORE_ABSOLUTE (1 << 9) /* explicitly unignore abs axes */
#define EVDEVMULTITOUCH_UNIGNORE_RELATIVE (1 << 10) /* explicitly unignore rel axes */
#define EVDEVMULTITOUCH_MULTITOUCH (1 << 11) /* device looks like a multi-touch screen? */
#define EVDEVMULTITOUCH_RESOLUTION (1 << 12) /* device has a resolution setting? */

#define MIN_KEYCODE 8
#define GLYPHS_PER_KEY 2
#define AltMask		Mod1Mask
#define NumLockMask	Mod2Mask
#define AltLangMask	Mod3Mask
#define KanaMask	Mod4Mask
#define ScrollLockMask	Mod5Mask

#define CAPSFLAG	1
#define NUMFLAG		2
#define SCROLLFLAG	4
#define MODEFLAG	8
#define COMPOSEFLAG	16

static const char *evdevmultitouchDefaults[] = {
    "XkbRules",     "evdevmultitouch",
    "XkbModel",     "evdevmultitouch",
    "XkbLayout",    "us",
    NULL
};

#ifdef _F_GESTURE_EXTENSION_
extern void mieqEnqueue(DeviceIntPtr pDev, InternalEvent *e);
static void EvdevMultitouchFrameSync(InputInfoPtr pInfo, MTSyncType sync);
#endif//_F_GESTURE_EXTENSION_
static void EvdevMultitouchOff(DeviceIntPtr device);
static int EvdevMultitouchOn(DeviceIntPtr);
static int EvdevMultitouchCacheCompare(InputInfoPtr pInfo, BOOL compare);
static void EvdevMultitouchKbdCtrl(DeviceIntPtr device, KeybdCtrl *ctrl);
static void EvdevMultitouchProcessSyncEvent(InputInfoPtr pInfo, struct input_event *ev);
static void EvdevMultitouchCopyFromData(InputInfoPtr pInfo, EvdevMultitouchDataMTPtr pData);
static void EvdevMultitouchReinitPEvdevMultitouch(InputInfoPtr pInfo);
static void EvdevMultitouchFakeOmittedEvents(InputInfoPtr pInfo);
static void EvdevMultitouchProcessEvent(InputInfoPtr pInfo, struct input_event *ev);
static void EvdevMultitouchEndOfMultiTouch(InputInfoPtr pInfo,EvdevMultitouchDataMTPtr pData);
static void EvdevMultitouchSetMultitouch(InputInfoPtr pInfo, int num_multitouch);
static void EvdevMultitouchGetGrabInfo(InputInfoPtr pInfo, BOOL val);
static void EvdevMultitouchSetResolution(InputInfoPtr pInfo, int num_resolution, int resolution[4]);
static void EvdevMultitouchSetCalibration(InputInfoPtr pInfo, int num_calibration, int calibration[4]);
static InputInfoPtr EvdevMultitouchCreateSubDevice(InputInfoPtr pInfo, int id);
static void EvdevMultitouchDeleteSubDevice(InputInfoPtr pInfo, InputInfoPtr subdev);
static void EvdevMultitouchSwapAxes(EvdevMultitouchPtr pEvdevMultitouch);
static void EvdevMultitouchSetTransform(InputInfoPtr pInfo, int num_transform, float *tmatrix);

#ifdef HAVE_PROPERTIES
static void EvdevMultitouchInitAxesLabels(EvdevMultitouchPtr pEvdevMultitouch, int natoms, Atom *atoms);
static void EvdevMultitouchInitButtonLabels(EvdevMultitouchPtr pEvdevMultitouch, int natoms, Atom *atoms);
static void EvdevMultitouchInitProperty(DeviceIntPtr dev);
static int EvdevMultitouchSetProperty(DeviceIntPtr dev, Atom atom,
                            XIPropertyValuePtr val, BOOL checkonly);
static Atom prop_invert = 0;
static Atom prop_reopen = 0;
static Atom prop_calibration = 0;
static Atom prop_swap = 0;
static Atom prop_axis_label = 0;
static Atom prop_btn_label = 0;
static Atom prop_tracking_id = 0;
static Atom prop_multitouch = 0;
static Atom prop_transform = 0;
static Atom prop_grabinfo = 0;
#endif

int g_pressed = 0;
static InputInfoPtr pCreatorInfo = NULL;

/* All devices the evdevmultitouch driver has allocated and knows about.
 * MAXDEVICES is safe as null-terminated array, as two devices (VCP and VCK)
 * cannot be used by evdevmultitouch, leaving us with a space of 2 at the end. */
static EvdevMultitouchPtr evdevmultitouch_devices[MAX_MT] = {NULL,};

static size_t EvdevMultitouchCountBits(unsigned long *array, size_t nlongs)
{
    unsigned int i;
    size_t count = 0;

    for (i = 0; i < nlongs; i++) {
        unsigned long x = array[i];

        while (x > 0)
        {
            count += (x & 0x1);
            x >>= 1;
        }
    }
    return count;
}

static int
EvdevMultitouchGetMajorMinor(InputInfoPtr pInfo)
{
    struct stat st;

    if (fstat(pInfo->fd, &st) == -1)
    {
        xf86Msg(X_ERROR, "%s: stat failed (%s). cannot check for duplicates.\n",
                pInfo->name, strerror(errno));
        return 0;
    }

    return st.st_rdev;
}

static BOOL
EvdevMultitouchIsCoreDevice(InputInfoPtr pInfo) {
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    return pEvdevMultitouch->core_device == pInfo;
}

/**
 * Return TRUE if one of the devices we know about has the same min/maj
 * number.
 */
static BOOL
EvdevMultitouchIsDuplicate(InputInfoPtr pInfo)
{
    int i;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    EvdevMultitouchPtr dev;

    if (pEvdevMultitouch->min_maj && dev)
    {
        for(i = 0 ; i < MAX_MT ; i++)
        {
        	dev = evdevmultitouch_devices[i];
        	if(dev)
    		{
			if ((dev) != pEvdevMultitouch &&
			     (dev)->min_maj &&
			     (dev)->min_maj == pEvdevMultitouch->min_maj)
				return TRUE;
    		}
        }
    }
    return FALSE;
}

/**
 * Add to internal device list.
 */
static void
EvdevMultitouchAddDevice(InputInfoPtr pInfo)
{
    int i;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    EvdevMultitouchPtr dev;

    for(i = 0 ; i < MAX_MT ; i++)
    {
    	dev = evdevmultitouch_devices[i];
	if(!dev)
	{
		evdevmultitouch_devices[i] = pEvdevMultitouch;
		return;
	}
    }
}

/**
 * Remove from internal device list.
 */
static void
EvdevMultitouchRemoveDevice(InputInfoPtr pInfo)
{
    int i;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    EvdevMultitouchPtr dev;

    for( i = 0 ; i < MAX_MT ; i++ )
    {
    	dev = evdevmultitouch_devices[i];
	if((dev) && (dev == pEvdevMultitouch))
	{
		evdevmultitouch_devices[i] = NULL;
		break;
	}
    }
}


static void
SetXkbOption(InputInfoPtr pInfo, char *name, char **option)
{
    char *s;

    if ((s = xf86SetStrOption(pInfo->options, name, NULL))) {
        if (!s[0]) {
            free(s);
            *option = NULL;
        } else {
            *option = s;
        }
    }
}

static int wheel_up_button = 4;
static int wheel_down_button = 5;
static int wheel_left_button = 6;
static int wheel_right_button = 7;

void
EvdevMultitouchQueueKbdEvent(InputInfoPtr pInfo, struct input_event *ev, int value)
{
    int code = ev->code + MIN_KEYCODE;
    static char warned[KEY_CNT];
    EventQueuePtr pQueue;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    /* Filter all repeated events from device.
       We'll do softrepeat in the server, but only since 1.6 */
    if (value == 2
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) <= 2
        && (ev->code == KEY_LEFTCTRL || ev->code == KEY_RIGHTCTRL ||
            ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT ||
            ev->code == KEY_LEFTALT || ev->code == KEY_RIGHTALT ||
            ev->code == KEY_LEFTMETA || ev->code == KEY_RIGHTMETA ||
            ev->code == KEY_CAPSLOCK || ev->code == KEY_NUMLOCK ||
            ev->code == KEY_SCROLLLOCK) /* XXX windows keys? */
#endif
            )
	return;

    if (code > 255)
    {
        if (ev->code <= KEY_MAX && !warned[ev->code])
        {
            xf86Msg(X_WARNING, "%s: unable to handle keycode %d\n",
                    pInfo->name, ev->code);
            warned[ev->code] = 1;
        }

        /* The X server can't handle keycodes > 255. */
        return;
    }

    if (pEvdevMultitouch->num_queue >= EVDEVMULTITOUCH_MAXQUEUE)
    {
        xf86Msg(X_NONE, "%s: dropping event due to full queue!\n", pInfo->name);
        return;
    }

    pQueue = &pEvdevMultitouch->queue[pEvdevMultitouch->num_queue];
    pQueue->type = EV_QUEUE_KEY;
    pQueue->key = code;
    pQueue->val = value;
    pEvdevMultitouch->num_queue++;
}

void
EvdevMultitouchQueueButtonEvent(InputInfoPtr pInfo, int button, int value)
{
    EventQueuePtr pQueue;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    if (pEvdevMultitouch->num_queue >= EVDEVMULTITOUCH_MAXQUEUE)
    {
        xf86Msg(X_NONE, "%s: dropping event due to full queue!\n", pInfo->name);
        return;
    }

    pQueue = &pEvdevMultitouch->queue[pEvdevMultitouch->num_queue];
    pQueue->type = EV_QUEUE_BTN;
    pQueue->key = button;
    pQueue->val = value;
    pEvdevMultitouch->num_queue++;
}

/**
 * Post button event right here, right now.
 * Interface for MB emulation since these need to post immediately.
 */
void
EvdevMultitouchPostButtonEvent(InputInfoPtr pInfo, int button, int value)
{
    xf86PostButtonEvent(pInfo->dev, 0, button, value, 0, 0);
}

void
EvdevMultitouchQueueButtonClicks(InputInfoPtr pInfo, int button, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        EvdevMultitouchQueueButtonEvent(pInfo, button, 1);
        EvdevMultitouchQueueButtonEvent(pInfo, button, 0);
    }
}

/**
 * 
 */
static CARD32
EvdevMultitouchSubdevTimer(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr pInfo = (InputInfoPtr)arg;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    int i;
    
    for (i=0;i<pEvdevMultitouch->num_multitouch;i++) {
        if (pEvdevMultitouch->vals_mt[i].containsValues) {
            EvdevMultitouchEndOfMultiTouch(pInfo, &(pEvdevMultitouch->vals_mt[i]));
        }
    }
    
    return 0;
    //return pEvdevMultitouch->timeout; /* come back in 100 ms */
}

/**
 * Coming back from resume may leave us with a file descriptor that can be
 * opened but fails on the first read (ENODEV).
 * In this case, try to open the device until it becomes available or until
 * the predefined count expires.
 */
static CARD32
EvdevMultitouchReopenTimer(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr pInfo = (InputInfoPtr)arg;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    do {
        pInfo->fd = open(pEvdevMultitouch->device, O_RDWR | O_NONBLOCK, 0);
    } while (pInfo->fd < 0 && errno == EINTR);

    if (pInfo->fd != -1)
    {
        if (EvdevMultitouchCacheCompare(pInfo, TRUE) == Success)
        {
            xf86Msg(X_INFO, "%s: Device reopened after %d attempts.\n", pInfo->name,
                    pEvdevMultitouch->reopen_attempts - pEvdevMultitouch->reopen_left + 1);
            EvdevMultitouchOn(pInfo->dev);
        } else
        {
            xf86Msg(X_ERROR, "%s: Device has changed - disabling.\n",
                    pInfo->name);
            xf86DisableDevice(pInfo->dev, FALSE);
            close(pInfo->fd);
            pInfo->fd = -1;
            pEvdevMultitouch->min_maj = 0; /* don't hog the device */
        }
        pEvdevMultitouch->reopen_left = 0;
        return 0;
    }

    pEvdevMultitouch->reopen_left--;

    if (!pEvdevMultitouch->reopen_left)
    {
        xf86Msg(X_ERROR, "%s: Failed to reopen device after %d attempts.\n",
                pInfo->name, pEvdevMultitouch->reopen_attempts);
        xf86DisableDevice(pInfo->dev, FALSE);
        pEvdevMultitouch->min_maj = 0; /* don't hog the device */
        return 0;
    }

    return 100; /* come back in 100 ms */
}

static CARD32
EvdevMultitouchMultitouchSettingTimer(OsTimerPtr timer, CARD32 time, pointer arg)
{
    InputInfoPtr pInfo = (InputInfoPtr)arg;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    int n_multitouch = xf86SetIntOption(pInfo->options, "MultiTouch", 0);

    if( n_multitouch >= 2 )
	    EvdevMultitouchSetMultitouch(pInfo, n_multitouch);
    pEvdevMultitouch->multitouch_setting_timer = TimerSet(pEvdevMultitouch->multitouch_setting_timer, 0, 0, NULL, NULL);
	
    return 0;
}

#define ABS_X_VALUE 0x1
#define ABS_Y_VALUE 0x2
#define ABS_VALUE   0x4
#define ABS_MT_X_VALUE   0x8
#define ABS_MT_Y_VALUE   0x10
#define ABS_MT_TOUCH_MAJOR_VALUE 0x20
/**
 * Take the valuators and process them accordingly.
 */
static void
EvdevMultitouchProcessValuators(InputInfoPtr pInfo, int v[MAX_VALUATORS], int *num_v,
                      int *first_v)
{
    int tmp;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    pixman_vector_t p;

    *num_v = *first_v = 0;

    /* convert to relative motion for touchpads */
    if (pEvdevMultitouch->abs && (pEvdevMultitouch->flags & EVDEVMULTITOUCH_TOUCHPAD)) {
        if (pEvdevMultitouch->tool) { /* meaning, touch is active */
            if (pEvdevMultitouch->old_vals[0] != -1)
                pEvdevMultitouch->delta[REL_X] = pEvdevMultitouch->vals[0] - pEvdevMultitouch->old_vals[0];
            if (pEvdevMultitouch->old_vals[1] != -1)
                pEvdevMultitouch->delta[REL_Y] = pEvdevMultitouch->vals[1] - pEvdevMultitouch->old_vals[1];
            if (pEvdevMultitouch->abs & ABS_X_VALUE)
                pEvdevMultitouch->old_vals[0] = pEvdevMultitouch->vals[0];
            if (pEvdevMultitouch->abs & ABS_Y_VALUE)
                pEvdevMultitouch->old_vals[1] = pEvdevMultitouch->vals[1];
        } else {
            pEvdevMultitouch->old_vals[0] = pEvdevMultitouch->old_vals[1] = -1;
        }
        pEvdevMultitouch->abs = 0;
        pEvdevMultitouch->rel = 1;
    }

    if (pEvdevMultitouch->rel) {
        int first = REL_CNT, last = 0;
        int i;

        if (pEvdevMultitouch->swap_axes) {
            tmp = pEvdevMultitouch->delta[REL_X];
            pEvdevMultitouch->delta[REL_X] = pEvdevMultitouch->delta[REL_Y];
            pEvdevMultitouch->delta[REL_Y] = tmp;
        }
        if (pEvdevMultitouch->invert_x)
            pEvdevMultitouch->delta[REL_X] *= -1;
        if (pEvdevMultitouch->invert_y)
            pEvdevMultitouch->delta[REL_Y] *= -1;

        for (i = 0; i < REL_CNT; i++)
        {
            int map = pEvdevMultitouch->axis_map[i];
            if (map != -1)
            {
                v[map] = pEvdevMultitouch->delta[i];
                if (map < first)
                    first = map;
                if (map > last)
                    last = map;
            }
        }

        *num_v = (last - first + 1);
        *first_v = first;
    }
    /*
     * Some devices only generate valid abs coords when BTN_DIGI is
     * pressed.  On wacom tablets, this means that the pen is in
     * proximity of the tablet.  After the pen is removed, BTN_DIGI is
     * released, and a (0, 0) absolute event is generated.  Checking
     * pEvdevMultitouch->digi here, lets us ignore that event.  pEvdevMultitouch is
     * initialized to 1 so devices that doesn't use this scheme still
     * just works.
     */
    else if (pEvdevMultitouch->abs && pEvdevMultitouch->tool) {
        memcpy(v, pEvdevMultitouch->vals, sizeof(int) * pEvdevMultitouch->num_vals);
        if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_CALIBRATED)
        {
            v[0] = xf86ScaleAxis(v[0],
                    pEvdevMultitouch->absinfo[ABS_X].maximum,
                    pEvdevMultitouch->absinfo[ABS_X].minimum,
                    pEvdevMultitouch->calibration.max_x, pEvdevMultitouch->calibration.min_x);
            v[1] = xf86ScaleAxis(v[1],
                    pEvdevMultitouch->absinfo[ABS_Y].maximum,
                    pEvdevMultitouch->absinfo[ABS_Y].minimum,
                    pEvdevMultitouch->calibration.max_y, pEvdevMultitouch->calibration.min_y);
        }

        if (pEvdevMultitouch->swap_axes) {
            int tmp = v[0];
            v[0] = v[1];
            v[1] = tmp;
        }

        if (pEvdevMultitouch->invert_x)
            v[0] = (pEvdevMultitouch->absinfo[ABS_X].maximum - v[0] +
                    pEvdevMultitouch->absinfo[ABS_X].minimum);
        if (pEvdevMultitouch->invert_y)
            v[1] = (pEvdevMultitouch->absinfo[ABS_Y].maximum - v[1] +
                    pEvdevMultitouch->absinfo[ABS_Y].minimum);

 	if( pEvdevMultitouch->use_transform )
	{
		p.vector[0] = pixman_int_to_fixed(v[0]);
		p.vector[1] = pixman_int_to_fixed(v[1]);
		p.vector[2] = pixman_int_to_fixed(1);

		pixman_transform_point(&pEvdevMultitouch->inv_transform, &p);

		v[0] = pixman_fixed_to_int(p.vector[0]);
		v[1] = pixman_fixed_to_int(p.vector[1]);
	}

        *num_v = pEvdevMultitouch->num_vals;
        *first_v = 0;
    }
}

/**
 * Take a button input event and process it accordingly.
 */
static void
EvdevMultitouchProcessButtonEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    unsigned int button;
    int value;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    button = EvdevMultitouchUtilButtonEventToButtonNumber(pEvdevMultitouch, ev->code);

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;

    /* Handle drag lock */
    if (EvdevMultitouchDragLockFilterEvent(pInfo, button, value))
        return;

    if (EvdevMultitouchWheelEmuFilterButton(pInfo, button, value))
        return;

    if (pEvdevMultitouch->num_multitouch)
        return;
    
    if (EvdevMultitouchMBEmuFilterEvent(pInfo, button, value))
        return;

    if (button)
        EvdevMultitouchQueueButtonEvent(pInfo, button, value);
    else
        EvdevMultitouchQueueKbdEvent(pInfo, ev, value);
}

/**
 * Take the relative motion input event and process it accordingly.
 */
static void
EvdevMultitouchProcessRelativeMotionEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    static int value;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;

    pEvdevMultitouch->rel = 1;

    switch (ev->code) {
        case REL_WHEEL:
            if (value > 0)
                EvdevMultitouchQueueButtonClicks(pInfo, wheel_up_button, value);
            else if (value < 0)
                EvdevMultitouchQueueButtonClicks(pInfo, wheel_down_button, -value);
            break;

        case REL_DIAL:
        case REL_HWHEEL:
            if (value > 0)
                EvdevMultitouchQueueButtonClicks(pInfo, wheel_right_button, value);
            else if (value < 0)
                EvdevMultitouchQueueButtonClicks(pInfo, wheel_left_button, -value);
            break;

        /* We don't post wheel events as axis motion. */
        default:
            /* Ignore EV_REL events if we never set up for them. */
            if (!(pEvdevMultitouch->flags & EVDEVMULTITOUCH_RELATIVE_EVENTS))
                return;

            /* Handle mouse wheel emulation */
            if (EvdevMultitouchWheelEmuFilterMotion(pInfo, ev))
                return;

            pEvdevMultitouch->delta[ev->code] += value;
            break;
    }
}

/**
 * Take the absolute motion input event and process it accordingly.
 */
static void
EvdevMultitouchProcessAbsoluteMotionEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    static int value;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;

    /* Ignore EV_ABS events if we never set up for them. */
    if (!(pEvdevMultitouch->flags & EVDEVMULTITOUCH_ABSOLUTE_EVENTS))
        return;

    if (ev->code > ABS_MAX)
        return;

    pEvdevMultitouch->vals[pEvdevMultitouch->axis_map[ev->code]] = value;

    if(pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH)
    {
        if(ev->code == ABS_MT_TOUCH_MAJOR)
    	 {
#ifdef _DEBUG_MT_SEQUENCE_
	     ErrorF("[AbsoluteMotionEvent] ABS_MT_TOUCH_MAJOR (value=%d)\n", value);
#endif
            pEvdevMultitouch->abs |= ABS_MT_TOUCH_MAJOR_VALUE;
    	 }
        else if (ev->code == ABS_MT_POSITION_X)
    	 {
#ifdef _DEBUG_MT_SEQUENCE_
	     ErrorF("[AbsoluteMotionEvent] ABS_MT_POSITION_X (value=%d)\n", value);
#endif
	     EvdevMultitouchFakeOmittedEvents(pInfo);
            pEvdevMultitouch->abs |= ABS_MT_X_VALUE;
    	 }
        else if (ev->code == ABS_MT_POSITION_Y)
    	 {
#ifdef _DEBUG_MT_SEQUENCE_
	     ErrorF("[AbsoluteMotionEvent] ABS_MT_POSITION_Y (value=%d)\n", value);
#endif
	     EvdevMultitouchFakeOmittedEvents(pInfo);
            pEvdevMultitouch->abs |= ABS_MT_Y_VALUE;
    	 }
        else
            pEvdevMultitouch->abs |= ABS_VALUE;
    }
    else
    {
        if (ev->code == ABS_X)
    	 {
#ifdef _DEBUG_MT_SEQUENCE_
	     ErrorF("[AbsoluteMotionEvent] ABS_X (value=%d)\n", value);
#endif
            pEvdevMultitouch->abs |= ABS_X_VALUE;
    	 }
        else if (ev->code == ABS_Y)
    	 {
#ifdef _DEBUG_MT_SEQUENCE_
	     ErrorF("[AbsoluteMotionEvent] ABS_Y (value=%d)\n", value);
#endif
            pEvdevMultitouch->abs |= ABS_Y_VALUE;
    	 }
        else
            pEvdevMultitouch->abs |= ABS_VALUE;
    }        
}

/**
 * Take the key press/release input event and process it accordingly.
 */
static void
EvdevMultitouchProcessKeyEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    static int value;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    if(pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH)
        return;

    /* Get the signed value, earlier kernels had this as unsigned */
    value = ev->value;

    /* don't repeat mouse buttons */
    if (ev->code >= BTN_MOUSE && ev->code < KEY_OK)
        if (value == 2)
            return;

#ifdef _DEBUG_MT_SEQUENCE_
	if( ev->code == BTN_TOUCH || ev->code == BTN_LEFT )
		ErrorF("[KeyEvent] BTN_TOUCH (value=%d)\n", value);
#endif

    switch (ev->code) {
        case BTN_TOUCH:
        case BTN_TOOL_PEN:
        case BTN_TOOL_RUBBER:
        case BTN_TOOL_BRUSH:
        case BTN_TOOL_PENCIL:
        case BTN_TOOL_AIRBRUSH:
        case BTN_TOOL_FINGER:
        case BTN_TOOL_MOUSE:
        case BTN_TOOL_LENS:
            pEvdevMultitouch->tool = value ? ev->code : 0;
            if (!(pEvdevMultitouch->flags & EVDEVMULTITOUCH_TOUCHSCREEN || pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH))
                break;
            /* Treat BTN_TOUCH from devices that only have BTN_TOUCH as
             * BTN_LEFT. */
            ev->code = BTN_LEFT;
            /* Intentional fallthrough! */

        default:
            EvdevMultitouchProcessButtonEvent(pInfo, ev);
            break;
    }
}

static void EvdevMultitouchEndOfMultiTouch(InputInfoPtr pInfo,EvdevMultitouchDataMTPtr pData)
{
    InputInfoPtr pSubdev = pData->pInfo;
    EvdevMultitouchPtr pEvdevMultitouchSubdev = pSubdev->private;
    pEvdevMultitouchSubdev->id = -1;
    pData->containsValues = FALSE;
    pData->id = -1;
}

#ifdef _F_GESTURE_EXTENSION_
static void EvdevMultitouchFrameSync(InputInfoPtr pInfo, MTSyncType sync)
{
	AnyEvent event;

	memset(&event, 0, sizeof(event));
	event.header = ET_Internal;
	event.type = ET_MTSync;
	event.length = sizeof(event);
	event.time = GetTimeInMillis();
	event.deviceid = pInfo->dev->id;
	event.sync = sync;
	mieqEnqueue (pInfo->dev, (InternalEvent*)&event);
}
#endif//_F_GESTURE_EXTENSION_

/**
 * Post the multtouch motion events.
 */
static void
EvdevMultitouchPostMTMotionEvents(InputInfoPtr pInfo,struct input_event *ev)
{
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private, pEvdevMultitouchSubdev;
    EvdevMultitouchDataMTPtr pData;
    InputInfoPtr pSubdev;
    int i;
    static int num_of_pressed = 0;

    for (i=0;i<pEvdevMultitouch->num_multitouch;++i) {
        pData = &(pEvdevMultitouch->vals_mt[i]);
        if (!pData->containsValues) {
            continue;
        }
        pSubdev = pData->pInfo;
        if (!pSubdev)
            continue;

        pData->containsValues = FALSE;
        EvdevMultitouchCopyFromData(pSubdev, pData);
        pEvdevMultitouchSubdev = pSubdev->private;
        pEvdevMultitouchSubdev->mt = 0;
        pEvdevMultitouchSubdev->abs = pData->abs;
        pEvdevMultitouchSubdev->rel = 0;
        pEvdevMultitouchSubdev->tool = 1;
        /* droping of the pressed/released events */
        memset(pEvdevMultitouchSubdev->queue, 0, sizeof(pEvdevMultitouchSubdev->queue));
        pEvdevMultitouchSubdev->num_queue = 0;

        /* generate button press/release event */
        if(pEvdevMultitouchSubdev->abs & ABS_MT_TOUCH_MAJOR_VALUE)
        {
            if(pEvdevMultitouchSubdev->vals[pEvdevMultitouchSubdev->axis_map[ABS_MT_TOUCH_MAJOR]] )
            {
                if(pEvdevMultitouchSubdev->touch_state == 0)
                {
			num_of_pressed += (pData->id+1);
			if( num_of_pressed && !g_pressed )
			{
			   g_pressed = 1;
#ifdef _F_GESTURE_EXTENSION_
		  	   EvdevMultitouchFrameSync(pInfo, MTOUCH_FRAME_SYNC_BEGIN);
#endif//_F_GESTURE_EXTENSION_
			}
                    EvdevMultitouchQueueButtonEvent(pSubdev, 1, 1);
                    pEvdevMultitouchSubdev->touch_state = 1;
                }
            }
            else
            {
		  num_of_pressed -= (pData->id+1);
		  if(num_of_pressed < 0)
		  	num_of_pressed = 0;
		  /* last finger release */
		  if( !num_of_pressed )
		  {
			g_pressed = 0;
		  }
                EvdevMultitouchQueueButtonEvent(pSubdev, 1, 0);
                pEvdevMultitouchSubdev->touch_state = 0;
                pEvdevMultitouchSubdev->evtime = 0;
            }
        }
        EvdevMultitouchProcessSyncEvent(pSubdev, ev);
        
    }
}

static void
EvdevMultitouchPostMTMotionEventsBySingle(InputInfoPtr pInfo,struct input_event *ev)
{
    EvdevMultitouchPtr pEvdevMultitouch, pEvdevMultitouchSubdev;
    EvdevMultitouchDataMTPtr pData;
    InputInfoPtr pSubdev;

    if( !pInfo || !pInfo->private )
	return;

    pEvdevMultitouch = pInfo->private;

    pData = &(pEvdevMultitouch->vals_mt[0]);

    if (!pData->containsValues) {
        return;
    }
    
    pSubdev = pInfo;
    pData->containsValues = FALSE;
    EvdevMultitouchCopyFromData(pSubdev, pData);
    pEvdevMultitouchSubdev = pSubdev->private;
    pEvdevMultitouchSubdev->mt = 0;
    pEvdevMultitouchSubdev->abs = pData->abs;
    pEvdevMultitouchSubdev->rel = 0;
    pEvdevMultitouchSubdev->tool = 1;
    /* droping of the pressed/released events */
    memset(pEvdevMultitouchSubdev->queue, 0, sizeof(pEvdevMultitouchSubdev->queue));
    pEvdevMultitouchSubdev->num_queue = 0;

    /* generate button press/release event */
    if(pEvdevMultitouchSubdev->abs & ABS_MT_TOUCH_MAJOR_VALUE)
    {
        if(pEvdevMultitouchSubdev->vals[pEvdevMultitouchSubdev->axis_map[ABS_MT_TOUCH_MAJOR]] )
        {
            if(pEvdevMultitouchSubdev->touch_state == 0)
            {
                EvdevMultitouchQueueButtonEvent(pSubdev, 1, 1);
                pEvdevMultitouchSubdev->touch_state = 1;
		  g_pressed = 1;
#ifdef _F_GESTURE_EXTENSION_
		  EvdevMultitouchFrameSync(pInfo, MTOUCH_FRAME_SYNC_BEGIN);
#endif//_F_GESTURE_EXTENSION_
            }
        }
        else
        {
	     g_pressed = 0;
            EvdevMultitouchQueueButtonEvent(pSubdev, 1, 0);
            pEvdevMultitouchSubdev->touch_state = 0;
            pEvdevMultitouchSubdev->evtime = 0;
        }
    }

    EvdevMultitouchProcessSyncEvent(pSubdev, ev);
}

/**
 * Post the relative motion events.
 */
void
EvdevMultitouchPostRelativeMotionEvents(InputInfoPtr pInfo, int *num_v, int *first_v,
                              int v[MAX_VALUATORS])
{
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    if (pEvdevMultitouch->rel) {
        xf86PostMotionEventP(pInfo->dev, FALSE, *first_v, *num_v, v + *first_v);
    }
}

/**
 * Post the absolute motion events.
 */
void
EvdevMultitouchPostAbsoluteMotionEvents(InputInfoPtr pInfo, int *num_v, int *first_v,
                              int v[MAX_VALUATORS])
{
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    /*
     * Some devices only generate valid abs coords when BTN_DIGI is
     * pressed.  On wacom tablets, this means that the pen is in
     * proximity of the tablet.  After the pen is removed, BTN_DIGI is
     * released, and a (0, 0) absolute event is generated.  Checking
     * pEvdevMultitouch->digi here, lets us ignore that event.  pEvdevMultitouch is
     * initialized to 1 so devices that doesn't use this scheme still
     * just works.
     */
    if (pEvdevMultitouch->abs && pEvdevMultitouch->tool) {
        xf86PostMotionEventP(pInfo->dev, TRUE, *first_v, *num_v, v);
    }
}

/**
 * Post the queued key/button events.
 */
static void EvdevMultitouchPostQueuedEvents(InputInfoPtr pInfo, int *num_v, int *first_v,
                                  int v[MAX_VALUATORS])
{
    int i;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    for (i = 0; i < pEvdevMultitouch->num_queue; i++) {
        switch (pEvdevMultitouch->queue[i].type) {
        case EV_QUEUE_KEY:
            xf86PostKeyboardEvent(pInfo->dev, pEvdevMultitouch->queue[i].key,
                                  pEvdevMultitouch->queue[i].val);
            break;
        case EV_QUEUE_BTN:
            /* FIXME: Add xf86PostButtonEventP to the X server so that we may
             * pass the valuators on ButtonPress/Release events, too.  Currently
             * only MotionNotify events contain the pointer position. */
            xf86PostButtonEvent(pInfo->dev, 0, pEvdevMultitouch->queue[i].key,
                                pEvdevMultitouch->queue[i].val, 0, 0);
            break;
        }
    }
}
static void
EvdevMultitouchCopyFromData(InputInfoPtr pInfo, EvdevMultitouchDataMTPtr pData) {
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    memcpy(pEvdevMultitouch->vals, pData->vals, MAX_VALUATORS * sizeof(int));
    /* we drop the buttons/key events */
}

static void
EvdevMultitouchStoreMTData(InputInfoPtr pInfo, EvdevMultitouchDataMTPtr pData) {
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    int id,x,y;
    Time currentTime = GetTimeInMillis();

    id = pEvdevMultitouch->current_id;

    if(pEvdevMultitouch->abs & ABS_MT_X_VALUE)
        x = pEvdevMultitouch->vals[pEvdevMultitouch->axis_map[ABS_MT_POSITION_X]];
    else
        x = pData->vals[pEvdevMultitouch->axis_map[ABS_X]]; 
        
    if(pEvdevMultitouch->abs & ABS_MT_Y_VALUE)    
        y = pEvdevMultitouch->vals[pEvdevMultitouch->axis_map[ABS_MT_POSITION_Y]];
    else
        y = pData->vals[pEvdevMultitouch->axis_map[ABS_Y]];

    {
        EvdevMultitouchPtr pEvdevMultitouchSub = pData->pInfo->private;
        pEvdevMultitouchSub->evtime = currentTime;
    }

    pData->id = id;
    memcpy(pData->vals, pEvdevMultitouch->vals, MAX_VALUATORS * sizeof(int));
    pData->vals[pEvdevMultitouch->axis_map[ABS_X]] = x;
    pData->vals[pEvdevMultitouch->axis_map[ABS_Y]] = y;
    pData->containsValues = TRUE;
    pData->expires = currentTime + pEvdevMultitouch->timeout;
    pData->abs = pEvdevMultitouch->abs;

    return;
}

/**
 * Take the synchronization input event and process it accordingly; the motion
 * notify events are sent first, then any button/key press/release events.
 */
static void
EvdevMultitouchProcessMTSyncReport(InputInfoPtr pInfo, struct input_event *ev)
{
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    int id;

    id = pEvdevMultitouch->current_id;
    
    if (id < 0) {
        EvdevMultitouchReinitPEvdevMultitouch(pInfo);
        return;
    }

    if(pEvdevMultitouch->num_multitouch == 0) //Single mode
    {
        if(id != 0)
        {
            EvdevMultitouchReinitPEvdevMultitouch(pInfo);
            return;
        }
    }
    else
    {
        if (id > pEvdevMultitouch->num_multitouch-1)
        {
            EvdevMultitouchReinitPEvdevMultitouch(pInfo);
            return;
        }      
    }

    EvdevMultitouchStoreMTData(pInfo, &(pEvdevMultitouch->vals_mt[id]));
    EvdevMultitouchReinitPEvdevMultitouch(pInfo);
    pEvdevMultitouch->mt = 1;
    pEvdevMultitouch->sync_mt = 1;
}

static void
EvdevMultitouchReinitPEvdevMultitouch(InputInfoPtr pInfo) {
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    memset(pEvdevMultitouch->delta, 0, sizeof(pEvdevMultitouch->delta));
    memset(pEvdevMultitouch->queue, 0, sizeof(pEvdevMultitouch->queue));
    pEvdevMultitouch->num_queue = 0;
    pEvdevMultitouch->abs = 0;
    pEvdevMultitouch->rel = 0;
    pEvdevMultitouch->mt = 0;
    pEvdevMultitouch->num_mt = 0;
    pEvdevMultitouch->current_id = -1;
    pEvdevMultitouch->sync_mt = 0;
}

/**
 * Take the synchronization input event and process it accordingly; the motion
 * notify events are sent first, then any button/key press/release events.
 */
static void
EvdevMultitouchProcessSyncEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    int num_v = 0, first_v = 0;
    int v[MAX_VALUATORS];
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    
    if (ev->code == SYN_MT_REPORT) {
        EvdevMultitouchProcessMTSyncReport(pInfo, ev);
        return;
    }

    if( 0 <= pEvdevMultitouch->current_id && !pEvdevMultitouch->sync_mt )
    {
#ifdef _DEBUG_MT_SEQUENCE_
	if( pEvdevMultitouch->vals[pEvdevMultitouch->axis_map[ABS_MT_TOUCH_MAJOR]] )
		ErrorF("[SyncEvent] Press or Motion !(current->id=%d)\n", pEvdevMultitouch->current_id);
	else
		ErrorF("[SyncEvent] Release!(current->id=%d)\n\n", pEvdevMultitouch->current_id);
#endif

        EvdevMultitouchProcessMTSyncReport(pInfo, ev);
    }

    if (pEvdevMultitouch->mt) {
        if(pEvdevMultitouch->num_multitouch > 1)
    	 {
            EvdevMultitouchPostMTMotionEvents(pInfo, ev);
    	 }
        else
        {
            EvdevMultitouchPostMTMotionEventsBySingle(pInfo, ev);
        }
    } else {
        EvdevMultitouchProcessValuators(pInfo, v, &num_v, &first_v);

        EvdevMultitouchPostRelativeMotionEvents(pInfo, &num_v, &first_v, v);
        EvdevMultitouchPostAbsoluteMotionEvents(pInfo, &num_v, &first_v, v);
        EvdevMultitouchPostQueuedEvents(pInfo, &num_v, &first_v, v);
#ifdef _F_GESTURE_EXTENSION_
	if( !g_pressed )
            EvdevMultitouchFrameSync(pInfo, MTOUCH_FRAME_SYNC_END);
#endif//_F_GESTURE_EXTENSION_
    }
    
    EvdevMultitouchReinitPEvdevMultitouch(pInfo);
}

/**
 * Take the trackingID input event and process it accordingly.
 */
static void
EvdevMultitouchProcessTrackingIDEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    // begining of a new touch
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    pEvdevMultitouch->last_slot = pEvdevMultitouch->current_id = ev->value;
#ifdef _DEBUG_MT_SEQUENCE_
    ErrorF("[TrackingIDEvent] current_id=%d, last_slot=%d\n", pEvdevMultitouch->current_id, pEvdevMultitouch->last_slot);
#endif
}

/**
 * Process the events from the device; nothing is actually posted to the server
 * until an EV_SYN event is received.
 */
static char* ev_name(int type, int code, int value)
{
   static char ename[100];

   char *stype = NULL;
   char *scode = NULL;
   char *svalue = NULL;

   char ttype[50];
   char tcode[50];
   char tvalue[50];
   
   switch(type)
   {
   case EV_SYN:
      stype = "EV_SYNC";
      switch(code)
      {
      case SYN_REPORT:
         scode = "SYN_REPORT";
         break;
      case SYN_CONFIG:
         scode = "SYN_CONFIG";
         break;
      case SYN_MT_REPORT:
         scode = "SYN_MT_REPORT";
         break;
      }
      break;
   case EV_KEY:
      stype = "EV_KEY";
      break;
   case EV_REL:
      stype = "EV_REL";
      break;
   case EV_ABS:
      stype = "EV_ABS";
      switch(code)
      {
      case ABS_X:
         scode =  "ABS_X";
         break;
      case ABS_Y:
         scode =  "ABS_Y";
         break;
      case ABS_PRESSURE:
         scode =  "ABS_PRESSURE";
         break;
      case ABS_MT_TOUCH_MAJOR:
         scode =  "ABS_MT_TOUCH_MAJOR";
         break;
      case ABS_MT_TOUCH_MINOR:
         scode =  "ABS_MT_TOUCH_MINOR";
         break;
      case ABS_MT_WIDTH_MAJOR:
         scode =  "ABS_MT_WIDTH_MAJOR";
         break;
      case ABS_MT_WIDTH_MINOR:
         scode =  "ABS_MT_WIDTH_MINOR";
         break;
      case ABS_MT_ORIENTATION:
         scode =  "ABS_MT_ORIENTATION";
         break;
      case ABS_MT_POSITION_X:
         scode =  "ABS_MT_POSITION_X";
         break;
      case ABS_MT_POSITION_Y:
         scode =  "ABS_MT_POSITION_Y";
         break;
      case ABS_MT_TOOL_TYPE:
         scode =  "ABS_MT_TOOL_TYPE";
         break;
      case ABS_MT_BLOB_ID:
         scode =  "ABS_MT_BLOB_ID";
         break;
      case ABS_MT_TRACKING_ID:
         scode =  "ABS_MT_TRACKING_ID";
         break;
      case ABS_MT_SLOT:
         scode =  "ABS_MT_SLOT";
         break;
      }
   default:
      break;
   }

   if(!stype)
   {
      sprintf(ttype, "T(0x%x)", type);
      stype = ttype;
   }

   if(!scode)
   {
      sprintf(tcode, "C(0x%x)", code);
      scode = tcode;
   }

   if(!svalue)
   {
      sprintf(tvalue, "V(%d)",value);
      svalue = tvalue;
   }

   sprintf(ename, "%s : %s : %s", stype, scode, svalue);
   return ename;
}

static void EvdevMultitouchFakeOmittedEvents(InputInfoPtr pInfo)
{
	EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

	if( !(pEvdevMultitouch->abs & ABS_MT_TOUCH_MAJOR_VALUE) )
	{
		pEvdevMultitouch->vals[pEvdevMultitouch->axis_map[ABS_MT_TOUCH_MAJOR]] = 1;
		pEvdevMultitouch->abs |= ABS_MT_TOUCH_MAJOR_VALUE;
#ifdef _DEBUG_MT_SEQUENCE_
		ErrorF("\t...Fake ABS_MT_TOUCH_MAJOR\n");
#endif
	}

	if( pEvdevMultitouch->current_id < 0 )
	{
		pEvdevMultitouch->current_id = pEvdevMultitouch->last_slot;
#ifdef _DEBUG_MT_SEQUENCE_
		ErrorF("\t...Fake ABS_MT_SLOT (current_id=%d)\n", pEvdevMultitouch->current_id);
#endif
	}
}

static void
EvdevMultitouchProcessEvent(InputInfoPtr pInfo, struct input_event *ev)
{
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
	
    switch (ev->type) {
        case EV_REL:
            return;
        case EV_ABS:
            switch(ev->code)
            {
            case ABS_X:
            case ABS_Y:
            case ABS_PRESSURE:
                return;
            }
			
	     if( ev->code == ABS_MT_TRACKING_ID )
	     {
#ifdef _DEBUG_MT_SEQUENCE_
		  ErrorF("[ProcessEvent] ABS_MT_TRACKING_ID (value=%d)\n", ev->value);
#endif

		  if( pEvdevMultitouch->mt_slot_supported )
		  {//MT protocol B Type
	  	         if( pEvdevMultitouch->current_id < 0 )
		         {
				pEvdevMultitouch->current_id = pEvdevMultitouch->last_slot;
		         }
			  
		         if( 0 > ev->value )//ABS_MT_TRACKING_ID == -1
	         	  {
#ifdef _DEBUG_MT_SEQUENCE_
		              ErrorF("\t...Fake ABS_MT_TOUCH_MAJOR\n");
#endif
	         	  	pEvdevMultitouch->vals[pEvdevMultitouch->axis_map[ABS_MT_TOUCH_MAJOR]] = 0;
	         	  	pEvdevMultitouch->abs |= ABS_MT_TOUCH_MAJOR_VALUE;
	         	  }
		  }
		  else
		  {//MT protocol A Type
			  EvdevMultitouchProcessTrackingIDEvent(pInfo, ev);
		  }
	     }
		 
            if (ev->code == ABS_MT_SLOT)
	     {
#ifdef _DEBUG_MT_SEQUENCE_
		  ErrorF("[ProcessEvent] ABS_MT_SLOT (value=%d)\n", ev->value);
#endif
		  if( pEvdevMultitouch->last_slot != ev->value )
	  	 {
	  	 	ev->code = SYN_REPORT;
	  	 	EvdevMultitouchProcessSyncEvent(pInfo, ev);
	  	 	ev->code = ABS_MT_SLOT;
	  	 }

                EvdevMultitouchProcessTrackingIDEvent(pInfo, ev);
            }
	     else
	     {
                EvdevMultitouchProcessAbsoluteMotionEvent(pInfo, ev);
            }
            break;
        case EV_KEY:
            return;
        case EV_SYN:
#ifdef _DEBUG_MT_SEQUENCE_
	     if( ev->code == SYN_MT_REPORT )
	     	ErrorF("[ProcessEvent] SYN_MT_REPORT (value=%d)\n", ev->value);
	     else
	     	ErrorF("[ProcessEvent] SYN_REPORT (value=%d)\n", ev->value);
#endif
            EvdevMultitouchProcessSyncEvent(pInfo, ev);
            break;
    }
}

#undef ABS_X_VALUE
#undef ABS_Y_VALUE
#undef ABS_VALUE

/* just a magic number to reduce the number of reads */
#define NUM_EVENTS 16

/**
 * Empty callback for subdevice.
 */
static void
EvdevMultitouchSubdevReadInput(InputInfoPtr pInfo) {
    return;
}

static void
EvdevMultitouchReadInput(InputInfoPtr pInfo)
{
    struct input_event ev[NUM_EVENTS];
    int i, len = sizeof(ev);
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    while (len == sizeof(ev))
    {
        len = read(pInfo->fd, &ev, sizeof(ev));
        if (len <= 0)
        {
            if (errno == ENODEV) /* May happen after resume */
            {
                EvdevMultitouchMBEmuFinalize(pInfo);
                xf86RemoveEnabledDevice(pInfo);
                close(pInfo->fd);
                pInfo->fd = -1;
                if (pEvdevMultitouch->reopen_timer)
                {
                    pEvdevMultitouch->reopen_left = pEvdevMultitouch->reopen_attempts;
                    pEvdevMultitouch->reopen_timer = TimerSet(pEvdevMultitouch->reopen_timer, 0, 100, EvdevMultitouchReopenTimer, pInfo);
                }
            } else if (errno != EAGAIN)
            {
                /* We use X_NONE here because it doesn't alloc */
                xf86MsgVerb(X_NONE, 0, "%s: Read error: %s\n", pInfo->name,
                        strerror(errno));
            }
            break;
        }

        /* The kernel promises that we always only read a complete
         * event, so len != sizeof ev is an error. */
        if (len % sizeof(ev[0])) {
            /* We use X_NONE here because it doesn't alloc */
            xf86MsgVerb(X_NONE, 0, "%s: Read error: %s\n", pInfo->name, strerror(errno));
            break;
        }

        for (i = 0; i < len/sizeof(ev[0]); i++)
            EvdevMultitouchProcessEvent(pInfo, &ev[i]);
    }
}

#define TestBit(bit, array) ((array[(bit) / LONG_BITS]) & (1L << ((bit) % LONG_BITS)))

static void
EvdevMultitouchPtrCtrlProc(DeviceIntPtr device, PtrCtrl *ctrl)
{
    /* Nothing to do, dix handles all settings */
}

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 5
static KeySym map[] = {
    /* 0x00 */  NoSymbol,       NoSymbol,
    /* 0x01 */  XK_Escape,      NoSymbol,
    /* 0x02 */  XK_1,           XK_exclam,
    /* 0x03 */  XK_2,           XK_at,
    /* 0x04 */  XK_3,           XK_numbersign,
    /* 0x05 */  XK_4,           XK_dollar,
    /* 0x06 */  XK_5,           XK_percent,
    /* 0x07 */  XK_6,           XK_asciicircum,
    /* 0x08 */  XK_7,           XK_ampersand,
    /* 0x09 */  XK_8,           XK_asterisk,
    /* 0x0a */  XK_9,           XK_parenleft,
    /* 0x0b */  XK_0,           XK_parenright,
    /* 0x0c */  XK_minus,       XK_underscore,
    /* 0x0d */  XK_equal,       XK_plus,
    /* 0x0e */  XK_BackSpace,   NoSymbol,
    /* 0x0f */  XK_Tab,         XK_ISO_Left_Tab,
    /* 0x10 */  XK_Q,           NoSymbol,
    /* 0x11 */  XK_W,           NoSymbol,
    /* 0x12 */  XK_E,           NoSymbol,
    /* 0x13 */  XK_R,           NoSymbol,
    /* 0x14 */  XK_T,           NoSymbol,
    /* 0x15 */  XK_Y,           NoSymbol,
    /* 0x16 */  XK_U,           NoSymbol,
    /* 0x17 */  XK_I,           NoSymbol,
    /* 0x18 */  XK_O,           NoSymbol,
    /* 0x19 */  XK_P,           NoSymbol,
    /* 0x1a */  XK_bracketleft, XK_braceleft,
    /* 0x1b */  XK_bracketright,XK_braceright,
    /* 0x1c */  XK_Return,      NoSymbol,
    /* 0x1d */  XK_Control_L,   NoSymbol,
    /* 0x1e */  XK_A,           NoSymbol,
    /* 0x1f */  XK_S,           NoSymbol,
    /* 0x20 */  XK_D,           NoSymbol,
    /* 0x21 */  XK_F,           NoSymbol,
    /* 0x22 */  XK_G,           NoSymbol,
    /* 0x23 */  XK_H,           NoSymbol,
    /* 0x24 */  XK_J,           NoSymbol,
    /* 0x25 */  XK_K,           NoSymbol,
    /* 0x26 */  XK_L,           NoSymbol,
    /* 0x27 */  XK_semicolon,   XK_colon,
    /* 0x28 */  XK_quoteright,  XK_quotedbl,
    /* 0x29 */  XK_quoteleft,	XK_asciitilde,
    /* 0x2a */  XK_Shift_L,     NoSymbol,
    /* 0x2b */  XK_backslash,   XK_bar,
    /* 0x2c */  XK_Z,           NoSymbol,
    /* 0x2d */  XK_X,           NoSymbol,
    /* 0x2e */  XK_C,           NoSymbol,
    /* 0x2f */  XK_V,           NoSymbol,
    /* 0x30 */  XK_B,           NoSymbol,
    /* 0x31 */  XK_N,           NoSymbol,
    /* 0x32 */  XK_M,           NoSymbol,
    /* 0x33 */  XK_comma,       XK_less,
    /* 0x34 */  XK_period,      XK_greater,
    /* 0x35 */  XK_slash,       XK_question,
    /* 0x36 */  XK_Shift_R,     NoSymbol,
    /* 0x37 */  XK_KP_Multiply, NoSymbol,
    /* 0x38 */  XK_Alt_L,	XK_Meta_L,
    /* 0x39 */  XK_space,       NoSymbol,
    /* 0x3a */  XK_Caps_Lock,   NoSymbol,
    /* 0x3b */  XK_F1,          NoSymbol,
    /* 0x3c */  XK_F2,          NoSymbol,
    /* 0x3d */  XK_F3,          NoSymbol,
    /* 0x3e */  XK_F4,          NoSymbol,
    /* 0x3f */  XK_F5,          NoSymbol,
    /* 0x40 */  XK_F6,          NoSymbol,
    /* 0x41 */  XK_F7,          NoSymbol,
    /* 0x42 */  XK_F8,          NoSymbol,
    /* 0x43 */  XK_F9,          NoSymbol,
    /* 0x44 */  XK_F10,         NoSymbol,
    /* 0x45 */  XK_Num_Lock,    NoSymbol,
    /* 0x46 */  XK_Scroll_Lock,	NoSymbol,
    /* These KP keys should have the KP_7 keysyms in the numlock
     * modifer... ? */
    /* 0x47 */  XK_KP_Home,	XK_KP_7,
    /* 0x48 */  XK_KP_Up,	XK_KP_8,
    /* 0x49 */  XK_KP_Prior,	XK_KP_9,
    /* 0x4a */  XK_KP_Subtract, NoSymbol,
    /* 0x4b */  XK_KP_Left,	XK_KP_4,
    /* 0x4c */  XK_KP_Begin,	XK_KP_5,
    /* 0x4d */  XK_KP_Right,	XK_KP_6,
    /* 0x4e */  XK_KP_Add,      NoSymbol,
    /* 0x4f */  XK_KP_End,	XK_KP_1,
    /* 0x50 */  XK_KP_Down,	XK_KP_2,
    /* 0x51 */  XK_KP_Next,	XK_KP_3,
    /* 0x52 */  XK_KP_Insert,	XK_KP_0,
    /* 0x53 */  XK_KP_Delete,	XK_KP_Decimal,
    /* 0x54 */  NoSymbol,	NoSymbol,
    /* 0x55 */  XK_F13,		NoSymbol,
    /* 0x56 */  XK_less,	XK_greater,
    /* 0x57 */  XK_F11,		NoSymbol,
    /* 0x58 */  XK_F12,		NoSymbol,
    /* 0x59 */  XK_F14,		NoSymbol,
    /* 0x5a */  XK_F15,		NoSymbol,
    /* 0x5b */  XK_F16,		NoSymbol,
    /* 0x5c */  XK_F17,		NoSymbol,
    /* 0x5d */  XK_F18,		NoSymbol,
    /* 0x5e */  XK_F19,		NoSymbol,
    /* 0x5f */  XK_F20,		NoSymbol,
    /* 0x60 */  XK_KP_Enter,	NoSymbol,
    /* 0x61 */  XK_Control_R,	NoSymbol,
    /* 0x62 */  XK_KP_Divide,	NoSymbol,
    /* 0x63 */  XK_Print,	XK_Sys_Req,
    /* 0x64 */  XK_Alt_R,	XK_Meta_R,
    /* 0x65 */  NoSymbol,	NoSymbol, /* KEY_LINEFEED */
    /* 0x66 */  XK_Home,	NoSymbol,
    /* 0x67 */  XK_Up,		NoSymbol,
    /* 0x68 */  XK_Prior,	NoSymbol,
    /* 0x69 */  XK_Left,	NoSymbol,
    /* 0x6a */  XK_Right,	NoSymbol,
    /* 0x6b */  XK_End,		NoSymbol,
    /* 0x6c */  XK_Down,	NoSymbol,
    /* 0x6d */  XK_Next,	NoSymbol,
    /* 0x6e */  XK_Insert,	NoSymbol,
    /* 0x6f */  XK_Delete,	NoSymbol,
    /* 0x70 */  NoSymbol,	NoSymbol, /* KEY_MACRO */
    /* 0x71 */  NoSymbol,	NoSymbol,
    /* 0x72 */  NoSymbol,	NoSymbol,
    /* 0x73 */  NoSymbol,	NoSymbol,
    /* 0x74 */  NoSymbol,	NoSymbol,
    /* 0x75 */  XK_KP_Equal,	NoSymbol,
    /* 0x76 */  NoSymbol,	NoSymbol,
    /* 0x77 */  NoSymbol,	NoSymbol,
    /* 0x78 */  XK_F21,		NoSymbol,
    /* 0x79 */  XK_F22,		NoSymbol,
    /* 0x7a */  XK_F23,		NoSymbol,
    /* 0x7b */  XK_F24,		NoSymbol,
    /* 0x7c */  XK_KP_Separator, NoSymbol,
    /* 0x7d */  XK_Meta_L,	NoSymbol,
    /* 0x7e */  XK_Meta_R,	NoSymbol,
    /* 0x7f */  XK_Multi_key,	NoSymbol,
    /* 0x80 */  NoSymbol,	NoSymbol,
    /* 0x81 */  NoSymbol,	NoSymbol,
    /* 0x82 */  NoSymbol,	NoSymbol,
    /* 0x83 */  NoSymbol,	NoSymbol,
    /* 0x84 */  NoSymbol,	NoSymbol,
    /* 0x85 */  NoSymbol,	NoSymbol,
    /* 0x86 */  NoSymbol,	NoSymbol,
    /* 0x87 */  NoSymbol,	NoSymbol,
    /* 0x88 */  NoSymbol,	NoSymbol,
    /* 0x89 */  NoSymbol,	NoSymbol,
    /* 0x8a */  NoSymbol,	NoSymbol,
    /* 0x8b */  NoSymbol,	NoSymbol,
    /* 0x8c */  NoSymbol,	NoSymbol,
    /* 0x8d */  NoSymbol,	NoSymbol,
    /* 0x8e */  NoSymbol,	NoSymbol,
    /* 0x8f */  NoSymbol,	NoSymbol,
    /* 0x90 */  NoSymbol,	NoSymbol,
    /* 0x91 */  NoSymbol,	NoSymbol,
    /* 0x92 */  NoSymbol,	NoSymbol,
    /* 0x93 */  NoSymbol,	NoSymbol,
    /* 0x94 */  NoSymbol,	NoSymbol,
    /* 0x95 */  NoSymbol,	NoSymbol,
    /* 0x96 */  NoSymbol,	NoSymbol,
    /* 0x97 */  NoSymbol,	NoSymbol,
    /* 0x98 */  NoSymbol,	NoSymbol,
    /* 0x99 */  NoSymbol,	NoSymbol,
    /* 0x9a */  NoSymbol,	NoSymbol,
    /* 0x9b */  NoSymbol,	NoSymbol,
    /* 0x9c */  NoSymbol,	NoSymbol,
    /* 0x9d */  NoSymbol,	NoSymbol,
    /* 0x9e */  NoSymbol,	NoSymbol,
    /* 0x9f */  NoSymbol,	NoSymbol,
    /* 0xa0 */  NoSymbol,	NoSymbol,
    /* 0xa1 */  NoSymbol,	NoSymbol,
    /* 0xa2 */  NoSymbol,	NoSymbol,
    /* 0xa3 */  NoSymbol,	NoSymbol,
    /* 0xa4 */  NoSymbol,	NoSymbol,
    /* 0xa5 */  NoSymbol,	NoSymbol,
    /* 0xa6 */  NoSymbol,	NoSymbol,
    /* 0xa7 */  NoSymbol,	NoSymbol,
    /* 0xa8 */  NoSymbol,	NoSymbol,
    /* 0xa9 */  NoSymbol,	NoSymbol,
    /* 0xaa */  NoSymbol,	NoSymbol,
    /* 0xab */  NoSymbol,	NoSymbol,
    /* 0xac */  NoSymbol,	NoSymbol,
    /* 0xad */  NoSymbol,	NoSymbol,
    /* 0xae */  NoSymbol,	NoSymbol,
    /* 0xaf */  NoSymbol,	NoSymbol,
    /* 0xb0 */  NoSymbol,	NoSymbol,
    /* 0xb1 */  NoSymbol,	NoSymbol,
    /* 0xb2 */  NoSymbol,	NoSymbol,
    /* 0xb3 */  NoSymbol,	NoSymbol,
    /* 0xb4 */  NoSymbol,	NoSymbol,
    /* 0xb5 */  NoSymbol,	NoSymbol,
    /* 0xb6 */  NoSymbol,	NoSymbol,
    /* 0xb7 */  NoSymbol,	NoSymbol,
    /* 0xb8 */  NoSymbol,	NoSymbol,
    /* 0xb9 */  NoSymbol,	NoSymbol,
    /* 0xba */  NoSymbol,	NoSymbol,
    /* 0xbb */  NoSymbol,	NoSymbol,
    /* 0xbc */  NoSymbol,	NoSymbol,
    /* 0xbd */  NoSymbol,	NoSymbol,
    /* 0xbe */  NoSymbol,	NoSymbol,
    /* 0xbf */  NoSymbol,	NoSymbol,
    /* 0xc0 */  NoSymbol,	NoSymbol,
    /* 0xc1 */  NoSymbol,	NoSymbol,
    /* 0xc2 */  NoSymbol,	NoSymbol,
    /* 0xc3 */  NoSymbol,	NoSymbol,
    /* 0xc4 */  NoSymbol,	NoSymbol,
    /* 0xc5 */  NoSymbol,	NoSymbol,
    /* 0xc6 */  NoSymbol,	NoSymbol,
    /* 0xc7 */  NoSymbol,	NoSymbol,
    /* 0xc8 */  NoSymbol,	NoSymbol,
    /* 0xc9 */  NoSymbol,	NoSymbol,
    /* 0xca */  NoSymbol,	NoSymbol,
    /* 0xcb */  NoSymbol,	NoSymbol,
    /* 0xcc */  NoSymbol,	NoSymbol,
    /* 0xcd */  NoSymbol,	NoSymbol,
    /* 0xce */  NoSymbol,	NoSymbol,
    /* 0xcf */  NoSymbol,	NoSymbol,
    /* 0xd0 */  NoSymbol,	NoSymbol,
    /* 0xd1 */  NoSymbol,	NoSymbol,
    /* 0xd2 */  NoSymbol,	NoSymbol,
    /* 0xd3 */  NoSymbol,	NoSymbol,
    /* 0xd4 */  NoSymbol,	NoSymbol,
    /* 0xd5 */  NoSymbol,	NoSymbol,
    /* 0xd6 */  NoSymbol,	NoSymbol,
    /* 0xd7 */  NoSymbol,	NoSymbol,
    /* 0xd8 */  NoSymbol,	NoSymbol,
    /* 0xd9 */  NoSymbol,	NoSymbol,
    /* 0xda */  NoSymbol,	NoSymbol,
    /* 0xdb */  NoSymbol,	NoSymbol,
    /* 0xdc */  NoSymbol,	NoSymbol,
    /* 0xdd */  NoSymbol,	NoSymbol,
    /* 0xde */  NoSymbol,	NoSymbol,
    /* 0xdf */  NoSymbol,	NoSymbol,
    /* 0xe0 */  NoSymbol,	NoSymbol,
    /* 0xe1 */  NoSymbol,	NoSymbol,
    /* 0xe2 */  NoSymbol,	NoSymbol,
    /* 0xe3 */  NoSymbol,	NoSymbol,
    /* 0xe4 */  NoSymbol,	NoSymbol,
    /* 0xe5 */  NoSymbol,	NoSymbol,
    /* 0xe6 */  NoSymbol,	NoSymbol,
    /* 0xe7 */  NoSymbol,	NoSymbol,
    /* 0xe8 */  NoSymbol,	NoSymbol,
    /* 0xe9 */  NoSymbol,	NoSymbol,
    /* 0xea */  NoSymbol,	NoSymbol,
    /* 0xeb */  NoSymbol,	NoSymbol,
    /* 0xec */  NoSymbol,	NoSymbol,
    /* 0xed */  NoSymbol,	NoSymbol,
    /* 0xee */  NoSymbol,	NoSymbol,
    /* 0xef */  NoSymbol,	NoSymbol,
    /* 0xf0 */  NoSymbol,	NoSymbol,
    /* 0xf1 */  NoSymbol,	NoSymbol,
    /* 0xf2 */  NoSymbol,	NoSymbol,
    /* 0xf3 */  NoSymbol,	NoSymbol,
    /* 0xf4 */  NoSymbol,	NoSymbol,
    /* 0xf5 */  NoSymbol,	NoSymbol,
    /* 0xf6 */  NoSymbol,	NoSymbol,
    /* 0xf7 */  NoSymbol,	NoSymbol,
};

static struct { KeySym keysym; CARD8 mask; } modifiers[] = {
    { XK_Shift_L,		ShiftMask },
    { XK_Shift_R,		ShiftMask },
    { XK_Control_L,		ControlMask },
    { XK_Control_R,		ControlMask },
    { XK_Caps_Lock,		LockMask },
    { XK_Alt_L,		AltMask },
    { XK_Alt_R,		AltMask },
    { XK_Meta_L,		Mod4Mask },
    { XK_Meta_R,		Mod4Mask },
    { XK_Num_Lock,		NumLockMask },
    { XK_Scroll_Lock,	ScrollLockMask },
    { XK_Mode_switch,	AltLangMask }
};

/* Server 1.6 and earlier */
static int
EvdevMultitouchInitKeysyms(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch;
    KeySymsRec keySyms;
    CARD8 modMap[MAP_LENGTH];
    KeySym sym;
    int i, j;

    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

     /* Compute the modifier map */
    memset(modMap, 0, sizeof modMap);

    for (i = 0; i < ArrayLength(map) / GLYPHS_PER_KEY; i++) {
        sym = map[i * GLYPHS_PER_KEY];
        for (j = 0; j < ArrayLength(modifiers); j++) {
            if (modifiers[j].keysym == sym)
                modMap[i + MIN_KEYCODE] = modifiers[j].mask;
        }
    }

    keySyms.map        = map;
    keySyms.mapWidth   = GLYPHS_PER_KEY;
    keySyms.minKeyCode = MIN_KEYCODE;
    keySyms.maxKeyCode = MIN_KEYCODE + ArrayLength(map) / GLYPHS_PER_KEY - 1;

    XkbSetRulesDflts(pEvdevMultitouch->rmlvo.rules, pEvdevMultitouch->rmlvo.model,
            pEvdevMultitouch->rmlvo.layout, pEvdevMultitouch->rmlvo.variant,
            pEvdevMultitouch->rmlvo.options);
    if (!XkbInitKeyboardDeviceStruct(device, &pEvdevMultitouch->xkbnames,
                &keySyms, modMap, NULL,
                EvdevMultitouchKbdCtrl))
        return 0;

    return 1;
}
#endif

static void
EvdevMultitouchKbdCtrl(DeviceIntPtr device, KeybdCtrl *ctrl)
{
    static struct { int xbit, code; } bits[] = {
        { CAPSFLAG,	LED_CAPSL },
        { NUMFLAG,	LED_NUML },
        { SCROLLFLAG,	LED_SCROLLL },
        { MODEFLAG,	LED_KANA },
        { COMPOSEFLAG,	LED_COMPOSE }
    };

    InputInfoPtr pInfo;
    struct input_event ev[ArrayLength(bits)];
    int i;

    memset(ev, 0, sizeof(ev));

    pInfo = device->public.devicePrivate;
    for (i = 0; i < ArrayLength(bits); i++) {
        ev[i].type = EV_LED;
        ev[i].code = bits[i].code;
        ev[i].value = (ctrl->leds & bits[i].xbit) > 0;
    }

    write(pInfo->fd, ev, sizeof ev);
}

static int
EvdevMultitouchAddKeyClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch;

    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

    /* sorry, no rules change allowed for you */
    xf86ReplaceStrOption(pInfo->options, "xkb_rules", "evdevmultitouch");
    SetXkbOption(pInfo, "xkb_rules", &pEvdevMultitouch->rmlvo.rules);
    SetXkbOption(pInfo, "xkb_model", &pEvdevMultitouch->rmlvo.model);
    if (!pEvdevMultitouch->rmlvo.model)
        SetXkbOption(pInfo, "XkbModel", &pEvdevMultitouch->rmlvo.model);
    SetXkbOption(pInfo, "xkb_layout", &pEvdevMultitouch->rmlvo.layout);
    if (!pEvdevMultitouch->rmlvo.layout)
        SetXkbOption(pInfo, "XkbLayout", &pEvdevMultitouch->rmlvo.layout);
    SetXkbOption(pInfo, "xkb_variant", &pEvdevMultitouch->rmlvo.variant);
    if (!pEvdevMultitouch->rmlvo.variant)
        SetXkbOption(pInfo, "XkbVariant", &pEvdevMultitouch->rmlvo.variant);
    SetXkbOption(pInfo, "xkb_options", &pEvdevMultitouch->rmlvo.options);
    if (!pEvdevMultitouch->rmlvo.options)
        SetXkbOption(pInfo, "XkbOptions", &pEvdevMultitouch->rmlvo.options);

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 5
    if (!InitKeyboardDeviceStruct(device, &pEvdevMultitouch->rmlvo, NULL, EvdevMultitouchKbdCtrl))
        return !Success;
#else
    if (!EvdevMultitouchInitKeysyms(device))
        return !Success;

#endif

    return Success;
}

static int
EvdevMultitouchAddAbsClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch;
    EvdevMultitouchPtr g_pEvdevMultitouch;
    int num_axes, axis, i = 0;
    Atom *atoms;

    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

    g_pEvdevMultitouch = pEvdevMultitouch->core_device->private;

    if (!TestBit(EV_ABS, g_pEvdevMultitouch->bitmask))
            return !Success;

    num_axes = EvdevMultitouchCountBits(g_pEvdevMultitouch->abs_bitmask, NLONGS(ABS_MAX));
    if (num_axes < 1)
        return !Success;
    pEvdevMultitouch->num_vals = num_axes;
    memset(pEvdevMultitouch->vals, 0, num_axes * sizeof(int));
    memset(pEvdevMultitouch->old_vals, -1, num_axes * sizeof(int));
    atoms = malloc(pEvdevMultitouch->num_vals * sizeof(Atom));

    if( !atoms )
    {
        ErrorF("[X11][EvdevMultitouchAddAbsClass] Failed to allocate memory !\n", __FUNCTION__);
	 return !Success;
    }

    for (axis = ABS_X; axis <= ABS_MAX; axis++) {
        pEvdevMultitouch->axis_map[axis] = -1;
        if (!TestBit(axis, pEvdevMultitouch->abs_bitmask))
            continue;
        pEvdevMultitouch->axis_map[axis] = i;
        i++;
    }

    EvdevMultitouchInitAxesLabels(g_pEvdevMultitouch, g_pEvdevMultitouch->num_vals, atoms);

    if (!InitValuatorClassDeviceStruct(device, num_axes,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                       atoms,
#endif
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 3
                                       GetMotionHistory,
#endif
                                       GetMotionHistorySize(), Absolute))
    {
        free(atoms);
        return !Success;
    }

    for (axis = ABS_X; axis <= ABS_MAX; axis++) {
        int axnum = g_pEvdevMultitouch->axis_map[axis];
        if (axnum == -1)
            continue;
        xf86InitValuatorAxisStruct(device, axnum,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                   atoms[axnum],
#endif
                                   pEvdevMultitouch->absinfo[axis].minimum,
                                   pEvdevMultitouch->absinfo[axis].maximum,
                                   10000, 0, 10000, Absolute);
        xf86InitValuatorDefaults(device, axnum);
        pEvdevMultitouch->old_vals[axnum] = -1;
    }

    free(atoms);

    if (!InitPtrFeedbackClassDeviceStruct(device, EvdevMultitouchPtrCtrlProc))
        return !Success;

#if 0
    if ((TestBit(ABS_X, pEvdevMultitouch->abs_bitmask) &&
         TestBit(ABS_Y, pEvdevMultitouch->abs_bitmask)) ||
        (TestBit(ABS_RX, pEvdevMultitouch->abs_bitmask) &&
         TestBit(ABS_RY, pEvdevMultitouch->abs_bitmask)) ||
        (TestBit(ABS_HAT0X, pEvdevMultitouch->abs_bitmask) &&
         TestBit(ABS_HAT0Y, pEvdevMultitouch->abs_bitmask)) ||
        (TestBit(ABS_HAT1X, pEvdevMultitouch->abs_bitmask) &&
         TestBit(ABS_HAT1Y, pEvdevMultitouch->abs_bitmask)) ||
        (TestBit(ABS_HAT2X, pEvdevMultitouch->abs_bitmask) &&
         TestBit(ABS_HAT2Y, pEvdevMultitouch->abs_bitmask)) ||
        (TestBit(ABS_HAT3X, pEvdevMultitouch->abs_bitmask) &&
         TestBit(ABS_HAT3Y, pEvdevMultitouch->abs_bitmask)) ||
        (TestBit(ABS_TILT_X, pEvdevMultitouch->abs_bitmask) &&
         TestBit(ABS_TILT_Y, pEvdevMultitouch->abs_bitmask)))
        pInfo->flags |= XI86_POINTER_CAPABLE;
#endif

    return Success;
}

static int
EvdevMultitouchAddRelClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch;
    int num_axes, axis, i = 0;
    Atom *atoms;

    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

    if (!TestBit(EV_REL, pEvdevMultitouch->bitmask))
        return !Success;

    num_axes = EvdevMultitouchCountBits(pEvdevMultitouch->rel_bitmask, NLONGS(REL_MAX));
    if (num_axes < 1)
        return !Success;

    /* Wheels are special, we post them as button events. So let's ignore them
     * in the axes list too */
    if (TestBit(REL_WHEEL, pEvdevMultitouch->rel_bitmask))
        num_axes--;
    if (TestBit(REL_HWHEEL, pEvdevMultitouch->rel_bitmask))
        num_axes--;
    if (TestBit(REL_DIAL, pEvdevMultitouch->rel_bitmask))
        num_axes--;

    if (num_axes <= 0)
        return !Success;

    pEvdevMultitouch->num_vals = num_axes;
    memset(pEvdevMultitouch->vals, 0, num_axes * sizeof(int));
    atoms = malloc(pEvdevMultitouch->num_vals * sizeof(Atom));

    if( !atoms )
    {
        ErrorF("[X11][EvdevMultitouchAddRelClass] Failed to allocate memory !\n", __FUNCTION__);
	 return !Success;
    }

    for (axis = REL_X; axis <= REL_MAX; axis++)
    {
        pEvdevMultitouch->axis_map[axis] = -1;
        /* We don't post wheel events, so ignore them here too */
        if (axis == REL_WHEEL || axis == REL_HWHEEL || axis == REL_DIAL)
            continue;
        if (!TestBit(axis, pEvdevMultitouch->rel_bitmask))
            continue;
        pEvdevMultitouch->axis_map[axis] = i;
        i++;
    }

    EvdevMultitouchInitAxesLabels(pEvdevMultitouch, pEvdevMultitouch->num_vals, atoms);

    if (!InitValuatorClassDeviceStruct(device, num_axes,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                       atoms,
#endif
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 3
                                       GetMotionHistory,
#endif
                                       GetMotionHistorySize(), Relative))
    {
        free(atoms);
        return !Success;
    }

    for (axis = REL_X; axis <= REL_MAX; axis++)
    {
        int axnum = pEvdevMultitouch->axis_map[axis];

        if (axnum == -1)
            continue;
        xf86InitValuatorAxisStruct(device, axnum,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                atoms[axnum],
#endif
                -1, -1, 1, 0, 1, Relative);
        xf86InitValuatorDefaults(device, axnum);
    }

    free(atoms);

    if (!InitPtrFeedbackClassDeviceStruct(device, EvdevMultitouchPtrCtrlProc))
        return !Success;

    return Success;
}

static int
EvdevMultitouchAddButtonClass(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch;
    Atom *labels;

    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

    labels = malloc(pEvdevMultitouch->num_buttons * sizeof(Atom));

    if( !labels )
        return !Success;

    EvdevMultitouchInitButtonLabels(pEvdevMultitouch, pEvdevMultitouch->num_buttons, labels);

    if (!InitButtonClassDeviceStruct(device, pEvdevMultitouch->num_buttons,
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 7
                                     labels,
#endif
                                     pEvdevMultitouch->btnmap))
    {
        free(labels);
        return !Success;
    }

    free(labels);
    return Success;
}

/**
 * Init the button mapping for the device. By default, this is a 1:1 mapping,
 * i.e. Button 1 maps to Button 1, Button 2 to 2, etc.
 *
 * If a mapping has been specified, the mapping is the default, with the
 * user-defined ones overwriting the defaults.
 * i.e. a user-defined mapping of "3 2 1" results in a mapping of 3 2 1 4 5 6 ...
 *
 * Invalid button mappings revert to the default.
 *
 * Note that index 0 is unused, button 0 does not exist.
 * This mapping is initialised for all devices, but only applied if the device
 * has buttons (in EvdevMultitouchAddButtonClass).
 */
static void
EvdevMultitouchInitButtonMapping(InputInfoPtr pInfo)
{
    int         i, nbuttons     = 1;
    char       *mapping         = NULL;
    EvdevMultitouchPtr    pEvdevMultitouch          = pInfo->private;

    /* Check for user-defined button mapping */
    if ((mapping = xf86CheckStrOption(pInfo->options, "ButtonMapping", NULL)))
    {
        char    *s  = " ";
        int     btn = 0;

        xf86Msg(X_CONFIG, "%s: ButtonMapping '%s'\n", pInfo->name, mapping);
        while (s && *s != '\0' && nbuttons < EVDEVMULTITOUCH_MAXBUTTONS)
        {
            btn = strtol(mapping, &s, 10);

            if (s == mapping || btn < 0 || btn > EVDEVMULTITOUCH_MAXBUTTONS)
            {
                xf86Msg(X_ERROR,
                        "%s: ... Invalid button mapping. Using defaults\n",
                        pInfo->name);
                nbuttons = 1; /* ensure defaults start at 1 */
                break;
            }

            pEvdevMultitouch->btnmap[nbuttons++] = btn;
            mapping = s;
        }
    }

    for (i = nbuttons; i < ArrayLength(pEvdevMultitouch->btnmap); i++)
        pEvdevMultitouch->btnmap[i] = i;

}

static void
EvdevMultitouchInitAnyClass(DeviceIntPtr device, EvdevMultitouchPtr pEvdevMultitouch)
{
    if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_RELATIVE_EVENTS &&
        EvdevMultitouchAddRelClass(device) == Success)
        xf86Msg(X_INFO, "%s: initialized for relative axes.\n", device->name);
    if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_ABSOLUTE_EVENTS &&
        EvdevMultitouchAddAbsClass(device) == Success)
        xf86Msg(X_INFO, "%s: initialized for absolute axes.\n", device->name);
}

static void
EvdevMultitouchInitAbsClass(DeviceIntPtr device, EvdevMultitouchPtr pEvdevMultitouch)
{
    if (EvdevMultitouchAddAbsClass(device) == Success) {

        xf86Msg(X_INFO,"%s: initialized for absolute axes.\n", device->name);

    } else {

        xf86Msg(X_ERROR,"%s: failed to initialize for absolute axes.\n",
                device->name);

        pEvdevMultitouch->flags &= ~EVDEVMULTITOUCH_ABSOLUTE_EVENTS;

    }
}

static void
EvdevMultitouchInitRelClass(DeviceIntPtr device, EvdevMultitouchPtr pEvdevMultitouch)
{
    int has_abs_axes = pEvdevMultitouch->flags & EVDEVMULTITOUCH_ABSOLUTE_EVENTS;

    if (EvdevMultitouchAddRelClass(device) == Success) {

        xf86Msg(X_INFO,"%s: initialized for relative axes.\n", device->name);

        if (has_abs_axes) {

            xf86Msg(X_WARNING,"%s: ignoring absolute axes.\n", device->name);
            pEvdevMultitouch->flags &= ~EVDEVMULTITOUCH_ABSOLUTE_EVENTS;
        }

    } else {

        xf86Msg(X_ERROR,"%s: failed to initialize for relative axes.\n",
                device->name);

        pEvdevMultitouch->flags &= ~EVDEVMULTITOUCH_RELATIVE_EVENTS;

        if (has_abs_axes)
            EvdevMultitouchInitAbsClass(device, pEvdevMultitouch);
    }
}

static void
EvdevMultitouchInitTouchDevice(DeviceIntPtr device, EvdevMultitouchPtr pEvdevMultitouch)
{
    if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_RELATIVE_EVENTS) {

        xf86Msg(X_WARNING,"%s: touchpads, tablets and (multi)touchscreens ignore "
                "relative axes.\n", device->name);

        pEvdevMultitouch->flags &= ~EVDEVMULTITOUCH_RELATIVE_EVENTS;
    }

    EvdevMultitouchInitAbsClass(device, pEvdevMultitouch);
}

static int
EvdevMultitouchInit(DeviceIntPtr device)
{
    int i;
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch;

    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

    /* clear all axis_map entries */
    for(i = 0; i < max(ABS_CNT,REL_CNT); i++)
      pEvdevMultitouch->axis_map[i]=-1;

    if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_KEYBOARD_EVENTS)
	EvdevMultitouchAddKeyClass(device);
    if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_BUTTON_EVENTS)
	EvdevMultitouchAddButtonClass(device);

    /* We don't allow relative and absolute axes on the same device. The
     * reason is that some devices (MS Optical Desktop 2000) register both
     * rel and abs axes for x/y.
     *
     * The abs axes register min/max; this min/max then also applies to the
     * relative device (the mouse) and caps it at 0..255 for both axes.
     * So, unless you have a small screen, you won't be enjoying it much;
     * consequently, absolute axes are generally ignored.
     *
     * However, currenly only a device with absolute axes can be registered
     * as a touch{pad,screen}. Thus, given such a device, absolute axes are
     * used and relative axes are ignored.
     */

    if (pEvdevMultitouch->flags & (EVDEVMULTITOUCH_UNIGNORE_RELATIVE | EVDEVMULTITOUCH_UNIGNORE_ABSOLUTE))
        EvdevMultitouchInitAnyClass(device, pEvdevMultitouch);
    else if (pEvdevMultitouch->flags & (EVDEVMULTITOUCH_TOUCHPAD | EVDEVMULTITOUCH_TOUCHSCREEN | EVDEVMULTITOUCH_TABLET | EVDEVMULTITOUCH_MULTITOUCH))
        EvdevMultitouchInitTouchDevice(device, pEvdevMultitouch);
    else if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_RELATIVE_EVENTS)
        EvdevMultitouchInitRelClass(device, pEvdevMultitouch);
    else if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_ABSOLUTE_EVENTS)
        EvdevMultitouchInitAbsClass(device, pEvdevMultitouch);

#ifdef HAVE_PROPERTIES
    /* We drop the return value, the only time we ever want the handlers to
     * unregister is when the device dies. In which case we don't have to
     * unregister anyway */
    EvdevMultitouchInitProperty(device);
    XIRegisterPropertyHandler(device, EvdevMultitouchSetProperty, NULL, NULL);
    EvdevMultitouchMBEmuInitProperty(device);
    EvdevMultitouchWheelEmuInitProperty(device);
    EvdevMultitouchDragLockInitProperty(device);
#endif

    return Success;
}

/**
 * Init all extras (wheel emulation, etc.) and grab the device.
 *
 * Coming from a resume, the grab may fail with ENODEV. In this case, we set a
 * timer to wake up and try to reopen the device later.
 */
static int
EvdevMultitouchOn(DeviceIntPtr device)
{
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch;
    int rc = 0;
    BOOL finish = False;


    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

    /* If this is an object device,
     * just add device to subdev list */
    if (!EvdevMultitouchIsCoreDevice(pInfo)) {
        finish = True;
    } else {
       if (pInfo->fd != -1 && pEvdevMultitouch->grabDevice &&
            (rc = ioctl(pInfo->fd, EVIOCGRAB, (void *)1)))
        {
            xf86Msg(X_WARNING, "%s: Grab failed (%s)\n", pInfo->name,
                    strerror(errno));

            /* ENODEV - device has disappeared after resume */
            if (rc && errno == ENODEV)
            {
                close(pInfo->fd);
                pInfo->fd = -1;
            }
        }

        if (pInfo->fd == -1)
        {
            pEvdevMultitouch->reopen_left = pEvdevMultitouch->reopen_attempts;
            pEvdevMultitouch->reopen_timer = TimerSet(pEvdevMultitouch->reopen_timer, 0, 100, EvdevMultitouchReopenTimer, pInfo);
        } else
        {
            pEvdevMultitouch->min_maj = EvdevMultitouchGetMajorMinor(pInfo);
            if (EvdevMultitouchIsDuplicate(pInfo))
            {
                xf86Msg(X_WARNING, "%s: Refusing to enable duplicate device.\n",
                        pInfo->name);
                
                return !Success;
            }

            pEvdevMultitouch->reopen_timer = TimerSet(pEvdevMultitouch->reopen_timer, 0, 0, NULL, NULL);

            xf86FlushInput(pInfo->fd);
            
            EvdevMultitouchSetMultitouch(pInfo, pEvdevMultitouch->num_multitouch);
            
            finish = True;
        }
    }

    if (finish) {
	 if( !strstr(pInfo->name, "subdev" ) )
 	{
        	xf86AddEnabledDevice(pInfo);
		pEvdevMultitouch->last_slot = 0;
		EvdevMultitouchReinitPEvdevMultitouch(pInfo);
 	}
        EvdevMultitouchMBEmuOn(pInfo);
        pEvdevMultitouch->flags |= EVDEVMULTITOUCH_INITIALIZED;
        device->public.on = TRUE;

	 pEvdevMultitouch->multitouch_setting_timer = TimerSet(pEvdevMultitouch->multitouch_setting_timer, 0, 100, EvdevMultitouchMultitouchSettingTimer, pInfo);
    }

    return Success;
}

static void
EvdevMultitouchOff(DeviceIntPtr device)
{
	InputInfoPtr pInfo;
	EvdevMultitouchPtr pEvdevMultitouch;

	pInfo = device->public.devicePrivate;
	pEvdevMultitouch = pInfo->private;
}

static int
EvdevMultitouchProc(DeviceIntPtr device, int what)
{
    InputInfoPtr pInfo;
    EvdevMultitouchPtr pEvdevMultitouch, g_pEvdevMultitouch;
    int i;


    pInfo = device->public.devicePrivate;
    pEvdevMultitouch = pInfo->private;

    switch (what)
    {
    case DEVICE_INIT:
	return EvdevMultitouchInit(device);

    case DEVICE_ON:
        return EvdevMultitouchOn(device);

    case DEVICE_OFF:
	 EvdevMultitouchOff(device);
        if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_INITIALIZED)
            EvdevMultitouchMBEmuFinalize(pInfo);
        if (EvdevMultitouchIsCoreDevice(pInfo)){
            EvdevMultitouchSetMultitouch(pInfo, 0);
            if (pInfo->fd != -1)
            {
                if (pEvdevMultitouch->grabDevice && ioctl(pInfo->fd, EVIOCGRAB, (void *)0))
                    xf86Msg(X_WARNING, "%s: Release failed (%s)\n", pInfo->name,
                            strerror(errno));
                xf86RemoveEnabledDevice(pInfo);
                close(pInfo->fd);
                pInfo->fd = -1;
            }
	     if( pEvdevMultitouch->multitouch_setting_timer )
	     {
	     	  TimerFree(pEvdevMultitouch->multitouch_setting_timer);
	         pEvdevMultitouch->multitouch_setting_timer = NULL;
	     }
            if (pEvdevMultitouch->reopen_timer)
            {
                TimerFree(pEvdevMultitouch->reopen_timer);
                pEvdevMultitouch->reopen_timer = NULL;
            }
        } else {
            /* removing it in the list of the core device */
            g_pEvdevMultitouch = pEvdevMultitouch->core_device->private;
            for (i=0; i<MAX_VALUATORS_MT; ++i) {
                if (g_pEvdevMultitouch->vals_mt[i].pInfo == pInfo) {
                    g_pEvdevMultitouch->vals_mt[i].pInfo = NULL;
                    break;
                }
            }
            if (pInfo->fd >= 0)
            {
                xf86RemoveEnabledDevice(pInfo);
            }
        }
        pEvdevMultitouch->min_maj = 0;
        pEvdevMultitouch->flags &= ~EVDEVMULTITOUCH_INITIALIZED;
        device->public.on = FALSE;
    break;

    case DEVICE_CLOSE:
        xf86Msg(X_INFO, "%s: Close\n", pInfo->name);
        if (EvdevMultitouchIsCoreDevice(pInfo)) { // master only
            //EvdevMultitouchDeleteAllSubdevices(pInfo);
            
            if (pInfo->fd != -1) {
                close(pInfo->fd);
                pInfo->fd = -1;
            }
            EvdevMultitouchRemoveDevice(pInfo);
        }
        pEvdevMultitouch->min_maj = 0;
    break;
    }

    return Success;
}

/**
 * Get as much information as we can from the fd and cache it.
 * If compare is True, then the information retrieved will be compared to the
 * one already cached. If the information does not match, then this function
 * returns an error.
 *
 * @return Success if the information was cached, or !Success otherwise.
 */
static int
EvdevMultitouchCacheCompare(InputInfoPtr pInfo, BOOL compare)
{
    int i;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    int len;

    char name[1024]                  = {0};
    unsigned long bitmask[NLONGS(EV_CNT)]      = {0};
    unsigned long key_bitmask[NLONGS(KEY_CNT)] = {0};
    unsigned long rel_bitmask[NLONGS(REL_CNT)] = {0};
    unsigned long abs_bitmask[NLONGS(ABS_CNT)] = {0};
    unsigned long led_bitmask[NLONGS(LED_CNT)] = {0};

    if (ioctl(pInfo->fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
        xf86Msg(X_ERROR, "ioctl EVIOCGNAME failed: %s\n", strerror(errno));
        goto error;
    }

    if (!compare) {
        strcpy(pEvdevMultitouch->name, name);
    } else if (strcmp(pEvdevMultitouch->name, name)) {
        xf86Msg(X_ERROR, "%s: device name changed: %s != %s\n",
                pInfo->name, pEvdevMultitouch->name, name);
        goto error;
    }

    len = ioctl(pInfo->fd, EVIOCGBIT(0, sizeof(bitmask)), bitmask);
    if (len < 0) {
        xf86Msg(X_ERROR, "%s: ioctl EVIOCGBIT failed: %s\n",
                pInfo->name, strerror(errno));
        goto error;
    }

    if (!compare) {
        memcpy(pEvdevMultitouch->bitmask, bitmask, len);
    } else if (memcmp(pEvdevMultitouch->bitmask, bitmask, len)) {
        xf86Msg(X_ERROR, "%s: device bitmask has changed\n", pInfo->name);
        goto error;
    }

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_REL, sizeof(rel_bitmask)), rel_bitmask);
    if (len < 0) {
        xf86Msg(X_ERROR, "%s: ioctl EVIOCGBIT failed: %s\n",
                pInfo->name, strerror(errno));
        goto error;
    }

    if (!compare) {
        memcpy(pEvdevMultitouch->rel_bitmask, rel_bitmask, len);
    } else if (memcmp(pEvdevMultitouch->rel_bitmask, rel_bitmask, len)) {
        xf86Msg(X_ERROR, "%s: device rel_bitmask has changed\n", pInfo->name);
        goto error;
    }

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_ABS, sizeof(abs_bitmask)), abs_bitmask);
    if (len < 0) {
        xf86Msg(X_ERROR, "%s: ioctl EVIOCGBIT failed: %s\n",
                pInfo->name, strerror(errno));
        goto error;
    }

    if( TestBit(ABS_MT_SLOT, abs_bitmask) && TestBit(ABS_MT_TRACKING_ID, abs_bitmask) )
    {
         ErrorF("[X11] MT Protocol B Type : ABS_MT_SLOT is supported : \n");
	  pEvdevMultitouch->mt_slot_supported = (BOOL)1;
    }
    else
    {
         ErrorF("[X11] MT Protocol A Type : ABS_MT_SLOT is NOT supported\n");
	  pEvdevMultitouch->mt_slot_supported = (BOOL)0;
    }

    if (!compare) {
        memcpy(pEvdevMultitouch->abs_bitmask, abs_bitmask, len);
    } else if (memcmp(pEvdevMultitouch->abs_bitmask, abs_bitmask, len)) {
        xf86Msg(X_ERROR, "%s: device abs_bitmask has changed\n", pInfo->name);
        goto error;
    }

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_LED, sizeof(led_bitmask)), led_bitmask);
    if (len < 0) {
        xf86Msg(X_ERROR, "%s: ioctl EVIOCGBIT failed: %s\n",
                pInfo->name, strerror(errno));
        goto error;
    }

    if (!compare) {
        memcpy(pEvdevMultitouch->led_bitmask, led_bitmask, len);
    } else if (memcmp(pEvdevMultitouch->led_bitmask, led_bitmask, len)) {
        xf86Msg(X_ERROR, "%s: device led_bitmask has changed\n", pInfo->name);
        goto error;
    }

    /*
     * Do not try to validate absinfo data since it is not expected
     * to be static, always refresh it in evdevmultitouch structure.
     */
	if( !xf86CheckStrOption(pInfo->options, "Resolution", NULL) )
	{
	    for (i = ABS_X; i <= ABS_MAX; i++) {
	        if (TestBit(i, abs_bitmask)) {
	            len = ioctl(pInfo->fd, EVIOCGABS(i), &pEvdevMultitouch->absinfo[i]);
	            if (len < 0) {
	                xf86Msg(X_ERROR, "%s: ioctl EVIOCGABSi(%d) failed: %s\n",
	                        pInfo->name, i, strerror(errno));
	                goto error;
	            }
	        }
	    }
	}

    len = ioctl(pInfo->fd, EVIOCGBIT(EV_KEY, sizeof(key_bitmask)), key_bitmask);
    if (len < 0) {
        xf86Msg(X_ERROR, "%s: ioctl EVIOCGBIT failed: %s\n",
                pInfo->name, strerror(errno));
        goto error;
    }

    if (compare) {
        /*
         * Keys are special as user can adjust keymap at any time (on
         * devices that support EVIOCSKEYCODE. However we do not expect
         * buttons reserved for mice/tablets/digitizers and so on to
         * appear/disappear so we will check only those in
         * [BTN_MISC, KEY_OK) range.
         */
        size_t start_word = BTN_MISC / LONG_BITS;
        size_t start_byte = start_word * sizeof(unsigned long);
        size_t end_word = KEY_OK / LONG_BITS;
        size_t end_byte = end_word * sizeof(unsigned long);

        if (len >= start_byte &&
            memcmp(&pEvdevMultitouch->key_bitmask[start_word], &key_bitmask[start_word],
                   min(len, end_byte) - start_byte + 1)) {
            xf86Msg(X_ERROR, "%s: device key_bitmask has changed\n", pInfo->name);
            goto error;
        }
    }

    /* Copy the data so we have reasonably up-to-date info */
    memcpy(pEvdevMultitouch->key_bitmask, key_bitmask, len);

    return Success;

error:
    return !Success;

}

static int
EvdevMultitouchProbe(InputInfoPtr pInfo)
{
    int i, has_rel_axes, has_abs_axes, has_keys, num_buttons, has_scroll;
    int kernel24 = 0;
    int ignore_abs = 0, ignore_rel = 0;
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    if (pEvdevMultitouch->grabDevice && ioctl(pInfo->fd, EVIOCGRAB, (void *)1)) {
        if (errno == EINVAL) {
            /* keyboards are unsafe in 2.4 */
            kernel24 = 1;
            pEvdevMultitouch->grabDevice = 0;
        } else {
            xf86Msg(X_ERROR, "Grab failed. Device already configured?\n");
            return 1;
        }
    } else if (pEvdevMultitouch->grabDevice) {
        ioctl(pInfo->fd, EVIOCGRAB, (void *)0);
    }

    /* Trinary state for ignoring axes:
       - unset: do the normal thing.
       - TRUE: explicitly ignore them.
       - FALSE: unignore axes, use them at all cost if they're present.
     */
    if (xf86FindOption(pInfo->options, "IgnoreRelativeAxes"))
    {
        if (xf86SetBoolOption(pInfo->options, "IgnoreRelativeAxes", FALSE))
            ignore_rel = TRUE;
        else
            pEvdevMultitouch->flags |= EVDEVMULTITOUCH_UNIGNORE_RELATIVE;

    }
    if (xf86FindOption(pInfo->options, "IgnoreAbsoluteAxes"))
    {
        if (xf86SetBoolOption(pInfo->options, "IgnoreAbsoluteAxes", FALSE))
           ignore_abs = TRUE;
        else
            pEvdevMultitouch->flags |= EVDEVMULTITOUCH_UNIGNORE_ABSOLUTE;
    }

    has_rel_axes = FALSE;
    has_abs_axes = FALSE;
    has_keys = FALSE;
    has_scroll = FALSE;
    num_buttons = 0;

    /* count all buttons */
    for (i = BTN_MISC; i < BTN_JOYSTICK; i++)
    {
        int mapping = 0;
        if (TestBit(i, pEvdevMultitouch->key_bitmask))
        {
            mapping = EvdevMultitouchUtilButtonEventToButtonNumber(pEvdevMultitouch, i);
            if (mapping > num_buttons)
                num_buttons = mapping;
        }
    }

    if (num_buttons)
    {
        pEvdevMultitouch->flags |= EVDEVMULTITOUCH_BUTTON_EVENTS;
        pEvdevMultitouch->num_buttons = num_buttons;
        xf86Msg(X_INFO, "%s: Found %d mouse buttons\n", pInfo->name,
                num_buttons);
    }

    for (i = 0; i < REL_MAX; i++) {
        if (TestBit(i, pEvdevMultitouch->rel_bitmask)) {
            has_rel_axes = TRUE;
            break;
        }
    }

    if (has_rel_axes) {
        if (TestBit(REL_WHEEL, pEvdevMultitouch->rel_bitmask) ||
            TestBit(REL_HWHEEL, pEvdevMultitouch->rel_bitmask) ||
            TestBit(REL_DIAL, pEvdevMultitouch->rel_bitmask)) {
            xf86Msg(X_INFO, "%s: Found scroll wheel(s)\n", pInfo->name);
            has_scroll = TRUE;
            if (!num_buttons)
                xf86Msg(X_INFO, "%s: Forcing buttons for scroll wheel(s)\n",
                        pInfo->name);
            num_buttons = (num_buttons < 3) ? 7 : num_buttons + 4;
            pEvdevMultitouch->num_buttons = num_buttons;
        }

        if (!ignore_rel)
        {
            xf86Msg(X_INFO, "%s: Found relative axes\n", pInfo->name);
            pEvdevMultitouch->flags |= EVDEVMULTITOUCH_RELATIVE_EVENTS;

            if (TestBit(REL_X, pEvdevMultitouch->rel_bitmask) &&
                TestBit(REL_Y, pEvdevMultitouch->rel_bitmask)) {
                xf86Msg(X_INFO, "%s: Found x and y relative axes\n", pInfo->name);
            }
        } else {
            xf86Msg(X_INFO, "%s: Relative axes present but ignored.\n", pInfo->name);
            has_rel_axes = FALSE;
        }
    }

    for (i = 0; i < ABS_MAX; i++) {
        if (TestBit(i, pEvdevMultitouch->abs_bitmask)) {
            has_abs_axes = TRUE;
            break;
        }
    }

    if (ignore_abs && has_abs_axes)
    {
        xf86Msg(X_INFO, "%s: Absolute axes present but ignored.\n", pInfo->name);
        has_abs_axes = FALSE;
    } else if (has_abs_axes) {
        xf86Msg(X_INFO, "%s: Found absolute axes\n", pInfo->name);
        pEvdevMultitouch->flags |= EVDEVMULTITOUCH_ABSOLUTE_EVENTS;

        if ((TestBit(ABS_X, pEvdevMultitouch->abs_bitmask) &&
             TestBit(ABS_Y, pEvdevMultitouch->abs_bitmask))) {
            xf86Msg(X_INFO, "%s: Found x and y absolute axes\n", pInfo->name);
            if (TestBit(BTN_TOOL_PEN, pEvdevMultitouch->key_bitmask))
            {
                xf86Msg(X_INFO, "%s: Found absolute tablet.\n", pInfo->name);
                pEvdevMultitouch->flags |= EVDEVMULTITOUCH_TABLET;
            } else if ((TestBit(ABS_MT_POSITION_X, pEvdevMultitouch->abs_bitmask) &&
                        TestBit(ABS_MT_POSITION_Y, pEvdevMultitouch->abs_bitmask))) {
                xf86Msg(X_INFO, "%s: Found absolute multitouch tablet.\n", pInfo->name);
                pEvdevMultitouch->flags |= EVDEVMULTITOUCH_MULTITOUCH;
            } else if (TestBit(ABS_PRESSURE, pEvdevMultitouch->abs_bitmask) ||
                TestBit(BTN_TOUCH, pEvdevMultitouch->key_bitmask)) {
                if (num_buttons || TestBit(BTN_TOOL_FINGER, pEvdevMultitouch->key_bitmask)) {
                    xf86Msg(X_INFO, "%s: Found absolute touchpad.\n", pInfo->name);
                    pEvdevMultitouch->flags |= EVDEVMULTITOUCH_TOUCHPAD;
                    memset(pEvdevMultitouch->old_vals, -1, sizeof(int) * pEvdevMultitouch->num_vals);
                } else {
                    xf86Msg(X_INFO, "%s: Found absolute touchscreen\n", pInfo->name);
                    pEvdevMultitouch->flags |= EVDEVMULTITOUCH_TOUCHSCREEN;
                    pEvdevMultitouch->flags |= EVDEVMULTITOUCH_BUTTON_EVENTS;
                }
            }
        }
    }

    for (i = 0; i < BTN_MISC; i++) {
        if (TestBit(i, pEvdevMultitouch->key_bitmask)) {
            xf86Msg(X_INFO, "%s: Found keys\n", pInfo->name);
            pEvdevMultitouch->flags |= EVDEVMULTITOUCH_KEYBOARD_EVENTS;
            has_keys = TRUE;
            break;
        }
    }

    if (has_rel_axes || has_abs_axes || num_buttons) {
	if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_TOUCHPAD) {
	    xf86Msg(X_INFO, "%s: Configuring as touchpad\n", pInfo->name);
	    pInfo->type_name = XI_TOUCHPAD;
	} else if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_TABLET) {
	    xf86Msg(X_INFO, "%s: Configuring as tablet\n", pInfo->name);
	    pInfo->type_name = XI_TABLET;
        } else if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_TOUCHSCREEN) {
            xf86Msg(X_INFO, "%s: Configuring as touchscreen\n", pInfo->name);
            pInfo->type_name = XI_TOUCHSCREEN;
        } else if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH) {
            xf86Msg(X_INFO, "%s: Configuring as multitouch screen\n", pInfo->name);
            pInfo->type_name = "MULTITOUCHSCREEN";
        } else {
            xf86Msg(X_INFO, "%s: Configuring as mouse\n", pInfo->name);
            pInfo->type_name = XI_MOUSE;
        }
    }

    if (has_keys) {
        if (kernel24) {
            xf86Msg(X_INFO, "%s: Kernel < 2.6 is too old, ignoring keyboard\n",
                    pInfo->name);
        } else {
            xf86Msg(X_INFO, "%s: Configuring as keyboard\n", pInfo->name);
	    pInfo->type_name = XI_KEYBOARD;
        }
    }

    if (has_scroll)
    {
        xf86Msg(X_INFO, "%s: Adding scrollwheel support\n", pInfo->name);
        pEvdevMultitouch->flags |= EVDEVMULTITOUCH_BUTTON_EVENTS;
        pEvdevMultitouch->flags |= EVDEVMULTITOUCH_RELATIVE_EVENTS;
    }

    return 0;
}

static void
EvdevMultitouchSetCalibration(InputInfoPtr pInfo, int num_calibration, int calibration[4])
{
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    if (num_calibration == 0) {
        pEvdevMultitouch->flags &= ~EVDEVMULTITOUCH_CALIBRATED;
        pEvdevMultitouch->calibration.min_x = 0;
        pEvdevMultitouch->calibration.max_x = 0;
        pEvdevMultitouch->calibration.min_y = 0;
        pEvdevMultitouch->calibration.max_y = 0;
    } else if (num_calibration == 4) {
        pEvdevMultitouch->flags |= EVDEVMULTITOUCH_CALIBRATED;
        pEvdevMultitouch->calibration.min_x = calibration[0];
        pEvdevMultitouch->calibration.max_x = calibration[1];
        pEvdevMultitouch->calibration.min_y = calibration[2];
        pEvdevMultitouch->calibration.max_y = calibration[3];
    }
}

static void
EvdevMultitouchSetResolution(InputInfoPtr pInfo, int num_resolution, int resolution[4])
{
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

    if (num_resolution == 0) {
        pEvdevMultitouch->flags &= ~EVDEVMULTITOUCH_RESOLUTION;
        pEvdevMultitouch->resolution.min_x = 0;
        pEvdevMultitouch->resolution.max_x = 0;
        pEvdevMultitouch->resolution.min_y = 0;
        pEvdevMultitouch->resolution.max_y = 0;
    } else if (num_resolution == 4) {
        pEvdevMultitouch->flags |= EVDEVMULTITOUCH_RESOLUTION;
        pEvdevMultitouch->resolution.min_x = resolution[0];
        pEvdevMultitouch->resolution.max_x = resolution[1];
        pEvdevMultitouch->resolution.min_y = resolution[2];
        pEvdevMultitouch->resolution.max_y = resolution[3];
    }
}

static void EvdevMultitouchSetTransform(InputInfoPtr pInfo, int num_transform, float *tmatrix)
{
	int x, y;
	struct pixman_transform tr;
	EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;

	if( num_transform != 9 )
	{
		pEvdevMultitouch->use_transform = False;
		return;
	}
	pEvdevMultitouch->use_transform = True;

	memcpy(pEvdevMultitouch->transform, tmatrix, sizeof(pEvdevMultitouch->transform));
	for (y=0; y<3; y++)
		for (x=0; x<3; x++)
			tr.matrix[y][x] = pixman_double_to_fixed((double)*tmatrix++);

	pixman_transform_invert(&pEvdevMultitouch->inv_transform, &tr);
}

static void
EvdevMultitouchSetMultitouch(InputInfoPtr pInfo, int num_multitouch) {
    EvdevMultitouchPtr pEvdevMultitouch = pInfo->private;
    int i, rc;

    if( !(pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH) ) 
    { 
            ErrorF("[X11][%s] Device is not a multitouch screen !(flags=%d)\n", __FUNCTION__, pEvdevMultitouch->flags); 
            return; 
    }

    if (num_multitouch > MAX_VALUATORS_MT)
        num_multitouch = MAX_VALUATORS_MT;
    if (num_multitouch < 0)
        num_multitouch = 0;

    for (i=0;i<num_multitouch;++i) {
        if (pEvdevMultitouch->vals_mt[i].pInfo == NULL){
            pEvdevMultitouch->vals_mt[i].containsValues = FALSE;
            pEvdevMultitouch->vals_mt[i].id = -1;
            pEvdevMultitouch->vals_mt[i].pInfo = EvdevMultitouchCreateSubDevice(pInfo, i);
        }
    }
    for (i=num_multitouch;i<MAX_VALUATORS_MT;++i) {
        pEvdevMultitouch->vals_mt[i].containsValues = FALSE;
        pEvdevMultitouch->vals_mt[i].id = -1;
        if (pEvdevMultitouch->vals_mt[i].pInfo && pEvdevMultitouch->vals_mt[i].pInfo != pInfo) {
            EvdevMultitouchDeleteSubDevice(pInfo, pEvdevMultitouch->vals_mt[i].pInfo);
            pEvdevMultitouch->vals_mt[i].pInfo = NULL;
        }
    }
    
    pEvdevMultitouch->num_multitouch = num_multitouch;

    rc = XIChangeDeviceProperty(pInfo->dev, prop_multitouch, XA_INTEGER, 8, 
		PropModeReplace, 1, &pEvdevMultitouch->num_multitouch, FALSE);

    if (rc != Success)
    	    ErrorF("[X11][%s] Failed to Change device property !\n", __FUNCTION__);
}

Bool
IsMaster(DeviceIntPtr dev)
{
    return dev->type == MASTER_POINTER || dev->type == MASTER_KEYBOARD;
}

DeviceIntPtr
GetPairedDevice(DeviceIntPtr dev)
{
    if (!IsMaster(dev) && dev->master)
        dev = dev->master;

    return dev->spriteInfo->paired;
}

DeviceIntPtr
GetMaster(DeviceIntPtr dev, int which)
{
    DeviceIntPtr master;

    if (IsMaster(dev))
        master = dev;
    else
        master = dev->master;

    if (master)
    {
        if (which == MASTER_KEYBOARD)
        {
            if (master->type != MASTER_KEYBOARD)
                master = GetPairedDevice(master);
        } else
        {
            if (master->type != MASTER_POINTER)
                master = GetPairedDevice(master);
        }
    }

    return master;
}

static void
EvdevMultitouchGetGrabInfo(InputInfoPtr pInfo, BOOL  val)
{
	DeviceIntPtr master_pointer;

	if( val == 1 )
	{
		if( pInfo->dev->deviceGrab.grab )
		{
			ErrorF("[X11][EvdevMultitouchGetGrabInfo] Device id=%d (grabbed) !\n", pInfo->dev->id);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  (event) type=%d\n", pInfo->dev->deviceGrab.grab->type);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  grabtype=%d\n", pInfo->dev->deviceGrab.grab->grabtype);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  resource=0x%x\n", (unsigned int)pInfo->dev->deviceGrab.grab->resource);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  keyboardMode=%d\n", pInfo->dev->deviceGrab.grab->keyboardMode);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  pointerMode=%d\n", pInfo->dev->deviceGrab.grab->pointerMode);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  sync.frozen=%d\n", pInfo->dev->deviceGrab.sync.frozen);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  fromPassiveGrab=%d\n", pInfo->dev->deviceGrab.fromPassiveGrab);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  implicitGrab=%d\n", pInfo->dev->deviceGrab.implicitGrab);
		}
		else if( (master_pointer = GetMaster(pInfo->dev, MASTER_POINTER)) && master_pointer->deviceGrab.grab )
		{
			ErrorF("[X11][EvdevMultitouchGetGrabInfo] Device id=%d (master_pointer, grabbed) !\n", master_pointer->id);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  (event) type=%d\n", master_pointer->deviceGrab.grab->type);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  grabtype=%d\n", master_pointer->deviceGrab.grab->grabtype);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  resource=0x%x\n", (unsigned int)master_pointer->deviceGrab.grab->resource);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  keyboardMode=%d\n", master_pointer->deviceGrab.grab->keyboardMode);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  pointerMode=%d\n", master_pointer->deviceGrab.grab->pointerMode);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  sync.frozen=%d\n", master_pointer->deviceGrab.sync.frozen);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  fromPassiveGrab=%d\n", master_pointer->deviceGrab.fromPassiveGrab);
			ErrorF("[X11][EvdevMultitouchGetGrabInfo]  implicitGrab=%d\n", master_pointer->deviceGrab.implicitGrab);
		}
		else
			ErrorF("[X11][EvdevMultitouchGetGrabInfo] Device id=%d (ungrabbed) !\n", pInfo->dev->id);
	}
}

static int
EvdevMultitouchPreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    int rc = BadAlloc;
    const char *device, *str;
    int num_calibration = 0, calibration[4] = { 0, 0, 0, 0 };
    int num_resolution = 0, resolution[4] = { 0, 0, 0, 0 };
    int num_transform = 0; float tr[9];
    EvdevMultitouchPtr pEvdevMultitouch;
    char *type;
    char *name;

    if(!pInfo)
    {
        xf86DrvMsg(-1, X_ERROR, "[X11][EvdevMultitouchPreInit] pInfo is NULL !\n");
        goto error;
    }

    /* Initialise the InputInfoRec. */
    pInfo->flags = 0;
    pInfo->type_name = "UNKNOWN";
    pInfo->device_control = EvdevMultitouchProc;
    pInfo->switch_mode = NULL;
    pInfo->dev = NULL;

    if (!(pEvdevMultitouch = calloc(sizeof(EvdevMultitouchRec), 1)))
        goto error;

    pInfo->private = pEvdevMultitouch;
    memset(evdevmultitouch_devices, 0, sizeof(EvdevMultitouchPtr) * MAX_MT);

    xf86CollectInputOptions(pInfo, evdevmultitouchDefaults);
    xf86ProcessCommonOptions(pInfo, pInfo->options);
    pEvdevMultitouch->id = -1;

 #ifdef _F_SUPPORT_PREFERRED_NAME_
    if (!strstr(pInfo->name, "subdev") && !strstr(pInfo->name, "Virtual")) {
        str = xf86CheckStrOption(pInfo->options, "PreferredName", NULL);
        if (str) {
            pInfo->name = str;
        }
        else {
            name = malloc((strlen("Touchscreen")+1)*sizeof(char));
            if(!name)
            {
            	ErrorF("[X11][EvdevMultitouchPreInit] Failed to allocate memory for a name of a touchscreen device !\n");
            	goto error;
            }
            snprintf(name, strlen("Touchscreen")+1, "Touchscreen");
            pInfo->name = name;
        }
    }
#endif//_F_SUPPORT_PREFERRED_NAME_
 
     /* If Type == Object, this is a device for an object to use */
    type = xf86CheckStrOption(pInfo->options, "Type", NULL);

    xf86Msg(X_INFO, "%s: EvdevMultitouch Type %s found\n", pInfo->name, type);
       
    if (type != NULL && strcmp(type, "Object") == 0) {
        EvdevMultitouchPtr pCreatorEvdevMultitouch;
        if (!pCreatorInfo){
            return Success;
        }
        pCreatorEvdevMultitouch = pCreatorInfo->private;
        xf86Msg(X_INFO, "%s: EvdevMultitouch subdevice found\n", pInfo->name);
        memcpy(pEvdevMultitouch, pCreatorEvdevMultitouch, sizeof(EvdevMultitouchRec));
        pInfo->read_input = EvdevMultitouchSubdevReadInput;
        pEvdevMultitouch->associated = FALSE;
        pInfo->type_name = pCreatorInfo->type_name;
        pEvdevMultitouch->num_multitouch = 1;
        //EvdevMultitouchInitButtonMapping(pInfo);
   } else {
        pInfo->read_input = EvdevMultitouchReadInput;

        pEvdevMultitouch->core_device = pInfo;
        pEvdevMultitouch->associated = FALSE;

        memset(pEvdevMultitouch->vals, 0, MAX_VALUATORS * sizeof(int));
        memset(pEvdevMultitouch->old_vals, -1, MAX_VALUATORS * sizeof(int));
        
        /*
         * We initialize pEvdevMultitouch->tool to 1 so that device that doesn't use
         * proximity will still report events.
         */
        pEvdevMultitouch->tool = 1;

        device = xf86CheckStrOption(pInfo->options, "Device", NULL);
        if (!device) {
            xf86Msg(X_ERROR, "%s: No device specified.\n", pInfo->name);
            rc = BadValue;
            goto error;
        }

        pEvdevMultitouch->device = device;

        xf86Msg(X_CONFIG, "%s: Device: \"%s\"\n", pInfo->name, device);
        do {
            pInfo->fd = open(device, O_RDWR | O_NONBLOCK, 0);
        } while (pInfo->fd < 0 && errno == EINTR);

        if (pInfo->fd < 0) {
            xf86Msg(X_ERROR, "Unable to open evdevmultitouch device \"%s\".\n", device);
            rc = BadValue;
            goto error;
        }

        /* Check major/minor of device node to avoid adding duplicate devices. */
        pEvdevMultitouch->min_maj = EvdevMultitouchGetMajorMinor(pInfo);
        if (EvdevMultitouchIsDuplicate(pInfo))
        {
            xf86Msg(X_WARNING, "%s: device file already in use. Ignoring.\n",
                    pInfo->name);
            close(pInfo->fd);
            rc = BadValue;
            goto error;
        }

        pEvdevMultitouch->reopen_attempts = xf86SetIntOption(pInfo->options, "ReopenAttempts", 10);
        pEvdevMultitouch->invert_x = xf86SetBoolOption(pInfo->options, "InvertX", FALSE);
        pEvdevMultitouch->invert_y = xf86SetBoolOption(pInfo->options, "InvertY", FALSE);
        pEvdevMultitouch->num_multitouch = xf86SetIntOption(pInfo->options, "MultiTouch", 0);
        pEvdevMultitouch->swap_axes = xf86SetBoolOption(pInfo->options, "SwapAxes", FALSE);

        str = xf86CheckStrOption(pInfo->options, "Calibration", NULL);
        if (str) {
            num_calibration = sscanf(str, "%d %d %d %d",
                                     &calibration[0], &calibration[1],
                                     &calibration[2], &calibration[3]);
            if (num_calibration == 4)
                EvdevMultitouchSetCalibration(pInfo, num_calibration, calibration);
            else
                xf86Msg(X_ERROR,
                        "%s: Insufficient calibration factors (%d). Ignoring calibration\n",
                        pInfo->name, num_calibration);
        }

        str = xf86CheckStrOption(pInfo->options, "Resolution", NULL);
        if (str) {
            num_resolution = sscanf(str, "%d %d %d %d",
                                     &resolution[0], &resolution[1],
                                     &resolution[2], &resolution[3]);
            if (num_resolution == 4)
                EvdevMultitouchSetResolution(pInfo, num_resolution, resolution);
            else
                xf86Msg(X_ERROR,
                        "%s: Insufficient resolution factors (%d). Ignoring resolution\n",
                        pInfo->name, num_resolution);
        }

	 pEvdevMultitouch->use_transform = False;
        str = xf86CheckStrOption(pInfo->options, "Transform", NULL);
        if (str) {
            num_transform = sscanf(str, "%f %f %f %f %f %f %f %f %f",
                                     &tr[0], &tr[1], &tr[2],
                                     &tr[3], &tr[4], &tr[5], 
                                     &tr[6], &tr[7], &tr[8]);
            if (num_transform == 9)
                EvdevMultitouchSetTransform(pInfo, num_transform, tr);
            else
            {
                xf86Msg(X_ERROR,
                        "%s: Insufficient transform factors (%d). Ignoring transform\n",
                        pInfo->name, num_transform);
            }
        }

        /* Grabbing the event device stops in-kernel event forwarding. In other
           words, it disables rfkill and the "Macintosh mouse button emulation".
           Note that this needs a server that sets the console to RAW mode. */
        pEvdevMultitouch->grabDevice = xf86CheckBoolOption(pInfo->options, "GrabDevice", 0);

        /* Get setting for checking wether a point is still alive */
        pEvdevMultitouch->timeout = xf86CheckIntOption(pInfo->options,
                "SubdevTimeout", DEFAULT_TIMEOUT);
        if (pEvdevMultitouch->timeout < 1) {
            pEvdevMultitouch->timeout = 1;
        }
        xf86Msg(X_INFO, "%s: SubdevTimeout set to %d\n",
                pInfo->name, (int)pEvdevMultitouch->timeout);

        EvdevMultitouchInitButtonMapping(pInfo);

        if (EvdevMultitouchCacheCompare(pInfo, FALSE) ||
            EvdevMultitouchProbe(pInfo)) {
            close(pInfo->fd);
            rc = BadValue;
            goto error;
        }
        
        if ((pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH) && !pEvdevMultitouch->num_buttons) {
            /* absolute multitouch screen :
             * forcing num_buttons = 1
             */
            pEvdevMultitouch->num_buttons = 1;
            pEvdevMultitouch->flags |= EVDEVMULTITOUCH_BUTTON_EVENTS;
        }

        if(pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH)
        {
            pEvdevMultitouch->num_multitouch = 1;
            pEvdevMultitouch->vals_mt[0].pInfo = pInfo;
        }

	if(pEvdevMultitouch->flags & EVDEVMULTITOUCH_RESOLUTION)
	{
		EvdevMultitouchSwapAxes(pEvdevMultitouch);
	}

        // register only the core device
        EvdevMultitouchAddDevice(pInfo);
    }

    pEvdevMultitouch->use_transform = False;

    if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_BUTTON_EVENTS)
    {
        EvdevMultitouchMBEmuPreInit(pInfo);
        EvdevMultitouchWheelEmuPreInit(pInfo);
        EvdevMultitouchDragLockPreInit(pInfo);
    }

    return Success;

error:
    if ((pInfo) && (pInfo->fd >= 0))
        close(pInfo->fd);
    return rc;
}

_X_EXPORT InputDriverRec EVDEVMULTITOUCHh = {
    1,
    "evdevmultitouch",
    NULL,
    EvdevMultitouchPreInit,
    NULL,
    NULL,
    0
};

static void
EvdevMultitouchUnplug(pointer	p)
{
}

static pointer
EvdevMultitouchPlug(pointer	module,
          pointer	options,
          int		*errmaj,
          int		*errmin)
{
    xf86AddInputDriver(&EVDEVMULTITOUCHh, module, 0);
    return module;
}

static XF86ModuleVersionInfo EvdevMultitouchVersionRec =
{
    "evdevmultitouch",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData evdevmultitouchModuleData =
{
    &EvdevMultitouchVersionRec,
    EvdevMultitouchPlug,
    EvdevMultitouchUnplug
};


/* Return an index value for a given button event code
 * returns 0 on non-button event.
 */
unsigned int
EvdevMultitouchUtilButtonEventToButtonNumber(EvdevMultitouchPtr pEvdevMultitouch, int code)
{
    unsigned int button = 0;

    switch(code) {
    case BTN_LEFT:
	button = 1;
	break;

    case BTN_RIGHT:
	button = 3;
	break;

    case BTN_MIDDLE:
	button = 2;
	break;

        /* Treat BTN_[0-2] as LMR buttons on devices that do not advertise
           BTN_LEFT, BTN_MIDDLE, BTN_RIGHT.
           Otherwise, treat BTN_[0+n] as button 5+n.
           XXX: This causes duplicate mappings for BTN_0 + n and BTN_SIDE + n
         */
    case BTN_0:
        button = (TestBit(BTN_LEFT, pEvdevMultitouch->key_bitmask)) ?  8 : 1;
        break;
    case BTN_1:
        button = (TestBit(BTN_MIDDLE, pEvdevMultitouch->key_bitmask)) ?  9 : 2;
        break;
    case BTN_2:
        button = (TestBit(BTN_RIGHT, pEvdevMultitouch->key_bitmask)) ?  10 : 3;
        break;

        /* FIXME: BTN_3.. and BTN_SIDE.. have the same button mapping */
    case BTN_3:
    case BTN_4:
    case BTN_5:
    case BTN_6:
    case BTN_7:
    case BTN_8:
    case BTN_9:
	button = (code - BTN_0 + 5);
        break;

    case BTN_SIDE:
    case BTN_EXTRA:
    case BTN_FORWARD:
    case BTN_BACK:
    case BTN_TASK:
	button = (code - BTN_LEFT + 5);
	break;

    default:
	if ((code > BTN_TASK) && (code < KEY_OK)) {
	    if (code < BTN_JOYSTICK) {
                if (code < BTN_MOUSE)
                    button = (code - BTN_0 + 5);
                else
                    button = (code - BTN_LEFT + 5);
            }
	}
    }

    if (button > EVDEVMULTITOUCH_MAXBUTTONS)
	return 0;

    return button;
}

#ifdef HAVE_PROPERTIES
#ifdef HAVE_LABELS
/* Aligned with linux/input.h.
   Note that there are holes in the ABS_ range, these are simply replaced with
   MISC here */
static char* abs_labels[] = {
    AXIS_LABEL_PROP_ABS_X,              /* 0x00 */
    AXIS_LABEL_PROP_ABS_Y,              /* 0x01 */
    AXIS_LABEL_PROP_ABS_Z,              /* 0x02 */
    AXIS_LABEL_PROP_ABS_RX,             /* 0x03 */
    AXIS_LABEL_PROP_ABS_RY,             /* 0x04 */
    AXIS_LABEL_PROP_ABS_RZ,             /* 0x05 */
    AXIS_LABEL_PROP_ABS_THROTTLE,       /* 0x06 */
    AXIS_LABEL_PROP_ABS_RUDDER,         /* 0x07 */
    AXIS_LABEL_PROP_ABS_WHEEL,          /* 0x08 */
    AXIS_LABEL_PROP_ABS_GAS,            /* 0x09 */
    AXIS_LABEL_PROP_ABS_BRAKE,          /* 0x0a */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_HAT0X,          /* 0x10 */
    AXIS_LABEL_PROP_ABS_HAT0Y,          /* 0x11 */
    AXIS_LABEL_PROP_ABS_HAT1X,          /* 0x12 */
    AXIS_LABEL_PROP_ABS_HAT1Y,          /* 0x13 */
    AXIS_LABEL_PROP_ABS_HAT2X,          /* 0x14 */
    AXIS_LABEL_PROP_ABS_HAT2Y,          /* 0x15 */
    AXIS_LABEL_PROP_ABS_HAT3X,          /* 0x16 */
    AXIS_LABEL_PROP_ABS_HAT3Y,          /* 0x17 */
    AXIS_LABEL_PROP_ABS_PRESSURE,       /* 0x18 */
    AXIS_LABEL_PROP_ABS_DISTANCE,       /* 0x19 */
    AXIS_LABEL_PROP_ABS_TILT_X,         /* 0x1a */
    AXIS_LABEL_PROP_ABS_TILT_Y,         /* 0x1b */
    AXIS_LABEL_PROP_ABS_TOOL_WIDTH,     /* 0x1c */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_MISC,           /* undefined */
    AXIS_LABEL_PROP_ABS_VOLUME          /* 0x20 */
};

static char* rel_labels[] = {
    AXIS_LABEL_PROP_REL_X,
    AXIS_LABEL_PROP_REL_Y,
    AXIS_LABEL_PROP_REL_Z,
    AXIS_LABEL_PROP_REL_RX,
    AXIS_LABEL_PROP_REL_RY,
    AXIS_LABEL_PROP_REL_RZ,
    AXIS_LABEL_PROP_REL_HWHEEL,
    AXIS_LABEL_PROP_REL_DIAL,
    AXIS_LABEL_PROP_REL_WHEEL,
    AXIS_LABEL_PROP_REL_MISC
};

static char* btn_labels[][16] = {
    { /* BTN_MISC group                 offset 0x100*/
        BTN_LABEL_PROP_BTN_0,           /* 0x00 */
        BTN_LABEL_PROP_BTN_1,           /* 0x01 */
        BTN_LABEL_PROP_BTN_2,           /* 0x02 */
        BTN_LABEL_PROP_BTN_3,           /* 0x03 */
        BTN_LABEL_PROP_BTN_4,           /* 0x04 */
        BTN_LABEL_PROP_BTN_5,           /* 0x05 */
        BTN_LABEL_PROP_BTN_6,           /* 0x06 */
        BTN_LABEL_PROP_BTN_7,           /* 0x07 */
        BTN_LABEL_PROP_BTN_8,           /* 0x08 */
        BTN_LABEL_PROP_BTN_9            /* 0x09 */
    },
    { /* BTN_MOUSE group                offset 0x110 */
        BTN_LABEL_PROP_BTN_LEFT,        /* 0x00 */
        BTN_LABEL_PROP_BTN_RIGHT,       /* 0x01 */
        BTN_LABEL_PROP_BTN_MIDDLE,      /* 0x02 */
        BTN_LABEL_PROP_BTN_SIDE,        /* 0x03 */
        BTN_LABEL_PROP_BTN_EXTRA,       /* 0x04 */
        BTN_LABEL_PROP_BTN_FORWARD,     /* 0x05 */
        BTN_LABEL_PROP_BTN_BACK,        /* 0x06 */
        BTN_LABEL_PROP_BTN_TASK         /* 0x07 */
    },
    { /* BTN_JOYSTICK group             offset 0x120 */
        BTN_LABEL_PROP_BTN_TRIGGER,     /* 0x00 */
        BTN_LABEL_PROP_BTN_THUMB,       /* 0x01 */
        BTN_LABEL_PROP_BTN_THUMB2,      /* 0x02 */
        BTN_LABEL_PROP_BTN_TOP,         /* 0x03 */
        BTN_LABEL_PROP_BTN_TOP2,        /* 0x04 */
        BTN_LABEL_PROP_BTN_PINKIE,      /* 0x05 */
        BTN_LABEL_PROP_BTN_BASE,        /* 0x06 */
        BTN_LABEL_PROP_BTN_BASE2,       /* 0x07 */
        BTN_LABEL_PROP_BTN_BASE3,       /* 0x08 */
        BTN_LABEL_PROP_BTN_BASE4,       /* 0x09 */
        BTN_LABEL_PROP_BTN_BASE5,       /* 0x0a */
        BTN_LABEL_PROP_BTN_BASE6,       /* 0x0b */
        NULL,
        NULL,
        NULL,
        BTN_LABEL_PROP_BTN_DEAD         /* 0x0f */
    },
    { /* BTN_GAMEPAD group              offset 0x130 */
        BTN_LABEL_PROP_BTN_A,           /* 0x00 */
        BTN_LABEL_PROP_BTN_B,           /* 0x01 */
        BTN_LABEL_PROP_BTN_C,           /* 0x02 */
        BTN_LABEL_PROP_BTN_X,           /* 0x03 */
        BTN_LABEL_PROP_BTN_Y,           /* 0x04 */
        BTN_LABEL_PROP_BTN_Z,           /* 0x05 */
        BTN_LABEL_PROP_BTN_TL,          /* 0x06 */
        BTN_LABEL_PROP_BTN_TR,          /* 0x07 */
        BTN_LABEL_PROP_BTN_TL2,         /* 0x08 */
        BTN_LABEL_PROP_BTN_TR2,         /* 0x09 */
        BTN_LABEL_PROP_BTN_SELECT,      /* 0x0a */
        BTN_LABEL_PROP_BTN_START,       /* 0x0b */
        BTN_LABEL_PROP_BTN_MODE,        /* 0x0c */
        BTN_LABEL_PROP_BTN_THUMBL,      /* 0x0d */
        BTN_LABEL_PROP_BTN_THUMBR       /* 0x0e */
    },
    { /* BTN_DIGI group                         offset 0x140 */
        BTN_LABEL_PROP_BTN_TOOL_PEN,            /* 0x00 */
        BTN_LABEL_PROP_BTN_TOOL_RUBBER,         /* 0x01 */
        BTN_LABEL_PROP_BTN_TOOL_BRUSH,          /* 0x02 */
        BTN_LABEL_PROP_BTN_TOOL_PENCIL,         /* 0x03 */
        BTN_LABEL_PROP_BTN_TOOL_AIRBRUSH,       /* 0x04 */
        BTN_LABEL_PROP_BTN_TOOL_FINGER,         /* 0x05 */
        BTN_LABEL_PROP_BTN_TOOL_MOUSE,          /* 0x06 */
        BTN_LABEL_PROP_BTN_TOOL_LENS,           /* 0x07 */
        NULL,
        NULL,
        BTN_LABEL_PROP_BTN_TOUCH,               /* 0x0a */
        BTN_LABEL_PROP_BTN_STYLUS,              /* 0x0b */
        BTN_LABEL_PROP_BTN_STYLUS2,             /* 0x0c */
        BTN_LABEL_PROP_BTN_TOOL_DOUBLETAP,      /* 0x0d */
        BTN_LABEL_PROP_BTN_TOOL_TRIPLETAP       /* 0x0e */
    },
    { /* BTN_WHEEL group                        offset 0x150 */
        BTN_LABEL_PROP_BTN_GEAR_DOWN,           /* 0x00 */
        BTN_LABEL_PROP_BTN_GEAR_UP              /* 0x01 */
    }
};

#endif /* HAVE_LABELS */

static void EvdevMultitouchInitAxesLabels(EvdevMultitouchPtr pEvdevMultitouch, int natoms, Atom *atoms)
{
#ifdef HAVE_LABELS
    Atom atom;
    int axis;
    char **labels;
    int labels_len = 0;
    char *misc_label;

    if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_ABSOLUTE_EVENTS)
    {
        labels     = abs_labels;
        labels_len = ArrayLength(abs_labels);
        misc_label = AXIS_LABEL_PROP_ABS_MISC;
    } else if ((pEvdevMultitouch->flags & EVDEVMULTITOUCH_RELATIVE_EVENTS))
    {
        labels     = rel_labels;
        labels_len = ArrayLength(rel_labels);
        misc_label = AXIS_LABEL_PROP_REL_MISC;
    }

    memset(atoms, 0, natoms * sizeof(Atom));

    /* Now fill the ones we know */
    for (axis = 0; axis < labels_len; axis++)
    {
        if (pEvdevMultitouch->axis_map[axis] == -1)
            continue;

        atom = XIGetKnownProperty(labels[axis]);
        if (!atom) /* Should not happen */
            continue;

        atoms[pEvdevMultitouch->axis_map[axis]] = atom;
    }
#endif
}

static void EvdevMultitouchInitButtonLabels(EvdevMultitouchPtr pEvdevMultitouch, int natoms, Atom *atoms)
{
#ifdef HAVE_LABELS
    Atom atom;
    int button, bmap;

    /* First, make sure all atoms are initialized */
    atom = XIGetKnownProperty(BTN_LABEL_PROP_BTN_UNKNOWN);
    for (button = 0; button < natoms; button++)
        atoms[button] = atom;

    for (button = BTN_MISC; button < BTN_JOYSTICK; button++)
    {
        if (TestBit(button, pEvdevMultitouch->key_bitmask))
        {
            int group = (button % 0x100)/16;
            int idx = button - ((button/16) * 16);

            if (!btn_labels[group][idx])
                continue;

            atom = XIGetKnownProperty(btn_labels[group][idx]);
            if (!atom)
                continue;

            /* Props are 0-indexed, button numbers start with 1 */
            bmap = EvdevMultitouchUtilButtonEventToButtonNumber(pEvdevMultitouch, button) - 1;
	     if( bmap >= 0 )
                atoms[bmap] = atom;
        }
    }

    /* wheel buttons, hardcoded anyway */
    if (natoms > 3)
        atoms[3] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_UP);
    if (natoms > 4)
        atoms[4] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_WHEEL_DOWN);
    if (natoms > 5)
        atoms[5] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_LEFT);
    if (natoms > 6)
        atoms[6] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_HWHEEL_RIGHT);
#endif
}

static void
EvdevMultitouchInitProperty(DeviceIntPtr dev)
{
    InputInfoPtr pInfo  = dev->public.devicePrivate;
    EvdevMultitouchPtr     pEvdevMultitouch = pInfo->private;
    int          rc;
    int          val;
    BOOL         invert[2];
    char         reopen;

    prop_reopen = MakeAtom(EVDEVMULTITOUCH_PROP_REOPEN, strlen(EVDEVMULTITOUCH_PROP_REOPEN),
            TRUE);

    reopen = pEvdevMultitouch->reopen_attempts;
    rc = XIChangeDeviceProperty(dev, prop_reopen, XA_INTEGER, 8,
                                PropModeReplace, 1, &reopen, FALSE);
    if (rc != Success)
        return;

    XISetDevicePropertyDeletable(dev, prop_reopen, FALSE);

    if (pEvdevMultitouch->flags & (EVDEVMULTITOUCH_RELATIVE_EVENTS | EVDEVMULTITOUCH_ABSOLUTE_EVENTS))
    {
        invert[0] = pEvdevMultitouch->invert_x;
        invert[1] = pEvdevMultitouch->invert_y;

        prop_invert = MakeAtom(EVDEVMULTITOUCH_PROP_INVERT_AXES, strlen(EVDEVMULTITOUCH_PROP_INVERT_AXES), TRUE);

        rc = XIChangeDeviceProperty(dev, prop_invert, XA_INTEGER, 8,
                PropModeReplace, 2,
                invert, FALSE);
        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_invert, FALSE);

        prop_calibration = MakeAtom(EVDEVMULTITOUCH_PROP_CALIBRATION,
                strlen(EVDEVMULTITOUCH_PROP_CALIBRATION), TRUE);
        rc = XIChangeDeviceProperty(dev, prop_calibration, XA_INTEGER, 32,
                PropModeReplace, 0, NULL, FALSE);
        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_calibration, FALSE);

        prop_swap = MakeAtom(EVDEVMULTITOUCH_PROP_SWAP_AXES,
                strlen(EVDEVMULTITOUCH_PROP_SWAP_AXES), TRUE);

        rc = XIChangeDeviceProperty(dev, prop_swap, XA_INTEGER, 8,
                PropModeReplace, 1, &pEvdevMultitouch->swap_axes, FALSE);
        if (rc != Success)
            return;

        XISetDevicePropertyDeletable(dev, prop_swap, FALSE);
        
        if (pEvdevMultitouch->flags & EVDEVMULTITOUCH_MULTITOUCH)
        {
            /* tracking ids for mt */
            prop_tracking_id = MakeAtom(EVDEVMULTITOUCH_PROP_TRACKING_ID,
                    strlen(EVDEVMULTITOUCH_PROP_TRACKING_ID), TRUE);
            rc = XIChangeDeviceProperty(dev, prop_tracking_id, XA_INTEGER, 32,
                    PropModeReplace, 1, &pEvdevMultitouch->id, FALSE);
            if (rc != Success)
                return;

            XISetDevicePropertyDeletable(dev, prop_tracking_id, FALSE);

            /* flag to emulate or not a touchscreen for mt */
            prop_multitouch = MakeAtom(EVDEVMULTITOUCH_PROP_MULTITOUCH_SUBDEVICES,
                    strlen(EVDEVMULTITOUCH_PROP_MULTITOUCH_SUBDEVICES), TRUE);
            rc = XIChangeDeviceProperty(dev, prop_multitouch, XA_INTEGER, 8,
                    PropModeReplace, 1, &pEvdevMultitouch->num_multitouch, FALSE);
            if (rc != Success)
                return;

            XISetDevicePropertyDeletable(dev, prop_multitouch, FALSE);
        }

	//property for checking pointer grab status
	val = 0;
	prop_grabinfo = MakeAtom(EVDEVMULTITOUCH_PROP_GRABINFO, strlen(EVDEVMULTITOUCH_PROP_GRABINFO),  TRUE);
	rc = XIChangeDeviceProperty(dev, prop_grabinfo, XA_INTEGER, 8, PropModeReplace, 1, &val, FALSE);

	if (rc != Success)
	    return;

	XISetDevicePropertyDeletable(dev, prop_grabinfo, FALSE);

	if( EvdevMultitouchIsCoreDevice(pInfo) )//master only
	{
		/* matrix to transform */
		prop_transform = MakeAtom(EVDEVMULTITOUCH_PROP_TRANSFORM, strlen(EVDEVMULTITOUCH_PROP_TRANSFORM),  TRUE);
		rc = XIChangeDeviceProperty(dev, prop_transform, XIGetKnownProperty(XATOM_FLOAT), 32, PropModeReplace, 9, pEvdevMultitouch->transform, FALSE);

	        if (rc != Success)
	            return;

	        XISetDevicePropertyDeletable(dev, prop_transform, FALSE);
	}

#ifdef HAVE_LABELS
        /* Axis labelling */
        if ((pEvdevMultitouch->num_vals > 0) && (prop_axis_label = XIGetKnownProperty(AXIS_LABEL_PROP)))
        {
            Atom atoms[pEvdevMultitouch->num_vals];
            EvdevMultitouchInitAxesLabels(pEvdevMultitouch, pEvdevMultitouch->num_vals, atoms);
            XIChangeDeviceProperty(dev, prop_axis_label, XA_ATOM, 32,
                                   PropModeReplace, pEvdevMultitouch->num_vals, atoms, FALSE);
            XISetDevicePropertyDeletable(dev, prop_axis_label, FALSE);
        }
        /* Button labelling */
        if ((pEvdevMultitouch->num_buttons > 0) && (prop_btn_label = XIGetKnownProperty(BTN_LABEL_PROP)))
        {
            Atom atoms[EVDEVMULTITOUCH_MAXBUTTONS];
            EvdevMultitouchInitButtonLabels(pEvdevMultitouch, EVDEVMULTITOUCH_MAXBUTTONS, atoms);
            XIChangeDeviceProperty(dev, prop_btn_label, XA_ATOM, 32,
                                   PropModeReplace, pEvdevMultitouch->num_buttons, atoms, FALSE);
            XISetDevicePropertyDeletable(dev, prop_btn_label, FALSE);
        }
#endif /* HAVE_LABELS */
    }

}

static void EvdevMultitouchSwapAxes(EvdevMultitouchPtr pEvdevMultitouch)
{
    if(pEvdevMultitouch->swap_axes)
    {
	pEvdevMultitouch->absinfo[ABS_Y].maximum = pEvdevMultitouch->resolution.max_x;
	pEvdevMultitouch->absinfo[ABS_Y].minimum = pEvdevMultitouch->resolution.min_x;
	pEvdevMultitouch->absinfo[ABS_X].maximum = pEvdevMultitouch->resolution.max_y;
	pEvdevMultitouch->absinfo[ABS_X].minimum = pEvdevMultitouch->resolution.min_y;
    }
    else
    {
	pEvdevMultitouch->absinfo[ABS_X].maximum = pEvdevMultitouch->resolution.max_x;
	pEvdevMultitouch->absinfo[ABS_X].minimum = pEvdevMultitouch->resolution.min_x;
	pEvdevMultitouch->absinfo[ABS_Y].maximum = pEvdevMultitouch->resolution.max_y;
	pEvdevMultitouch->absinfo[ABS_Y].minimum = pEvdevMultitouch->resolution.min_y;
    }
}

static int
EvdevMultitouchSetProperty(DeviceIntPtr dev, Atom atom, XIPropertyValuePtr val,
                 BOOL checkonly)
{
    InputInfoPtr pInfo  = dev->public.devicePrivate;
    EvdevMultitouchPtr     pEvdevMultitouch = pInfo->private;

    if (atom == prop_invert)
    {
        BOOL* data;
        if (val->format != 8 || val->size != 2 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
        {
            data = (BOOL*)val->data;
            pEvdevMultitouch->invert_x = data[0];
            pEvdevMultitouch->invert_y = data[1];
        }
    } else if (atom == prop_reopen)
    {
        if (val->format != 8 || val->size != 1 || val->type != XA_INTEGER)
            return BadMatch;

        if (!checkonly)
            pEvdevMultitouch->reopen_attempts = *((CARD8*)val->data);
    } else if (atom == prop_calibration)
    {
        if (val->format != 32 || val->type != XA_INTEGER)
            return BadMatch;
        if (val->size != 4 && val->size != 0)
            return BadMatch;

        if (!checkonly)
            EvdevMultitouchSetCalibration(pInfo, val->size, val->data);
    } else if (atom == prop_swap)
    {
        if (val->format != 8 || val->type != XA_INTEGER || val->size != 1)
            return BadMatch;

        if (!checkonly)
            pEvdevMultitouch->swap_axes = *((BOOL*)val->data);
	 EvdevMultitouchSwapAxes(pEvdevMultitouch);
    } else if (atom == prop_axis_label || atom == prop_btn_label)
        return BadAccess; /* Axis/Button labels can't be changed */
    else if (atom == prop_tracking_id)
    {
        if (val->format != 32 || val->type != XA_INTEGER || val->size != 1)
            return BadMatch;

        if (!checkonly)
            pEvdevMultitouch->id = *((int*)val->data);
    } else if (atom == prop_multitouch)
    {
        BOOL data;
        if (val->format != 8 || val->type != XA_INTEGER || val->size != 1)
            return BadMatch;
        if (!checkonly) {
            data = *((BOOL*)val->data);
            if (pEvdevMultitouch->num_multitouch != data)
                EvdevMultitouchSetMultitouch(pInfo,data);
        }
    } else if (atom == prop_transform)
    {
        float *f;
        if (val->format != 32 || val->type != XIGetKnownProperty(XATOM_FLOAT) || val->size != 9)
            return BadMatch;
        if (!checkonly) {
            f = (float*)val->data;
            EvdevMultitouchSetTransform(pInfo, val->size, f);
        }
    }
    else if (atom == prop_grabinfo)
    {
        BOOL data;
        if (val->format != 8 || val->type != XA_INTEGER || val->size != 1)
            return BadMatch;
        if (!checkonly) {
            data = *((BOOL*)val->data);
            EvdevMultitouchGetGrabInfo(pInfo, (BOOL)data);
        }
    }

    return Success;
}
#endif

/* Duplicate xf86 options and convert them to InputOption */
static InputOption *EvdevMultitouchOptionDupConvert(pointer original)
{
    InputOption *iopts = NULL, *new;
    InputInfoRec dummy;

    memset(&dummy, 0, sizeof(dummy));
    dummy.options = xf86OptionListDuplicate(original);

    while(dummy.options)
    {
        new = calloc(1, sizeof(struct _InputOption));

        new->opt_name = xf86OptionName(dummy.options);
        new->opt_val = xf86OptionValue(dummy.options);
        new->list.next = iopts;
        iopts = new;
        dummy.options = xf86NextOption(dummy.options);
    }

    return iopts;
}
static void EvdevMultitouchFreeInputOpts(InputOption* opts)
{
    InputOption *tmp = opts;

    while(opts)
    {
        tmp = opts->list.next;
        free(opts->opt_name);
        free(opts->opt_val);
        free(opts);
        opts = tmp;
    }
}
static void EvdevMultitouchReplaceOption(InputOption *opts,const char* key, char * value)
{

    while(opts)
    {
        if (xf86NameCmp(opts->opt_name, key) == 0)
        {

            free(opts->opt_val);
            opts->opt_val = strdup(value);
        }
        opts = opts->list.next;
    }
}

/**
 * New device creation through xorg/input
 *
 * @return 0 if successful, 1 if failure
 */
static InputInfoPtr
EvdevMultitouchCreateSubDevice(InputInfoPtr pInfo, int id) {
    InputInfoPtr pSubdev;

    DeviceIntPtr dev; /* dummy */
    InputOption *input_options = NULL;
    InputOption *iopts = NULL;
    char* name;

    pInfo->options = xf86AddNewOption(pInfo->options, "Type", "core");
    pInfo->options = xf86AddNewOption(pInfo->options, "SendCoreEvents", "off");

    /* Create new device */
    input_options = EvdevMultitouchOptionDupConvert(pInfo->options);
    EvdevMultitouchReplaceOption(input_options, "type","Object");
    EvdevMultitouchReplaceOption(input_options, "MultiTouch","1");

    name = malloc( (strlen(pInfo->name) + strlen(" subdev ") + 20 )*sizeof(char)); // 20 for adding the id

    if( !name )
    {
        EvdevMultitouchFreeInputOpts(input_options);
        xf86DrvMsg(-1, X_ERROR, "[X11][%s] Failed to allocate memory !\n", __FUNCTION__);
	 return NULL;
    }

    sprintf(name, "%s subdev %i", pInfo->name, id);
    EvdevMultitouchReplaceOption(input_options, "name",name);

    pCreatorInfo = pInfo;
    NewInputDeviceRequest(input_options, NULL, &dev);
    pSubdev = dev->public.devicePrivate;
    pCreatorInfo = NULL;

    EvdevMultitouchFreeInputOpts(input_options);

    free(name);
    return pSubdev;
}

static void
EvdevMultitouchDeleteSubDevice(InputInfoPtr pInfo, InputInfoPtr subdev)
{
    /* We need to explicitely flush the events so as not deleting
     * a device that still has events in queue
     */
    xf86Msg(X_INFO, "%s: Removing subdevice %s\n", pInfo->name,subdev->name);
    DeleteInputDeviceRequest(subdev->dev);
}
