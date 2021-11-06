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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/kv/kv_storage_engine.h"

#include <algorithm>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::vector;

/*
_mdb_catalog.wt��洢�����м��ϵ�Ԫ���ݣ��������϶�Ӧ��WT table���֣����ϵĴ���ѡ����ϵ�������Ϣ�ȣ�
WT�洢�����ʼ��ʱ�����_mdb_catalog.wt���ȡ���еļ�����Ϣ��������Ԫ��Ϣ���ڴ档
��������WT table���Ķ�Ӧ��ϵ����ͨ��db.collection.stats()��ȡ

mongo-9552:PRIMARY> db.system.users.stats().wiredTiger.uri
statistics:table:admin/collection-10--1436312956560417970
Ҳ����ֱ��dump��_mdb_catalog.wt������ݲ鿴��dump��������ΪBSON��ʽ���Ķ��������Ǻܷ��㡣

wt -C "extensions=[/usr/local/lib/libwiredtiger_snappy.so]" -h . dump table:_mdb_catalog

*/
namespace {
//KVStorageEngine::KVStorageEngine�д�����Ӧ��_mdb_catalog.wtԪ�����ļ��������һ�δ�����Ⱥ��ʵ��������ʱ����Ҫ����
const std::string catalogInfo = "_mdb_catalog";
}

//KVEngine(WiredTigerKVEngine)��StorageEngine(KVStorageEngine)�Ĺ�ϵ: KVStorageEngine._engine����ΪWiredTigerKVEngine
//Ҳ����KVStorageEngine�������WiredTigerKVEngine���Ա

//KVStorageEngine._engineΪWiredTigerKVEngine��ͨ��KVStorageEngine._engine��WiredTigerKVEngine��������
class KVStorageEngine::RemoveDBChange : public RecoveryUnit::Change {
public:
	//ɾ�����ͨ�������¼����
    RemoveDBChange(KVStorageEngine* engine, StringData db, KVDatabaseCatalogEntryBase* entry)
        : _engine(engine), _db(db.toString()), _entry(entry) {}
	
    virtual void commit() {
        delete _entry;
    }

    virtual void rollback() {
        stdx::lock_guard<stdx::mutex> lk(_engine->_dbsLock);
        _engine->_dbs[_db] = _entry;
    }

    KVStorageEngine* const _engine;
    const std::string _db;
    KVDatabaseCatalogEntryBase* const _entry;
};

//wiredtiger��ӦWiredTigerKVEngine  KVStorageEngine._engineΪWiredTigerKVEngine

//mongodʵ��������������Ҫ����_mdb_catalog.wt�ļ���ȡԪ������Ϣ

//WiredTigerFactory::create��new���� 
KVStorageEngine::KVStorageEngine(
	//��ӦWiredTigerKVEngine
    KVEngine* engine,
    const KVStorageEngineOptions& options,
    //Ĭ��ΪdefaultDatabaseCatalogEntryFactory
    stdx::function<KVDatabaseCatalogEntryFactory> databaseCatalogEntryFactory)
    : _databaseCatalogEntryFactory(std::move(databaseCatalogEntryFactory)),
      _options(options),
      //WiredTigerKVEngine  KVStorageEngine._engineΪWiredTigerKVEngine
      _engine(engine), 
      //wiredtiger��֧�ֵģ��� WiredTigerKVEngine::supportsDocLocking
      _supportsDocLocking(_engine->supportsDocLocking()),
      _supportsDBLocking(_engine->supportsDBLocking()) {
    uassert(28601,
            "Storage engine does not support --directoryperdb",
            !(options.directoryPerDB && !engine->supportsDirectoryPerDB()));

    OperationContextNoop opCtx(_engine->newRecoveryUnit()); //WiredTigerKVEngine::newRecoveryUnit
	
    bool catalogExists = engine->hasIdent(&opCtx, catalogInfo);

    if (options.forRepair && catalogExists) {
        log() << "Repairing catalog metadata";
        // TODO should also validate all BSON in the catalog.
        engine->repairIdent(&opCtx, catalogInfo).transitional_ignore();
    }

    if (!catalogExists) {
        WriteUnitOfWork uow(&opCtx);

		//WiredTigerKVEngine::createGroupedRecordStore
		//_mdb_catalog.wtԪ�����ļ������ڣ��򴴽���Ӧ��_mdb_catalog.wtԪ�����ļ��������һ�δ�����Ⱥ��ʵ��������ʱ����Ҫ����
        Status status = _engine->createGroupedRecordStore(
        
            &opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);
        // BadValue is usually caused by invalid configuration string.
        // We still fassert() but without a stack trace.
        if (status.code() == ErrorCodes::BadValue) {
            fassertFailedNoTrace(28562);
        }
        fassert(28520, status);
        uow.commit();
    }

	//WiredTigerKVEngine::getGroupedRecordStore��Ĭ�Ϸ���StandardWiredTigerRecordStore��
    _catalogRecordStore = _engine->getGroupedRecordStore(
    //const std::string catalogInfo = "_mdb_catalog"; Ҳ���Ǹ�StandardWiredTigerRecordStore��Ӧ_mdb_catalog�ļ�
        &opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);
    _catalog.reset(new KVCatalog(
		//StandardWiredTigerRecordStore
        _catalogRecordStore.get(), _options.directoryPerDB, _options.directoryForIndexes));
	//KVCatalog::init
	//KVStorageEngine::KVStorageEngine->KVCatalog::init��ʼ�������ʱ��ʹ�Ԫ����
	//�ļ�_mdb_catalog.wt�л�ȡԪ������Ϣ
	_catalog->init(&opCtx);

    std::vector<std::string> collections;
	//KVCatalog::getAllCollections ��ȡ����Ϣ������ʵ����������Ҫͨ��_mdb_catalog.wt��ȡ��Ԫ������Ϣ
    _catalog->getAllCollections(&collections);

    KVPrefix maxSeenPrefix = KVPrefix::kNotPrefixed;
	//��_mdb_catalog.wt�н��������Ԫ������Ϣ
	//mongodʵ��������������Ҫ����_mdb_catalog.wt�ļ���ȡԪ������Ϣ
    for (size_t i = 0; i < collections.size(); i++) {
		//��_mdb_catalog.wt�ļ��л�ȡ�����ͱ���
        std::string coll = collections[i];
        NamespaceString nss(coll);
        string dbName = nss.db().toString();

        // No rollback since this is only for committed dbs.

		//�����_dbs��ֵ�����Կ���һ��dbname��Ӧһ��db KVDatabaseCatalogEntryBase
        KVDatabaseCatalogEntryBase*& db = _dbs[dbName];
        if (!db) {
            db = _databaseCatalogEntryFactory(dbName, this).release();
        }

		//KVDatabaseCatalogEntryBase::initCollection����
		//����������ͬһ��db��������еı��洢����KVDatabaseCatalogEntryBase._collections[]������
        db->initCollection(&opCtx, coll, options.forRepair);
        auto maxPrefixForCollection = _catalog->getMetaData(&opCtx, coll).getMaxPrefix();
        maxSeenPrefix = std::max(maxSeenPrefix, maxPrefixForCollection);
    }
	
    KVPrefix::setLargestPrefix(maxSeenPrefix);
    opCtx.recoveryUnit()->abandonSnapshot();
}

/**
 * This method reconciles differences between idents the KVEngine is aware of and the
 * KVCatalog. There are three differences to consider:
 *
 * First, a KVEngine may know of an ident that the KVCatalog does not. This method will drop
 * the ident from the KVEngine.
 *
 * Second, a KVCatalog may have a collection ident that the KVEngine does not. This is an
 * illegal state and this method fasserts.
 *
 * Third, a KVCatalog may have an index ident that the KVEngine does not. This method will
 * rebuild the index.
 */
/*
��MongoDB��ɼ����������ݱ���_mdb_catalog����ά����MongoDB��Ҫ��Ԫ���ݣ�ͬ����WiredTiger���У�
����һ�ݶ�Ӧ��WiredTiger��Ҫ��Ԫ����ά����WiredTiger.wt���С���ˣ���ʵ���������������ݱ���б�
������ĳЩ����¿��ܻ���ڲ�һ�£����磬�쳣崻��ĳ��������MongoDB�����������У��������������
����һ���Լ�飬������쳣崻��������̣�����WiredTiger.wt���е�����Ϊ׼����_mdb_catalog���еļ�¼����������������̻���Ҫ����WiredTiger.wt��õ��������ݱ���б�

���ϣ����Կ�������MongoDB���������У��жദ�漰����Ҫ��WiredTiger.wt���ж�ȡ���ݱ��Ԫ���ݡ�
����������WiredTigerר���ṩ��һ������ġ�metadata�����͵�cursor��
*/

//repairDatabasesAndCheckVersion�е���
StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>>
	KVStorageEngine::reconcileCatalogAndIdents(OperationContext* opCtx) {
    // Gather all tables known to the storage engine and drop those that aren't cross-referenced
    // in the _mdb_catalog. This can happen for two reasons.
    //
    // First, collection creation and deletion happen in two steps. First the storage engine
    // creates/deletes the table, followed by the change to the _mdb_catalog. It's not assumed a
    // storage engine can make these steps atomic.
    //
    // Second, a replica set node in 3.6+ on supported storage engines will only persist "stable"
    // data to disk. That is data which replication guarantees won't be rolled back. The
    // _mdb_catalog will reflect the "stable" set of collections/indexes. However, it's not
    // expected for a storage engine's ability to persist stable data to extend to "stable
    // tables".
    
	//WiredTigerKVEngine::getAllIdents��KVCatalog::getAllIdents����
	// 1. WiredTigerKVEngine::getAllIdents��ӦWiredTiger.wtԪ�����ļ�����wiredtiger�洢�����Լ�ά��
	// 2. KVCatalog::getAllIdents��Ӧ_mdb_catalog.wt����mongodb server��storageģ��ά��
	// 3. ������Ԫ������Ƚϣ���ͻ��ʱ��collection��_mdb_catalog.wtΪ׼���ñ������������WiredTiger.wtΪ׼
	//    �ο�KVStorageEngine::reconcileCatalogAndIdents

    std::set<std::string> engineIdents;
    {   //��ӦWiredTiger.wt
    	//WiredTigerKVEngine::getAllIdents
        std::vector<std::string> vec = _engine->getAllIdents(opCtx);
        engineIdents.insert(vec.begin(), vec.end());
        engineIdents.erase(catalogInfo);
    }

    std::set<std::string> catalogIdents;
    {   //��Ӧ_mdb_catalog.wt
    	//KVCatalog::getAllIdents
        std::vector<std::string> vec = _catalog->getAllIdents(opCtx);
        catalogIdents.insert(vec.begin(), vec.end());
    }

    // Drop all idents in the storage engine that are not known to the catalog. This can happen in
    // the case of a collection or index creation being rolled back.
    
    //��WiredTiger.wt���У�����_mdb_catalog.wt��û�е�Ԫ������Ϣ��� 
    for (const auto& it : engineIdents) {
		log() << "yang test ....reconcileCatalogAndIdents...... ident: " << it;
		//�ҵ�����ͬ��ident����Ŀ¼�ļ���������һ��
        if (catalogIdents.find(it) != catalogIdents.end()) {
            continue;
        }

		//�Ƿ���ͨ���ݼ��ϻ�����������
        if (!_catalog->isUserDataIdent(it)) {
            continue;
        }

        const auto& toRemove = it;
        log() << "Dropping unknown ident: " << toRemove;
        WriteUnitOfWork wuow(opCtx);
		//WiredTigerKVEngine::dropIdent ɾ����Ӧident�ļ�
        fassertStatusOK(40591, _engine->dropIdent(opCtx, toRemove));
        wuow.commit();
    }

    // Scan all collections in the catalog and make sure their ident is known to the storage
    // engine. An omission here is fatal. A missing ident could mean a collection drop was rolled
    // back. Note that startup already attempts to open tables; this should only catch errors in
    // other contexts such as `recoverToStableTimestamp`.
    std::vector<std::string> collections;
    _catalog->getAllCollections(&collections);
	//���_mdb_catalog.wt���У�����WiredTiger.wt��û�ж�ӦԪ������Ϣ����ֱ���׳��쳣
    for (const auto& coll : collections) {
        const auto& identForColl = _catalog->getCollectionIdent(coll);
        if (engineIdents.find(identForColl) == engineIdents.end()) {
            return {ErrorCodes::UnrecoverableRollbackError,
                    str::stream() << "Expected collection does not exist. NS: " << coll
                                  << " Ident: "
                                  << identForColl};
        }
    }

    // Scan all indexes and return those in the catalog where the storage engine does not have the
    // corresponding ident. The caller is expected to rebuild these indexes.
    std::vector<CollectionIndexNamePair> ret;
	//����_mdb_catalog.wt�е�Ԫ���ݼ�����Ϣ
    for (const auto& coll : collections) {
        const BSONCollectionCatalogEntry::MetaData metaData = _catalog->getMetaData(opCtx, coll);
        for (const auto& indexMetaData : metaData.indexes) {
            const std::string& indexName = indexMetaData.name();
            std::string indexIdent = _catalog->getIndexIdent(opCtx, coll, indexName);
            if (engineIdents.find(indexIdent) != engineIdents.end()) {
                continue;
            }

			//_mdb_catalog.wtԪ�����и�����������WiredTiger.wtԪ������ȴû�ø���������˵����Ҫ����������
            log() << "Expected index data is missing, rebuilding. NS: " << coll
                  << " Index: " << indexName << " Ident: " << indexIdent;

			//��Щ������ӵ�ret����
            ret.push_back(CollectionIndexNamePair(coll, indexName));
        }
    }

    return ret;
}

//ServiceContextMongoD::shutdownGlobalStorageEngineCleanly()����
//shutdown���մ���
void KVStorageEngine::cleanShutdown() {
    for (DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it) {
        delete it->second;
    }
    _dbs.clear();

    _catalog.reset(NULL);
    _catalogRecordStore.reset(NULL);

    _engine->cleanShutdown();
    // intentionally not deleting _engine
}

KVStorageEngine::~KVStorageEngine() {}

void KVStorageEngine::finishInit() {}

// ServiceContextMongoD::_newOpCtx->KVStorageEngine::newRecoveryUnit
RecoveryUnit* KVStorageEngine::newRecoveryUnit() {
    if (!_engine) {
        // shutdown
        return NULL;
    }
    return _engine->newRecoveryUnit(); //WiredTigerKVEngine::newRecoveryUnit
}

void KVStorageEngine::listDatabases(std::vector<std::string>* out) const {
    stdx::lock_guard<stdx::mutex> lk(_dbsLock);
    for (DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it) {
        if (it->second->isEmpty())
            continue;
        out->push_back(it->first);
    }
}


KVDatabaseCatalogEntryBase* KVStorageEngine::getDatabaseCatalogEntry(OperationContext* opCtx,
                                                                     StringData dbName) {
    stdx::lock_guard<stdx::mutex> lk(_dbsLock);
    KVDatabaseCatalogEntryBase*& db = _dbs[dbName.toString()];
    if (!db) {
        // Not registering change since db creation is implicit and never rolled back.
        //defaultDatabaseCatalogEntryFactory
        //����һ��KVDatabaseCatalogEntry��
        db = _databaseCatalogEntryFactory(dbName, this).release();
    }
    return db;
}

Status KVStorageEngine::closeDatabase(OperationContext* opCtx, StringData db) {
    // This is ok to be a no-op as there is no database layer in kv.
    return Status::OK();
}

//DatabaseImpl::dropDatabase���ã���ɾ�����еı�Ȼ���ٴ�_dbs�����������db
Status KVStorageEngine::dropDatabase(OperationContext* opCtx, StringData db) {
    KVDatabaseCatalogEntryBase* entry;
	//�ҵ���Ӧ��DB,û��ֱ�ӷ���
    {
        stdx::lock_guard<stdx::mutex> lk(_dbsLock);
        DBMap::const_iterator it = _dbs.find(db.toString());
        if (it == _dbs.end())
            return Status(ErrorCodes::NamespaceNotFound, "db not found to drop");
        entry = it->second;
    }

    // This is called outside of a WUOW since MMAPv1 has unfortunate behavior around dropping
    // databases. We need to create one here since we want db dropping to all-or-nothing
    // wherever possible. Eventually we want to move this up so that it can include the logOp
    // inside of the WUOW, but that would require making DB dropping happen inside the Dur
    // system for MMAPv1.
    //���в�������һ��������
    WriteUnitOfWork wuow(opCtx);

    std::list<std::string> toDrop;
	//��ȡҪɾ���ı���Ϣ
    entry->getCollectionNamespaces(&toDrop);

    for (std::list<std::string>::iterator it = toDrop.begin(); it != toDrop.end(); ++it) {
        string coll = *it;
		//KVDatabaseCatalogEntry::dropCollection
		//�����������ݱ���������ɾ������
        entry->dropCollection(opCtx, coll).transitional_ignore();
    }
    toDrop.clear();
    entry->getCollectionNamespaces(&toDrop);
    invariant(toDrop.empty());

    {
        stdx::lock_guard<stdx::mutex> lk(_dbsLock);
		//WiredTigerRecoveryUnit::registerChange ע�ᵽWiredTigerRecoveryUnit��
		//�����������commit�л����WiredTigerRecoveryUnit::_commit()��WiredTigerRecoveryUnit::_abort()
		//ִ��RemoveDBChange��rollback����commit
        opCtx->recoveryUnit()->registerChange(new RemoveDBChange(this, db, entry));
		//��_dbs���������db��Ϣ
        _dbs.erase(db.toString());
    }

	//����������ִ��registerChangeע���RemoveDBChange::commit�ͷ��ڴ�
    wuow.commit();
    return Status::OK();
}

//FSyncLockThread::run()����  db.adminCommand( { fsync: 1, lock: true } )
int KVStorageEngine::flushAllFiles(OperationContext* opCtx, bool sync) {
    return _engine->flushAllFiles(opCtx, sync);
}

Status KVStorageEngine::beginBackup(OperationContext* opCtx) {
    // We should not proceed if we are already in backup mode
    if (_inBackupMode)
        return Status(ErrorCodes::BadValue, "Already in Backup Mode");
    Status status = _engine->beginBackup(opCtx);
    if (status.isOK())
        _inBackupMode = true;
    return status;
}

void KVStorageEngine::endBackup(OperationContext* opCtx) {
    // We should never reach here if we aren't already in backup mode
    invariant(_inBackupMode);
    _engine->endBackup(opCtx);
    _inBackupMode = false;
}

bool KVStorageEngine::isDurable() const {
    return _engine->isDurable();
}

/*
ephemeralForTest�洢���棨ephemeralForTest Storage Engine��

MongoDB 3.2�ṩ��һ���µ����ڲ��ԵĴ洢���档������һЩԪ���ݣ����ڲ��ԵĴ洢���治ά��
�κδ������ݣ�����Ҫ�ڲ��������ڼ����������ڲ��ԵĴ洢��������֧�ֵġ�
*/
bool KVStorageEngine::isEphemeral() const {
    return _engine->isEphemeral();
}

SnapshotManager* KVStorageEngine::getSnapshotManager() const {
    return _engine->getSnapshotManager();
}

////CmdRepairDatabase::errmsgRun
Status KVStorageEngine::repairRecordStore(OperationContext* opCtx, const std::string& ns) {
    Status status = _engine->repairIdent(opCtx, _catalog->getCollectionIdent(ns));
    if (!status.isOK())
        return status;

    _dbs[nsToDatabase(ns)]->reinitCollectionAfterRepair(opCtx, ns);
    return Status::OK();
}

//ReplicationCoordinatorExternalStateImpl::startThreads
void KVStorageEngine::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}

//StorageInterfaceImpl::setStableTimestamp����
void KVStorageEngine::setStableTimestamp(Timestamp stableTimestamp) {
	//WiredTigerKVEngine::setStableTimestamp
    _engine->setStableTimestamp(stableTimestamp);
}


void KVStorageEngine::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
	//WiredTigerKVEngine::setInitialDataTimestamp
    _engine->setInitialDataTimestamp(initialDataTimestamp);
}

/*
WiredTiger �ṩ���� oldest timestamp �Ĺ��ܣ������� MongoDB �����ø�ʱ�����������Read as of a timestamp
�����ṩ��С��ʱ���������һ���Զ���Ҳ����˵��WiredTiger ����ά�� oldest timestamp ֮ǰ��������ʷ�汾��
MongoDB ����ҪƵ������ʱ������ oldest timestamp�������� WT cache ѹ��̫��
�ο�https://mongoing.com/%3Fp%3D6084
*/
void KVStorageEngine::setOldestTimestamp(Timestamp oldestTimestamp) {
	//WiredTigerKVEngine::setOldestTimestamp
    _engine->setOldestTimestamp(oldestTimestamp);
}

bool KVStorageEngine::supportsRecoverToStableTimestamp() const {
    return _engine->supportsRecoverToStableTimestamp();
}

void KVStorageEngine::replicationBatchIsComplete() const {
    return _engine->replicationBatchIsComplete();
}
}  // namespace mongo
