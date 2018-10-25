#include "peers.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <event2/bufferevent.h>

struct peer
{
	struct bufferevent* bev;
	struct event*		reconnect_ev;
	struct sockaddr_in	addr;
	bufferevent_data_cb	cb;
	void*				arg;
};

struct peers
{
	int					count;
	struct peer**		peers;/*peer����*/
	struct event_base*	base;
};

/*����ʱ��*/
static struct timeval reconnect_timeout = {2, 0};

static struct peer* make_peer(struct event_base* base, struct sockaddr_in* addr, bufferevent_data_cb cb, void* arg);
static void free_peer(struct peer* p);
static void connect_peer(struct peer* p);

struct peers* peers_new(struct event_base* base)
{
	struct peers* p = (struct peers *)malloc(sizeof(struct peers));
	p->count = 0;
	p->peers = NULL;
	p->base = base;

	return p;
}

void peers_free(struct peers* p)
{
	int i;
	if(p != NULL){
		for(i = 0; i < p->count; i ++)
			free_peer(p->peers[i]);

		if(p->count > 0)
			free(p->peers);

		free(p);
	}
}
void peers_connect(struct peers* p, struct sockaddr_in* addr, bufferevent_data_cb cb, void* arg)
{
	p->peers = realloc(p->peers, sizeof(struct peer*) * (p->count+1));
	p->peers[p->count] = make_peer(p->base, addr, cb, arg);
	p->count++;
}
/*�����е�acceptor��������*/
void peers_connect_to_acceptors(struct peers* p, struct evpaxos_config* conf, bufferevent_data_cb cb, void* arg)
{
	int i;
	for(i = 0; i < evpaxos_acceptor_count(conf); i++){
		struct sockaddr_in addr = evpaxos_acceptor_address(c, i);
		peers_connect(p, &addr, cb, arg);
	}
}

int peer_count(struct peers* p)
{
	return p->count;
}

struct bufferevent* peers_get_buffer(struct peers* p, int i)
{
	return p->peers[i]->bev;
}

static void on_socket_event(struct bufferevent* bev, short ev, void* arg)
{
	struct peer* p = (struct peer*)arg;

	if (ev & BEV_EVENT_CONNECTED){
		paxos_log_info("Connected to %s:%d", inet_ntoa(p->addr.sin_addr), ntohs(p->addr.sin_port));
	} else if (ev & BEV_EVENT_ERROR || ev & BEV_EVENT_EOF) { /*����ʧ�ܻ���socket��д����*/
		struct event_base* base;
		int err = EVUTIL_SOCKET_ERROR();
		paxos_log_error("%s (%s:%d)", evutil_socket_error_to_string(err), inet_ntoa(p->addr.sin_addr), ntohs(p->addr.sin_port));

		base = bufferevent_get_base(p->bev);
		bufferevent_free(p->bev);
		/*���¿���һ���µ�SOCKET����*/
		p->bev = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE);

		/*�����¼��ص�����*/
		bufferevent_setcb(p->bev, on_read, NULL, on_socket_event, p);
		/*����������ʱ*/
		event_add(p->reconnect_ev, &reconnect_timeout);
	} else {
		paxos_log_error("Event %d not handled", ev);
	}
}

static void on_connection_timeout(int fd, short ev, void* arg)
{
	connect_peer((struct peer *)arg);
}

static void connect_peer(struct peer* p)
{
	bufferevent_enable(p->bev, EV_READ|EV_WRITE);
	bufferevent_socket_connect(p->bev, struct sockaddr*)&p->addr, sizeof(p->addr));
	paxos_log_info("Connect to %s:%d", inet_ntoa(p->addr.sin_addr), ntohs(p->addr.sin_port));
}

/*����һ��peer����*/
static struct peer* make_peer(struct event_base* base, struct sockaddr_in* addr, bufferevent_data_cb cb, void* arg)
{
	struct peer* p = (struct peer *)malloc(sizeof(struct peer));
	p->addr = *addr;
	p->reconnect_ev = evtimer_new(base, on_connection_timeout, p); /*���ӳ�ʱ�ص�*/
	p->cb = cb;
	p->arg = arg;

	bufferevent_setcb(p->bev, on_read, NULL, on_socket_event, p); /*�����¼��ص�*/
	connect_peer(p);

	return p;
}

static void free_peer(struct peer* p)
{
	if(p != NULL){
		bufferevent_free(p->bev);
		event_free(p->reconnect_ev);
		free(p);
	}
}

