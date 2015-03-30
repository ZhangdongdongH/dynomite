/*
 * Dynomite - A thin, distributed replication layer for multi non-distributed storages.
 * Copyright (C) 2014 Netflix, Inc.
 */ 

/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>

#include <sys/uio.h>

#include "dyn_core.h"
#include "dyn_server.h"
#include "proto/dyn_proto.h"

#if (IOV_MAX > 128)
#define DN_IOV_MAX 128
#else
#define DN_IOV_MAX IOV_MAX
#endif

/*
 *            dyn_message.[ch]
 *         message (struct msg)
 *            +        +            .
 *            |        |            .
 *            /        \            .
 *         Request    Response      .../ dyn_mbuf.[ch]  (mesage buffers)
 *      dyn_request.c  dyn_response.c .../ dyn_memcache.c; dyn_redis.c (message parser)
 *
 * Messages in dynomite are manipulated by a chain of processing handlers,
 * where each handler is responsible for taking the input and producing an
 * output for the next handler in the chain. This mechanism of processing
 * loosely conforms to the standard chain-of-responsibility design pattern
 *
 * At the high level, each handler takes in a message: request or response
 * and produces the message for the next handler in the chain. The input
 * for a handler is either a request or response, but never both and
 * similarly the output of an handler is either a request or response or
 * nothing.
 *
 * Each handler itself is composed of two processing units:
 *
 * 1). filter: manipulates output produced by the handler, usually based
 *     on a policy. If needed, multiple filters can be hooked into each
 *     location.
 * 2). forwarder: chooses one of the backend servers to send the request
 *     to, usually based on the configured distribution and key hasher.
 *
 * Handlers are registered either with Client or Server or Proxy
 * connections. A Proxy connection only has a read handler as it is only
 * responsible for accepting new connections from client. Read handler
 * (conn_recv_t) registered with client is responsible for reading requests,
 * while that registered with server is responsible for reading responses.
 * Write handler (conn_send_t) registered with client is responsible for
 * writing response, while that registered with server is responsible for
 * writing requests.
 *
 * Note that in the above discussion, the terminology send is used
 * synonymously with write or OUT event. Similarly recv is used synonymously
 * with read or IN event
 *
 *             Client+             Proxy           Server+
 *                              (dynomite)
 *                                   .
 *       msg_recv {read event}       .       msg_recv {read event}
 *         +                         .                         +
 *         |                         .                         |
 *         \                         .                         /
 *         req_recv_next             .             rsp_recv_next
 *           +                       .                       +
 *           |                       .                       |       Rsp
 *           req_recv_done           .           rsp_recv_done      <===
 *             +                     .                     +
 *             |                     .                     |
 *    Req      \                     .                     /
 *    ===>     req_filter*           .           *rsp_filter
 *               +                   .                   +
 *               |                   .                   | 
 *               \                   .                   /
 *               req_forward-//  (a) . (c)  \\-rsp_forward
 *                                   .
 *                                   .
 *       msg_send {write event}      .      msg_send {write event}
 *         +                         .                         +
 *         |                         .                         |
 *    Rsp' \                         .                         /     Req'
 *   <===  rsp_send_next             .             req_send_next     ===>
 *           +                       .                       +
 *           |                       .                       |
 *           \                       .                       /
 *           rsp_send_done-//    (d) . (b)    //-req_send_done
 *
 *
 * (a) -> (b) -> (c) -> (d) is the normal flow of transaction consisting
 * of a single request response, where (a) and (b) handle request from
 * client, while (c) and (d) handle the corresponding response from the
 * server.
 */

static uint64_t msg_id;          /* message id counter */
static uint64_t frag_id;         /* fragment id counter */
static uint32_t nfree_msgq;      /* # free msg q */
static struct msg_tqh free_msgq; /* free msg q */
static struct rbtree tmo_rbt;    /* timeout rbtree */
static struct rbnode tmo_rbs;    /* timeout rbtree sentinel */

static struct msg *
msg_from_rbe(struct rbnode *node)
{
    struct msg *msg;
    int offset;

    offset = offsetof(struct msg, tmo_rbe);
    msg = (struct msg *)((char *)node - offset);

    return msg;
}

struct msg *
msg_tmo_min(void)
{
    struct rbnode *node;

    node = rbtree_min(&tmo_rbt);
    if (node == NULL) {
        return NULL;
    }

    return msg_from_rbe(node);
}

void
msg_tmo_insert(struct msg *msg, struct conn *conn)
{
    struct rbnode *node;
    int timeout;

    ASSERT(msg->request);
    ASSERT(!msg->quit && !msg->noreply);

    //TODOs: need to fix this for different timeouts
    timeout = server_timeout(conn);
    if (timeout <= 0) {
        return;
    }

    node = &msg->tmo_rbe;
    node->key = dn_msec_now() + timeout;
    node->data = conn;

    rbtree_insert(&tmo_rbt, node);

    log_debug(LOG_VERB, "insert msg %"PRIu64" into tmo rbt with expiry of "
              "%d msec", msg->id, timeout);
}

void
msg_tmo_delete(struct msg *msg)
{
    struct rbnode *node;

    node = &msg->tmo_rbe;

    /* already deleted */

    if (node->data == NULL) {
        return;
    }

    rbtree_delete(&tmo_rbt, node);

    log_debug(LOG_VERB, "delete msg %"PRIu64" from tmo rbt", msg->id);
}


static uint32_t alloc_msg_count = 0;

static struct msg *
_msg_get(bool force_alloc)
{
    struct msg *msg;

    if (!TAILQ_EMPTY(&free_msgq)) {
        ASSERT(nfree_msgq > 0);

        msg = TAILQ_FIRST(&free_msgq);
        nfree_msgq--;
        TAILQ_REMOVE(&free_msgq, msg, m_tqe);
        goto done;
    }

    //protect our server in the slow network and high traffics.
    //we drop client requests but still honor our peer requests
    if (alloc_msg_count >= ALLOWED_ALLOC_MSGS && !force_alloc) {
   	  log_debug(LOG_WARN, "allocated #msgs %d hit max limit", alloc_msg_count);
   	  return NULL;
    }

    if (alloc_msg_count >= MAX_ALLOC_MSGS) {
   	 return NULL; //we hit the max limit
    }

    alloc_msg_count++;
    log_debug(LOG_WARN, "alloc_msg_count : %d", alloc_msg_count);

    msg = dn_alloc(sizeof(*msg));
    if (msg == NULL) {
        return NULL;
    }

done:
    /* c_tqe, s_tqe, and m_tqe are left uninitialized */
    msg->id = ++msg_id;
    msg->peer = NULL;
    msg->owner = NULL;
    msg->stime_in_microsec = 0L;

    rbtree_node_init(&msg->tmo_rbe);

    STAILQ_INIT(&msg->mhdr);
    msg->mlen = 0;

    msg->state = 0;
    msg->pos = NULL;
    msg->token = NULL;

    msg->parser = NULL;
    msg->result = MSG_PARSE_OK;

    msg->pre_splitcopy = NULL;
    msg->post_splitcopy = NULL;
    msg->pre_coalesce = NULL;
    msg->post_coalesce = NULL;

    msg->type = MSG_UNKNOWN;

    msg->key_start = NULL;
    msg->key_end = NULL;

    msg->vlen = 0;
    msg->end = NULL;

    msg->frag_owner = NULL;
    msg->nfrag = 0;
    msg->frag_id = 0;

    msg->narg_start = NULL;
    msg->narg_end = NULL;
    msg->narg = 0;
    msg->rnarg = 0;
    msg->rlen = 0;
    msg->integer = 0;

    msg->err = 0;
    msg->error = 0;
    msg->ferror = 0;
    msg->request = 0;
    msg->quit = 0;
    msg->noreply = 0;
    msg->done = 0;
    msg->fdone = 0;
    msg->first_fragment = 0;
    msg->last_fragment = 0;
    msg->swallow = 0;
    msg->redis = 0;

    //dynomite
    msg->is_read = 1;
    msg->dyn_state = 0;
    msg->dmsg = NULL;
    msg->msg_type = 0;

    return msg;
}

uint32_t msg_free_queue_size(void)
{
	return nfree_msgq;
}

struct msg *
msg_get(struct conn *conn, bool request, bool redis)
{
    struct msg *msg;

    msg = _msg_get(conn->dyn_mode);
    if (msg == NULL) {
        return NULL;
    }

    msg->owner = conn;
    msg->request = request ? 1 : 0;
    msg->redis = redis ? 1 : 0;

    if (redis) {
        if (request) {
            if (conn->dyn_mode) {
               msg->parser = dyn_parse_req;
            } else {
               msg->parser = redis_parse_req;
            }
        } else {
            if (conn->dyn_mode) {
               msg->parser = dyn_parse_rsp;
            } else {
               msg->parser = redis_parse_rsp;
            }
        }
        msg->pre_splitcopy = redis_pre_splitcopy;
        msg->post_splitcopy = redis_post_splitcopy;
        msg->pre_coalesce = redis_pre_coalesce;
        msg->post_coalesce = redis_post_coalesce;
    } else {
        if (request) {
            if (conn->dyn_mode) {
               msg->parser = dyn_parse_req;
            } else {
               msg->parser = memcache_parse_req;
            }
        } else {
            if (conn->dyn_mode) {
               msg->parser = dyn_parse_rsp;
            } else {
               msg->parser = memcache_parse_rsp;
            }
        }
        msg->pre_splitcopy = memcache_pre_splitcopy;
        msg->post_splitcopy = memcache_post_splitcopy;
        msg->pre_coalesce = memcache_pre_coalesce;
        msg->post_coalesce = memcache_post_coalesce;
    }

    log_debug(LOG_VVERB, "get msg %p id %"PRIu64" request %d owner sd %d",
              msg, msg->id, msg->request, conn->sd);

    return msg;
}

rstatus_t 
msg_clone(struct msg *src, struct mbuf *mbuf_start, struct msg *target)
{
    target->owner = src->owner;
    target->request = src->request;
    target->redis = src->redis;

    target->parser = src->parser;
    target->pre_splitcopy = src->pre_splitcopy;
    target->post_splitcopy = src->post_splitcopy; 
    target->pre_coalesce = src->pre_coalesce;
    target->post_coalesce = src->post_coalesce;

    target->noreply = src->noreply;
    target->type = src->type;
    target->key_start = src->key_start;
    target->key_end = src->key_end;
    target->mlen = src->mlen;
    target->pos = src->pos;
    target->vlen = src->vlen;
    target->is_read = src->is_read;

    struct mbuf *mbuf, *nbuf;
    bool started = false;
    STAILQ_FOREACH(mbuf, &src->mhdr, next) {
        if (!started && mbuf != mbuf_start) {
            continue;
        } else {
            started = true;
        }
        nbuf = mbuf_get();
        if (nbuf == NULL) {
            return ENOMEM;
        }

        uint32_t len = mbuf_length(mbuf);
        mbuf_copy(nbuf, mbuf->pos, len);
        mbuf_insert(&target->mhdr, nbuf);
    }

    return DN_OK;
}


struct msg *
msg_get_error(bool redis, dyn_error_t dyn_err, err_t err)
{
    struct msg *msg;
    struct mbuf *mbuf;
    int n;
    char *errstr = err ? strerror(err) : "unknown";
    char *protstr = redis ? "-ERR" : "SERVER_ERROR";
    char *source;

    if (dyn_err == PEER_CONNECTION_REFUSE) {
    	source = "Peer:";
    } else if (dyn_err == STORAGE_CONNECTION_REFUSE) {
    	source = "Storage:";
    }

    msg = _msg_get(1);
    if (msg == NULL) {
        return NULL;
    }

    msg->state = 0;
    msg->type = MSG_RSP_MC_SERVER_ERROR;

    mbuf = mbuf_get();
    if (mbuf == NULL) {
        msg_put(msg);
        return NULL;
    }
    mbuf_insert(&msg->mhdr, mbuf);

    n = dn_scnprintf(mbuf->last, mbuf_size(mbuf), "%s %s %s"CRLF, protstr, source, errstr);
    mbuf->last += n;
    msg->mlen = (uint32_t)n;

    log_debug(LOG_VVERB, "get msg %p id %"PRIu64" len %"PRIu32" error '%s'",
              msg, msg->id, msg->mlen, errstr);

    return msg;
}

static void
msg_free(struct msg *msg)
{
    ASSERT(STAILQ_EMPTY(&msg->mhdr));

    log_debug(LOG_VVERB, "free msg %p id %"PRIu64"", msg, msg->id);
    dn_free(msg);
}

void
msg_put(struct msg *msg)
{
    log_debug(LOG_VVERB, "put msg %p id %"PRIu64"", msg, msg->id);

    struct dmsg *dmsg = msg->dmsg;
    if (dmsg != NULL) {
    	dmsg_put(dmsg);
    	msg->dmsg = NULL;
    }

    while (!STAILQ_EMPTY(&msg->mhdr)) {
        struct mbuf *mbuf = STAILQ_FIRST(&msg->mhdr);
        mbuf_remove(&msg->mhdr, mbuf);
        mbuf_put(mbuf);
    }

    nfree_msgq++;
    TAILQ_INSERT_HEAD(&free_msgq, msg, m_tqe);
}


uint32_t msg_mbuf_size(struct msg *msg)
{
	uint32_t count = 0;
	struct mbuf *mbuf;

	STAILQ_FOREACH(mbuf, &msg->mhdr, next) {
		count++;
	}

	return count;
}

uint32_t msg_length(struct msg *msg)
{
	uint32_t count = 0;
	struct mbuf *mbuf;

	STAILQ_FOREACH(mbuf, &msg->mhdr, next) {
		count += mbuf->last - mbuf->start;
	}

	return count;
}

void
msg_dump(struct msg *msg)
{

    struct mbuf *mbuf;

    if (msg == NULL) {
    	loga("msg is NULL - cannot display its info");
    	return;
    }

    loga("msg dump id %"PRIu64" request %d len %"PRIu32" type %d done %d "
         "error %d (err %d)", msg->id, msg->request, msg->mlen, msg->type,
         msg->done, msg->error, msg->err);

    STAILQ_FOREACH(mbuf, &msg->mhdr, next) {
        uint8_t *p, *q;
        long int len;

        p = mbuf->start;
        q = mbuf->last;
        len = q - p;

        loga_hexdump(p, len, "mbuf with %ld bytes of data", len);
    }

}

void
msg_init(void)
{
    log_debug(LOG_DEBUG, "msg size %d", sizeof(struct msg));
    msg_id = 0;
    frag_id = 0;
    nfree_msgq = 0;
    TAILQ_INIT(&free_msgq);
    rbtree_init(&tmo_rbt, &tmo_rbs);
}

void
msg_deinit(void)
{
    struct msg *msg, *nmsg;

    for (msg = TAILQ_FIRST(&free_msgq); msg != NULL;
         msg = nmsg, nfree_msgq--) {
        ASSERT(nfree_msgq > 0);
        nmsg = TAILQ_NEXT(msg, m_tqe);
        msg_free(msg);
    }
    ASSERT(nfree_msgq == 0);
}

bool
msg_empty(struct msg *msg)
{
    return msg->mlen == 0 ? true : false;
}

static rstatus_t
msg_parsed(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *nmsg;
    struct mbuf *mbuf, *nbuf;

    mbuf = STAILQ_LAST(&msg->mhdr, mbuf, next);

    if (msg->pos == mbuf->last) {
       /* no more data to parse */
       conn->recv_done(ctx, conn, msg, NULL);
       return DN_OK;
     }


    /*
     * Input mbuf has un-parsed data. Split mbuf of the current message msg
     * into (mbuf, nbuf), where mbuf is the portion of the message that has
     * been parsed and nbuf is the portion of the message that is un-parsed.
     * Parse nbuf as a new message nmsg in the next iteration.
     */
    nbuf = mbuf_split(&msg->mhdr, msg->pos, NULL, NULL);
    if (nbuf == NULL) {
        return DN_ENOMEM;
    }

    nmsg = msg_get(msg->owner, msg->request, conn->redis);
    if (nmsg == NULL) {
        mbuf_put(nbuf);
        return DN_ENOMEM;
    }
    mbuf_insert(&nmsg->mhdr, nbuf);
    nmsg->pos = nbuf->pos;

    /* update length of current (msg) and new message (nmsg) */
    nmsg->mlen = mbuf_length(nbuf);
    msg->mlen -= nmsg->mlen;

    conn->recv_done(ctx, conn, msg, nmsg);

    return DN_OK;
}

static rstatus_t
msg_fragment(struct context *ctx, struct conn *conn, struct msg *msg)
{
    rstatus_t status;  /* return status */
    struct msg *nmsg;  /* new message */
    struct mbuf *nbuf; /* new mbuf */

    ASSERT((conn->client && !conn->proxy) || (conn->dnode_client && !conn->dnode_server));
    ASSERT(msg->request);

    nbuf = mbuf_split(&msg->mhdr, msg->pos, msg->pre_splitcopy, msg);
    if (nbuf == NULL) {
        return DN_ENOMEM;
    }

    status = msg->post_splitcopy(msg);
    if (status != DN_OK) {
        mbuf_put(nbuf);
        return status;
    }

    nmsg = msg_get(msg->owner, msg->request, msg->redis);
    if (nmsg == NULL) {
        mbuf_put(nbuf);
        return DN_ENOMEM;
    }
    mbuf_insert(&nmsg->mhdr, nbuf);
    nmsg->pos = nbuf->pos;

    /* update length of current (msg) and new message (nmsg) */
    nmsg->mlen = mbuf_length(nbuf);
    msg->mlen -= nmsg->mlen;

    /*
     * Attach unique fragment id to all fragments of the message vector. All
     * fragments of the message, including the first fragment point to the
     * first fragment through the frag_owner pointer. The first_fragment and
     * last_fragment identify first and last fragment respectively.
     *
     * For example, a message vector given below is split into 3 fragments:
     *  'get key1 key2 key3\r\n'
     *  Or,
     *  '*4\r\n$4\r\nmget\r\n$4\r\nkey1\r\n$4\r\nkey2\r\n$4\r\nkey3\r\n'
     *
     *   +--------------+
     *   |  msg vector  |
     *   |(original msg)|
     *   +--------------+
     *
     *       frag_owner         frag_owner
     *     /-----------+      /------------+
     *     |           |      |            |
     *     |           v      v            |
     *   +--------------------+     +---------------------+
     *   |   frag_id = 10     |     |   frag_id = 10      |
     *   | first_fragment = 1 |     |  first_fragment = 0 |
     *   | last_fragment = 0  |     |  last_fragment = 0  |
     *   |     nfrag = 3      |     |      nfrag = 0      |
     *   +--------------------+     +---------------------+
     *               ^
     *               |  frag_owner
     *               \-------------+
     *                             |
     *                             |
     *                  +---------------------+
     *                  |   frag_id = 10      |
     *                  |  first_fragment = 0 |
     *                  |  last_fragment = 1  |
     *                  |      nfrag = 0      |
     *                  +---------------------+
     *
     *
     */
    if (msg->frag_id == 0) {
        msg->frag_id = ++frag_id;
        msg->first_fragment = 1;
        msg->nfrag = 1;
        msg->frag_owner = msg;
    }
    nmsg->frag_id = msg->frag_id;
    msg->last_fragment = 0;
    nmsg->last_fragment = 1;
    nmsg->frag_owner = msg->frag_owner;
    msg->frag_owner->nfrag++;

    if (!conn->dyn_mode) {
       stats_pool_incr(ctx, conn->owner, fragments);
    } else {
    	
    }

    log_debug(LOG_VERB, "fragment msg into %"PRIu64" and %"PRIu64" frag id "
              "%"PRIu64"", msg->id, nmsg->id, msg->frag_id);

    conn->recv_done(ctx, conn, msg, nmsg);

    return DN_OK;
}

static rstatus_t
msg_repair(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct mbuf *nbuf;

    nbuf = mbuf_split(&msg->mhdr, msg->pos, NULL, NULL);
    if (nbuf == NULL) {
        return DN_ENOMEM;
    }
    mbuf_insert(&msg->mhdr, nbuf);
    msg->pos = nbuf->pos;

    return DN_OK;
}


static rstatus_t
msg_parse(struct context *ctx, struct conn *conn, struct msg *msg)
{
	rstatus_t status;

	if (msg_empty(msg)) {
		/* no data to parse */
		conn->recv_done(ctx, conn, msg, NULL);
		return DN_OK;
	}

	msg->parser(msg);

	switch (msg->result) {
	case MSG_PARSE_OK:
		//log_debug(LOG_VVERB, "MSG_PARSE_OK");
		status = msg_parsed(ctx, conn, msg);
		break;

	case MSG_PARSE_FRAGMENT:
		//log_debug(LOG_VVERB, "MSG_PARSE_FRAGMENT");
		status = msg_fragment(ctx, conn, msg);
		break;

	case MSG_PARSE_REPAIR:
		//log_debug(LOG_VVERB, "MSG_PARSE_REPAIR");
		status = msg_repair(ctx, conn, msg);
		break;

	case MSG_PARSE_AGAIN:
		//log_debug(LOG_VVERB, "MSG_PARSE_AGAIN");
		status = DN_OK;
		break;

	default:
		if (!conn->dyn_mode) {
			status = DN_ERROR;
			conn->err = errno;
		} else {
			log_debug(LOG_VVERB, "Parsing error in dyn_mode");
			status = DN_OK;
		}

		break;
	}

	return conn->err != 0 ? DN_ERROR : status;
}


static rstatus_t
msg_recv_chain(struct context *ctx, struct conn *conn, struct msg *msg)
{
	rstatus_t status;
	struct msg *nmsg;
	struct mbuf *mbuf;
	size_t msize;
	ssize_t n;
	int expected_fill = (msg->dmsg != NULL && msg->dmsg->bit_field == 1) ?
			msg->dmsg->plen : -1;  //used in encryption case only

	mbuf = STAILQ_LAST(&msg->mhdr, mbuf, next);
	if (mbuf == NULL || mbuf_full(mbuf) ||
			(expected_fill != -1 && mbuf->last == mbuf->end_extra)) {
		mbuf = mbuf_get();
		if (mbuf == NULL) {
			return DN_ENOMEM;
		}
		mbuf_insert(&msg->mhdr, mbuf);
		msg->pos = mbuf->pos;
	}

	ASSERT(mbuf->end_extra - mbuf->last > 0);

	msize = (expected_fill == -1)? mbuf_size(mbuf) :
			(msg->dmsg->plen < mbuf->end_extra - mbuf->last) ? msg->dmsg->plen : mbuf->end_extra - mbuf->last;

	n = conn_recv(conn, mbuf->last, msize);

	if (n < 0) {
		if (n == DN_EAGAIN) {
			return DN_OK;
		}

		return DN_ERROR;
	}

	ASSERT((mbuf->last + n) <= mbuf->end_extra);
	mbuf->last += n;
	msg->mlen += (uint32_t)n;

	if (expected_fill != -1) {
		if ( n >=  msg->dmsg->plen  || mbuf->end_extra == mbuf->last) {
			log_debug(LOG_VERB, "About to decrypt this mbuf as it is full or eligible!");
			struct mbuf *nbuf = mbuf_get();

			if (nbuf == NULL) {
				loga("Enough enough memory error!!!");
				return DN_ENOMEM;
			}

			if (mbuf->end_extra == mbuf->last) {
				dyn_aes_decrypt(mbuf->start, mbuf->last - mbuf->start, nbuf, msg->owner->aes_key);
			} else {
				dyn_aes_decrypt(mbuf->start, msg->dmsg->plen, nbuf, msg->owner->aes_key);
				mbuf_copy(nbuf, mbuf->start + msg->dmsg->plen, mbuf->last - mbuf->start - msg->dmsg->plen);
			}
			nbuf->read_flip = 1;

			mbuf_remove(&msg->mhdr, mbuf);
			mbuf_insert(&msg->mhdr, nbuf);
			msg->pos = nbuf->start;

			msg->mlen -= mbuf->last - mbuf->start;
			msg->mlen += nbuf->last - nbuf->start;

			mbuf_put(mbuf);
		}

		msg->dmsg->plen -= n;
	}

	for (;;) {
		status = msg_parse(ctx, conn, msg);
		if (status != DN_OK) {
			return status;
		}

		/* get next message to parse */
		nmsg = conn->recv_next(ctx, conn, false);
		if (nmsg == NULL || nmsg == msg) {
			/* no more data to parse */
			break;
		}

		msg = nmsg;
	}

	return DN_OK;
}

rstatus_t
msg_recv(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    struct msg *msg;

    ASSERT(conn->recv_active);

    conn->recv_ready = 1;
    do {
        msg = conn->recv_next(ctx, conn, true);
        if (msg == NULL) {
            return DN_OK;
        }

        status = msg_recv_chain(ctx, conn, msg);
        if (status != DN_OK) {
            return status;
        }
    } while (conn->recv_ready);

    return DN_OK;
}

static rstatus_t
msg_send_chain(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg_tqh send_msgq;            /* send msg q */
    struct msg *nmsg;                    /* next msg */
    struct mbuf *mbuf, *nbuf;            /* current and next mbuf */
    size_t mlen;                         /* current mbuf data length */
    struct iovec *ciov, iov[DN_IOV_MAX]; /* current iovec */
    struct array sendv;                  /* send iovec */
    size_t nsend, nsent;                 /* bytes to send; bytes sent */
    size_t limit;                        /* bytes to send limit */
    ssize_t n;                           /* bytes sent by sendv */

    if (get_tracking_level() >= LOG_VVERB) {
       loga("About to dump out the content of msg");
       msg_dump(msg);
    }

    TAILQ_INIT(&send_msgq);

    array_set(&sendv, iov, sizeof(iov[0]), DN_IOV_MAX);

    /* preprocess - build iovec */

    nsend = 0;
    /*
     * readv() and writev() returns EINVAL if the sum of the iov_len values
     * overflows an ssize_t value Or, the vector count iovcnt is less than
     * zero or greater than the permitted maximum.
     */
    limit = SSIZE_MAX;

    for (;;) {
        ASSERT(conn->smsg == msg);

        TAILQ_INSERT_TAIL(&send_msgq, msg, m_tqe);

        for (mbuf = STAILQ_FIRST(&msg->mhdr);
             mbuf != NULL && array_n(&sendv) < DN_IOV_MAX && nsend < limit;
             mbuf = nbuf) {
            nbuf = STAILQ_NEXT(mbuf, next);

            if (mbuf_empty(mbuf)) {
                continue;
            }

            mlen = mbuf_length(mbuf);
            if ((nsend + mlen) > limit) {
                mlen = limit - nsend;
            }

            ciov = array_push(&sendv);
            ciov->iov_base = mbuf->pos;
            ciov->iov_len = mlen;

            nsend += mlen;
        }

        if (array_n(&sendv) >= DN_IOV_MAX || nsend >= limit) {
            break;
        }

        msg = conn->send_next(ctx, conn);
        if (msg == NULL) {
            break;
        }
    }

    ASSERT(!TAILQ_EMPTY(&send_msgq) && nsend != 0);

    conn->smsg = NULL;

    n = conn_sendv(conn, &sendv, nsend);

    nsent = n > 0 ? (size_t)n : 0;

    /* postprocess - process sent messages in send_msgq */

    for (msg = TAILQ_FIRST(&send_msgq); msg != NULL; msg = nmsg) {
        nmsg = TAILQ_NEXT(msg, m_tqe);

        TAILQ_REMOVE(&send_msgq, msg, m_tqe);

        if (nsent == 0) {
            if (msg->mlen == 0) {
                conn->send_done(ctx, conn, msg);
            }
            continue;
        }

        /* adjust mbufs of the sent message */
        for (mbuf = STAILQ_FIRST(&msg->mhdr); mbuf != NULL; mbuf = nbuf) {
            nbuf = STAILQ_NEXT(mbuf, next);

            if (mbuf_empty(mbuf)) {
                continue;
            }

            mlen = mbuf_length(mbuf);
            if (nsent < mlen) {
                /* mbuf was sent partially; process remaining bytes later */
                mbuf->pos += nsent;
                ASSERT(mbuf->pos < mbuf->last);
                nsent = 0;
                break;
            }

            /* mbuf was sent completely; mark it empty */
            mbuf->pos = mbuf->last;
            nsent -= mlen;
        }

        /* message has been sent completely, finalize it */
        if (mbuf == NULL) {
            conn->send_done(ctx, conn, msg);
        }
    }

    ASSERT(TAILQ_EMPTY(&send_msgq));

    if (n > 0) {
        return DN_OK;
    }

    return (n == DN_EAGAIN) ? DN_OK : DN_ERROR;
}

rstatus_t
msg_send(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    struct msg *msg;

    ASSERT(conn->send_active);

    conn->send_ready = 1;
    do {
        msg = conn->send_next(ctx, conn);
        if (msg == NULL) {
            /* nothing to send */
            return DN_OK;
        }

        status = msg_send_chain(ctx, conn, msg);
        if (status != DN_OK) {
            return status;
        }

    } while (conn->send_ready);

    return DN_OK;
}
