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
 * Copyright © 2008 Red Hat, Inc.
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
 *	Peter Hutterer (peter.hutterer@redhat.com)
 */


#ifndef _EVDEVMULTITOUCH_PROPERTIES_H_
#define _EVDEVMULTITOUCH_PROPERTIES_H_

/* Middle mouse button emulation */
/* BOOL */
#define EVDEVMULTITOUCH_PROP_MIDBUTTON "EvdevMultitouch Middle Button Emulation"
/* CARD32 */
#define EVDEVMULTITOUCH_PROP_MIDBUTTON_TIMEOUT "EvdevMultitouch Middle Button Timeout"

/* Wheel emulation */
/* BOOL */
#define EVDEVMULTITOUCH_PROP_WHEEL "EvdevMultitouch Wheel Emulation"
/* CARD8, 4 values [x up, x down, y up, y down], 0 to disable a value*/
#define EVDEVMULTITOUCH_PROP_WHEEL_AXES "EvdevMultitouch Wheel Emulation Axes"
/* CARD16 */
#define EVDEVMULTITOUCH_PROP_WHEEL_INERTIA "EvdevMultitouch Wheel Emulation Inertia"
/* CARD16 */
#define EVDEVMULTITOUCH_PROP_WHEEL_TIMEOUT "EvdevMultitouch Wheel Emulation Timeout"
/* CARD8, value range 0-32, 0 to always scroll */
#define EVDEVMULTITOUCH_PROP_WHEEL_BUTTON "EvdevMultitouch Wheel Emulation Button"

/* Drag lock */
/* CARD8, either 1 value or pairs, value range 0-32, 0 to disable a value*/
#define EVDEVMULTITOUCH_PROP_DRAGLOCK "EvdevMultitouch Drag Lock Buttons"

/* Axis inversion */
/* BOOL, 2 values [x, y], 1 inverts axis */
#define EVDEVMULTITOUCH_PROP_INVERT_AXES "EvdevMultitouch Axis Inversion"

/* Reopen attempts. */
/* CARD8 */
#define EVDEVMULTITOUCH_PROP_REOPEN "EvdevMultitouch Reopen Attempts"

/* Run-time calibration */
/* CARD32, 4 values [minx, maxx, miny, maxy], or no values for unset */
#define EVDEVMULTITOUCH_PROP_CALIBRATION "EvdevMultitouch Axis Calibration"

/* Swap x and y axis. */
/* BOOL */
#define EVDEVMULTITOUCH_PROP_SWAP_AXES "EvdevMultitouch Axes Swap"

#endif
