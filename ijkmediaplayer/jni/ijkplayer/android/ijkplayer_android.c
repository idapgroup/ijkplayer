/*
 * ijkplayer_android.c
 *
 * Copyright (c) 2013 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ijkplayer_android.h"

#include <assert.h>
#include "../ff_fferror.h"
#include "../ff_ffplay.h"

#define MPST_CHECK_NOT_RET_INT(real, expected, errcode) \
    do { \
        if (real == expected) return errcode; \
    } while(0)

#define MPST_CHECK_NOT_RET(real, expected) \
    MPST_CHECK_NOT_RET_INT(real, expected, EIJK_INVALID_STATE)

typedef struct IjkMediaPlayer {
    volatile int ref_count;
    pthread_mutex_t mutex;
    pthread_t msg_thread;
    FFPlayer *ffplayer;

    int mp_state;
    char *data_source;
    void *weak_thiz;
} IjkMediaPlayer;

inline static void ijkmp_destroy(IjkMediaPlayer *mp)
{
    if (!mp)
        return;

    ffp_destroy_p(&mp->ffplayer);

    pthread_mutex_destroy(&mp->mutex);

    av_freep(&mp->data_source);
    memset(mp, 0, sizeof(IjkMediaPlayer));
    av_freep(&mp);
}

inline static void ijkmp_destroy_p(IjkMediaPlayer **pmp)
{
    if (!pmp)
        return;

    ijkmp_destroy(*pmp);
    *pmp = NULL;
}

void ijkmp_global_init()
{
    ffp_global_init();
}

void ijkmp_global_uninit()
{
    ffp_global_uninit();
}

IjkMediaPlayer *ijkmp_create(void (*msg_loop)(void*))
{
    FFPlayer *ffp;
    IjkMediaPlayer *mp = (IjkMediaPlayer *) av_mallocz(sizeof(IjkMediaPlayer));
    if (!mp)
        goto fail;

    mp->ffplayer = ffp_create();
    if (!mp)
        goto fail;

    mp->ffplayer->vout = SDL_VoutAndroid_CreateForAndroidSurface();
    if (!mp->ffplayer->vout)
        goto fail;

    mp->ffplayer->aout = SDL_AoutAndroid_CreateForAudioTrack();
    if (!mp->ffplayer->vout)
        goto fail;

    ijkmp_inc_ref(mp);
    pthread_mutex_init(&mp->mutex, NULL);

    ijkmp_inc_ref(mp);
    pthread_create(&mp->msg_thread, NULL, msg_loop, mp);

    return mp;

    fail:
    ijkmp_destroy_p(&mp);
    return NULL;
}

void ijkmp_shutdown_l(IjkMediaPlayer *mp)
{
    assert(mp);

    if (mp->ffplayer) {
        ffp_stop_l(mp->ffplayer);
        ffp_wait_stop_l(mp->ffplayer);
    }
}

void ijkmp_shutdown(IjkMediaPlayer *mp)
{
    return ijkmp_shutdown_l(mp);
}

void ijkmp_reset_l(IjkMediaPlayer *mp)
{
    assert(mp);

    ijkmp_shutdown_l(mp);
    ffp_reset(mp->ffplayer);

    av_freep(&mp->data_source);
    mp->mp_state = MP_STATE_IDLE;
}

void ijkmp_reset(IjkMediaPlayer *mp)
{
    assert(mp);

    pthread_mutex_lock(&mp->mutex);
    ijkmp_reset_l(mp);
    pthread_mutex_unlock(&mp->mutex);
}

void ijkmp_inc_ref(IjkMediaPlayer *mp)
{
    assert(mp);
    __sync_fetch_and_add(&mp->ref_count, 1);
}

void ijkmp_dec_ref(IjkMediaPlayer *mp)
{
    if (!mp)
        return;

    int ref_count = __sync_fetch_and_sub(&mp->ref_count, 1);
    if (ref_count == 0) {
        ijkmp_shutdown(mp);
        destroy_mp(&mp);
    }
}

void ijkmp_dec_ref_p(IjkMediaPlayer **pmp)
{
    if (!pmp)
        return;

    ijkmp_dec_ref(*pmp);
    *pmp = NULL;
}

static int ijkmp_set_data_source_l(IjkMediaPlayer *mp, const char *url)
{
    assert(mp);
    assert(url);

    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_IDLE);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_INITIALIZED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PREPARED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STARTED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PAUSED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_COMPLETED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STOPPED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ERROR);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_END);

    char *dup_url = strdup(url);
    if (!dup_url)
        return EIJK_OUT_OF_MEMORY;

    av_freep(mp->data_source);
    mp->data_source = av_strdup(url);
    mp->mp_state = MP_STATE_INITIALIZED;
    return 0;
}

int ijkmp_set_data_source(IjkMediaPlayer *mp, const char *url)
{
    assert(mp);
    assert(url);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_set_data_source_l(mp, url);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

static int ijkmp_prepare_async_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_IDLE);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_INITIALIZED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PREPARED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STARTED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PAUSED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_COMPLETED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STOPPED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ERROR);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_END);

    assert(mp->data_source);

    mp->mp_state = MP_STATE_ASYNC_PREPARING;
    msg_queue_start(&mp->ffplayer->msg_queue);
    int retval = ffp_prepare_async_l(mp->ffplayer, mp->data_source);
    if (retval < 0) {
        mp->mp_state = MP_STATE_ERROR;
        return retval;
    }

    return 0;
}

int ijkmp_prepare_async(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_prepare_async_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

static int ijkmp_start_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_IDLE);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_INITIALIZED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PREPARED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STARTED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PAUSED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_COMPLETED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STOPPED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ERROR);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_END);

    int retval = ffp_start_l(mp->ffplayer);
    if (retval < 0) {
        return retval;
    }

    if (mp->mp_state == MP_STATE_COMPLETED)
    {
        // FIXME: 0 handle start after completed
    }

    mp->mp_state = MP_STATE_STARTED;
    return 0;
}

int ijkmp_start(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_start_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

static int ijkmp_pause_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_IDLE);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_INITIALIZED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PREPARED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STARTED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PAUSED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_COMPLETED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STOPPED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ERROR);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_END);

    int retval = ffp_pause_l(mp->ffplayer);
    if (retval < 0) {
        return retval;
    }

    if (mp->mp_state == MP_STATE_STARTED)
        mp->mp_state = MP_STATE_PAUSED;

    return 0;
}

int ijkmp_pause(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_pause_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

static int ijkmp_stop_l(IjkMediaPlayer *mp)
{
    assert(mp);

    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_IDLE);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_INITIALIZED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PREPARED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STARTED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PAUSED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_COMPLETED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STOPPED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ERROR);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_END);

    int retval = ffp_stop_l(mp->ffplayer);
    if (retval < 0) {
        return retval;
    }

    // FIXME: 9 change to MP_STATE_STOPPED in read_thread() */
    mp->mp_state = MP_STATE_STOPPED;
    return 0;
}

int ijkmp_stop(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_stop_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

bool ijkmp_is_playing(IjkMediaPlayer *mp)
{
    assert(mp);
    if (mp->mp_state == MP_STATE_PREPARED ||
        mp->mp_state == MP_STATE_STARTED) {
        return true;
    }

    return false;
}

int ijkmp_seek_to_l(IjkMediaPlayer *mp, long msec)
{
    assert(mp);

    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_IDLE);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_INITIALIZED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ASYNC_PREPARING);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PREPARED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STARTED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_PAUSED);
    // MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_COMPLETED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_STOPPED);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_ERROR);
    MPST_CHECK_NOT_RET(mp->mp_state, MP_STATE_END);

    int retval = ffp_seek_to_l(mp->ffplayer, msec);
    if (retval < 0) {
        return retval;
    }

    return 0;
}

int ijkmp_seek_to(IjkMediaPlayer *mp, long msec)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_seek_to(mp, msec);
    pthread_mutex_unlock(&mp->mutex);

    return retval;
}

static long ijkmp_get_current_position_l(IjkMediaPlayer *mp)
{
    return ffp_get_current_position_l(mp->ffplayer);
}

long ijkmp_get_current_position(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    long retval = ijkmp_get_current_position_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

static long ijkmp_get_duration_l(IjkMediaPlayer *mp)
{
    return ffp_get_duration_l(mp->ffplayer);
}

long ijkmp_get_duration(IjkMediaPlayer *mp)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    int retval = ijkmp_get_duration_l(mp);
    pthread_mutex_unlock(&mp->mutex);
    return retval;
}

void ijkmp_set_android_surface_l(IjkMediaPlayer *mp, jobject android_surface)
{
    assert(mp);
    assert(mp->ffplayer);
    assert(mp->ffplayer->vout);

    SDL_VoutAndroid_SetAndroidSurface(mp->ffplayer->vout, android_surface);
}

void ijkmp_set_android_surface(IjkMediaPlayer *mp, jobject android_surface)
{
    assert(mp);
    pthread_mutex_lock(&mp->mutex);
    ijkmp_set_android_surface_l(mp, android_surface);
    pthread_mutex_unlock(&mp->mutex);
}

void *ijkmp_set_weak_thiz(JNIEnv *env, IjkMediaPlayer *mp, void *weak_thiz)
{
    void *prev_weak_thiz = mp->weak_thiz;

    mp->weak_thiz = weak_thiz;

    return prev_weak_thiz;
}

int ijkmp_get_msg(IjkMediaPlayer *mp, AVMessage *msg, int block)
{
    assert(mp);
    return msg_queue_get(&mp->ffplayer->msg_queue, msg, block);
}
