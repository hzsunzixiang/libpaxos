#ifndef __TCP_SENDBUF_H_
#define __TCP_SENDBUF_H_

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <event2/bufferevent.h>

#include "evpaxos.h"
#include "libpaxos_message.h"

void sendbuf_add_prepare_req(struct bufferevent* bev, prepare_req* pr);
void sendbuf_add_prepare_ack(struct bufferevent* bev, acceptor_record* rec);
void sendbuf_add_accept_req(struct bufferevent* bev, accept_req* ar);
void sendbuf_add_accept_ack(struct bufferevent* bev, acceptor_record* rec);
void sendbuf_add_repeat_req(struct bufferevent* bev, iid_t iid);

#endif

