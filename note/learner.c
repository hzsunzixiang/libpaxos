#include "learner.h"
#include "khash.h"
#include <stdlib.h>
#include <assert.h>

/*һ�������ʵ������*/
struct instance
{
	iid_t			iid;					/*�᰸��ţ�ȫ��Ψһ*/
	ballot_t		last_update_ballot;
	accept_ack**	acks;					/*���е�ack����*/
	accept_ack*		final_value;			/*�����ͨ����ack����*/
};

/*һ��instance��HASH MAP*/
KHASH_MAP_INIT_INT(instance, struct instance*);

struct learner
{
	int				acceptors;				/*ȷ��acceptor�ĸ���*/
	int				late_start;				/*�Ƿ���Ҫͬ��״̬��Ϣ��iid��*/
	iid_t			current_iid;			/*��ǰ����ľ���ID*/
	iid_t			highest_iid_closed;		/*������Ϊ�����ͨ���������ţ�iid��*/
	khash_t(instance)* instances;
}; 

static struct instance* learner_get_instance(struct learner* l, iid_t iid);
static struct instance* learner_get_current_instance(struct learner* l);
static struct instance* learner_get_instance_or_create(struct learner* l, iid_t iid);

static void				learner_delete_instance(struct learner* l, struct instance* inst);

static struct instance* instance_new(int acceptors);
static void				instance_free(struct instance* i, int acceptors);

static void				instance_update(struct instance* i, accept_ack* ack, int acceptors);
static int				instance_has_quorum(struct instance* i, int acceptors);
static void				instance_add_accept(struct instance* i, accept_ack* ack);

static accept_ack*		accept_ack_dup(accept_ack* ack);

struct learner* learner_new(int acceptors)
{
	struct learner* l = (struct learner*)malloc(sizeof(struct learner));
	l->acceptors = acceptors;
	l->current_iid = 1;
	l->highest_iid_closed = 1;
	l->late_start = !paxos_config.learner_catch_up;
	l->instances = kh_init(instance);

	return l;
}

void learner_free(struct learner* l)
{
	struct instance* inst;
	/*ɾ�����е�ʵ��*/
	kh_foreach_value(l->instances, inst, instance_free(inst, l->acceptors));
	/*�ͷ�hash map*/
	kh_destroy(instance, l->instances);
	free(l);
}

/*����һ������acceptor��accept ack�¼�*/
void learner_recevie_accept(struct learner* l, accept_ack* ack)
{
	/*����ǵ�һ��accept ack,����Ҫ����������Ϣ������Ϊ��ʼֵ,�൱���������ν�*/
	if(l->late_start){
		l->late_start = 0;
		l->current_iid = ack->iid;
	}

	/*�Ѿ�������᰸��ţ�����̫�ɵ��᰸*/
	if(ack->iid < l->current_iid){
		paxos_log_debug("Dropped accept_ack for iid %u. Already delivered.", ack->iid);
		return ;
	}

	/*ͨ��ack iid���һ��߹���instance,���û���ҵ��ͻṹ��һ��*/
	struct instance* inst = learner_get_instance_or_create(l, ack->iid);

	/*���ܵ���ack�¼���״̬���뵽instance����*/
	instance_update(inst, ack, l->acceptors);

	/*�Ѿ��Ǵ����������ʵ��iid�����Ѿ����أ���Ϊ��ͨ������������iid,����highest iid��ֵ*/
	if(instance_has_quorum(inst, l->acceptors) && (inst->iid > l->highest_iid_closed)){
		l->highest_iid_closed = inst->iid;
	}
}

/*����һ���᰸*/
accept_ack* learner_deliver_next(struct learner* l)
{
	struct instance* inst = learner_get_current_instance(l);
	if(inst == NULL)
		return NULL;

	/*�᰸�������ͨ��,�����᰸����*/
	if(instance_has_quorum(inst, l->acceptors)){
		/*����һ��accept ack��Ϊ����ͨ����Ϣ��*/
		accept_ack* ack = accept_ack_dup(inst->final_value);
		/*ɾ����������᰸ʵ��*/
		learner_delete_instance(l, inst);

		l->current_iid ++;/*++,Ԥ����һ��������᰸���*/
	}
}

int learner_has_holes(struct learner* l, iid_t* f, iid_t* from, iid_t* to)
{
	/*��ǰ���᰸��ž�l->highest_iid_closed���λ��,�м�ȫ��δ���ͨ����iid*/
	if(l->highest_iid_closed > l->current_iid){
		*from = l->current_iid;
		*to = l->highest_iid_closed;

		return 1;
	}

	return 0;
}

/*ͨ��iid�����᰸ʵ��*/
static struct instance* learner_get_instance(struct learner* l, iid_t iid)
{
	khiter_t k = kh_get_instance(l->instances, iid);
	if(k != kh_end(l->instances)){
		return kh_value(l->instances, k);
	}

	return NULL;
}

static struct instance* learner_get_current_instance(struct learner* l)
{
	return learner_get_instance(l, l->current_iid);
}

static struct instance* learner_get_instance_or_create(struct learner* l, iid_t iid)
{
	struct instance* inst = learner_get_instance(l, iid);
	if (inst == NULL) { /*û���ҵ�instance,����һ���µ�instance*/
		int rv;
		khiter_t k = kh_put_instance(l->instances, iid, &rv);
		assert(rv != -1);

		inst = instance_new(l->acceptors);
		kh_value(l->instances, k) = inst;
	}

	return inst;
}

static void learner_delete_instance(struct learner* l, struct instance* inst)
{
	khiter_t k;
	k = kh_get_instance(l->instances, inst->iid);
	kh_del_instance(l->instances, k);

	instance_free(inst, l->acceptors);
}

static struct instance* instance_new(int acceptors)
{
	int i;
	struct instance* inst;
	inst = (struct instance*)malloc(sizeof(struct instance));
	memset(inst, 0, sizeof(struct instance));

	/*������acceptor����һ�µ�acks���飬��Ϊһ��acceptorֻ��ͨ��һ���᰸��������һһ��Ӧ��*/
	inst->acks = (accept_ack**)malloc(sizeof(accept_ack*) * acceptors);
	for (i = 0; i < acceptors; ++i)
		inst->acks[i] = NULL;

	return inst;
}

static void instance_free(struct instance* inst, int acceptors)
{
	int i;
	for (i = 0; i < acceptors; i++){
		if (inst->acks[i] != NULL)
			free(inst->acks[i]);
	}

	free(inst->acks);
	free(inst);
}

static void instance_update(struct instance* inst, accept_ack* ack, int acceptors)
{
	if(inst->iid == 0){ /*δ��ֵ��instance����һ��ack���ͽ��и�ֵ*/
		paxos_log_debug("Received first message for iid: %u", ack->iid);
		inst->iid = ack->iid;
		inst->last_update_ballot = ack->ballot;
	}

	/*������Ѿ�ͨ�������Բ��������������ܻ��¼��ظ�*/
	if(instance_has_quorum(inst, acceptors)){
		paxos_log_debug("Dropped accept_ack iid %u. Already closed.", ack->iid);
		return;
	}

	/*�ж�ack�Ƿ�����*/
	accept_ack* prev_ack = inst->acks[ack->acceptor_id];
	if(prev_ack != NULL && prev_ack->ballot >= ack->ballot){
		paxos_log_debug("Dropped accept_ack for iid %u. Previous ballot is newer or equal.", ack->iid);
		return;
	}
	
	instance_add_accept(inst, ack);
}

static int instance_has_quorum(struct instance* inst, int acceptors)
{
	accept_ack* curr_ack;
	int i, a_valid_index = -1, count = 0;

	/*�Ѿ���ɴ�������ܣ����ҽ��������ack���ݼ�¼��final_value*/
	if(inst->final_value != NULL)
		return 1;

	for(i = 0; i < acceptors; i++){
		curr_ack = inst->acks[i];
		if(curr_ack == NULL)
			continue;

		if (curr_ack->ballot == inst->last_update_ballot) { /*ͬ��ľ���ID*/
			count ++;
			a_valid_index = i;

			/*��acceptor�ϼ�¼�Ѿ������ͨ����ֱ�ӱ�ʶΪ�����ͨ��״̬*/
			if (curr_ack->is_final){
				count += acceptors;
				break;
			}
		}
	}

	/*�ж��Ƿ��Ǵ����ͨ��������Ǳ�ʶͨ����ֵ*/
	if(count >= paxos_quorum(acceptors)){
		paxos_log_debug("Reached quorum, iid: %u is closed!", inst->iid);
		inst->final_value = inst->acks[a_valid_index];
		return 1;
	}

	return 0;
}

static void instance_add_accept(struct instance* inst, accept_ack* ack)
{
	if (inst->acks[ack->acceptor_id] != NULL){ /*�Ѿ�������ack*/
		free(inst->acks[ack->acceptor_id]); /*�ͷŵ��ɵ�*/
	}

	/*�滻�����µ�*/
	inst->acks[ack->acceptor_id] = accept_ack_dup(ack);
	inst->last_update_ballot = ack->ballot;
}

/*����һ��accept ack���󲢶�ack���и���*/
static accept_ack* accept_ack_dup(accept_ack* ack)
{
	accept_ack* copy = (accept_ack *)malloc(ACCEPT_ACK_SIZE(ack));
	memcpy(copy, ack, ACCEPT_ACK_SIZE(ack));

	return copy;
}
