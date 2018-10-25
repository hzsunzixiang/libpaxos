#include "evpaxos.h"
#include "learner.h"
#include "peers.h"
#include "tcp_sendbuf.h"
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>


struct evlearner
{
	struct learner*			state;		/*learner����Ϣ������*/	
	deliver_function*		delfun;		/*�᰸����call back*/
	void*					delarg;		/*defun�ص�����*/
	struct event*			hole_timer;	/*���holes�Ķ�ʱ��*/
	struct timeval			tv;			/*��ʱ��ʱ��*/
	struct peers*			acceptors;	/*��acceptors�����ӹ�����*/
};

#define LEARNER_CHUNNK 10000

static void learner_check_holes(evutil_socket_t fd, short event, void* arg)
{
	int i;
	iid_t iid, from, to;
	struct evlearner* l = (struct evlearner*)arg;

	/*���holes,���Ƿ��еȴ���ɵ��᰸*/
	if(learner_has_holes(l->state, &from, &to)){
		if(to - from > LEARNER_CHUNNK)
			to = from + LEARNER_CHUNNK;

		for(iid = from; iid < to; iid ++){
			for(i = 0; i < peers_count(l->acceptors); i ++){ /*����������acceptor���;�����*/
				struct bufferevent* bev = peers_get_buffer(l->acceptors, i);
				sendbuf_add_repeat_req(bev, iid);
			}
		}
	}

	/*���붨ʱ��*/
	event_add(l->hole_timer, &l->tv);
}

static void learner_deliver_next_closed(struct evlearner* l)
{
	int prop_id;
	accept_ack* ack;
	while((ack = learner_deliver_next(l->state)) != NULL){
		/*���������proposer id*/
		prop_id = ack->ballot % MAX_N_OF_PROPOSERS;
		l->delfun(ack->value, ack->value_size, ack->iid, ack->ballot, prop_id, l->delarg);

		free(ack);
	}
}

static void learner_handle_accept_ack(struct evlearner* l, accept_ack * aa)
{
	/*����accept ack����*/
	learner_receive_accept(l->state, aa);

	/*����Ƿ��п��Է�����᰸*/
	learner_deliver_next_closed(l);
}

static void learner_handle_msg(struct evlearn* l, struct bufferevent* bev)
{
	paxos_msg msg;
	struct evbuffer* in;
	char* buffer = NULL;

	/*������Ϣͷ*/
	in = bufferevent_get_input(bev);
	evbuffer_remove(in, &msg, sizeof(paxos_msg));

	/*������Ϣ��*/
	if(msg.data_size > 0){
		buffer = (char *)malloc(msg.data_size);
		evbuffer_remove(in, buffer, msg.data_size);
	}

	switch(msg.type){
	case accept_acks: 
		learner_handle_accept_ack(l, (accept_ack*)buffer);
		break;

	default:
		paxos_log_error("Unknow msg type %d not handled", msg.type);
	}

	if(buffer != NULL)
		free(buffer);
}

static void on_acceptor_msg(struct bufferevent* bev, void* arg)
{
	size_t len;
	paxos_msg msg;
	struct evlearner* l = arg;
	struct evbuffer* in = bufferevent_get_input(bev);

	/*����Ϣ����ѭ������*/
	while ((len = evbuffer_get_length(in)) > sizeof(paxos_msg)) {
		evbuffer_copyout(in, &msg, sizeof(paxos_msg));
		if (len >= PAXOS_MSG_SIZE((&msg))) /*�����Ϣ����*/
			learner_handle_msg(l, bev); /*������Ϣ����*/
	}
}

/*���������ļ���Ϣ����һ��evlearner����*/
static struct evlearner* evlearner_init_conf(struct evpaxos_config* c, deliver_function f, void* arg, struct event_base* b)
{
	struct evlearner* l;
	/*��ȡacceptor�ĸ���*/
	int acceptor_count = evpaxos_acceptor_count(c);

	l = (struct evlearner*)malloc(sizeof(struct evlearner*));
	l->delfun = f;
	l->delarg = arg;
	l->state = learner_new(acceptor_count);
	/*����һ��acceptor���ӹ���*/
	l->acceptors = peers_new(b);
	/*�����acceptor������*/
	peers_connect_to_acceptors(l->acceptors, c, on_acceptor_msg, l);

	l->tv.tv_sec = 0;
	l->tv.tv_usec = 100000; /*100ms*/
	/*���ü��holes�Ķ�ʱ���¼�*/
	l->hole_timer = evtimer_new(b, learner_check_holes, l);
	/*���һ����ʱ�¼�*/
	event_add(l->hole_timer, &l->tv);

	return l;
}

struct evlearner* evlearner_init(const char* config_file, deliver_function f, void* arg, struct event_base* base)
{
	/*��ȡ�����ļ�*/
	struct evpaxos_config* c = evpaxos_config_read(config_file);
	if(c) /*���������ļ�*/
		return evlearner_init_conf(c, f, arg, base);

	return NULL;
}

void evlearner_free(struct evlearner* l)
{
	/*�ͷ����ӹ�����*/
	peers_free(l->acceptors);
	/*�ͷż��hole�Ķ�ʱ��*/
	event_free(l->hole_timer);
	/*�ͷ���Ϣ������*/
	learner_free(l->state);

	free(l);
}





