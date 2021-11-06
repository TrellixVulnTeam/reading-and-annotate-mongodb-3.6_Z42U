/**
 *    Copyright (C) 2016 MongoDB Inc.
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
/*  _mdb_catalog.wt����
{
	ns: "test.test1",
	md: {
		ns: "test.test1",
		options: {
			uuid: UUID("520904ec-0432-4c00-a15d-788e2f5d707b")
		},
		indexes: [{
			spec: {
				v: 2,
				key: {
					_id: 1
				},
				name: "_id_",
				ns: "test.test1"
			},
			ready: true,
			multikey: false,
			multikeyPaths: {
				_id: BinData(0, 00)
			},
			head: 0,
			prefix: -1
		}, {
			spec: {
				v: 2,
				key: {
					name: 1.0,
					age: 1.0
				},
				name: "name_1_age_1",
				ns: "test.test1",
				background: true
			},
			ready: true,
			multikey: false,
			multikeyPaths: {
				name: BinData(0, 00),
				age: BinData(0, 00)
			},
			head: 0,
			prefix: -1
		}, {
			spec: {
				v: 2,
				key: {
					zipcode: 1.0
				},
				name: "zipcode_1",
				ns: "test.test1",
				background: true
			},
			ready: true,
			multikey: false,
			multikeyPaths: {
				zipcode: BinData(0, 00)
			},
			head: 0,
			prefix: -1
		}],
		prefix: -1
	},
	idxIdent: {
		_id_: "test/index/8-380857198902467499",
		name_1_age_1: "test/index/0--6948813758302814892",
		zipcode_1: "test/index/3--6948813758302814892"
	},
	ident: "test/collection/7-380857198902467499"
}

*/
#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/storage/kv/kv_database_catalog_entry.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {

using std::string;
using std::vector;

//KVDatabaseCatalogEntryBase::createCollection�й���ʹ��
class KVDatabaseCatalogEntryBase::AddCollectionChange : public RecoveryUnit::Change {
public:
    AddCollectionChange(OperationContext* opCtx,
                        KVDatabaseCatalogEntryBase* dce,
                        StringData collection,
                        StringData ident,
                        bool dropOnRollback)
        : _opCtx(opCtx),
          _dce(dce),
          _collection(collection.toString()),
          _ident(ident.toString()),
          _dropOnRollback(dropOnRollback) {}

	
	////KVDatabaseCatalogEntryBase::commit->WiredTigerKVEngine::dropIdentɾ���е��ã������ı�ɾ��
	//KVCollectionCatalogEntry::RemoveIndexChange::commit()->WiredTigerKVEngine::dropIdent ɾ�������е��ã�����ɾ������������
    virtual void commit() {}
    virtual void rollback() {
        if (_dropOnRollback) {
            // Intentionally ignoring failure
            //������ɾ�����������
            _dce->_engine->getEngine()->dropIdent(_opCtx, _ident).transitional_ignore();
        }

        const CollectionMap::iterator it = _dce->_collections.find(_collection);
        if (it != _dce->_collections.end()) {
            delete it->second;
            _dce->_collections.erase(it);
        }
    }

    OperationContext* const _opCtx;
    KVDatabaseCatalogEntryBase* const _dce;
    const std::string _collection;
    const std::string _ident;
    const bool _dropOnRollback;
};

class KVDatabaseCatalogEntryBase::RemoveCollectionChange : public RecoveryUnit::Change {
public:
	////�����ı�ɾ�������ͨ�����ﴥ�����ο�KVDatabaseCatalogEntryBase::dropCollection
    RemoveCollectionChange(OperationContext* opCtx,
                           KVDatabaseCatalogEntryBase* dce,
                           StringData collection,
                           StringData ident,
                           KVCollectionCatalogEntry* entry,
                           bool dropOnCommit)
        : _opCtx(opCtx),
          _dce(dce),
          _collection(collection.toString()),
          _ident(ident.toString()),
          _entry(entry),
          _dropOnCommit(dropOnCommit) {}

	
    virtual void commit() {
        delete _entry;

        // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
        // collection, we should never see it again anyway.
        if (_dropOnCommit)
			//����������������
			//WiredTigerKVEngine::dropIdent
            _dce->_engine->getEngine()->dropIdent(_opCtx, _ident).transitional_ignore();
    }

    virtual void rollback() {
        _dce->_collections[_collection] = _entry;
    }

    OperationContext* const _opCtx;
    KVDatabaseCatalogEntryBase* const _dce;
    const std::string _collection;
    const std::string _ident;
    KVCollectionCatalogEntry* const _entry;
    const bool _dropOnCommit;
};

KVDatabaseCatalogEntryBase::KVDatabaseCatalogEntryBase(StringData db, KVStorageEngine* engine)
    : DatabaseCatalogEntry(db), _engine(engine) {}

KVDatabaseCatalogEntryBase::~KVDatabaseCatalogEntryBase() {
    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        delete it->second;
    }
    _collections.clear();
}

//����DB�����Ƿ��п��ñ���Ϣ
bool KVDatabaseCatalogEntryBase::exists() const {
    return !isEmpty();
}

//�Ƿ��б����
bool KVDatabaseCatalogEntryBase::isEmpty() const {
    return _collections.empty();
}

//�б���Ϣ��˵�������ݴ���
bool KVDatabaseCatalogEntryBase::hasUserData() const {
    return !isEmpty();
}

//db.runCommand({ listDatabases : 1 })��ȡ���п�Ĵ�����Ϣ
/*
> db.runCommand({ listDatabases : 1 })
{
        "databases" : [
                {
                        "name" : "admin",
                        "sizeOnDisk" : 32768,
                        "empty" : false
                },
                {
                        "name" : "config",
                        "sizeOnDisk" : 73728,
                        "empty" : false
                },
                {
                        "name" : "local",
                        "sizeOnDisk" : 77824,
                        "empty" : false
                },
                {
                        "name" : "test",
                        "sizeOnDisk" : 90112,
                        "empty" : false
                }
        ],
        "totalSize" : 274432,
        "ok" : 1
}
*/
//�������ݴ�С=���б������+���б�������ܺ�
int64_t KVDatabaseCatalogEntryBase::sizeOnDisk(OperationContext* opCtx) const {
    int64_t size = 0;

    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        const KVCollectionCatalogEntry* coll = it->second;
        if (!coll)
            continue;
        size += coll->getRecordStore()->storageSize(opCtx);

        vector<string> indexNames;
        coll->getAllIndexes(opCtx, &indexNames);

        for (size_t i = 0; i < indexNames.size(); i++) {
            string ident =
                _engine->getCatalog()->getIndexIdent(opCtx, coll->ns().ns(), indexNames[i]);
            size += _engine->getEngine()->getIdentSize(opCtx, ident);
        }
    }

    return size;
}

void KVDatabaseCatalogEntryBase::appendExtraStats(OperationContext* opCtx,
                                                  BSONObjBuilder* out,
                                                  double scale) const {}

Status KVDatabaseCatalogEntryBase::currentFilesCompatible(OperationContext* opCtx) const {
    // Delegate to the FeatureTracker as to whether the data files are compatible or not.
    return _engine->getCatalog()->getFeatureTracker()->isCompatibleWithCurrentCode(opCtx);
}

//��ȡ���еı�������out����
void KVDatabaseCatalogEntryBase::getCollectionNamespaces(std::list<std::string>* out) const {
    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        out->push_back(it->first);
    }
}

//��ȡ���Ӧ��KVCollectionCatalogEntry��һ�����Ӧһ��KVCollectionCatalogEntry
CollectionCatalogEntry* KVDatabaseCatalogEntryBase::getCollectionCatalogEntry(StringData ns) const {
    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return NULL;
    }

    return it->second;
}


//WiredTigerIndexUnique(Ψһ�����ļ�����)��WiredTigerIndexStandard(��ͨ�����ļ�����)
//WiredTigerRecordStore(�������ļ�����)

//��ȡ�Ըñ���еײ�����KV������WiredTigerRecordStore
RecordStore* KVDatabaseCatalogEntryBase::getRecordStore(StringData ns) const {
    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return NULL;
    }

	//KVCollectionCatalogEntry::getRecordStore,Ҳ���ǻ�ȡKVCollectionCatalogEntry._recordStore��Ա
	//Ĭ��ΪĬ��ΪStandardWiredTigerRecordStore����
	
	//KVCollectionCatalogEntry::getRecordStore, Ҳ���ǻ�ȡKVCollectionCatalogEntry._recordStore��Ա��Ϣ
    return it->second->getRecordStore();
}


//insertBatchAndHandleErrors->makeCollection->mongo::userCreateNS->mongo::userCreateNSImpl
//->DatabaseImpl::createCollection->KVDatabaseCatalogEntryBase::createCollection
// Collection* createCollection����
//��ʼ���õײ�WT�洢������ؽӿڽ���ͬʱ����һ��KVCollectionCatalogEntry����_collections����

//KVDatabaseCatalogEntryBase::createCollection��KVDatabaseCatalogEntryBase::initCollection����������:
// 1. KVDatabaseCatalogEntryBase::createCollection��Ӧ�����¼��ı�
// 2. KVDatabaseCatalogEntryBase::initCollection��Ӧ����mongod��������_mdb_catalog.wtԪ�����ļ��м��صı�


//ע�⣬������ֻ�пձ��Ӧ����KV ident�������ձ��Ӧid������Ӧ�����ļ�idxident�����DatabaseImpl::createCollection��ʵ��
Status KVDatabaseCatalogEntryBase::createCollection(OperationContext* opCtx,
                                                    StringData ns,
                                                    const CollectionOptions& options,
                                                    bool allocateDefaultSpace) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    if (ns.empty()) {
        return Status(ErrorCodes::BadValue, "Collection namespace cannot be empty");
    }

    if (_collections.count(ns.toString())) {
        invariant(_collections[ns.toString()]);
        return Status(ErrorCodes::NamespaceExists, "collection already exists");
    }

    KVPrefix prefix = KVPrefix::getNextPrefix(NamespaceString(ns));

	//��collection��ns��ident�洢��Ԫ�����ļ�_mdb_catalog�С�
    // need to create it  ����KVCatalog::newCollection ����wiredtiger �����ļ�
    //����_idents����¼�¼��϶�ӦԪ������Ϣ��Ҳ���Ǽ���·��  ����uuid �����������Լ���Ԫ����_mdb_catalog.wt�е�λ��
	//KVCatalog::newCollection������_mdb_catalog.wt�ļ�Ԫ���ݣ����±���
	Status status = _engine->getCatalog()->newCollection(opCtx, ns, options, prefix);
    if (!status.isOK())
        return status;

	//Ҳ����newCollection�����ɵļ���ident��Ҳ����Ԫ����Ԫ����_mdb_catalog.wt�ļ�·��
	//��ȡ��Ӧ���Ԫ������Ϣ
    string ident = _engine->getCatalog()->getCollectionIdent(ns);  
	
	//WiredTigerKVEngine::createGroupedRecordStore(�����ļ����)  
	//WiredTigerKVEngine::createGroupedSortedDataInterface(�����ļ����)
	//����WT�洢�����create�ӿڽ����ײ㽨������
    status = _engine->getEngine()->createGroupedRecordStore(opCtx, ns, ident, options, prefix);
    if (!status.isOK())
        return status;

    // Mark collation feature as in use if the collection has a non-simple default collation.
    if (!options.collation.isEmpty()) {
        const auto feature = KVCatalog::FeatureTracker::NonRepairableFeature::kCollation;
        if (_engine->getCatalog()->getFeatureTracker()->isNonRepairableFeatureInUse(opCtx,
                                                                                    feature)) {
            _engine->getCatalog()->getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx,
                                                                                        feature);
        }
    }


	//�½�collection����¼���¼����
    opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, ns, ident, true));
	//WiredTigerKVEngine::getGroupedRecordStore
	//����StandardWiredTigerRecordStore��,����ͱ�ʵ���Ϲ����������Ը������ؽӿڲ���ʵ���Ͼ��ǶԱ��KV����
	//һ�����һ��StandardWiredTigerRecordStore��Ӧ���Ժ�Ըñ�ĵײ�洢����KV�������ɸ���ʵ��
	auto rs = _engine->getEngine()->getGroupedRecordStore(opCtx, ns, ident, options, prefix);
    invariant(rs);

	//�浽map���У���WiredTigerKVEngine  
	//����һ�����Ӧһ��KVCollectionCatalogEntry���洢��_collections������
    _collections[ns.toString()] = new KVCollectionCatalogEntry(
       //WiredTigerKVEngine--�洢����    
       //     KVStorageEngine::getCatalog(KVStorageEngine._catalog(KVCatalog����))---"_mdb_catalog.wt"Ԫ���ݽӿ�
       //                                                        ident-----���Ӧ�����ļ�Ŀ¼
       //                                                            StandardWiredTigerRecordStore--�ײ�WT�洢����KV����--���Ʊ�ײ�洢����KV�ӿ�
        _engine->getEngine(), _engine->getCatalog(), ns, ident, std::move(rs));

    return Status::OK();
}

//KVStorageEngine::KVStorageEngine->KVDatabaseCatalogEntryBase::initCollection��Ԫ����_mdb_catalog.wt�м��ر���Ϣ
//��mongod������ʱ��ᣬ�����KVStorageEngine::KVStorageEngine���ñ��ӿ�

//KVDatabaseCatalogEntryBase::createCollection��KVDatabaseCatalogEntryBase::initCollection����������:
// 1. KVDatabaseCatalogEntryBase::createCollection��Ӧ�����¼��ı�
// 2. KVDatabaseCatalogEntryBase::initCollection��Ӧ����mongod��������_mdb_catalog.wtԪ�����ļ��м��صı�
void KVDatabaseCatalogEntryBase::initCollection(OperationContext* opCtx,
                                                const std::string& ns,
                                                bool forRepair) {
    invariant(!_collections.count(ns));
	
	//��ȡns��Ӧwt�ļ�����Ҳ���Ǵ���·����
    const std::string ident = _engine->getCatalog()->getCollectionIdent(ns);

    std::unique_ptr<RecordStore> rs;
    if (forRepair) {
        // Using a NULL rs since we don't want to open this record store before it has been
        // repaired. This also ensures that if we try to use it, it will blow up.
        rs = nullptr;
    } else {
    	//��ȡ�ñ��Ԫ������Ϣ
        BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(opCtx, ns);
		//WiredTigerKVEngine::getGroupedRecordStore
		//��ȡ�Ըñ���еײ�KV������RecordStore
		rs = _engine->getEngine()->getGroupedRecordStore(opCtx, ns, ident, md.options, md.prefix);
        invariant(rs);
    }

    // No change registration since this is only for committed collections
   //WiredTigerKVEngine--�洢����    
   //     KVStorageEngine::getCatalog(KVStorageEngine._catalog(KVCatalog����))---"_mdb_catalog.wt"Ԫ���ݽӿ�
   //                                           StandardWiredTigerRecordStore--�ײ�WT�洢����KV����--���Ʊ�ײ�洢����KV�ӿ�
    _collections[ns] = new KVCollectionCatalogEntry(
   //WiredTigerKVEngine--�洢����    
   //     KVStorageEngine::getCatalog(KVStorageEngine._catalog(KVCatalog����))---"_mdb_catalog.wt"Ԫ���ݽӿ�
   //                                                        ident-----���Ӧ�����ļ�Ŀ¼
   //                                                            StandardWiredTigerRecordStore--�ײ�WT�洢����KV����--���Ʊ�ײ�洢����KV�ӿ�
        _engine->getEngine(), _engine->getCatalog(), ns, ident, std::move(rs));
}

void KVDatabaseCatalogEntryBase::reinitCollectionAfterRepair(OperationContext* opCtx,
                                                             const std::string& ns) {
    // Get rid of the old entry.
    CollectionMap::iterator it = _collections.find(ns);
    invariant(it != _collections.end());
    delete it->second;
    _collections.erase(it);

    // Now reopen fully initialized.
    initCollection(opCtx, ns, false);
}

//DatabaseImpl::renameCollection���ã�����������
//�Ӹýӿڿ��Կ�����һ���������Ҫ����������Ϣ��
// 1. ����sizeStorer.wt sizeԪ�����ļ��ж�Ӧ�ı���Ϊ�����Ѿ��ı���
// 2. �����޸ĺ�_mdb_catalog.wtԪ����Ҳ��Ҫ���£�����������ident�ȣ�ident��ͨ���������ɵģ��������ˣ����identҲ��Ҫ�޸�
// 3. �������˺�identҲ���ˣ���˲����ñ��WiredTigerRecordStoreҲ��Ҫ�ı䣬��Ҫ��������
// 4. ��cache�и��������µı������µ�ident���µ�WiredTigerRecordStore�����µ�KVCollectionCatalogEntry����entry���ڴ�cache�л�������
//  ���ʣ�Ϊ��û�ж�idxIdent(name_1_age_1: "test/index/0--6948813758302814892")������ԭ����idxIdent�ж�Ӧ��test�ǿ⣬û�б���Ϣ
Status KVDatabaseCatalogEntryBase::renameCollection(OperationContext* opCtx,
                                                    StringData fromNS,
                                                    StringData toNS,
                                                    bool stayTemp) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    RecordStore* originalRS = NULL;

	//��ȡ����Ϣ
    CollectionMap::const_iterator it = _collections.find(fromNS.toString());
    if (it == _collections.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "rename cannot find collection");
    }

	//��ȡ�����ñ��WiredTigerRecordStore
    originalRS = it->second->getRecordStore();

	//Ŀ�ı����Ƿ��Ѿ����ڣ�����˵���ظ��ˣ�ֱ�ӱ���
    it = _collections.find(toNS.toString());
    if (it != _collections.end()) {
        return Status(ErrorCodes::NamespaceExists, "for rename to already exists");
    }

	//ԭ���Ӧident
    const std::string identFrom = _engine->getCatalog()->getCollectionIdent(fromNS);

	//WiredTigerKVEngine::okToRename
	//cache�м�¼�ı����ݴ�С�����������󣬼�¼��Ԫ�����ļ�sizeStorer.wtҲ��Ҫ��Ӧ�޸�
    Status status = _engine->getEngine()->okToRename(opCtx, fromNS, toNS, identFrom, originalRS);
    if (!status.isOK())
        return status;

	//_mdb_catalog.wtԪ�����ļ��еı�����Ҫ���£�Ԫ����Ҳ��Ҫ���£�����_mdb_catalog.wt�����±��Ԫ������Ϣ��
	//KVCatalog::renameCollection
	status = _engine->getCatalog()->renameCollection(opCtx, fromNS, toNS, stayTemp);
    if (!status.isOK())
        return status;

	//Դ������Ӧ��identҲ��Ҫ��������Ϊ������Ӧident�������ָ���㷨���ɣ��������ˣ�indent�϶�Ҳ�Ͳ�һ����
    const std::string identTo = _engine->getCatalog()->getCollectionIdent(toNS);

    invariant(identFrom == identTo);

	//��ȡ�±��Ԫ������Ϣ�ļ�_mdb_catalog.wt�е�Ԫ������Ϣmd
    BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(opCtx, toNS);

	//���ԭ����ڴ�cache��Ϣ
    const CollectionMap::iterator itFrom = _collections.find(fromNS.toString());
    invariant(itFrom != _collections.end());
    opCtx->recoveryUnit()->registerChange(
        new RemoveCollectionChange(opCtx, this, fromNS, identFrom, itFrom->second, false));
    _collections.erase(itFrom);

    opCtx->recoveryUnit()->registerChange(
        new AddCollectionChange(opCtx, this, toNS, identTo, false));

	//��ȡ�����±��WiredTigerRecordStore
    auto rs =
        _engine->getEngine()->getGroupedRecordStore(opCtx, toNS, identTo, md.options, md.prefix);

	//�±����ɶ�Ӧ�µ�KVCollectionCatalogEntry cache��Ϣ
    _collections[toNS.toString()] = new KVCollectionCatalogEntry(
        _engine->getEngine(), _engine->getCatalog(), toNS, identTo, std::move(rs));

    return Status::OK();
}

//dropɾ��CmdDrop::errmsgRun->dropCollection->DatabaseImpl::dropCollectionEvenIfSystem->DatabaseImpl::_finishDropCollection
//    ->DatabaseImpl::_finishDropCollection->KVDatabaseCatalogEntryBase::dropCollection->KVCatalog::dropCollection
Status KVDatabaseCatalogEntryBase::dropCollection(OperationContext* opCtx, StringData ns) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

	//�ҵ��ñ��Ӧ��KVCollectionCatalogEntry
    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "cannnot find collection to drop");
    }

    KVCollectionCatalogEntry* const entry = it->second;

    invariant(entry->getTotalIndexCount(opCtx) == entry->getCompletedIndexCount(opCtx));
	//������ñ����������
    {
        std::vector<std::string> indexNames;
        entry->getAllIndexes(opCtx, &indexNames);
        for (size_t i = 0; i < indexNames.size(); i++) {
			//KVCollectionCatalogEntry::removeIndex
			//�Ӹñ�MetaDataԪ�����������index������������ļ�
            entry->removeIndex(opCtx, indexNames[i]).transitional_ignore();
        }
    }

    invariant(entry->getTotalIndexCount(opCtx) == 0);

    const std::string ident = _engine->getCatalog()->getCollectionIdent(ns);

	//KVStorageEngine::getCatalog��ȡKVDatabaseCatalogEntry   KVStorageEngine::getCatalog��ȡKVCatalog
	//KVCatalog::dropCollection 
	//ɾ�������Ҫ��Ԫ������ɾ���ñ�
    Status status = _engine->getCatalog()->dropCollection(opCtx, ns);
    if (!status.isOK()) {
        return status;
    }

    // This will lazily delete the KVCollectionCatalogEntry and notify the storageEngine to
    // drop the collection only on WUOW::commit().
    //�����ı�ɾ�������ͨ�����ﴥ����������KVStorageEngine::dropDatabase�е���WUOW::commit()����������ɾ��
    opCtx->recoveryUnit()->registerChange(
    //���������ô���RemoveCollectionChange::commit��������ɾ��
        new RemoveCollectionChange(opCtx, this, ns, ident, it->second, true));

	//��cache������ñ�
    _collections.erase(ns.toString());

    return Status::OK();
}
}  // namespace mongo
