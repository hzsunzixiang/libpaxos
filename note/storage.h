#ifndef __STORAGE_H
#define __STORAGE_H

#include "paxos.h"
#include "libpaxos_message.h"

struct storage;

struct storage*		storage_open(int acceptor_id);
int					storage_close(struct storage* s);

void				storage_tx_begin(struct storage* s);
void				storage_tx_commit(struct storage* s);

void				storage_free_record(struct storage* s, acceptor_record* r);
acceptor_record*	storage_get_record(struct storage* s, iid_t iid);

acceptor_record*	storage_save_accept(struct storage* s, accept_req * ar);
acceptor_record*	storage_save_prepare(struct storage* s, prepare_req * pr);
acceptor_record*	storage_save_final_value(struct storage* s, char * value, size_t size, iid_t iid, ballot_t ballot);
iid_t				storage_get_max_iid(struct storage * s);

#endif

