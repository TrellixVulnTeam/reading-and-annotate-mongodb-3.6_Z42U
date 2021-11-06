/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <map>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry_base.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class KVCatalog;
class KVEngine;

//WiredTigerFactory::create�л�ʹ�øýṹ
struct KVStorageEngineOptions {
    bool directoryPerDB = false;
    bool directoryForIndexes = false;
    bool forRepair = false;
};

/*
 * The actual definition for this function is in
 * `src/mongo/db/storage/kv/kv_database_catalog_entry.cpp` This unusual forward declaration is to
 * facilitate better linker error messages.  Tests need to pass a mock construction factory, whereas
 * main implementations should pass the `default...` factory which is linked in with the main
 * `KVDatabaseCatalogEntry` code.
 */
//Kv_database_catalog_entry.cpp��ʵ��
std::unique_ptr<KVDatabaseCatalogEntryBase> defaultDatabaseCatalogEntryFactory(
    const StringData name, KVStorageEngine* const engine);

using KVDatabaseCatalogEntryFactory = decltype(defaultDatabaseCatalogEntryFactory);


/*
DatabaseHolder
DatabaseHolder��Mongodb���ݿ��������ڣ��ṩ�˴򿪡��ر����ݿ�Ľӿڣ�����openDb�ӿڻᴴ��һ��Database����

C#

class DatabaseHoler {
public:
Database* openDb(string dbname);
void close(string dbname);
Database* get(string dbname);
pirate:
map<string, Database*> dbs;
};

class DatabaseHoler {
public:
Database* openDb(string dbname);
void close(string dbname);
Database* get(string dbname);
pirate:
map<string, Database*> dbs;
};
Database
Database�������Mongodb���һ��db�����ṩ���ڼ��ϲ��������нӿڣ�����������ɾ�������������ϣ�����
Databaseʱ�����mongod���̵�storageEngine����������ʹ���ĸ��洢���档

C#

class Database {
public:
Collection* createCollection(string& coll_name);
void dropCollection(string& coll_name);
Collection* getCollection(string& coll_name);
private:
map<string, Collection*> _collections;
};

class Database {
public:
Collection* createCollection(string& coll_name);
void dropCollection(string& coll_name);
Collection* getCollection(string& coll_name);
private:
map<string, Collection*> _collections;
};
Collection
Collection����Mongodb���һ�����ϣ����ṩ�����ĵ���ɾ�Ĳ�����нӿڣ���Щ�ӿ����ջ����RecordStore��
����Ӧ�ӿ�ʵ�֡�

C#

class Collection {
public:
insertDocument();
deleteDocument();
updateDocument();
findDoc();
private:
RecordStore* _recordStore;
};


class Collection {
public:
insertDocument();
deleteDocument();
updateDocument();
findDoc();
private:
RecordStore* _recordStore;
};
GlobalEnvironmentMongoD
GlobalEnvironmentMongoD��mongod��ȫ�����л�����Ϣ�����еĴ洢����������ʱ����ע�ᣬmongd������������
��ǰʹ�õĴ洢����; ע������ʱ���ṩ��������֣���mmapv1��wiredTiger�������ڴ����������Ĺ�������
������ʵ��create�Ľӿڣ����ڴ���StorageEngine���󣩡�

class GlobalEnvironmentMongoD {
pubic:
void registerStorageEngine(const std::string& name,
const StorageEngine::Factory* factory);
void setGlobalStorageEngine(const std::string& name);
StorageEngine* getGlobalStorageEngine();
private:
StorageEngine* _storageEngine; // ��ǰ�洢����
FactoryMap _storageFactories;
}��

class GlobalEnvironmentMongoD {
pubic:
void registerStorageEngine(const std::string& name,
const StorageEngine::Factory* factory);
void setGlobalStorageEngine(const std::string& name);
StorageEngine* getGlobalStorageEngine();
private:
StorageEngine* _storageEngine; // ��ǰ�洢����
FactoryMap _storageFactories;
}��
StorageEngine
StorageEngine������һϵ��Mongdb�洢������Ҫʵ�ֵĽӿڣ���һ���ӿ��࣬���еĴ洢������̳�����࣬ʵ��
����Ĵ洢�߼��� getDatabaseCatalogEntry�ӿ����ڻ�ȡһ��DatabaseCatalogEntry���󣬸ö���ʵ���˹��ڼ��ϡ�
�ĵ������Ľӿڡ�

C#

class StorageEngine {
    public:
    DatabaseCatalogEntry* getDatabaseCatalogEntry(string& ns);
    void listDatabases( std::vector<std::string>* out );
};

class DatabaseCatalogEntry {
    public:
    createCollection();
    dropCollection();
    getRecordStore(); / * ʵ���ĵ������ӿ� * /
};

class StorageEngine {
public:
DatabaseCatalogEntry* getDatabaseCatalogEntry(string& ns);
void listDatabases( std::vector<std::string>* out );
};
 
class DatabaseCatalogEntry {
public:
createCollection();
dropCollection();
getRecordStore(); / * ʵ���ĵ������ӿ� * /
};
MMAPV1StorageEngine
MMAPV1StorageEngine������mmapv1�洢���������ʵ���߼���

KVStorageEngine
KVStorageEngineʵ���ϲ���һ�������洢�����ʵ�֣�ֻ��Ϊ�˷������wiredTiger��rocks��KV���͵Ĵ洢����
�����ӵ�һ������㡣 KVStorageEngineʵ����StorageEngine�Ľӿڣ�����ʵ����KVEngine�����wiredTiger��
KV�洢�������mongdbʱ��ֻ��ʵ��KVEngine����Ľӿڼ��ɡ�

WiredTigerKVEngine
WiredTigerKVEngine�̳�KVEngine��ʵ��KVEngine�Ľӿڣ�������������RocksEngine���ơ�

��http://blog.jobbole.com/89351/
*/

/*
KVStorageEngine
KVStorageEngineʵ���ϲ���һ�������洢�����ʵ�֣�ֻ��Ϊ�˷������wiredTiger��rocks��KV���͵Ĵ洢���������
��һ������㡣 KVStorageEngineʵ����StorageEngine�Ľӿڣ�����ʵ����KVEngine�����wiredTiger��KV�洢����
����mongdbʱ��ֻ��ʵ��KVEngine����Ľӿڼ��ɡ�
*/

//KVEngine(WiredTigerKVEngine)��StorageEngine(KVStorageEngine)�Ĺ�ϵ: KVStorageEngine._engine����ΪWiredTigerKVEngine
//Ҳ����KVStorageEngine�������WiredTigerKVEngine���Ա


//KVDatabaseCatalogEntryBase._engineΪ������

//WiredTigerFactory::create��new����, KVStorageEngineֻ���wiredtiger�洢���棬���������֧��rocksdb��Ҳ���Բο�WTģ�黯ʵ��ROCKSDB֧��
class KVStorageEngine final : public StorageEngine { 
public:
    /**
     * @param engine - ownership passes to me
     */
    KVStorageEngine(KVEngine* engine,
                    const KVStorageEngineOptions& options = KVStorageEngineOptions(),
                    stdx::function<KVDatabaseCatalogEntryFactory> databaseCatalogEntryFactory =
                        defaultDatabaseCatalogEntryFactory);

    virtual ~KVStorageEngine();

    virtual void finishInit();

    virtual RecoveryUnit* newRecoveryUnit();

    virtual void listDatabases(std::vector<std::string>* out) const;

    KVDatabaseCatalogEntryBase* getDatabaseCatalogEntry(OperationContext* opCtx,
                                                        StringData db) override;

    virtual bool supportsDocLocking() const {
        return _supportsDocLocking;
    }

    virtual bool supportsDBLocking() const {
        return _supportsDBLocking;
    }

    virtual Status closeDatabase(OperationContext* opCtx, StringData db);

    virtual Status dropDatabase(OperationContext* opCtx, StringData db);

    virtual int flushAllFiles(OperationContext* opCtx, bool sync);

    virtual Status beginBackup(OperationContext* opCtx);

    virtual void endBackup(OperationContext* opCtx);

    virtual bool isDurable() const;

    virtual bool isEphemeral() const;

    virtual Status repairRecordStore(OperationContext* opCtx, const std::string& ns);

    virtual void cleanShutdown();

    virtual void setStableTimestamp(Timestamp stableTimestamp) override;

    virtual void setInitialDataTimestamp(Timestamp initialDataTimestamp) override;

    virtual void setOldestTimestamp(Timestamp oldestTimestamp) override;

    virtual bool supportsRecoverToStableTimestamp() const override;

    virtual void replicationBatchIsComplete() const override;

    SnapshotManager* getSnapshotManager() const final;

    void setJournalListener(JournalListener* jl) final;

    // ------ kv ------

    KVEngine* getEngine() {
        return _engine.get();
    }
    const KVEngine* getEngine() const {
        return _engine.get();
    }

    KVCatalog* getCatalog() {
        return _catalog.get();
    }
    const KVCatalog* getCatalog() const {
        return _catalog.get();
    }

    /**
     * Drop abandoned idents. Returns a parallel list of index name, index spec pairs to rebuild.
     */
    StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>> reconcileCatalogAndIdents(
        OperationContext* opCtx) override;

private:
    class RemoveDBChange;

    //KVStorageEngine::getDatabaseCatalogEntry��KVStorageEngine::KVStorageEngine�е��ø�factory
    //Ĭ��ΪdefaultDatabaseCatalogEntryFactory
    stdx::function<KVDatabaseCatalogEntryFactory> _databaseCatalogEntryFactory;

    KVStorageEngineOptions _options;

    // This must be the first member so it is destroyed last.
    //WiredTigerFactory::create->new KVStorageEngine(kv, options);�е��ø�ֵ
    
    std::unique_ptr<KVEngine> _engine; //WiredTigerKVEngine����

    const bool _supportsDocLocking;
    const bool _supportsDBLocking;

    //Ĭ�Ϸ���StandardWiredTigerRecordStore�࣬����̳�WiredTigerRecordStore
    //��Ӧ����Ŀ¼��"_mdb_catalog.wt"���洢�����Ԫ������Ϣ��������� ������Ϣ��
    std::unique_ptr<RecordStore> _catalogRecordStore; //ʵ���ϸ������_catalogʹ��
    //KVStorageEngine::KVStorageEngine�г�ʼ��
    //��Ӧ����Ŀ¼��"_mdb_catalog.wt"��ز���  _mdb_catalog.wt�洢Ԫ������Ϣ
    std::unique_ptr<KVCatalog> _catalog; 

    //���������ӦKVDatabaseCatalogEntryBase
    typedef std::map<std::string, KVDatabaseCatalogEntryBase*> DBMap;
    //DatabaseHolderImpl.dbs[]��KVStorageEngine._dbs[]��������ϵ��
    //1. DatabaseHolderImpl.dbs[]����ʵ��������ʹ�ù���������ʹ�õ�DB��Ϣ
    //2. KVStorageEngine._dbs[]��Ӧ��_mdb_catalog.wtԪ�����ļ����ص����п⼰��������Ԫ������Ϣ
    //3. mongodb�����󣬵�ͨ��db.xx.collection��ĳ���ĳ�������ʱ�򣬻�����һ��DatabaseImpl,Ȼ�����
    //   DatabaseImpl::init()�Ӹÿ��ӦKVDatabaseCatalogEntryBase�л�ȡ�ñ�Ԫ������Ϣ
    //4. KVStorageEngine._dbs[]�д����ȫ����Ԫ������Ϣ����DatabaseHolderImpl.dbs[]�д������ʵ��������
    //   ������Ϊֹ��ͨ��db.xx.collection.insert()��ʹ�ù��Ŀ���Ϣ��������������DB1��DB2����mongodʵ��������
    //   ����ֻ������DB1����KVStorageEngine._dbs[]������DB1��DB2��������Ϣ����DatabaseHolderImpl.dbs[]ֻ����DB1��Ϣ
    
    //��Դ��KVStorageEngine::KVStorageEngine����mongodʵ�������󣬻��Ԫ����_mdb_catalog.wt�л�ȡdb��Ϣ
    DBMap _dbs; 
    mutable stdx::mutex _dbsLock;

    // Flag variable that states if the storage engine is in backup mode.
    bool _inBackupMode = false;
};
}  // namespace mongo
