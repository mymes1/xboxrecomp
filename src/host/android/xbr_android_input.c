/*
 * xbr_android_input.c -- the on-screen controller's native side. [android-port]
 *
 * TouchControlsView (templates/xbr-game/app) draws the controls and calls
 * nativeTouchPad on every change, from Android's UI thread. The state goes
 * to the input layer as an overlay pad (xbox_InputOverlaySet): merged into
 * controller port 0 the way the keyboard is, so a real controller there
 * keeps working, and port 0 reads as connected while the controls show.
 *
 * Primitive arguments only, and no call back into the JNIEnv, so the
 * function needs nothing from <jni.h> but its types -- which keeps it
 * compilable by the compile-only check, whose sysroot has no jni.h.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<jni.h>)
#include <jni.h>
#define XBR_HAVE_JNI_H 1
#endif
#endif
#ifndef XBR_HAVE_JNI_H
/* The same ABI as <jni.h>: pointers and 32-bit ints in the platform's C
 * calling convention, which is JNI's. */
typedef int32_t jint;
typedef void *jclass;
typedef void JNIEnv;
#define JNIEXPORT __attribute__((visibility("default")))
#define JNICALL
#endif

#include "xinput_xbox.h"

static int clamp_axis(jint v)
{
    return v < -32767 ? -32767 : v > 32767 ? 32767 : (int)v;
}

JNIEXPORT void JNICALL
Java_org_xboxrecomp_app_TouchControlsView_nativeTouchPad(JNIEnv *env, jclass cls,
                                                         jint visible, jint buttons,
                                                         jint abxy, jint bwlr,
                                                         jint lx, jint ly, jint rx, jint ry)
{
    static int shown = -1;
    XBOX_GAMEPAD g;
    uint32_t lo = (uint32_t)abxy, hi = (uint32_t)bwlr;
    int i;

    (void)env;
    (void)cls;
    if (!visible) {
        xbox_InputOverlaySet(NULL);
        if (shown != 0)
            printf("[HOST] on-screen controller hidden\n");
        shown = 0;
        fflush(stdout);
        return;
    }
    memset(&g, 0, sizeof g);
    g.wButtons = (WORD)(buttons & 0xFF);
    for (i = 0; i < 4; i++) {
        g.bAnalogButtons[i] = (BYTE)(lo >> (8 * i));
        g.bAnalogButtons[4 + i] = (BYTE)(hi >> (8 * i));
    }
    g.sThumbLX = (SHORT)clamp_axis(lx);
    g.sThumbLY = (SHORT)clamp_axis(ly);
    g.sThumbRX = (SHORT)clamp_axis(rx);
    g.sThumbRY = (SHORT)clamp_axis(ry);
    xbox_InputOverlaySet(&g);
    if (shown != 1) {
        printf("[HOST] on-screen controller shown (merged into controller port 0)\n");
        fflush(stdout);
    }
    shown = 1;
}
