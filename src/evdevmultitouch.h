/*
 * xserver-xorg-input-evdev-multitouch
 *
 * Copyright (c) 2000 - 2011 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact: Sung-Jin Park <sj76.park@samsung.com>
 *          Sangjin LEE <lsj119@samsung.com>
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
 * Copyright © 2004-2008 Red Hat, Inc.
 * Copyright © 2008 University of South Australia
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
 *	Peter Hutterer (peter@cs.unisa.edu.au)
 *	Oliver McFadden (oliver.mcfadden@nokia.com)
 *	Benjamin Tissoires (tissoire@cena.fr)
 */

#ifndef EVDEVMULTITOUCH_H
#define EVDEVMULTITOUCH_H

#include <linux/input.h>
#include <linux/types.h>

#include <xf86Xinput.h>
#include <xf86_OSproc.h>
#include <xkbstr.h>

#include <math.h>
#include <pixman.h>

#ifdef _F_GESTURE_EXTENSION_
typedef enum _MTSyncType
{
	MTOUCH_FRAME_SYNC_END,
	MTOUCH_FRAME_SYNC_BEGIN
} MTSyncType;

enum EventType
{
    ET_MTSync = 0x7E,
    ET_Internal = 0xFF /* First byte */
};

typedef struct _AnyEvent AnyEvent;
struct _AnyEvent
{
    unsigned char header; /**< Always ET_Internal */
    enum EventType type;  /**< One of EventType */
    int length;           /**< Length in bytes */
    Time time;            /**< Time in ms */
    int deviceid;
    MTSyncType sync;
    int x;
    int y;
};

union _InternalEvent {
	struct {
	    unsigned char header; /**< Always ET_Internal */
	    enum EventType type;  /**< One of ET_* */
	    int length;           /**< Length in bytes */
	    Time time;            /**< Time in ms. */
	} any;
	AnyEvent any_event;
};
#endif//_F_GESTURE_EXTENSION_

#ifndef EV_CNT /* linux 2.4 kernels and earlier lack _CNT defines */
#define EV_CNT (EV_MAX+1)
#endif
#ifndef KEY_CNT
#define KEY_CNT (KEY_MAX+1)
#endif
#ifndef REL_CNT
#define REL_CNT (REL_MAX+1)
#endif
#ifndef ABS_CNT
#define ABS_CNT (ABS_MAX+1)
#endif
#ifndef LED_CNT
#define LED_CNT (LED_MAX+1)
#endif

#define EVDEVMULTITOUCH_MAXBUTTONS 32
#define EVDEVMULTITOUCH_MAXQUEUE 32

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) >= 3
#define HAVE_PROPERTIES 1
#endif

#ifndef MAX_VALUATORS
#define MAX_VALUATORS 36
#endif

#define MAX_VALUATORS_MT 10
#define DEFAULT_TIMEOUT 100

#define EVDEVMULTITOUCH_PROP_TRACKING_ID "EvdevMultitouch Tracking ID"
#define EVDEVMULTITOUCH_PROP_MULTITOUCH_SUBDEVICES "EvdevMultitouch MultiTouch"
#define EVDEVMULTITOUCH_PROP_TRANSFORM	"EvdevMultitouch Transform Matrix"
#define EVDEVMULTITOUCH_PROP_GRABINFO	"EvdevMultitouch Grab Info"

#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 5
typedef struct {
    char *rules;
    char *model;
    char *layout;
    char *variant;
    char *options;
} XkbRMLVOSet;
#endif


#define LONG_BITS (sizeof(long) * 8)

/* Number of longs needed to hold the given number of bits */
#define NLONGS(x) (((x) + LONG_BITS - 1) / LONG_BITS)

/* axis specific data for wheel emulation */
typedef struct {
    int up_button;
    int down_button;
    int traveled_distance;
} WheelAxis, *WheelAxisPtr;

/* Event queue used to defer keyboard/button events until EV_SYN time. */
typedef struct {
    enum {
        EV_QUEUE_KEY,	/* xf86PostKeyboardEvent() */
        EV_QUEUE_BTN,	/* xf86PostButtonEvent() */
    } type;
    int key;		/* May be either a key code or button number. */
    int val;		/* State of the key/button; pressed or released. */
} EventQueueRec, *EventQueuePtr;

typedef struct _EvdevMultitouchDataMTRec{
    int id;
    int slot;
    int abs;
   BOOL containsValues;
    Time expires;
    int vals[MAX_VALUATORS];
    InputInfoPtr pInfo;
} EvdevMultitouchDataMTRec, *EvdevMultitouchDataMTPtr;


/**
 * EvdevMultitouch device information, including list of current object
 */
typedef struct {
    const char *device;
    int grabDevice;         /* grab the event device? */

    int num_vals;           /* number of valuators */
    int axis_map[max(ABS_CNT, REL_CNT)]; /* Map evdevmultitouch <axis> to index */
    int vals[MAX_VALUATORS];
    int old_vals[MAX_VALUATORS]; /* Translate absolute inputs to relative */
    EvdevMultitouchDataMTRec vals_mt[MAX_VALUATORS_MT];

    int flags;
    int tool;
    int num_buttons;            /* number of buttons */
    BOOL swap_axes;
    BOOL invert_x;
    BOOL invert_y;
    int num_multitouch;
    OsTimerPtr multitouch_setting_timer;

    int delta[REL_CNT];
    unsigned int abs, rel, mt;

    /* XKB stuff has to be per-device rather than per-driver */
#if GET_ABI_MAJOR(ABI_XINPUT_VERSION) < 5
    XkbComponentNamesRec    xkbnames;
#endif
    XkbRMLVOSet rmlvo;

    /* Middle mouse button emulation */
    struct {
        BOOL                enabled;
        BOOL                pending;     /* timer waiting? */
        int                 buttonstate; /* phys. button state */
        int                 state;       /* state machine (see bt3emu.c) */
        Time                expires;     /* time of expiry */
        Time                timeout;
    } emulateMB;
    struct {
	int                 meta;           /* meta key to lock any button */
	BOOL                meta_state;     /* meta_button state */
	unsigned int        lock_pair[EVDEVMULTITOUCH_MAXBUTTONS];  /* specify a meta/lock pair */
	BOOL                lock_state[EVDEVMULTITOUCH_MAXBUTTONS]; /* state of any locked buttons */
    } dragLock;
    struct {
        BOOL                enabled;
        int                 button;
        int                 button_state;
        int                 inertia;
        WheelAxis           X;
        WheelAxis           Y;
        Time                expires;     /* time of expiry */
        Time                timeout;
    } emulateWheel;
    /* run-time calibration */
    struct {
        int                 min_x;
        int                 max_x;
        int                 min_y;
        int                 max_y;
    } calibration;

    struct {
        int                 min_x;
        int                 max_x;
        int                 min_y;
        int                 max_y;
    } resolution;

    unsigned char btnmap[32];           /* config-file specified button mapping */

    int reopen_attempts; /* max attempts to re-open after read failure */
    int reopen_left;     /* number of attempts left to re-open the device */
    OsTimerPtr reopen_timer;

    /* Cached info from device. */
    char name[1024];
    unsigned long bitmask[NLONGS(EV_CNT)];
    unsigned long key_bitmask[NLONGS(KEY_CNT)];
    unsigned long rel_bitmask[NLONGS(REL_CNT)];
    unsigned long abs_bitmask[NLONGS(ABS_CNT)];
    unsigned long led_bitmask[NLONGS(LED_CNT)];
    struct input_absinfo absinfo[ABS_CNT];

    /* minor/major number */
    dev_t min_maj;

    /* Event queue used to defer keyboard/button events until EV_SYN time. */
    int                     num_queue;
    EventQueueRec           queue[EVDEVMULTITOUCH_MAXQUEUE];

    Time timeout; /* Maximum difference between consecutive fseq values
                           that will allow a packet to be dropped */
    
    OsTimerPtr subdevice_timer;
    int current_id;
    int num_mt;
    int id;
    int last_slot;
    BOOL mt_slot_supported;
    BOOL sync_mt;
    BOOL associated;

    float transform[9];
    BOOL use_transform;
    struct pixman_transform inv_transform;

    int touch_state;
    Time evtime;
    InputInfoPtr core_device;
    
} EvdevMultitouchRec, *EvdevMultitouchPtr;

/* Event posting functions */
void EvdevMultitouchQueueKbdEvent(InputInfoPtr pInfo, struct input_event *ev, int value);
void EvdevMultitouchQueueButtonEvent(InputInfoPtr pInfo, int button, int value);
void EvdevMultitouchPostButtonEvent(InputInfoPtr pInfo, int button, int value);
void EvdevMultitouchQueueButtonClicks(InputInfoPtr pInfo, int button, int count);
void EvdevMultitouchPostRelativeMotionEvents(InputInfoPtr pInfo, int *num_v, int *first_v,
				   int v[MAX_VALUATORS]);
void EvdevMultitouchPostAbsoluteMotionEvents(InputInfoPtr pInfo, int *num_v, int *first_v,
				   int v[MAX_VALUATORS]);
unsigned int EvdevMultitouchUtilButtonEventToButtonNumber(EvdevMultitouchPtr pEvdevMultitouch, int code);

/* Middle Button emulation */
int  EvdevMultitouchMBEmuTimer(InputInfoPtr);
BOOL EvdevMultitouchMBEmuFilterEvent(InputInfoPtr, int, BOOL);
void EvdevMultitouchMBEmuWakeupHandler(pointer, int, pointer);
void EvdevMultitouchMBEmuBlockHandler(pointer, struct timeval**, pointer);
void EvdevMultitouchMBEmuPreInit(InputInfoPtr);
void EvdevMultitouchMBEmuOn(InputInfoPtr);
void EvdevMultitouchMBEmuFinalize(InputInfoPtr);
void EvdevMultitouchMBEmuEnable(InputInfoPtr, BOOL);

/* Mouse Wheel emulation */
void EvdevMultitouchWheelEmuPreInit(InputInfoPtr pInfo);
BOOL EvdevMultitouchWheelEmuFilterButton(InputInfoPtr pInfo, unsigned int button, int value);
BOOL EvdevMultitouchWheelEmuFilterMotion(InputInfoPtr pInfo, struct input_event *pEv);

/* Draglock code */
void EvdevMultitouchDragLockPreInit(InputInfoPtr pInfo);
BOOL EvdevMultitouchDragLockFilterEvent(InputInfoPtr pInfo, unsigned int button, int value);

#ifdef HAVE_PROPERTIES
void EvdevMultitouchMBEmuInitProperty(DeviceIntPtr);
void EvdevMultitouchWheelEmuInitProperty(DeviceIntPtr);
void EvdevMultitouchDragLockInitProperty(DeviceIntPtr);
#endif
#endif
