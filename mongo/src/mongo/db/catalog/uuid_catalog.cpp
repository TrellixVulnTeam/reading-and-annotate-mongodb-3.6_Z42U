/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "uuid_catalog.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

//һ�����Ӧһ��UUID��ͨ�����������������һ��UUID������Ŀ¼������ͨ��UUID���и�Ч�ļ��ϲ���

namespace mongo {
namespace {
//����һ��ȫ��getCatalog
const ServiceContext::Decoration<UUIDCatalog> getCatalog =
    ServiceContext::declareDecoration<UUIDCatalog>();
}  // namespace

//��ȡ��svcCtx��Ӧ��UUIDCatalog��Ϣ,DatabaseImpl::_getOrCreateCollectionInstance�е���
//��ȡȫ��UUIDCatalog��Ҳ���������getCatalog
UUIDCatalog& UUIDCatalog::get(ServiceContext* svcCtx) {
    return getCatalog(svcCtx);
}

//�ο�DatabaseHolderImpl::close����
//��ȡ��opCtx��ӦUUIDCatalog��Ϣ
UUIDCatalog& UUIDCatalog::get(OperationContext* opCtx) {
    return getCatalog(opCtx->getServiceContext());
}

//DatabaseImpl::_getOrCreateCollectionInstance����
void UUIDCatalog::onCreateCollection(OperationContext* opCtx,
                                     Collection* coll,
                                     CollectionUUID uuid) {
	//���uuid
	removeUUIDCatalogEntry(uuid);
	//UUIDCatalog::registerUUIDCatalogEntry
    registerUUIDCatalogEntry(uuid, coll);
    opCtx->recoveryUnit()->onRollback([this, uuid] { removeUUIDCatalogEntry(uuid); });
}

//KVCollectionCatalogEntry::removeUUID  OpObserverImpl::onDropCollection����  
void UUIDCatalog::onDropCollection(OperationContext* opCtx, CollectionUUID uuid) {
    Collection* foundColl = removeUUIDCatalogEntry(uuid);
    opCtx->recoveryUnit()->onRollback(
        [this, foundColl, uuid] { registerUUIDCatalogEntry(uuid, foundColl); });
}

void UUIDCatalog::onRenameCollection(OperationContext* opCtx,
                                     GetNewCollectionFunction getNewCollection,
                                     CollectionUUID uuid) {
    Collection* oldColl = removeUUIDCatalogEntry(uuid);
    opCtx->recoveryUnit()->onCommit([this, getNewCollection, uuid] {
        // Reset current UUID entry in case some other operation updates the UUID catalog before the
        // WUOW is committed. registerUUIDCatalogEntry() is a no-op if there's an existing UUID
        // entry.
        removeUUIDCatalogEntry(uuid);
        auto newColl = getNewCollection();
        invariant(newColl);
        registerUUIDCatalogEntry(uuid, newColl);
    });
    opCtx->recoveryUnit()->onRollback([this, oldColl, uuid] {
        // Reset current UUID entry in case some other operation updates the UUID catalog before the
        // WUOW is rolled back. registerUUIDCatalogEntry() is a no-op if there's an existing UUID
        // entry.
        removeUUIDCatalogEntry(uuid);
        registerUUIDCatalogEntry(uuid, oldColl);
    });
}

//�����DB��������б�uuid��Ϣ  DatabaseHolderImpl::close����
void UUIDCatalog::onCloseDatabase(Database* db) {
    for (auto&& coll : *db) {
        if (coll->uuid()) {
            // While the collection does not actually get dropped, we're going to destroy the
            // Collection object, so for purposes of the UUIDCatalog it looks the same.
            removeUUIDCatalogEntry(coll->uuid().get());
        }
    }
}
//AutoGetDb::AutoGetDb����AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get��DatabaseHolderImpl._dbs������һ�ȡDatabase
//DatabaseImpl::createCollection����collection�ı�ȫ����ӵ�DatabaseImpl._collections������
//AutoGetCollection::AutoGetCollectionͨ��Database::getCollection����UUIDCatalog::lookupCollectionByUUID(��UUIDCatalog._catalog����ͨ������uuid���Ի�ȡcollection����Ϣ)
//ע��AutoGetCollection::AutoGetCollection���캯��������uuid��Ҳ��һ�����캯����nss��Ҳ���ǿ���ͨ��uuid���ң�Ҳ����ͨ��nss����

//UUIDCatalog::registerUUIDCatalogEntry���uuid��collection��_catalog��lookupCollectionByUUID�в���
//AutoGetCollection::AutoGetCollection����

//һ�����Ӧһ��UUID��ͨ�����������������һ��UUID������Ŀ¼������ͨ��UUID���и�Ч�ļ��ϲ���

Collection* UUIDCatalog::lookupCollectionByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second;
}

//һ�����Ӧһ��UUID��ͨ�����������������һ��UUID������Ŀ¼������ͨ��UUID���и�Ч�ļ��ϲ���

NamespaceString UUIDCatalog::lookupNSSByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    Collection* coll = foundIt == _catalog.end() ? nullptr : foundIt->second;
    return foundIt == _catalog.end() ? NamespaceString() : coll->ns();
}

//�����µ�<CollectionUUID, Collection*>�ԣ���ӵ�_catalog map����
//DatabaseImpl::_getOrCreateCollectionInstance����
void UUIDCatalog::registerUUIDCatalogEntry(CollectionUUID uuid, Collection* coll) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    if (coll && !_catalog.count(uuid)) {
        // Invalidate this database's ordering, since we're adding a new UUID.
    //���һ��db�µ�uuid��˵����ά�����е�ͬһ��db�����uuid��Ҫ���������ˣ��������������db���
    //����ͨ�������UUIDCatalog::_getOrdering_inlock��������
        _orderedCollections.erase(coll->ns().db());

        std::pair<CollectionUUID, Collection*> entry = std::make_pair(uuid, coll);
        LOG(2) << "registering collection " << coll->ns() << " with UUID " << uuid.toString();
        invariant(_catalog.insert(entry).second == true);
    }
}

//��_catalog��_orderedCollections map�������uuid��Ϣ
Collection* UUIDCatalog::removeUUIDCatalogEntry(CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    auto foundIt = _catalog.find(uuid);
    if (foundIt == _catalog.end())
        return nullptr;

    // Invalidate this database's ordering, since we're deleting a UUID.
    //���һ��db�µ�uuid��˵����ά�����е�ͬһ��db�����uuid��Ҫ���������ˣ��������������db���
    //����ͨ�������UUIDCatalog::_getOrdering_inlock��������
    _orderedCollections.erase(foundIt->second->ns().db());

    auto foundCol = foundIt->second;
    LOG(2) << "unregistering collection " << foundCol->ns() << " with UUID " << uuid.toString();
    _catalog.erase(foundIt);
    return foundCol;
}

//ͬһ��db�����uuid���ź���ģ������Ϳ��Կ��ٻ�ȡuuid��ǰһ��id
boost::optional<CollectionUUID> UUIDCatalog::prev(const StringData& db, CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    const auto& ordering = _getOrdering_inlock(db, lock);
    auto current = std::lower_bound(ordering.cbegin(), ordering.cend(), uuid);

    // If the element does not appear, or is the first element.
    if (current == ordering.cend() || *current != uuid || current == ordering.cbegin()) {
        return boost::none;
    }

    return *(current - 1);
}

//ͬһ��db�����uuid���ź���ģ������Ϳ��Կ��ٻ�ȡuuid����һ��id
boost::optional<CollectionUUID> UUIDCatalog::next(const StringData& db, CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    const auto& ordering = _getOrdering_inlock(db, lock);
    auto current = std::lower_bound(ordering.cbegin(), ordering.cend(), uuid);

    if (current == ordering.cend() || *current != uuid || current + 1 == ordering.cend()) {
        return boost::none;
    }

    return *(current + 1);
}

//����_catalog map���е�uuid������������ú����_orderedCollections
//���uuid�������ź����_orderedCollections[i][j]��λ�����У���ֱ�ӻ�ȡ
//��������ڣ���˵����Ҫ��������
const std::vector<CollectionUUID>& UUIDCatalog::_getOrdering_inlock(
    const StringData& db, const stdx::lock_guard<stdx::mutex>&) {
    // If an ordering is already cached,
    auto it = _orderedCollections.find(db);
    if (it != _orderedCollections.end()) {
        // return it.
        return it->second;
    }
	
    //����_catalog map���е�uuid������������ú����_orderedCollections
    //ͬһ��DB�����CollectionUUID���ŵ�һ�𣬷ŵ���λ����ĵ�һ�㣬����_orderedCollections[db][]

    // Otherwise, get all of the UUIDs for this database,
    auto& newOrdering = _orderedCollections[db];
    for (const auto& pair : _catalog) {
        if (pair.second->ns().db() == db) {
            newOrdering.push_back(pair.first);
        }
    }

    // and sort them.
    std::sort(newOrdering.begin(), newOrdering.end());

    return newOrdering;
}
}  // namespace mongo
