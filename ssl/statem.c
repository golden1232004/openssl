/* ssl/statem.c */
/*
 * Written by Matt Caswell for the OpenSSL project.
 */
/* ====================================================================
 * Copyright (c) 1998-2015 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.openssl.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@openssl.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.openssl.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <openssl/rand.h>
#include "ssl_locl.h"

/*
 * This file implements the SSL/TLS/DTLS state machines.
 *
 * There are two primary state machines:
 *
 * 1) Message flow state machine
 * 2) Handshake state machine
 *
 * The Message flow state machine controls the reading and sending of messages
 * including handling of non-blocking IO events, flushing of the underlying
 * write BIO, handling unexpected messages, etc. It is itself broken into two
 * separate sub-state machines which control reading and writing respectively.
 *
 * The Handshake state machine keeps track of the current SSL/TLS handshake
 * state. Transitions of the handshake state are the result of events that
 * occur within the Message flow state machine.
 *
 * Overall it looks like this:
 *
 * ---------------------------------------------            -------------------
 * |                                           |            |                 |
 * | Message flow state machine                |            |                 |
 * |                                           |            |                 |
 * | -------------------- -------------------- | Transition | Handshake state |
 * | | MSG_FLOW_READING    | | MSG_FLOW_WRITING    | | Event      | machine         |
 * | | sub-state        | | sub-state        | |----------->|                 |
 * | | machine for      | | machine for      | |            |                 |
 * | | reading messages | | writing messages | |            |                 |
 * | -------------------- -------------------- |            |                 |
 * |                                           |            |                 |
 * ---------------------------------------------            -------------------
 *
 */

/* Sub state machine return values */
enum SUB_STATE_RETURN {
    /* Something bad happened or NBIO */
    SUB_STATE_ERROR,
    /* Sub state finished go to the next sub state */
    SUB_STATE_FINISHED,
    /* Sub state finished and handshake was completed */
    SUB_STATE_END_HANDSHAKE
};

int state_machine(SSL *s, int server);
static void init_read_state_machine(SSL *s);
static enum SUB_STATE_RETURN read_state_machine(SSL *s);
static void init_write_state_machine(SSL *s);
static enum SUB_STATE_RETURN write_state_machine(SSL *s);

/*
 * Clear the state machine state and reset back to MSG_FLOW_UNINITED
 */
void statem_clear(SSL *s)
{
    s->statem.state = MSG_FLOW_UNINITED;
}

/*
 * Set the state machine up ready for a renegotiation handshake
 */
void statem_set_renegotiate(SSL *s)
{
    s->statem.state = MSG_FLOW_RENEGOTIATE;
}

/*
 * Put the state machine into an error state. This is a permanent error for
 * the current connection.
 */
void statem_set_error(SSL *s)
{
    s->statem.state = MSG_FLOW_ERROR;
    /* TODO: This is temporary - remove me */
    s->state = SSL_ST_ERR;
}

/*
 * The main message flow state machine. We start in the MSG_FLOW_UNINITED or
 * MSG_FLOW_RENEGOTIATE state and finish in MSG_FLOW_FINISHED. Valid states and
 * transitions are as follows:
 *
 * MSG_FLOW_UNINITED     MSG_FLOW_RENEGOTIATE
 *        |                       |
 *        +-----------------------+
 *        v
 * MSG_FLOW_WRITING <---> MSG_FLOW_READING
 *        |
 *        V
 * MSG_FLOW_FINISHED
 *        |
 *        V
 *    [SUCCESS]
 *
 * We may exit at any point due to an error or NBIO event. If an NBIO event
 * occurs then we restart at the point we left off when we are recalled.
 * MSG_FLOW_WRITING and MSG_FLOW_READING have sub-state machines associated with them.
 *
 * In addition to the above there is also the MSG_FLOW_ERROR state. We can move
 * into that state at any point in the event that an irrecoverable error occurs.
 *
 * Valid return values are:
 *   1: Success
 * <=0: NBIO or error
 */
int state_machine(SSL *s, int server) {
    BUF_MEM *buf = NULL;
    unsigned long Time = (unsigned long)time(NULL);
    void (*cb) (const SSL *ssl, int type, int val) = NULL;
    STATEM *st = &s->statem;
    int ret = -1;
    int ssret;

    if (st->state == MSG_FLOW_ERROR) {
        /* Shouldn't have been called if we're already in the error state */
        return -1;
    }

    RAND_add(&Time, sizeof(Time), 0);
    ERR_clear_error();
    clear_sys_error();

    if (s->info_callback != NULL)
        cb = s->info_callback;
    else if (s->ctx->info_callback != NULL)
        cb = s->ctx->info_callback;

    s->in_handshake++;
    if (!SSL_in_init(s) || SSL_in_before(s)) {
        if (!SSL_clear(s))
            return -1;
    }

#ifndef OPENSSL_NO_HEARTBEATS
    /*
     * If we're awaiting a HeartbeatResponse, pretend we already got and
     * don't await it anymore, because Heartbeats don't make sense during
     * handshakes anyway.
     */
    if (s->tlsext_hb_pending) {
        if (SSL_IS_DTLS(s))
            dtls1_stop_timer(s);
        s->tlsext_hb_pending = 0;
        s->tlsext_hb_seq++;
    }
#endif

    /* Initialise state machine */

    if (st->state == MSG_FLOW_RENEGOTIATE) {
        s->renegotiate = 1;
        if (!server)
            s->ctx->stats.sess_connect_renegotiate++;
    }

    if (st->state == MSG_FLOW_UNINITED || st->state == MSG_FLOW_RENEGOTIATE) {
        /* TODO: Temporary - fix this */
        if (server)
            s->state = SSL_ST_ACCEPT;
        else
            s->state = SSL_ST_CONNECT;

        if (st->state == MSG_FLOW_UNINITED) {
            st->hand_state = TLS_ST_BEFORE;
        }

        s->server = server;
        if (cb != NULL)
            cb(s, SSL_CB_HANDSHAKE_START, 1);

        if (SSL_IS_DTLS(s)) {
            if ((s->version & 0xff00) != (DTLS1_VERSION & 0xff00) &&
                    (server
                    || (s->version & 0xff00) != (DTLS1_BAD_VER & 0xff00))) {
                SSLerr(SSL_F_STATE_MACHINE, ERR_R_INTERNAL_ERROR);
                goto end;
            }
        } else {
            if ((s->version >> 8) != SSL3_VERSION_MAJOR
                    && s->version != TLS_ANY_VERSION) {
                SSLerr(SSL_F_STATE_MACHINE, ERR_R_INTERNAL_ERROR);
                goto end;
            }
        }

        if (s->version != TLS_ANY_VERSION &&
                !ssl_security(s, SSL_SECOP_VERSION, 0, s->version, NULL)) {
            SSLerr(SSL_F_STATE_MACHINE, SSL_R_VERSION_TOO_LOW);
            goto end;
        }

        if (server)
            s->type = SSL_ST_ACCEPT;
        else
            s->type = SSL_ST_CONNECT;

        if (s->init_buf == NULL) {
            if ((buf = BUF_MEM_new()) == NULL) {
                goto end;
            }
            if (!BUF_MEM_grow(buf, SSL3_RT_MAX_PLAIN_LENGTH)) {
                goto end;
            }
            s->init_buf = buf;
            buf = NULL;
        }

        if (!ssl3_setup_buffers(s)) {
            goto end;
        }
        s->init_num = 0;

        /*
         * Should have been reset by tls_process_finished, too.
         */
        s->s3->change_cipher_spec = 0;

        if (!server || st->state != MSG_FLOW_RENEGOTIATE) {
                /*
                 * Ok, we now need to push on a buffering BIO ...but not with
                 * SCTP
                 */
#ifndef OPENSSL_NO_SCTP
                if (!SSL_IS_DTLS(s) || !BIO_dgram_is_sctp(SSL_get_wbio(s)))
#endif
                    if (!ssl_init_wbio_buffer(s, server ? 1 : 0)) {
                        goto end;
                    }

            ssl3_init_finished_mac(s);
        }

        if (server) {
            if (st->state != MSG_FLOW_RENEGOTIATE) {
                s->ctx->stats.sess_accept++;
            } else if (!s->s3->send_connection_binding &&
                       !(s->options &
                         SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION)) {
                /*
                 * Server attempting to renegotiate with client that doesn't
                 * support secure renegotiation.
                 */
                SSLerr(SSL_F_STATE_MACHINE,
                       SSL_R_UNSAFE_LEGACY_RENEGOTIATION_DISABLED);
                ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_HANDSHAKE_FAILURE);
                statem_set_error(s);
                goto end;
            } else {
                /*
                 * s->state == SSL_ST_RENEGOTIATE, we will just send a
                 * HelloRequest
                 */
                s->ctx->stats.sess_accept_renegotiate++;
            }
        } else {
            s->ctx->stats.sess_connect++;

            /* mark client_random uninitialized */
            memset(s->s3->client_random, 0, sizeof(s->s3->client_random));
            s->hit = 0;

            s->s3->tmp.cert_request = 0;

            if (SSL_IS_DTLS(s)) {
                st->use_timer = 1;
            }
        }

        st->state = MSG_FLOW_WRITING;
        init_write_state_machine(s);
        st->read_state_first_init = 1;
    }

    while(st->state != MSG_FLOW_FINISHED) {
        if(st->state == MSG_FLOW_READING) {
            ssret = read_state_machine(s);
            if (ssret == SUB_STATE_FINISHED) {
                st->state = MSG_FLOW_WRITING;
                init_write_state_machine(s);
            } else {
                /* NBIO or error */
                goto end;
            }
        } else if (st->state == MSG_FLOW_WRITING) {
            ssret = write_state_machine(s);
            if (ssret == SUB_STATE_FINISHED) {
                st->state = MSG_FLOW_READING;
                init_read_state_machine(s);
            } else if (ssret == SUB_STATE_END_HANDSHAKE) {
                st->state = MSG_FLOW_FINISHED;
            } else {
                /* NBIO or error */
                goto end;
            }
        } else {
            /* Error */
            statem_set_error(s);
            goto end;
        }
    }

    st->state = MSG_FLOW_UNINITED;
    ret = 1;

 end:
    s->in_handshake--;
    BUF_MEM_free(buf);
    if (cb != NULL) {
        if (server)
            cb(s, SSL_CB_ACCEPT_EXIT, ret);
        else
            cb(s, SSL_CB_CONNECT_EXIT, ret);
    }
    return ret;
}

/*
 * Initialise the MSG_FLOW_READING sub-state machine
 */
static void init_read_state_machine(SSL *s)
{
    STATEM *st = &s->statem;

    st->read_state = READ_STATE_HEADER;
}

/*
 * This function implements the sub-state machine when the message flow is in
 * MSG_FLOW_READING. The valid sub-states and transitions are:
 *
 * READ_STATE_HEADER <--+<-------------+
 *        |             |              |
 *        v             |              |
 * READ_STATE_BODY -----+-->READ_STATE_POST_PROCESS
 *        |                            |
 *        +----------------------------+
 *        v
 * [SUB_STATE_FINISHED]
 *
 * READ_STATE_HEADER has the responsibility for reading in the message header
 * and transitioning the state of the handshake state machine.
 *
 * READ_STATE_BODY reads in the rest of the message and then subsequently
 * processes it.
 *
 * READ_STATE_POST_PROCESS is an optional step that may occur if some post
 * processing activity performed on the message may block.
 *
 * Any of the above states could result in an NBIO event occuring in which case
 * control returns to the calling application. When this function is recalled we
 * will resume in the same state where we left off.
 */
static enum SUB_STATE_RETURN read_state_machine(SSL *s) {
    STATEM *st = &s->statem;
    int ret, mt;
    unsigned long len;
    int (*transition)(SSL *s, int mt);
    enum MSG_PROCESS_RETURN (*process_message)(SSL *s, unsigned long n);
    enum WORK_STATE (*post_process_message)(SSL *s, enum WORK_STATE wst);
    unsigned long (*max_message_size)(SSL *s);
    void (*cb) (const SSL *ssl, int type, int val) = NULL;

    if (s->info_callback != NULL)
        cb = s->info_callback;
    else if (s->ctx->info_callback != NULL)
        cb = s->ctx->info_callback;

    if(s->server) {
        /* TODO: Fill these in later when we've implemented them */
        transition = NULL;
        process_message = NULL;
        post_process_message = NULL;
        max_message_size = NULL;
    } else {
        /* TODO: Fill these in later when we've implemented them */
        transition = NULL;
        process_message = NULL;
        post_process_message = NULL;
        max_message_size = NULL;
    }

    if (st->read_state_first_init) {
        s->first_packet = 1;
        st->read_state_first_init = 0;
    }

    while(1) {
        switch(st->read_state) {
        case READ_STATE_HEADER:
            s->init_num = 0;
            /* Get the state the peer wants to move to */
            ret = tls_get_message_header(s, &mt);

            if (ret == 0) {
                /* Could be non-blocking IO */
                return SUB_STATE_ERROR;
            }

            if (cb != NULL) {
                /* Notify callback of an impending state change */
                if (s->server)
                    cb(s, SSL_CB_ACCEPT_LOOP, 1);
                else
                    cb(s, SSL_CB_CONNECT_LOOP, 1);
            }
            /*
             * Validate that we are allowed to move to the new state and move
             * to that state if so
             */
            if(!transition(s, mt)) {
                ssl3_send_alert(s, SSL3_AL_FATAL, SSL3_AD_UNEXPECTED_MESSAGE);
                SSLerr(SSL_F_READ_STATE_MACHINE, SSL_R_UNEXPECTED_MESSAGE);
                return SUB_STATE_ERROR;
            }

            if (s->s3->tmp.message_size > max_message_size(s)) {
                ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_ILLEGAL_PARAMETER);
                SSLerr(SSL_F_READ_STATE_MACHINE, SSL_R_EXCESSIVE_MESSAGE_SIZE);
                return SUB_STATE_ERROR;
            }

            st->read_state = READ_STATE_BODY;
            /* Fall through */

        case READ_STATE_BODY:
            if (!SSL_IS_DTLS(s)) {
                /* We already got this above for DTLS */
                ret = tls_get_message_body(s, &len);
                if (ret == 0) {
                    /* Could be non-blocking IO */
                    return SUB_STATE_ERROR;
                }
            }

            s->first_packet = 0;
            ret = process_message(s, len);
            if (ret == MSG_PROCESS_ERROR) {
                return SUB_STATE_ERROR;
            }

            if (ret == MSG_PROCESS_FINISHED_READING) {
                if (SSL_IS_DTLS(s)) {
                    dtls1_stop_timer(s);
                }
                return SUB_STATE_FINISHED;
            }

            if (ret == MSG_PROCESS_CONTINUE_PROCESSING) {
                st->read_state = READ_STATE_POST_PROCESS;
                st->read_state_work = WORK_MORE_A;
            } else {
                st->read_state = READ_STATE_HEADER;
            }
            break;

        case READ_STATE_POST_PROCESS:
            st->read_state_work = post_process_message(s, st->read_state_work);
            switch(st->read_state_work) {
            default:
                return SUB_STATE_ERROR;

            case WORK_FINISHED_CONTINUE:
                st->read_state = READ_STATE_HEADER;
                break;

            case WORK_FINISHED_STOP:
                if (SSL_IS_DTLS(s)) {
                    dtls1_stop_timer(s);
                }
                return SUB_STATE_FINISHED;
            }
            break;

        default:
            /* Shouldn't happen */
            ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
            SSLerr(SSL_F_READ_STATE_MACHINE, ERR_R_INTERNAL_ERROR);
            statem_set_error(s);
            return SUB_STATE_ERROR;
        }
    }
}

/*
 * Send a previously constructed message to the peer.
 */
static int statem_do_write(SSL *s)
{
    STATEM *st = &s->statem;

    if (st->hand_state == TLS_ST_CW_CHANGE
            || st->hand_state == TLS_ST_SW_CHANGE) {
        if (SSL_IS_DTLS(s))
            return dtls1_do_write(s, SSL3_RT_CHANGE_CIPHER_SPEC);
        else
            return ssl3_do_write(s, SSL3_RT_CHANGE_CIPHER_SPEC);
    } else {
        return ssl_do_write(s);
    }
}

/*
 * Initialise the MSG_FLOW_WRITING sub-state machine
 */
static void init_write_state_machine(SSL *s)
{
    STATEM *st = &s->statem;

    st->write_state = WRITE_STATE_TRANSITION;
}

/*
 * This function implements the sub-state machine when the message flow is in
 * MSG_FLOW_WRITING. The valid sub-states and transitions are:
 *
 * +-> WRITE_STATE_TRANSITION ------> [SUB_STATE_FINISHED]
 * |             |
 * |             v
 * |      WRITE_STATE_PRE_WORK -----> [SUB_STATE_END_HANDSHAKE]
 * |             |
 * |             v
 * |       WRITE_STATE_SEND
 * |             |
 * |             v
 * |     WRITE_STATE_POST_WORK
 * |             |
 * +-------------+
 *
 * WRITE_STATE_TRANSITION transitions the state of the handshake state machine

 * WRITE_STATE_PRE_WORK performs any work necessary to prepare the later
 * sending of the message. This could result in an NBIO event occuring in
 * which case control returns to the calling application. When this function
 * is recalled we will resume in the same state where we left off.
 *
 * WRITE_STATE_SEND sends the message and performs any work to be done after
 * sending.
 *
 * WRITE_STATE_POST_WORK performs any work necessary after the sending of the
 * message has been completed. As for WRITE_STATE_PRE_WORK this could also
 * result in an NBIO event.
 */
static enum SUB_STATE_RETURN write_state_machine(SSL *s)
{
    STATEM *st = &s->statem;
    int ret;
    enum WRITE_TRAN (*transition)(SSL *s);
    enum WORK_STATE (*pre_work)(SSL *s, enum WORK_STATE wst);
    enum WORK_STATE (*post_work)(SSL *s, enum WORK_STATE wst);
    int (*construct_message)(SSL *s);
    void (*cb) (const SSL *ssl, int type, int val) = NULL;

    if (s->info_callback != NULL)
        cb = s->info_callback;
    else if (s->ctx->info_callback != NULL)
        cb = s->ctx->info_callback;

    if(s->server) {
        /* TODO: Fill these in later when we've implemented them */
        transition = NULL;
        pre_work = NULL;
        post_work = NULL;
        construct_message = NULL;
    } else {
        /* TODO: Fill these in later when we've implemented them */
        transition = NULL;
        pre_work = NULL;
        post_work = NULL;
        construct_message = NULL;
    }

    while(1) {
        switch(st->write_state) {
        case WRITE_STATE_TRANSITION:
            if (cb != NULL) {
                /* Notify callback of an impending state change */
                if (s->server)
                    cb(s, SSL_CB_ACCEPT_LOOP, 1);
                else
                    cb(s, SSL_CB_CONNECT_LOOP, 1);
            }
            switch(transition(s)) {
            case WRITE_TRAN_CONTINUE:
                st->write_state = WRITE_STATE_PRE_WORK;
                st->write_state_work = WORK_MORE_A;
                break;

            case WRITE_TRAN_FINISHED:
                return SUB_STATE_FINISHED;
                break;

            default:
                return SUB_STATE_ERROR;
            }
            break;

        case WRITE_STATE_PRE_WORK:
            switch(st->write_state_work = pre_work(s, st->write_state_work)) {
            default:
                return SUB_STATE_ERROR;

            case WORK_FINISHED_CONTINUE:
                st->write_state = WRITE_STATE_SEND;
                break;

            case WORK_FINISHED_STOP:
                return SUB_STATE_END_HANDSHAKE;
            }
            if(construct_message(s) == 0)
                return SUB_STATE_ERROR;

            /* Fall through */

        case WRITE_STATE_SEND:
            if (SSL_IS_DTLS(s) && st->use_timer) {
                dtls1_start_timer(s);
            }
            ret = statem_do_write(s);
            if (ret <= 0) {
                return SUB_STATE_ERROR;
            }
            st->write_state = WRITE_STATE_POST_WORK;
            st->write_state_work = WORK_MORE_A;
            /* Fall through */

        case WRITE_STATE_POST_WORK:
            switch(st->write_state_work = post_work(s, st->write_state_work)) {
            default:
                return SUB_STATE_ERROR;

            case WORK_FINISHED_CONTINUE:
                st->write_state = WRITE_STATE_TRANSITION;
                break;

            case WORK_FINISHED_STOP:
                return SUB_STATE_END_HANDSHAKE;
            }
            break;

        default:
            return SUB_STATE_ERROR;
        }
    }
}

/*
 * Called by the record layer to determine whether application data is
 * allowed to be sent in the current handshake state or not.
 *
 * Return values are:
 *   1: Yes (application data allowed)
 *   0: No (application data not allowed)
 */
int statem_client_app_data_allowed(SSL *s)
{
    STATEM *st = &s->statem;

    if(st->hand_state != TLS_ST_BEFORE &&
            st->hand_state != TLS_ST_OK &&
            st->hand_state != TLS_ST_CW_CLNT_HELLO)
        return 0;

    return 1;
}
