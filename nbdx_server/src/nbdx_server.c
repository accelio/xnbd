/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/queue.h>
#include "libxio.h"
#include "nbdx_handlers.h"
#include "libnbdx.h"
#include <arpa/inet.h>


/*---------------------------------------------------------------------------*/
/* preprocessor macros							     */
/*---------------------------------------------------------------------------*/
#define SIMULATE_DESTROY	0
#define MAX_SGL_LEN		128

#ifndef SLIST_FOREACH_SAFE
#define	SLIST_FOREACH_SAFE(var, head, field, tvar)			 \
	for ((var) = SLIST_FIRST((head));				 \
			(var) && ((tvar) = SLIST_NEXT((var), field), 1); \
			(var) = (tvar))
#endif

/*---------------------------------------------------------------------------*/
/* structures								     */
/*---------------------------------------------------------------------------*/
struct portals_vec {
	int				vec_len;
	int				pad;
	const char			*vec[MAX_THREADS];
};

/*---------------------------------------------------------------------------*/
/* portals_get								     */
/*---------------------------------------------------------------------------*/
static struct portals_vec *portals_get(struct nbdx_server_data *server_data,
				const char *uri, void *user_context)
{
	/* fill portals array and return it. */
	int			i, j;
	struct portals_vec	*portals = calloc(1, sizeof(*portals));
	if (server_data->last_reaped != -1) {
		server_data->last_used = server_data->last_reaped;
		server_data->last_reaped = -1;
	}
	for (i = 0; i < MAX_THREADS; i++) {
		j = (server_data->last_used + i)%MAX_THREADS;

		portals->vec[i] = strdup(server_data->tdata[j].portal);
		portals->vec_len++;
	}
	server_data->last_used = (server_data->last_used + 1)%MAX_THREADS;

	return portals;
}

/*---------------------------------------------------------------------------*/
/* portals_free								     */
/*---------------------------------------------------------------------------*/
static void portals_free(struct portals_vec *portals)
{
	int			i;
	for (i = 0; i < portals->vec_len; i++)
		free((char *)(portals->vec[i]));

	free(portals);
}

/*---------------------------------------------------------------------------*/
/* on_response_comp							     */
/*---------------------------------------------------------------------------*/
static int on_response_comp(struct xio_session *session,
			struct xio_msg *rsp,
			void *cb_user_context)
{
	struct nbdx_thread_data		*tdata = cb_user_context;
	struct nbdx_session_data	*ses_data, *tmp_ses_data;
	int				i = 0;

	SLIST_FOREACH_SAFE(ses_data, &tdata->server_data->ses_list,
			   srv_ses_list, tmp_ses_data) {
		if (ses_data->session == session) {
			for (i = 0; i < MAX_THREADS; i++) {
				if (ses_data->portal_data[i].tdata == tdata) {
					/* process request */
					nbdx_handler_on_rsp_comp(
					      ses_data->dd_data,
					      ses_data->portal_data[i].dd_data,
					      rsp);
					return 0;
				}
			}
		}
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_request callback							     */
/*---------------------------------------------------------------------------*/
static int on_request(struct xio_session *session,
			struct xio_msg *req,
			int more_in_batch,
			void *cb_user_context)
{
	struct nbdx_thread_data		*tdata = cb_user_context;
	struct nbdx_session_data	*ses_data, *tmp_ses_data;
	int				i, retval;

	SLIST_FOREACH_SAFE(ses_data, &tdata->server_data->ses_list,
			   srv_ses_list, tmp_ses_data) {
		if (ses_data->session == session) {
			for (i = 0; i < MAX_THREADS; i++) {
				if (ses_data->portal_data[i].tdata == tdata) {
					/* process request */
					retval = nbdx_handler_on_req(ses_data->dd_data,
								     ses_data->portal_data[i].dd_data,
								     req);
					if (retval)
						fprintf(stdout, "failed to process request\n");
					return retval;
				}
			}
		}
	}
	fprintf(stdout, "session not found\n");


	return 0;
}

/*---------------------------------------------------------------------------*/
/* asynchronous callbacks						     */
/*---------------------------------------------------------------------------*/
static struct xio_session_ops  portal_server_ops = {
	.on_session_event		=  NULL,
	.on_new_session			=  NULL,
	.on_msg_send_complete		=  on_response_comp,
	.on_msg				=  on_request,
	.on_msg_error			=  NULL
};
/*---------------------------------------------------------------------------*/
/* worker thread callback						     */
/*---------------------------------------------------------------------------*/
static void *portal_server_cb(void *data)
{
	struct nbdx_thread_data	*tdata = data;
	cpu_set_t		cpuset;
	pthread_t		thread;
	struct xio_server	*server;

	/* set affinity to thread */
	thread = pthread_self();

	CPU_ZERO(&cpuset);
	CPU_SET(tdata->affinity, &cpuset);

	pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);

	/* create thread context for the client */
	tdata->ctx = xio_context_create(NULL, 0, tdata->affinity);

	/* bind a listener server to a portal/url */
	server = xio_bind(tdata->ctx, &portal_server_ops, tdata->portal,
			  NULL, 0, tdata);
	if (server == NULL) {
		fprintf(stderr, "failed to bind server\n");
		goto cleanup;
	}

	/* the default xio supplied main loop */
	xio_context_run_loop(tdata->ctx, XIO_INFINITE);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* detach the server */
	xio_unbind(server);

cleanup:
	/* free the context */
	xio_context_destroy(tdata->ctx);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* on_session_event							     */
/*---------------------------------------------------------------------------*/
static int on_session_event(struct xio_session *session,
		struct xio_session_event_data *event_data,
		void *cb_user_context)
{
	struct nbdx_session_data *ses_data, *tmp_ses_data;
	struct nbdx_server_data	 *server_data = cb_user_context;
	int			 i;



	switch (event_data->event) {
	case XIO_SESSION_NEW_CONNECTION_EVENT:
	case XIO_SESSION_CONNECTION_CLOSED_EVENT:
	case XIO_SESSION_CONNECTION_DISCONNECTED_EVENT:
		break;
	case XIO_SESSION_TEARDOWN_EVENT:
		SLIST_FOREACH_SAFE(ses_data, &server_data->ses_list,
				   srv_ses_list, tmp_ses_data) {
			if (ses_data->session == session) {
				for (i = 0; i < MAX_THREADS; i++) {
					if (ses_data->portal_data[i].tdata) {
						nbdx_handler_free_portal_data(
					   ses_data->portal_data[i].dd_data);
					   ses_data->portal_data[i].tdata =
						   NULL;
					   server_data->last_reaped = i;
					}
				}
				nbdx_handler_free_session_data(
						ses_data->dd_data);
				SLIST_REMOVE(&server_data->ses_list,
					     ses_data, nbdx_session_data,
					     srv_ses_list);
				free(ses_data);
				break;
			}
		}
		xio_session_destroy(session);
#if SIMULATE_DESTROY
		for (i = 0; i < MAX_THREADS; i++)
			xio_context_stop_loop(server_data->tdata[i].ctx, 0);
		xio_context_stop_loop(server_data->ctx, 0);
#endif
		break;
	case XIO_SESSION_CONNECTION_TEARDOWN_EVENT:
		xio_connection_destroy(event_data->conn);
		break;
	default:
		printf("unexpected session event: session:%p, %s. reason: %s\n",
		       session,
		       xio_session_event_str(event_data->event),
		       xio_strerror(event_data->reason));
		break;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_new_session							     */
/*---------------------------------------------------------------------------*/
static int on_new_session(struct xio_session *session,
			struct xio_new_session_req *req,
			void *cb_user_context)
{
	struct portals_vec *portals;
	struct nbdx_server_data *server_data = cb_user_context;
	struct nbdx_session_data *ses_data;
	int i, j;

	portals = portals_get(server_data, req->uri, req->private_data);

	/* alloc and  and initialize */
	ses_data = calloc(1, sizeof(*ses_data));
	ses_data->session = session;
	ses_data->dd_data = nbdx_handler_init_session_data(MAX_THREADS, server_data);
	if (!ses_data->dd_data) {
		printf("failed to init session data\n");
		goto free_ses_data;
	}
	for (i = 0; i < MAX_THREADS; i++) {
		ses_data->portal_data[i].tdata = &server_data->tdata[i];
		ses_data->portal_data[i].dd_data =
			nbdx_handler_init_portal_data(
				ses_data->dd_data,
				i,
				ses_data->portal_data[i].tdata->ctx);
		if (!ses_data->portal_data[i].dd_data) {
			printf("failed to init portal data\n");
			goto free_portal_data;
		}
	}
	SLIST_INSERT_HEAD(&server_data->ses_list, ses_data, srv_ses_list);

	/* automatic accept the request */
	xio_accept(session, portals->vec, portals->vec_len, NULL, 0);

	portals_free(portals);

	return 0;

free_portal_data:
	for (j = 0; j < i; j++) {
		nbdx_handler_free_portal_data(ses_data->portal_data[j].dd_data);
		ses_data->portal_data[j].dd_data = NULL;
	}
	nbdx_handler_free_session_data(ses_data->dd_data);
free_ses_data:
	free(ses_data);
	return 1;
}

/*---------------------------------------------------------------------------*/
/* asynchronous callbacks						     */
/*---------------------------------------------------------------------------*/
static struct xio_session_ops  server_ops = {
	.on_session_event		=  on_session_event,
	.on_new_session			=  on_new_session,
	.on_msg_send_complete		=  NULL,
	.on_msg				=  NULL,
	.on_msg_error			=  NULL
};

/*---------------------------------------------------------------------------*/
/* main									     */
/*---------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	struct xio_server	*server;	/* server portal */
	struct nbdx_server_data	server_data;
	char			url[256];
	int			i;
	uint16_t		port = atoi(argv[2]);
	int			curr_cpu;
	int			max_cpus;
	int         size_iov = MAX_SGL_LEN;

	xio_init();

	/* set accelio max message vector used (default is 4).
	 * this values should be equal for client and server.
	 * TODO: check way more than 4 elements are needed, if using
	 * only one sgl entry.
	 */
	xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO,
		    XIO_OPTNAME_MAX_IN_IOVLEN, &size_iov, sizeof(int));
	xio_set_opt(NULL, XIO_OPTLEVEL_ACCELIO,
		    XIO_OPTNAME_MAX_OUT_IOVLEN, &size_iov, sizeof(int));

	curr_cpu = sched_getcpu();
	max_cpus = sysconf(_SC_NPROCESSORS_ONLN);

	memset(&server_data, 0, sizeof(server_data));
	server_data.last_reaped = -1;
	SLIST_INIT(&server_data.ses_list);

	/* create thread context for the client */
	server_data.ctx	= xio_context_create(NULL, 0, curr_cpu);

	/* create url to connect to */
	sprintf(url, "rdma://%s:%d", argv[1], port);
	/* bind a listener server to a portal/url */
	server = xio_bind(server_data.ctx, &server_ops, url, NULL, 0, &server_data);
	if (server == NULL) {
		fprintf(stderr, "failed to bind server\n");
		goto cleanup;
	}

	TAILQ_INIT(&server_data.control_work_queue_list);
	server_data.evt_fd = eventfd(0, EFD_NONBLOCK);
	if (server_data.evt_fd < 0) {
		printf("failed to create eventfd, %d\n", server_data.evt_fd);
		goto cleanup;
	}
	if (xio_context_add_ev_handler(server_data.ctx, server_data.evt_fd,
								   XIO_POLLIN, nbdx_process_control, &server_data)) {
		printf("failed to add event handler to xio context\n");
		goto free_fd;
	}
	if (pthread_mutex_init(&server_data.l_lock, NULL) != 0) {
			printf("mutex init failed\n");
			goto free_ev_handler;
	}
	/* spawn portals */
	for (i = 0; i < MAX_THREADS; i++) {
		server_data.tdata[i].server_data = &server_data;
		server_data.tdata[i].affinity = (curr_cpu + i)%max_cpus;
		printf("[%d] affinity:%d/%d\n", i,
		       server_data.tdata[i].affinity, max_cpus);
		port += 1;
		sprintf(server_data.tdata[i].portal, "rdma://%s:%d",
			argv[1], port);
		pthread_create(&server_data.thread_id[i], NULL,
			       portal_server_cb, &server_data.tdata[i]);
	}
	xio_context_run_loop(server_data.ctx, XIO_INFINITE);

	/* normal exit phase */
	fprintf(stdout, "exit signaled\n");

	/* join the threads */
	for (i = 0; i < MAX_THREADS; i++)
		pthread_join(server_data.thread_id[i], NULL);

	/* free the server */
	xio_unbind(server);
	pthread_mutex_destroy(&server_data.l_lock);
free_ev_handler:
	/* delete event handler */
	xio_context_del_ev_handler(server_data.ctx, server_data.evt_fd);
free_fd:
	/* close the event fd */
	close(server_data.evt_fd);
cleanup:
	/* free the context */
	xio_context_destroy(server_data.ctx);

	xio_shutdown();

	return 0;
}

