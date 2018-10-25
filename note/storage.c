#include "storage.h"
#include <db.h> /*Berkeley DB*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>


struct storage
{
	DB*		db;
	DB_ENV*	env;
	DB_TXN*	txn;
	int		acceptor_id;
};


static int bdb_init_tx_handle(struct storage* s, char* db_env_path)
{
	int result,	flags;
	DB_ENV* dbenv;

	/*����һ��DB�Ļ�������*/
	result = db_env_create(&dbenv, 0);
	if(result != 0){
		paxos_log_error("DB_ENV creation failed: %s", db_strerror(result));
		return -1;
	}

	/*����д��ʽ���첽д*/
	if (!paxos_config.bdb_sync){
		result = dbenv->set_flags(dbenv, DB_TXN_WRITE_NOSYNC, 1);

		if(result != 0) {
			paxos_log_error("DB_ENV set_flags failed: %s", db_strerror(result));
			return -1;
		}
	}
	/*�ض���������*/
	dbenv->set_errfile(dbenv, stdout);

	/*�����ڴ滺�����Ĵ�С*/
	result = dbenv->set_cachesize(dbenv, 0, paxos_config.bdb_cachesize, 1);
	if (result != 0){
		paxos_log_error("DB_ENV set_cachesize failed: %s", db_strerror(result));
		return -1;
	}

	flags =
		DB_CREATE       |  /* Create if not existing */ 
		DB_RECOVER      |  /* Run normal recovery. */
		DB_INIT_LOCK    |  /* Initialize the locking subsystem */
		DB_INIT_LOG     |  /* Initialize the logging subsystem */
		DB_INIT_TXN     |  /* Initialize the transactional subsystem. */
		DB_THREAD       |  /* Cause the environment to be free-threaded */  
		DB_REGISTER 	|
		DB_INIT_MPOOL;     /* Initialize the memory pool (in-memory cache) */

	/*��DB�Ļ�������*/
	result = dbenv->open(dbenv, db_env_path, flags, 0);                    
	if (result != 0) {
		paxos_log_error("DB_ENV open failed: %s", db_strerror(result));
		return -1;
	}

	paxos_log_info("Berkeley DB storage opened successfully");

	s->env = dbenv;

	return 0;
}

static int bdb_init_db(struct storage* s, char* db_path)
{
	int result, flags;
	DB* dbp;

	/*����һ��DB�ļ�*/
	result = db_create(&(s->db), s->env, 0);
	if(result != 0){
		paxos_log_error("Berkeley DB storage call to db_create failed: %s", db_strerror(result));
		return -1;
	}

	dbp = s->db;
	flags = DB_CREATE;
	/*�����ݿ��ļ�*/
	storage_tx_begin(s);
	result = dbp->open(dbp, s->txn, db_path, NULL, DB_BTREE, flags, 0);
	storage_tx_commit(s);

	if(result != 0){
		paxos_log_error("Berkeley DB storage open failed: %s", db_strerror(result));
		return -1;
	}

	return result;
}

struct storage* storage_open(int acceptor_id)
{
	char* db_env_path;
	struct stat sb;
	struct storage* s;

	s = malloc(sizeof(struct storage));
	memset(s, 0, sizeof(struct storage));

	s->acceptor_id = acceptor_id;

	/*����һ���ļ�·��*/
	asprintf(&db_env_path, "%s_%d", paxos_config.bdb_env_path, acceptor_id);
	char* db_filename = paxos_config.bdb_db_filename;

	/*�ж�·���ļ��Ƿ����,��������ڣ�����һ��·��*/
	int dir_exists = (stat(db_env_path, &sb) == 0);
	if (!dir_exists && (mkdir(db_env_path, S_IRWXU) != 0)) {
		paxos_log_error("Failed to create env dir %s: %s", db_env_path, strerror(errno));
		free(s);
		return NULL;
	}
	/*���������ж��Ƿ���Ҫɾ�����ݿ��ļ���һ������Ҫ���¿�ʼ�����������*/
	if (paxos_config.bdb_trash_files && dir_exists) {
		char rm_command[600];
		sprintf(rm_command, "rm -r %s", db_env_path);

		if ((system(rm_command) != 0) || (mkdir(db_env_path, S_IRWXU) != 0))
				paxos_log_error("Failed to recreate empty env dir %s: %s", db_env_path, strerror(errno));
	}

	/*��ʼ��BDB�Ļ���*/
	char * db_file = db_filename;
	int ret = bdb_init_tx_handle(s, db_env_path);
	if(ret != 0)
		paxos_log_error("Failed to open DB handle");

	/*��ʼ�����ݿ��ļ�,�������ݿ�*/
	if (bdb_init_db(s, db_file) != 0) {
		paxos_log_error("Failed to open DB file");
		free(s);
		return NULL;
	}

	free(db_env_path);
}

int storage_close(struct storage* s)
{
	int result = 0;
	if(s == NULL)
		return ;

	if (s->db->close(s->db, 0) != 0) {
		paxos_log_error("DB_ENV close failed");
		result = -1;
	}

	if (s->env->close(s->env, 0) != 0) {
		paxos_log_error("DB close failed");
		result = -1;
	}
	
	paxos_log_info("Berkeley DB storage closed successfully");

	free(s);

	return result;
}

void storage_tx_begin(struct storage* s)
{
	if(s != NULL){
		s->env->txn_begin(s->env, NULL, &s->txn, 0);
	}
}

void storage_tx_commit(struct storage* s)
{
	if(s != NULL){
		s->txn->commit(s->txn, 0);
	}
}

void storage_free_record(struct storage* s, acceptor_record* r)
{
	if(s != NULL && r != NULL)
		free(r);
}

acceptor_record* storage_get_record(struct storage* s, iid_t iid)
{
	int flags, result;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;
	acceptor_record* record_buffer = NULL;

	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	dbkey.data = &iid;
	dkbkey.size = sizeof(iid_t);

	dbdata.flags = DB_DBT_MALLOC;

	flags = 0;
	result = dbp->get(dbp, txn, &dbkey, &dbdata, flags);
	if(result == DB_NOTFOUND || result == DB_KEYEMPTY){/*û�ҵ���Ӧ�ļ�¼*/
		paxos_log_debug("The record for iid: %d does not exist", iid);
		return NULL;
	}else if(result != 0){ /*����ʧ��*/
		paxos_log_error("Error while reading record with iid%u : %s", iid, db_strerror(result));
		return NULL;
	}

	record_buffer = (acceptor_record*) dbdata.data;
	assert(record_buffer != NULL);
	assert(iid == record_buffer->iid);

	return record_buffer;
}

acceptor_record* storage_save_accept(struct storage* s, accept_req* ar)
{
	int flags;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;
	acceptor_record* record_buffer = malloc(ACCEPT_RECORD_BUFF_SIZE(ar->value_size));
	assert(record_buffer != NULL);

	/*��accept_ack��ֵ���б���*/
	record_buffer->acceptor_id = s->acceptor_id;
	record_buffer->iid = ar->iid;
	record_buffer->ballot = ar->ballot;
	record_buffer->value_ballot = ar->ballot;
	record_buffer->is_final = 0;
	record_buffer->value_size = ar->value_size;
	memcpy(record_buffer->value, ar->value, ar->value_size);

	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	/*Key is iid*/
	dbkey.data = &ar->iid;
	dbkey.size = sizeof(iid_t);

	/*data*/
	dbdata.data = record_buffer;
	dbdata.size = ACCEPT_ACK_SIZE(record_buffer);

	return dbp->put(dbp, txn, &dbkey, &dbdata, 0);
}

acceptor_record* storage_save_prepare(struct storage* s, prepare_req * pr)
{
	int flags, result;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;

	acceptor_record* record_buffer = storage_get_record(s, pr->iid);
	if(record_buffer == NULL){
		record_buffer = malloc(ACCEPT_RECORD_BUFF_SIZE(0));
		assert(record_buffer != NULL);

		record_buffer->acceptor_id = s->acceptor_id;
		record_buffer->iid = pr->iid;
		record_buffer->value_ballot = 0;
		record_buffer->is_final = 0;
		record_buffer->value_size = 0;
	}
	/*����ͶƱID*/
	record_buffer->ballot = pr->ballot;

	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	dbkey.data = &pr->iid;
	dbkey.size = sizeof(iid_t);

	dbdata.data = record_buffer;
	dbdata.size = ACCEPT_RECORD_BUFF_SIZE(record_buffer->value_size);
	/*д�����ݿ�*/
	result = dbp->put(dbp, txn, &dbkey, &dbdata, 0);
	assert(result);

	return record_buffer;
}

acceptor_record* storage_save_final_value(struct storage* s, char* value, size_t size, iid_t iid, ballot_t b)
{
	int flags, result;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;
	acceptor_record* record_buffer = malloc(ACCEPT_RECORD_BUFF_SIZE(size));

	record_buffer->iid = iid;
	record_buffer->ballot = b;
	record_buffer->value_ballot = b;
	record_buffer->is_final = 1;
	record_buffer->value_size = size;
	memcpy(record_buffer->value, value, size);

	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	dbkey.data = &iid;
	dbkey.size = sizeof(iid_t);

	dbdata.data = record_buffer;
	dbdata.size = ACCEPT_ACK_SIZE(record_buffer);
	result = dbp->put(dbp, txn, &dbkey, &dbdata, 0);
	assert(result);

	return record_buffer;
}

iid_t storage_get_max_iid(struct storage * s)
{
	int ret;
	DB *dbp = s->db;
	DBC *dbcp;
	DBT key, data;
	iid_t max_iid = 0;

	/*��һ���α�*/
	if ((ret = dbp->cursor(dbp, NULL, &dbcp, 0)) != 0) {
		dbp->err(dbp, ret, "DB->cursor");
		return (1);
	}

	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	while((ret = dbcp->c_get(dbcp, &key, &data, DB_NEXT)) == 0){
		max_iid = *(iid_t *)key.data;
	}

	/*c_getʧ��*/
	if(ret != DB_NOTFOUND){
		dbp->error(dbp, ret, "DBcursor->get");
		max_iid = 0;
	}

	/*�ر��α�*/
	if ((ret = dbcp->c_close(dbcp)) != 0){
		dbp->err(dbp, ret, "DBcursor->close");
	}

	return max_iid;
}

