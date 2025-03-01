/** @file
 *
 * Copyright 2020, Odysseus Yang
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __W_ETW_NDISCAP_H__
#define __W_ETW_NDISCAP_H__

#include <glib.h>

#include <windows.h>
#include <SDKDDKVer.h>
#include <strsafe.h>
#include <evntcons.h>
#include <tdh.h>
#include <stdlib.h>

extern void etw_dump_write_ndiscap_event(PEVENT_RECORD ev, ULARGE_INTEGER timestamp);

#endif


/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
