// kv_collection_catalog_entry.cpp

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

#include <memory>

#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"

#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"

namespace mongo {

using std::string;

namespace {

bool indexTypeSupportsPathLevelMultikeyTracking(StringData accessMethod) {
    return accessMethod == IndexNames::BTREE || accessMethod == IndexNames::GEO_2DSPHERE;
}

}  // namespace 

//KVDatabaseCatalogEntryBase::createCollection��new����
class KVCollectionCatalogEntry::AddIndexChange : public RecoveryUnit::Change {
public:
    AddIndexChange(OperationContext* opCtx, KVCollectionCatalogEntry* cce, StringData ident)
        : _opCtx(opCtx), _cce(cce), _ident(ident.toString()) {}

    virtual void commit() {}
    virtual void rollback() {
        // Intentionally ignoring failure.
        //������������ɾ��������
        _cce->_engine->dropIdent(_opCtx, _ident).transitional_ignore();
    }

    OperationContext* const _opCtx;
    KVCollectionCatalogEntry* const _cce;
    const std::string _ident; //.wt�ļ���
};

//ɾ������Ҫ��¼����ʷ��Ϣ
class KVCollectionCatalogEntry::RemoveIndexChange : public RecoveryUnit::Change {
public:
    RemoveIndexChange(OperationContext* opCtx, KVCollectionCatalogEntry* cce, StringData ident)
        : _opCtx(opCtx), _cce(cce), _ident(ident.toString()) {}

    virtual void rollback() {}
	//IndexRemoveChange commit�������IndexCatalogImpl._entries[]�����ж�Ӧ������IndexCatalogEntryImpl
    //RemoveIndexChange commit���������������ļ����
    
	//KVDatabaseCatalogEntryBase::commit->WiredTigerKVEngine::dropIdentɾ���е��ã������ı�ɾ��
	//KVCollectionCatalogEntry::RemoveIndexChange::commit()->WiredTigerKVEngine::dropIdent ɾ�������е��ã�����ɾ������������
    virtual void commit() {
        // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
        // index, we should never see it again anyway.
        //������������ɾ��������
        _cce->_engine->dropIdent(_opCtx, _ident).transitional_ignore();
    }

    OperationContext* const _opCtx;
    KVCollectionCatalogEntry* const _cce;
    const std::string _ident;
};

//KVDatabaseCatalogEntryBase::createCollection��KVDatabaseCatalogEntryBase::initCollection����������:
// 1. KVDatabaseCatalogEntryBase::createCollection��Ӧ�����¼��ı�
// 2. KVDatabaseCatalogEntryBase::initCollection��Ӧ����mongod��������_mdb_catalog.wtԪ�����ļ��м��صı�

//KVDatabaseCatalogEntryBase::createCollection��KVDatabaseCatalogEntryBase::initCollection�������ϵ�ʱ��new����
KVCollectionCatalogEntry::KVCollectionCatalogEntry(KVEngine* engine,
                                                   KVCatalog* catalog,
                                                   StringData ns,
                                                   StringData ident,
                                                   std::unique_ptr<RecordStore> rs)
    : BSONCollectionCatalogEntry(ns),
      //WiredTigerKVEngine
      _engine(engine),  
      //KVDatabaseCatalogEntryBase
      _catalog(catalog),
      //��¼�¼��϶�ӦԪ������Ϣ��Ҳ���Ǽ���·��  ����uuid �����������Լ���Ԫ����_mdb_catalog.wt�е�λ��
      _ident(ident.toString()),
      //StandardWiredTigerRecordStore
      _recordStore(std::move(rs)) {}

KVCollectionCatalogEntry::~KVCollectionCatalogEntry() {}

//IndexCatalogEntryImpl::setMultikey�е��ã���indexname��Ӧ������multikey�ֶθ�ֵ
//�Ƿ�ΪMultikey Indexes�������ͣ�����������Ԫ����
bool KVCollectionCatalogEntry::setIndexIsMultikey(OperationContext* opCtx,
                                                  StringData indexName,
                                                  const MultikeyPaths& multikeyPaths) {
    MetaData md = _getMetaData(opCtx);

	//�ҵ�������������������±�
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);

    const bool tracksPathLevelMultikeyInfo = !md.indexes[offset].multikeyPaths.empty();
    if (tracksPathLevelMultikeyInfo) {
        invariant(!multikeyPaths.empty());
        invariant(multikeyPaths.size() == md.indexes[offset].multikeyPaths.size());
    } else {
        invariant(multikeyPaths.empty());

        if (md.indexes[offset].multikey) {
            // The index is already set as multikey and we aren't tracking path-level multikey
            // information for it. We return false to indicate that the index metadata is unchanged.
            return false;
        }
    }

    md.indexes[offset].multikey = true;

    if (tracksPathLevelMultikeyInfo) {
        bool newPathIsMultikey = false;
        bool somePathIsMultikey = false;

        // Store new path components that cause this index to be multikey in catalog's index
        // metadata.
        for (size_t i = 0; i < multikeyPaths.size(); ++i) {
            std::set<size_t>& indexMultikeyComponents = md.indexes[offset].multikeyPaths[i];
            for (const auto multikeyComponent : multikeyPaths[i]) {
                auto result = indexMultikeyComponents.insert(multikeyComponent);
                newPathIsMultikey = newPathIsMultikey || result.second;
                somePathIsMultikey = true;
            }
        }

        // If all of the sets in the multikey paths vector were empty, then no component of any
        // indexed field caused the index to be multikey. setIndexIsMultikey() therefore shouldn't
        // have been called.
        invariant(somePathIsMultikey);

        if (!newPathIsMultikey) {
            // We return false to indicate that the index metadata is unchanged.
            return false;
        }
    }

	//����Ԫ����
    _catalog->putMetaData(opCtx, ns().toString(), md);
    return true;
}

//head��Ա��ֵ
void KVCollectionCatalogEntry::setIndexHead(OperationContext* opCtx,
                                            StringData indexName,
                                            const RecordId& newHead) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    md.indexes[offset].head = newHead;
    _catalog->putMetaData(opCtx, ns().toString(), md);
}

//�Ӹñ�MetaDataԪ�����������index������������ļ�
//IndexCatalogImpl::_deleteIndexFromDisk����
Status KVCollectionCatalogEntry::removeIndex(OperationContext* opCtx, StringData indexName) {
	//��ȡԪ����md
	MetaData md = _getMetaData(opCtx);

	//û�ҵ���ֱ�ӷ���
    if (md.findIndexOffset(indexName) < 0)
        return Status::OK();  // never had the index so nothing to do.

	//��ȡ��index��Ӧ·���ļ���
    const string ident = _catalog->getIndexIdent(opCtx, ns().ns(), indexName);

	//BSONCollectionCatalogEntry::MetaData::eraseIndex
	//��Ԫ����md�б������index
    md.eraseIndex(indexName);
    _catalog->putMetaData(opCtx, ns().toString(), md);

    // Lazily remove to isolate underlying engine from rollback.
    //ɾ�������¼���¼�����������������ļ�ɾ��������
    //RemoveIndexChange commit���������������ļ����
    opCtx->recoveryUnit()->registerChange(new RemoveIndexChange(opCtx, this, ident));
    return Status::OK();
}

//
//DatabaseImpl::createCollection->IndexCatalogImpl::createIndexOnEmptyCollection->IndexCatalogImpl::IndexBuildBlock::init
//->KVCollectionCatalogEntry::prepareForIndexBuild

//��backgroud������ʽ����������׼���������������洢�����Ӧ����Ŀ¼�ļ�
Status KVCollectionCatalogEntry::prepareForIndexBuild(OperationContext* opCtx,
                                                      const IndexDescriptor* spec) {
	//BSONCollectionCatalogEntry::MetaData
	MetaData md = _getMetaData(opCtx);

    KVPrefix prefix = KVPrefix::getNextPrefix(ns());
    IndexMetaData imd(spec->infoObj(), false, RecordId(), false, prefix);
	//btree��������
    if (indexTypeSupportsPathLevelMultikeyTracking(spec->getAccessMethodName())) {
        const auto feature =
            KVCatalog::FeatureTracker::RepairableFeature::kPathLevelMultikeyTracking;
        if (!_catalog->getFeatureTracker()->isRepairableFeatureInUse(opCtx, feature)) {
            _catalog->getFeatureTracker()->markRepairableFeatureAsInUse(opCtx, feature);
        }
        imd.multikeyPaths = MultikeyPaths{static_cast<size_t>(spec->keyPattern().nFields())};
    }

    // Mark collation feature as in use if the index has a non-simple collation.
    if (imd.spec["collation"]) {
        const auto feature = KVCatalog::FeatureTracker::NonRepairableFeature::kCollation;
        if (!_catalog->getFeatureTracker()->isNonRepairableFeatureInUse(opCtx, feature)) {
            _catalog->getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx, feature);
        }
    }

	//BSONCollectionCatalogEntry::MetaData::indexes��ֵ
    md.indexes.push_back(imd);
	//KVCatalog::putMetaData
    _catalog->putMetaData(opCtx, ns().toString(), md);

	//KVCatalog::getIndexIdent  
	//��ȡ������Ӧ�ļ�Ŀ¼����
    string ident = _catalog->getIndexIdent(opCtx, ns().ns(), spec->indexName());

	//WiredTigerKVEngine::createGroupedSortedDataInterface
    const Status status = _engine->createGroupedSortedDataInterface(opCtx, ident, spec, prefix);
    if (status.isOK()) {
        opCtx->recoveryUnit()->registerChange(new AddIndexChange(opCtx, this, ident));
    }

    return status;
}

//IndexCatalogImpl::IndexBuildBlock::success()�е���
//����Ԫ����"_mdb_catalog.wt"��Ϣ�е�����
void KVCollectionCatalogEntry::indexBuildSuccess(OperationContext* opCtx, StringData indexName) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(indexName);
    invariant(offset >= 0);
    md.indexes[offset].ready = true;
    _catalog->putMetaData(opCtx, ns().toString(), md);
}

void KVCollectionCatalogEntry::updateTTLSetting(OperationContext* opCtx,
                                                StringData idxName,
                                                long long newExpireSeconds) {
    MetaData md = _getMetaData(opCtx);
    int offset = md.findIndexOffset(idxName);
    invariant(offset >= 0);
    md.indexes[offset].updateTTLSetting(newExpireSeconds);
    _catalog->putMetaData(opCtx, ns().toString(), md);
}

//_collModInternal  syncFixUp����
void KVCollectionCatalogEntry::addUUID(OperationContext* opCtx,
                                       CollectionUUID uuid,
                                       Collection* coll) {
    // Add a UUID to CollectionOptions if a UUID does not yet exist.
    MetaData md = _getMetaData(opCtx);
    if (!md.options.uuid) {
		
        md.options.uuid = uuid;
        _catalog->putMetaData(opCtx, ns().toString(), md);
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx->getServiceContext());
		//UUIDCatalog::onCreateCollection
        catalog.onCreateCollection(opCtx, coll, uuid);
    } else {
        fassert(40564, md.options.uuid.get() == uuid);
    }
}


void KVCollectionCatalogEntry::removeUUID(OperationContext* opCtx) {
    // Remove the UUID from CollectionOptions if a UUID exists.
    MetaData md = _getMetaData(opCtx);
    if (md.options.uuid) {
        CollectionUUID uuid = md.options.uuid.get();
        md.options.uuid = boost::none;
        _catalog->putMetaData(opCtx, ns().toString(), md);
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx->getServiceContext());
        Collection* coll = catalog.lookupCollectionByUUID(uuid);
        if (coll) {
            catalog.onDropCollection(opCtx, uuid);
        }
    }
}

bool KVCollectionCatalogEntry::isEqualToMetadataUUID(OperationContext* opCtx,
                                                     OptionalCollectionUUID uuid) {
    MetaData md = _getMetaData(opCtx);
    if (uuid) {
        return md.options.uuid && md.options.uuid.get() == uuid.get();
    } else {
        return !md.options.uuid;
    }
}

void KVCollectionCatalogEntry::updateFlags(OperationContext* opCtx, int newValue) {
    MetaData md = _getMetaData(opCtx);
    md.options.flags = newValue;
    md.options.flagsSet = true;
    _catalog->putMetaData(opCtx, ns().toString(), md);
}

void KVCollectionCatalogEntry::updateValidator(OperationContext* opCtx,
                                               const BSONObj& validator,
                                               StringData validationLevel,
                                               StringData validationAction) {
    MetaData md = _getMetaData(opCtx);
    md.options.validator = validator;
    md.options.validationLevel = validationLevel.toString();
    md.options.validationAction = validationAction.toString();
    _catalog->putMetaData(opCtx, ns().toString(), md);
}

void KVCollectionCatalogEntry::setIsTemp(OperationContext* opCtx, bool isTemp) {
    MetaData md = _getMetaData(opCtx);
    md.options.temp = isTemp;
    _catalog->putMetaData(opCtx, ns().toString(), md);
}

void KVCollectionCatalogEntry::updateCappedSize(OperationContext* opCtx, long long size) {
    MetaData md = _getMetaData(opCtx);
    md.options.cappedSize = size;
	//KVCatalog::putMetaData
    _catalog->putMetaData(opCtx, ns().toString(), md);
}

//��ȡMetaData��Ϣ(Ҳ���Ǳ��������Ϣ)����MetaData�ĸ���Ҳ���Ǹ����������ؽӿڵ���_catalog->putMetaDataʵ��
BSONCollectionCatalogEntry::MetaData KVCollectionCatalogEntry::_getMetaData(
    OperationContext* opCtx) const {
    //KVCatalog::getMetaData
    return _catalog->getMetaData(opCtx, ns().toString());
}
}
