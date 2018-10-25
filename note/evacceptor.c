#include <event2/event.h>
#include <event2/util.h>
#include <event2/event_struct.h>
#include <event2/buffer.h>

#include "evpaxos.h"
#include "config.h"
#include "tcp_receiver.h"
#include "acceptor.h"
#include "libpaxos_message.h"
#include "tcp_sendbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

struct evacceptor
{
	int						acceptor_id;		/*acceptor id��һ�������õ�*/
	struct acceptor*		state;				/*��Ϣ������*/
	struct event_base*		base;				/*libevent base*/
	struct tcp_receiver*	receiver;			/*TCP receiver,һ����learner������*/
	struct evpaxos_config*	conf;				
};

/*Received a prepare request (phase 1a).*/
static void handle_prepare_req(struct evacceptor* a, struct bufferevent* bev, prepare_req* pr)
{
	paxos_log_debug("Handling prepare for instance %d ballot %d", pr->iid, pr->ballot);
	/*acceptor��prepare_reqs����*/
	acceptor_record* rec = acceptor_receive_prepare(a->state, pr); 
	/*���ʹ�����*/
	sendbuf_add_prepare_ack(bev, rec);
	/*�ͷ�record��ʱ����*/
	acceptor_free_record(a->state, rec);
}

/*Received a accept request (phase 2a).*/
static void handle_accept_req(struct evacceptor* a, struct bufferevent* bev, accept_req* ar)
{
	paxos_log_debug("Handling accept for instance %d ballot %d", ar->iid, ar->ballot);
	
	int i;

	struct carray* bevs = tcp_receiver_get_events(a->receiver);
	acceptor_record* rec = acceptor_receive_accept(a->state, ar);
	if(ar->ballot == rec->ballot){/*�ѽ��������鰸�������е�����(proposer��learner)����ack,*/
		for(i = 0; i < carray_count(bevs); i++)
			sendbuf_add_accept_ack(carray_at(&bevs, i), rec);
	}
	else{/*Ϊ�������飬����nack��propose���������µ����᰸*/
		sendbuf_add_accept_ack(bev, rec);
	}
	
	acceptor_free_record(a->state, rec);
}

/*����repeat reqs*/
static void handle_repeat_req(struct evacceptor* a, struct bufferevent* bev, iid_t iid)
{
	paxos_log_debug("Handling repeat for instance %d", iid);
	acceptor_record* rec = acceptor_receive_repeat(a->state, iid);
	if(rec != NULL){
		sendbuf_add_accept_ack(bev, rec); /*�ط�һ��accept_acks*/
		acceptor_free_record(a->state, rec);
	}
}

static void handle_req(struct bufferevent* bev, void* arg)
{
	paxos_msg msg;
	struct evbuffer* in;
	char* buffer = NULL;

	/*��Ϣͷ���*/
	struct evacceptor* a = (struct evacceptor *)arg;
	evbuffer_remove(in, &msg, sizeof(paxos_msg));
	
	/*��Ϣ����*/
	if(msg.data_size > 0){
		buffer = malloc(msg.data_size);
		assert(buffer != NULL);
		evbuffer_remove(in, buffer, msg.data_size);
	}

	/*��Ϣ����*/
	switch(msg.type){
	case prepare_reqs:
		handle_prepare_req(a, bev, (prepare_req *)buffer);
		break;

	case accept_reqs:
		handle_accept_req(a, bev, (accept_req *)buffer);
		break;

	case repeat_reqs:
		handle_repeat_req(a, bev, *(iid_t *)buffer)
		break;

	default:
		paxos_log_error("Unknow msg type %d not handled", msg.type);
	}

	if(buffer != NULL)
		free(buffer);
}

struct evacceptor* evacceptor_init(int id, const char* config, struct event_base* b)
{
	int port, acceptor_count;
	struct evacceptor* a = (struct evacceptor *)malloc(sizeof(struct evacceptor));
	
	/*��ȡ�����ļ���Ϣ,������һ��paxos_config����*/
	a->conf = evpaxos_config_read(config);
	if(a->conf == NULL){
		free(a);
		return NULL;
	}

	/*��ȡ�����˿�*/
	port = evpaxos_proposer_listen_port(a->conf, id);
	/*��ȡacceptor�ĸ���*/
	acceptor_count = evpaxos_acceptor_count(a->conf);
	/*�Ƿ�������Ϣ*/
	if(id < 0 || id > acceptor_count){
		paxos_log_error("Invalid acceptor id: %d.", id);
		paxos_log_error("Should be between 0 and %d", acceptor_count);
		return NULL;
	}

	a->acceptor_id = id;
	a->base = b;
	/*����һ��tcp recevier�������ö�Ӧ����Ϣ�ص�*/
	a->receiver = tcp_receiver_new(b, port, handle_req, a); 
	/*����һ��accept��Ϣ������*/
	a->state = acceptor_new(id); 

	return a;
}

int evacceptor_free(struct evacceptor* a)
{
	if(a != NULL){
		if(a->state != NULL)
			acceptor_free(a->state);

		if(a->receiver != NULL)
			tcp_receiver_free(a->receiver);

		if(a->conf != NULL)
			evpaxos_config_free(a->conf);
	}
}



