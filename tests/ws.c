/*
    Copyright (c) 2012 250bpm s.r.o.  All rights reserved.
    Copyright (c) 2014-2015 Wirebird Labs LLC. All rights reserved.
    Copyright 2015 Garrett D'Amore <garrett@damore.org>

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "../src/nn.h"
#include "../src/pair.h"
#include "../src/ws.h"

#include "../src/utils/int.h"

#include "testutil.h"

#define SOCKET_ADDRESS  "ws://127.0.0.1:5555"

/*  Basic tests for WebSocket transport. */

/*  test_text() verifies that we drop messages properly when sending invalid
    UTF-8, but not when we send valid data. */
void test_text() {

    int sb;
    int sc;
    int opt;
    uint8_t bad[20];

    /*  Negative testing... bad UTF-8 data for text. */
    sb = test_socket (AF_SP, NN_PAIR);
    sc = test_socket (AF_SP, NN_PAIR);

    opt = NN_WS_MSG_TYPE_TEXT;
    test_setsockopt(sb, NN_WS, NN_WS_MSG_TYPE, &opt, sizeof (opt));
    opt = NN_WS_MSG_TYPE_TEXT;
    test_setsockopt(sc, NN_WS, NN_WS_MSG_TYPE, &opt, sizeof (opt));
    opt = 500;
    test_setsockopt(sb, NN_SOL_SOCKET, NN_RCVTIMEO, &opt, sizeof (opt));

    test_bind (sb, SOCKET_ADDRESS);
    test_connect (sc, SOCKET_ADDRESS);

    test_send (sc, "GOOD");
    test_recv (sb, "GOOD");

    /*  and the bad ... */
    strcpy((char *)bad, "BAD.");
    bad[2] = (char)0xDD;
    test_send (sc, (char *)bad);

    /*  Make sure we dropped the frame. */
    test_drop (sb, ETIMEDOUT);

    test_close (sb);
    test_close (sc);

    return;
}

int main ()
{
    int rc;
    int sb;
    int sc;
    int sb2;
    int opt;
    size_t sz;
    int i;

    /*  Try closing bound but unconnected socket. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, "ws://*:5555");
    test_close (sb);

    /*  Try closing a TCP socket while it not connected. At the same time
        test specifying the local address for the connection. */
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, "ws://127.0.0.1:5555");
    test_close (sc);

    /*  Open the socket anew. */
    sc = test_socket (AF_SP, NN_PAIR);

    /*  Check socket options. */
    sz = sizeof (opt);
    rc = nn_getsockopt (sc, NN_WS, NN_WS_MSG_TYPE, &opt, &sz);
    errno_assert (rc == 0);
    nn_assert (sz == sizeof (opt));
    nn_assert (opt == NN_WS_MSG_TYPE_BINARY);
    opt = NN_WS_MSG_TYPE_TEXT;
    sz = sizeof (opt);
    test_setsockopt (sc, NN_WS, NN_WS_MSG_TYPE, &opt, sz);
    
    /*  Default port 80 should be assumed if not explicitly declared. */
    rc = nn_connect (sc, "ws://127.0.0.1");
    errno_assert (rc >= 0);

    /*  Try using invalid address strings. */
    rc = nn_connect (sc, "ws://*:");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://*:1000000");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://*:some_port");
    nn_assert (rc < 0);
    rc = nn_connect (sc, "ws://eth10000;127.0.0.1:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == ENODEV);

    rc = nn_bind (sc, "ws://127.0.0.1:");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_bind (sc, "ws://127.0.0.1:1000000");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_bind (sc, "ws://eth10000:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == ENODEV);

    rc = nn_connect (sc, "ws://:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://-hostname:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://abc.123.---.#:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://[::1]:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://abc.123.:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://abc...123:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    rc = nn_connect (sc, "ws://.123:5555");
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);

    test_close (sc);

    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, SOCKET_ADDRESS);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, SOCKET_ADDRESS);

    /*  Ping-pong test. */
    for (i = 0; i != 100; ++i) {

        test_send (sc, "ABC");
        test_recv (sb, "ABC");

        test_send (sb, "DEF");
        test_recv (sc, "DEF");
    }

    /*  Batch transfer test. */
    for (i = 0; i != 100; ++i) {
        test_send (sc, "0123456789012345678901234567890123456789");
    }
    for (i = 0; i != 100; ++i) {
        test_recv (sb, "0123456789012345678901234567890123456789");
    }

    test_close (sc);
    test_close (sb);

    /*  Test that NN_RCVMAXSIZE can be -1, but not lower */
    sb = test_socket (AF_SP, NN_PAIR);
    opt = -1;
    rc = nn_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    nn_assert (rc >= 0);
    opt = -2;
    rc = nn_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    nn_assert (rc < 0);
    errno_assert (nn_errno () == EINVAL);
    test_close (sb);

    /*  Test NN_RCVMAXSIZE limit */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, SOCKET_ADDRESS);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, SOCKET_ADDRESS);
    opt = 100;
    test_setsockopt (sc, NN_SOL_SOCKET, NN_SNDTIMEO, &opt, sizeof (opt));
    opt = 100;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVTIMEO, &opt, sizeof (opt));
    nn_sleep (100);
    opt = 4;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    test_send (sc, "ABC");
    test_recv (sb, "ABC");
    test_send (sc, "ABCD");
    test_recv (sb, "ABCD");
    test_send (sc, "ABCDE");
    test_drop (sb, ETIMEDOUT);

    /*  Increase the size limit, then try sending again. Though, this first
        send after violating the protocol is expected to fail, since the peer
        failed the connection. In this scenario, the failed socket does not
        begin reconnection attempts. */
    opt = 5;
    test_setsockopt (sb, NN_SOL_SOCKET, NN_RCVMAXSIZE, &opt, sizeof (opt));
    rc = nn_send (sc, "ABCDE", 5, 0);
    nn_assert (rc < 0);
    errno_assert (nn_err_errno () == ETIMEDOUT);

    /*  Reconnect and expect success this time. */
    test_connect (sc, SOCKET_ADDRESS);
    test_send (sc, "ABCDE");
    test_recv (sb, "ABCDE");
    test_close (sb);
    test_close (sc);

    test_text ();

    /*  Test closing a socket that is waiting to bind. */
    sb = test_socket (AF_SP, NN_PAIR);
    test_bind (sb, SOCKET_ADDRESS);
    nn_sleep (100);
    sb2 = test_socket (AF_SP, NN_PAIR);
    test_bind (sb2, SOCKET_ADDRESS);
    sc = test_socket (AF_SP, NN_PAIR);
    test_connect (sc, SOCKET_ADDRESS);
    nn_sleep (100);
    test_send (sb, "ABC");
    test_recv (sc, "ABC");
    test_close (sb2);
    test_send (sb, "ABC");
    test_recv (sc, "ABC");
    test_close (sb);
    test_close (sc);

    return 0;
}
