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

#pragma once

#include <unordered_map>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * This class comprises a UUID to collection catalog, allowing for efficient
 * collection lookup by UUID.
 ��������һ��UUID������Ŀ¼������ͨ��UUID���и�Ч�ļ��ϲ���
 */
using CollectionUUID = UUID;
class Database;

/*
//����һ��ȫ��getCatalog
const ServiceContext::Decoration<UUIDCatalog> getCatalog =
    ServiceContext::declareDecoration<UUIDCatalog>();
}  // namespace
���б��uuidͨ�����ȫ��getCatalog������������
*/
//һ�����Ӧһ��UUID��ͨ�����������������һ��UUID������Ŀ¼������ͨ��UUID���и�Ч�ļ��ϲ���
class UUIDCatalog {
    MONGO_DISALLOW_COPYING(UUIDCatalog);

public:
    static UUIDCatalog& get(ServiceContext* svcCtx);
    static UUIDCatalog& get(OperationContext* opCtx);
    UUIDCatalog() = default;

    /**
     * This function inserts the entry for uuid, coll into the UUID Collection. It is called by
     * the op observer when a collection is created.
     */
    void onCreateCollection(OperationContext* opCtx, Collection* coll, CollectionUUID uuid);

    /**
     * This function removes the entry for uuid from the UUID catalog. It is called by the op
     * observer when a collection is dropped.
     */
    void onDropCollection(OperationContext* opCtx, CollectionUUID uuid);

    /**
     * Combination of onDropCollection and onCreateCollection.
     * 'getNewCollection' is a function that returns collection to be registered when the current
     * write unit of work is committed.
     */
    using GetNewCollectionFunction = stdx::function<Collection*()>;
    void onRenameCollection(OperationContext* opCtx,
                            GetNewCollectionFunction getNewCollection,
                            CollectionUUID uuid);

    /**
     * Implies onDropCollection for all collections in db, but is not transactional.
     */
    void onCloseDatabase(Database* db);

    void registerUUIDCatalogEntry(CollectionUUID uuid, Collection* coll);
    Collection* removeUUIDCatalogEntry(CollectionUUID uuid);

    /* This function gets the Collection* pointer that corresponds to
     * CollectionUUID uuid. The required locks should be obtained prior
     * to calling this function, or else the found Collection pointer
     * might no longer be valid when the call returns.
     */
    Collection* lookupCollectionByUUID(CollectionUUID uuid) const;

    /* This function gets the NamespaceString from the Collection* pointer that
     * corresponds to CollectionUUID uuid. If there is no such pointer, an empty
     * NamespaceString is returned.
     */
    NamespaceString lookupNSSByUUID(CollectionUUID uuid) const;

    /**
     * Return the UUID lexicographically preceding `uuid` in the database named by `db`.
     *
     * Return `boost::none` if `uuid` is not found, or is the first UUID in that database.
     */
    boost::optional<CollectionUUID> prev(const StringData& db, CollectionUUID uuid);

    /**
     * Return the UUID lexicographically following `uuid` in the database named by `db`.
     *
     * Return `boost::none` if `uuid` is not found, or is the last UUID in that database.
     */
    boost::optional<CollectionUUID> next(const StringData& db, CollectionUUID uuid);

private:
    const std::vector<CollectionUUID>& _getOrdering_inlock(const StringData& db,
                                                           const stdx::lock_guard<stdx::mutex>&);

    mutable mongo::stdx::mutex _catalogLock;

    /**
     * Map from database names to ordered `vector`s of their UUIDs.
     *
     * Works as a cache of such orderings: every ordering in this map is guaranteed to be valid, but
     * not all databases are guaranteed to have an ordering in it.
     */
    //��ֵ�ο�UUIDCatalog::_getOrdering_inlock 
    //����_catalog map���е�uuid������������ú����_orderedCollections
    //ͬһ��DB�����CollectionUUID���ŵ�һ�𣬷ŵ���λ����ĵ�һ�㣬����_orderedCollections[db][]
    StringMap<std::vector<CollectionUUID>> _orderedCollections;
    //UUIDCatalog::registerUUIDCatalogEntry���uuid��collection��_catalog��lookupCollectionByUUID�в���

   
    //AutoGetDb::AutoGetDb����AutoGetOrCreateDb::AutoGetOrCreateDb->DatabaseHolderImpl::get��DatabaseHolderImpl._dbs������һ�ȡDatabase
    //DatabaseImpl::createCollection����collection�ı�ȫ����ӵ�DatabaseImpl._collections������
    //AutoGetCollection::AutoGetCollectionͨ��Database::getCollection����UUIDCatalog::lookupCollectionByUUID(��UUIDCatalog._catalog����ͨ������uuid���Ի�ȡcollection����Ϣ)
    //ע��AutoGetCollection::AutoGetCollection���캯��������uuid��Ҳ��һ�����캯����nss��Ҳ���ǿ���ͨ��uuid���ң�Ҳ����ͨ��nss����
   //��������DB��uuid��Ϣ������ź������_orderedCollections[i][j]��ά���飬ͬһ��DB��i��ͬ��ͨ��j����ͬһ��db�Ĳ�ͬ��uuid
   mongo::stdx::unordered_map<CollectionUUID, Collection*, CollectionUUID::Hash> _catalog;
};

}  // namespace mongo
