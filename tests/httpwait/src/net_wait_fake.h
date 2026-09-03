/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Injectable link gate for the httpd suite (RFO-B11).
 *
 * This file provides the net.h contract in place of src/net/l4.c, so httpd.c
 * can be linked into a test image whose link never comes up on its own. l4.c
 * is the file that listens to NET_EVENT_L4_* and holds the semaphore; there is
 * no Wi-Fi radio and no USB host behind a simulated platform, so the semaphore
 * would never be given and every wait here would be a real timeout.
 *
 * Same reasoning as tests/controller/src/temp_fake.c, and the same limit: the
 * substitution happens at link time, in this directory's CMakeLists, so no
 * shipped image can contain it.
 */

#ifndef REFLOW_NET_WAIT_FAKE_H_
#define REFLOW_NET_WAIT_FAKE_H_

#include <stdint.h>

/*
 * How many reflow_net_wait_ready() calls report a timeout before the link is
 * declared usable.
 *
 * Set at BUILD time - see this directory's CMakeLists.txt - and deliberately
 * not at runtime, for the reason tests/coldstart records: the httpd thread
 * runs its wait from K_THREAD_DEFINE, before any test body or setup hook could
 * write a knob. A runtime knob would advertise a capability it does not have
 * (RFO-G17).
 */
#ifndef REFLOW_NET_WAIT_FAKE_FAILURES
#define REFLOW_NET_WAIT_FAKE_FAILURES 0
#endif

/* How many times reflow_net_wait_ready() has been called. */
uint32_t reflow_net_wait_fake_calls(void);

#endif /* REFLOW_NET_WAIT_FAKE_H_ */
