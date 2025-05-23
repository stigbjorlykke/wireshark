/* mcast_stream.c
 *
 * Copyright 2006, Iskratel , Slovenia
 * By Jakob Bratkovic <j.bratkovic@iskratel.si> and
 * Miha Jemec <m.jemec@iskratel.si>
 *
 * based on rtp_stream.c
 * Copyright 2003, Alcatel Business Systems
 * By Lars Ruoff <lars.ruoff@gmx.net>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "file.h"

#include <epan/epan.h>
#include <epan/address.h>
#include <epan/packet.h>
#include <epan/tap.h>
#include <epan/to_str.h>
#include <epan/dissectors/packet-udp.h>

#include "ui/mcast_stream.h"

int32_t mcast_stream_trigger         =     50; /* limit for triggering the burst alarm (in packets per second) */
int32_t mcast_stream_bufferalarm     =  10000; /* limit for triggering the buffer alarm (in bytes) */
uint16_t mcast_stream_burstint       =    100; /* burst interval in ms */
int32_t mcast_stream_emptyspeed      =   5000; /* outgoing speed for single stream (kbps)*/
int32_t mcast_stream_cumulemptyspeed = 100000; /* outgoing speed for all streams (kbps)*/

/* sliding window and buffer usage */
static int32_t buffsize = (int)((double)MAX_SPEED * 100 / 1000) * 2;
static uint16_t comparetimes(nstime_t *t1, nstime_t *t2, uint16_t burstint_lcl);
static void    buffusagecalc(mcast_stream_info_t *strinfo, uint32_t nbytes, double emptyspeed_lcl);
static void    slidingwindow(mcast_stream_info_t *strinfo, packet_info *pinfo, uint32_t nbytes);

/****************************************************************************/
/* GCompareFunc style comparison function for _mcast_stream_info */
static int
mcast_stream_info_cmp(const void *aa, const void *bb)
{
    const struct _mcast_stream_info* a = (const struct _mcast_stream_info *)aa;
    const struct _mcast_stream_info* b = (const struct _mcast_stream_info *)bb;

    if (a==b)
        return 0;
    if (a==NULL || b==NULL)
        return 1;
    if (addresses_equal(&(a->src_addr), &(b->src_addr))
        && (a->src_port == b->src_port)
        && addresses_equal(&(a->dest_addr), &(b->dest_addr))
        && (a->dest_port == b->dest_port))
        return 0;
    else
        return 1;

}


/****************************************************************************/
/* when there is a [re]reading of packet's */
void
mcaststream_reset(mcaststream_tapinfo_t *tapinfo)
{
    GList* list;
    mcast_stream_info_t *strinfo;

    /* free the data items first */
    list = g_list_first(tapinfo->strinfo_list);
    while (list)
    {
        strinfo = (mcast_stream_info_t*)list->data;
        g_free(strinfo->element.buff);
        free_address_wmem(NULL, &strinfo->src_addr);
        free_address_wmem(NULL, &strinfo->dest_addr);
        g_free(strinfo);
        list = g_list_next(list);
    }
    g_list_free(tapinfo->strinfo_list);
    tapinfo->strinfo_list = NULL;

    if (tapinfo->allstreams != NULL) {
        g_free(tapinfo->allstreams->element.buff);
        g_free(tapinfo->allstreams);
        tapinfo->allstreams = NULL;
    }

    tapinfo->npackets = 0;

    return;
}

static void
mcaststream_reset_cb(void *ti_ptr)
{
    mcaststream_tapinfo_t *tapinfo = (mcaststream_tapinfo_t *)ti_ptr;
    if (tapinfo) {
        if (tapinfo->tap_reset) {
           tapinfo->tap_reset(tapinfo);
        }
        mcaststream_reset(tapinfo);
    }
}

/****************************************************************************/
/* redraw the output */
static void
mcaststream_draw(void *ti_ptr)
{
    mcaststream_tapinfo_t *tapinfo = (mcaststream_tapinfo_t *)ti_ptr;
/* XXX: see mcaststream_on_update in mcast_streams_dlg.c for comments
    g_signal_emit_by_name(top_level, "signal_mcaststream_update");
*/
    if (tapinfo && tapinfo->tap_draw) {
        tapinfo->tap_draw(tapinfo);
    }
    return;
}



/****************************************************************************/
/* whenever a udp packet is seen by the tap listener */
tap_packet_status
mcaststream_packet(void *arg, packet_info *pinfo, epan_dissect_t *edt _U_, const void *arg2 _U_, tap_flags_t flags _U_)
{
    mcaststream_tapinfo_t *tapinfo = (mcaststream_tapinfo_t *)arg;
    const e_udphdr *udphdr = (const e_udphdr *)arg2;
    mcast_stream_info_t tmp_strinfo;
    mcast_stream_info_t *strinfo = NULL;
    GList* list;
    nstime_t delta;
    double deltatime;
    uint32_t nbytes;

    /*
     * Restrict statistics to standard multicast IPv4 and IPv6 addresses.
     * We might want to check for and allow ethernet addresses starting
     * with 01:00:05 and 33:33 as well.
     */
    switch (pinfo->net_dst.type) {
        case AT_IPv4:
            /* 224.0.0.0/4 */
            if (pinfo->net_dst.len == 0 || (((const uint8_t*)pinfo->net_dst.data)[0] & 0xf0) != 0xe0)
                return TAP_PACKET_DONT_REDRAW;
            break;
        case AT_IPv6:
            /* ff00::/8 */
            /* XXX This includes DHCPv6. */
            if (pinfo->net_dst.len == 0 || ((const uint8_t*)pinfo->net_dst.data)[0] != 0xff)
                return TAP_PACKET_DONT_REDRAW;
            break;
        default:
            return TAP_PACKET_DONT_REDRAW;
    }

    /* New payload bytes - UDP datagram length (including UDP header but not
     * IP header nor link-layer frame header). This is a uint32_t because
     * of UDP jumbograms (RFC 2675) but overflow probably causes unexpected
     * results if jumbograms over INT32_MAX are used. (They aren't.)
     */
    nbytes = udphdr->uh_ulen;

    /* shallow copy information to temporary key for lookup */
    copy_address_shallow(&(tmp_strinfo.src_addr), &(pinfo->net_src));
    tmp_strinfo.src_port = pinfo->srcport;
    copy_address_shallow(&(tmp_strinfo.dest_addr), &(pinfo->net_dst));
    tmp_strinfo.dest_port = pinfo->destport;

    /* check whether we already have a stream with these parameters in the list */
    list = g_list_first(tapinfo->strinfo_list);
    while (list)
    {
        if (mcast_stream_info_cmp(&tmp_strinfo, (mcast_stream_info_t*)(list->data))==0)
        {
            strinfo = (mcast_stream_info_t*)(list->data);  /*found!*/
            break;
        }
        list = g_list_next(list);
    }

    /* not in the list? then create a new entry */
    if (!strinfo) {
        /*printf("nov sip %s sp %d dip %s dp %d\n", address_to_display(NULL, &(pinfo->src)),
            pinfo->srcport, address_to_display(NULL, &(pinfo->dst)), pinfo->destport);*/

        strinfo = g_new0(mcast_stream_info_t, 1);

        strinfo->src_port = pinfo->srcport;
        strinfo->dest_port = pinfo->destport;
        copy_address_wmem(NULL, &(strinfo->src_addr), &(pinfo->net_src));
        copy_address_wmem(NULL, &(strinfo->dest_addr), &(pinfo->net_dst));
        strinfo->first_frame_num = pinfo->num;
        nstime_copy(&strinfo->start_abs, &pinfo->abs_ts);
        nstime_copy(&strinfo->start_rel, &pinfo->rel_ts);

        /* slidingwindow and buffer parameters */
        strinfo->element.buff = g_new(nstime_t, buffsize);
        strinfo->element.burstsize=1;
        strinfo->element.topburstsize=1;
        strinfo->element.count=1;
        strinfo->element.buffusage=nbytes;
        strinfo->element.topbuffusage=nbytes;

        tapinfo->strinfo_list = g_list_append(tapinfo->strinfo_list, strinfo);

        /* set time with the first packet */
        if (tapinfo->npackets == 0) {
            tapinfo->allstreams = g_new0(mcast_stream_info_t, 1);
            tapinfo->allstreams->element.buff =
                    g_new(nstime_t, buffsize);
            nstime_copy(&tapinfo->allstreams->start_rel, &pinfo->rel_ts);
            tapinfo->allstreams->element.burstsize=1;
            tapinfo->allstreams->element.topburstsize=1;
            tapinfo->allstreams->element.count=1;
            tapinfo->allstreams->element.buffusage=nbytes;
            tapinfo->allstreams->element.topbuffusage=nbytes;
        }
    }

    /* time between first and last packet in the group */
    strinfo->stop_rel = pinfo->rel_ts;
    nstime_delta(&delta, &strinfo->stop_rel, &strinfo->start_rel);
    deltatime = nstime_to_sec(&delta);

    /* calculate average bandwidth for this stream */
    strinfo->total_bytes = strinfo->total_bytes + nbytes;

    /* increment the packets counter for this stream and calculate average pps */
    ++(strinfo->npackets);

    if (deltatime > 0) {
        strinfo->apackets = strinfo->npackets / deltatime;
        strinfo->average_bw = ((double)(strinfo->total_bytes*8) / deltatime);
    } else {
        strinfo->apackets = strinfo->average_bw = 0.0;
    }

    /* time between first and last packet in any group */
    tapinfo->allstreams->stop_rel = pinfo->rel_ts;
    nstime_delta(&delta, &tapinfo->allstreams->stop_rel, &tapinfo->allstreams->start_rel);
    deltatime = nstime_to_sec(&delta);

    /* increment the packets counter of all streams */
    ++(tapinfo->npackets);

    /* calculate average bandwidth for all streams */
    tapinfo->allstreams->total_bytes = tapinfo->allstreams->total_bytes + nbytes;
    if (deltatime > 0)
        tapinfo->allstreams->average_bw = ((double)(tapinfo->allstreams->total_bytes*8) / deltatime);

    /* sliding window and buffercalc for this group*/
    slidingwindow(strinfo, pinfo, nbytes);
    buffusagecalc(strinfo, nbytes, mcast_stream_emptyspeed*1000);
    /* sliding window and buffercalc for all groups */
    slidingwindow(tapinfo->allstreams, pinfo, nbytes);
    buffusagecalc(tapinfo->allstreams, nbytes, mcast_stream_cumulemptyspeed*1000);
    /* end of sliding window */

    return TAP_PACKET_REDRAW;  /* refresh output */

}

/****************************************************************************/
/* TAP INTERFACE */
/****************************************************************************/

/****************************************************************************/
void
remove_tap_listener_mcast_stream(mcaststream_tapinfo_t *tapinfo)
{
    if (tapinfo && tapinfo->is_registered) {
        remove_tap_listener(tapinfo);
        tapinfo->is_registered = false;
    }
}


/****************************************************************************/
GString *
register_tap_listener_mcast_stream(mcaststream_tapinfo_t *tapinfo)
{
    GString *error_string;
    if (!tapinfo) {
        return NULL;
    }

    if (tapinfo->is_registered) {
        return NULL;
    }

    error_string = register_tap_listener("udp", tapinfo,
        NULL, 0, mcaststream_reset_cb, mcaststream_packet,
        mcaststream_draw, NULL);

    if (NULL == error_string) {
        tapinfo->is_registered = true;
    }
    return error_string;
}

/*******************************************************************************/
/* sliding window and buffer calculations */

/* compare two times */
static uint16_t
comparetimes(nstime_t *t1, nstime_t *t2, uint16_t burstint_lcl)
{
    if(((t2->secs - t1->secs)*1000 + (t2->nsecs - t1->nsecs)/1000000) > burstint_lcl){
        return 1;
    } else{
        return 0;
    }
}

/* calculate buffer usage */
static void
buffusagecalc(mcast_stream_info_t *strinfo, uint32_t nbytes, double emptyspeed_lcl)
{
    int32_t cur, prev;
    nstime_t *buffer;
    nstime_t delta;
    double timeelapsed;

    buffer = strinfo->element.buff;
    cur = strinfo->element.last;
    if(cur == 0){
        cur = buffsize - 1;
        prev = cur - 1;
    } else if(cur == 1){
        prev = buffsize - 1;
        cur = 0;
    } else{
        cur=cur-1;
        prev=cur-1;
    }

    nstime_delta(&delta, &buffer[cur], &buffer[prev]);
    timeelapsed = nstime_to_sec(&delta);

    /* bytes added to buffer */
    strinfo->element.buffusage+=nbytes;

    /* bytes cleared from buffer */
    strinfo->element.buffusage-= (uint32_t) (timeelapsed * emptyspeed_lcl / 8);

    if(strinfo->element.buffusage < 0) strinfo->element.buffusage=0;
    if(strinfo->element.buffusage > strinfo->element.topbuffusage)
        strinfo->element.topbuffusage = strinfo->element.buffusage;
    /* check for buffer losses */
    if((strinfo->element.buffusage >= mcast_stream_bufferalarm) && (strinfo->element.buffstatus == 0)){
        strinfo->element.buffstatus = 1;
        strinfo->element.numbuffalarms++;
    } else if(strinfo->element.buffusage < mcast_stream_bufferalarm){
        strinfo->element.buffstatus = 0;
    }

    return;
}

/* sliding window calculation */
static void
slidingwindow(mcast_stream_info_t *strinfo, packet_info *pinfo, uint32_t nbytes)
{
    nstime_t *buffer;
    int32_t diff;

    buffer = strinfo->element.buff;

    diff = strinfo->element.last - strinfo->element.first;
    if(diff < 0) diff+=buffsize;

    /* check if buffer is full */
    if(diff >= (buffsize - 2)){
        fprintf(stderr, "Warning: capture buffer full\n");
        strinfo->element.first++;
        if(strinfo->element.first >= buffsize) strinfo->element.first = strinfo->element.first % buffsize;
    }

    /* burst count */
    buffer[strinfo->element.last] = pinfo->rel_ts;
    while(comparetimes(&buffer[strinfo->element.first],
                       &buffer[strinfo->element.last], mcast_stream_burstint)){
        strinfo->element.first++;
        if(strinfo->element.first >= buffsize) strinfo->element.first = strinfo->element.first % buffsize;
        diff--;
    }
    strinfo->element.burstsize = diff;
    if(strinfo->element.burstsize > strinfo->element.topburstsize) {
        strinfo->element.topburstsize = strinfo->element.burstsize;
        strinfo->element.maxbw = (double)(strinfo->element.topburstsize) * 1000 / mcast_stream_burstint * nbytes * 8;
    }

    strinfo->element.last++;
    if(strinfo->element.last >= buffsize) strinfo->element.last = strinfo->element.last % buffsize;
    /* trigger check */
    if((strinfo->element.burstsize >= mcast_stream_trigger) && (strinfo->element.burststatus == 0)){
        strinfo->element.burststatus = 1;
        strinfo->element.numbursts++;
    } else if(strinfo->element.burstsize < mcast_stream_trigger){
        strinfo->element.burststatus = 0;
    }

    strinfo->element.count++;
}
