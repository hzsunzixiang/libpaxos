#include "evpaxos.h"
#include "peers.h"
#include "config.h"
#include "libpaxos_message.h"
#include "tcp_sendbuf.h"
#include "tcp_receiver.h"
#include "proposer.h"

#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>

struct evproposer
{
	int						id;				/*proposer id*/
	int						preexec_window;	/*ͬʱ���Է�������ĸ���,���޵�һ�׶�*/
	struct tcp_receiver*	receiver;		/*tcp��Ϣ������*/
	struct event_base*		base;			/*libevent base*/
	struct proposer*		state;			/*proposer ��Ϣ������*/
	struct peers*			acceptors;		/*acceptor���ӽڵ������*/
	struct timval			tv;				/*��ʱʱ��*/
	struct event*			timeout_ev;		/*��ʱʱ����*/
};

/*����prepare_req�����е�acceptor���е�һ�׶ε�����*/
static void send_prepares(struct evproposer* p, prepare_req* pr)
{
	int i;
	for(i = 0; i < peers_count(p->acceptors); i ++){
		struct bufferevent* bev = peers_get_buffer(p->acceptors, i);
		sendbuf_add_prepare_req(bev, pr);
	}
}

/*����accept_req�����е�acceptor���еڶ��׶ε�����*/
static void send_accepts(struct evproposer* p, accept_req* ar)
{
	int i;
	for(i = 0; i < peers_count(p->acceptors); i++){
		struct bufferevent* bev = peers_get_buffer(p->acceptors, i);
		sendbuf_add_accept_req(bev, ar);
	}
}

static void proposer_preexecute(struct evproposer* p)
{
	int i;
	prepare_req pr;

	/*��ÿ��Է����᰸�ĸ���*/
	int count = p->preexec_window - proposer_prepared_count(p->state);
	for(i = 0; i < count; i ++){
		 /*����һ��prepare_req��Ϣ*/
		proposer_prepare(p->state, &pr);
		/*����һ���᰸*/
		send_prepares(p, &pr);
	}
}

static void try_accept(struct evproposer* p)
{
	accept_req* ar;
	while((ar = proposer_accept(p->state)) != NULL){ /*��ȡһ������˵�һ�׶ε����飬�������乹��һ��accept_reqs*/
		/*��������ĵڶ��׶�*/
		send_accepts(p, ar);
		free(ar);
	}

	/*����Ƿ���Է��͸���ĵ�һ�׶ε�����*/
	proposer_preexecute(p);
}

/*proposer��prepare ack�Ĵ������Ӧ*/
static void proposer_handle_prepare_ack(struct evproposer* p, prepare_ack* ack)
{
	prepare_req pr;
	if(proposer_receive_prepare_ack(p->state, ack, &pr)) /*��prepare ack����Ӧ�����жϷ���ֵ�Ƿ�����Ҫ���·����һ�׶�*/
		send_prepares(p, &pr);
}

/*proposer��accept ack�Ĵ������Ӧ*/
static void proposer_handle_accept_ack(struct evproposer* p, accept_ack* ack)
{
	prepare_req pr;
	if (proposer_receive_accept_ack(p->state, ack, &pr))/*��accept ack����Ӧ�����жϷ���ֵ�Ƿ�����Ҫ���·����һ�׶�*/
		send_prepares(p, &pr);
}

/*�������Կͻ��˵���Ϣ������Ϣת��Ϊ�ȴ����������*/
static void proposer_handle_client_msg(struct evproposer* p, char* value, int size)
{
	proposer_propose(p->state, value, size);
}

/*proposer����������Ϣ�ӿ�*/
static void proposer_handle_msg(struct evproposer* p, struct bufferevent* bev)
{
	paxos_msg msg;
	struct evbuffer* in;
	char* buffer = NULL;

	/*�����Ϣͷ*/
	in = bufferevent_get_input(bev);
	evbuffer_remove(in, &msg, sizeof(paxos_msg));

	/*�����Ϣ��*/
	if (msg.data_size > 0) {
		buffer = malloc(msg.data_size);
		evbuffer_remove(in, buffer, msg.data_size);
	}

	/*������Ϣ*/
	switch (msg.type){
	case prepare_acks:
		proposer_handle_prepare_ack(p, (prepare_ack*)buffer);
		break;
	case accept_acks:
		proposer_handle_accept_ack(p, (accept_ack*)buffer);
		break;
	case submit:
		proposer_handle_client_msg(p, buffer, msg.data_size);
		break;
	default:
		paxos_log_error("Unknow msg type %d not handled", msg.type);
		return;
	}

	/*���Է�������ĵڶ��׶�,�׶��Լ��*/
	try_accept(p);

	if (buffer != NULL)
		free(buffer);
}

static void handle_request(struct bufferevent* bev, void* arg)
{
	size_t len;
	paxos_msg msg;
	struct evproposer* p = (struct evproposer*)arg;

	struct evbuffer* in = bufferevent_get_input(bev);

	/*��ȡ��������Ϣ��ѭ����ȡ����ֹճ��*/
	while ((len = evbuffer_get_length(in)) > sizeof(paxos_msg)){
		evbuffer_copyout(in, &msg, sizeof(paxos_msg));
		
		if(len >= PAXOS_MSG_SIZE((&msg))){
			proposer_handle_msg(p, bev);
		}
	}
}

/*��鳬ʱ���᰸��������������*/
static void proposer_check_timeouts(evutil_socket_t fd, short event, void* arg)
{
	struct evproposer* p = arg;
	struct timeout_iterator* iter = proposer_timeout_iterator(p->state);

	/*��һ���׶γ�ʱ�᰸*/
	prepare_req* pr;
	while((pr == timeout_iterator_prepare(iter)) != NULL){ /*��ȡ��ʱ���᰸(��һ�׶�)*/
		paxos_log_info("Instance %d timed out.", pr->iid);
		/*�Գ�ʱ�᰸���·����������*/
		send_prepares(p, pr);
		free(pr);
	}
	
	accept_req* ar;
	while((ar = timeout_iterator_accept(iter)) != NULL){ /*��ó�ʱ�᰸(�ڶ��׶�)*/
		paxos_log_info("Instance %d timed out.", ar->iid);
		send_accepts(p, ar);
		free(ar);
	}

	/*�ͷų�ʱ����ĵ�����*/
	timeout_iterator_free(iter);
	/*����һ����ʱ��*/
	event_add(p->timeout_ev, &p->tv);
}

/*����һ��proposer���󣬲�������*/
struct evproposer* evproposer_init(int id, const char* config, struct event_base* b)
{
	int port, acceptor_count;
	struct evproposer* p;

	/*��ȡ�����ļ�*/
	struct evpaxos_config* conf = evpaxos_config_read(config);
	if(conf == NULL)
		return NULL;

	/*�Ƿ���proposer id*/
	if (id < 0 || id >= MAX_N_OF_PROPOSERS) {
		paxos_log_error("Invalid proposer id: %d", id);
		return NULL;
	}

	/*��ȡproposer�ļ����˿�*/
	port = evpaxos_proposer_listen_port(conf, id);
	/*��ȡacceptor������*/
	acceptor_count = evpaxos_acceptor_count(conf);

	p = (struct evproposer *)malloc(sizeof(struct evproposer));
	p->id = id;
	p->base = b;

	/*���ͬʱ�ύ���鰸����*/
	p->preexec_window = paxos_config.proposer_preexec_window;
	
	/*����һ��������Ϣ������*/
	p->receiver = tcp_receiver_new(b, port, handle_request, p);
	
	/*����һ��acceptor�Ĺ�����*/
	p->acceptors = peers_new(b);
	/*��ÿ��acceptor��������*/
	peers_connect_to_acceptors(p->acceptors, conf, handle_request, p);
	
	/*���ö�ʱ��*/
	p->tv.tv_sec = paxos_config.proposer_timeout;
	p->tv.tv_usec = 0;
	/*����һ��libevent��ʱ���¼�����,������һ����ʱ��*/
	p->timeout_ev = evtimer_new(b, proposer_check_timeouts, p);
	event_add(p->timeout_ev, &p->tv);

	/*����һ��proposer ��Ϣ������*/
	p->state = proposer_new(p->id, acceptor_count);

	/*��̽��ִ��prepare����(�᰸��һ�׶�)*/
	proposer_preexecute(p);

	evpaxos_config_free(conf);

	return p;
}

/*�ͷ�evproposer����*/
void proposer_free(struct evproposer* p)
{
	if(p != NULL){
		if(p->state != NULL)
			proposer_free(p->state);

		if(p->acceptors != NULL)
			peers_free(p->acceptors);

		if(p->receiver != NULL)
			tcp_receiver_free(p->receiver);

		free(p);
	}
}







