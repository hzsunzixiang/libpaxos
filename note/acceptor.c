#include "acceptor.h"
#include "storage.h"

#include <stdlib.h>

struct acceptor
{
	struct storage* store;
};

static acceptor_record* apply_prepare(struct storage* s, prepare_req* ar, acceptor_record* rec);
static acceptor_record* apply_accept(struct storage* s, accept_req* ar, acceptor_record* rec);

struct acceptor* acceptor_new(int id)
{
	struct acceptor* s = (struct acceptor *)malloc(sizeof(struct acceptor));
	s->store = storage_open(id);/*�򿪹̻��洢ϵͳ��acceptor���ܵ���ϢӦ�ù̻�*/
	if(s->store == NULL){
		free(s);
		return NULL;
	}

	return s;
}

int acceptor_free(struct acceptor* a)
{
	int rv = 0;
	if(a && a->store){
		rv = storage_close(a->store);
		free(a);
	}

	return rv;
}

void acceptor_free_record(struct acceptor* a, acceptor_record* r)
{
	if(a && a->store && r)
		storage_free_record(a->store, r);
}

acceptor_record* acceptor_receive_prepare(struct acceptor* a, prepare_req* req)
{
	acceptor_record* rec;
	storage_tx_begin(a->store);
	rec = storage_get_record(a->store, req->iid);
	/*����prepare req����*/
	rec = apply_prepare(a->store, req, rec);
	storage_tx_commit(a->store);
	return rec;
}

acceptor_record* acceptor_receive_accept(struct acceptor* a, accept_req* req)
{
	acceptor_record* rec;
	storage_tx_begin(a->store);
	rec = storage_get_record(a->store, req->iid);
	/*����accept_req����*/
	rec = apply_accept(a->store, req, rec);
	storage_tx_commit(s->store);

	return rec;
}
acceptor_record* acceptor_receive_repeat(struct acceptor* a, iid_t iid)
{
	acceptor_record* rec;
	storage_tx_begin(a->store);
	rec = storage_get_record(a->store, iid);
	storage_tx_commit(a->store);
	return rec;
}
static acceptor_record* apply_prepare(struct storage* s, prepare_req* pr, acceptor_record* rec)
{
	/*������С�ڱ�acceptor�ѽ��ܵ��������ID�����飬���������ܵ�������Ϣ*/
	if(rec != NULL && rec->ballot >= pr->ballot){
		paxos_log_debug("Prepare iid: %u dropped (ballots curr:%u recv:%u)", pr->iid, rec->ballot, pr->ballot);
		return rec;
	}

	/*���������Ѿ�����ͨ�����������µ����飬����ͨ����������Ϣ*/
	if (rec != NULL && rec->is_final) {
		paxos_log_debug("Prepare request for iid: %u dropped (stored value is final)", pr->iid);
		return rec;
	}

	/*��������*/
	paxos_log_debug("Preparing iid: %u, ballot: %u", pr->iid, pr->ballot);
	if(rec != NULL)
		storage_free_record(s, rec);

	/*����һ���µ�������Ϣ*/
	return storage_save_prepare(s, pr);
}

static acceptor_record* apply_accept(struct storage* s, accept_req* ar, acceptor_record* rec)
{
	if (rec != NULL && rec->ballot > ar->ballot) { /*����ǿ��Եģ���Ϊ��ͬһ���᰸*/
		paxos_log_debug("Accept for iid:%u dropped (ballots curr:%u recv:%u)", ar->iid, rec->ballot, ar->ballot);
		return rec;
	}

	paxos_log_debug("Accepting iid: %u, ballot: %u", ar->iid, ar->ballot);

	/*�ͷžɵ�record*/
	if (rec != NULL)
		storage_free_record(s, rec);
	/*�����᰸����*/
	return storage_save_accept(s, ar);
}

