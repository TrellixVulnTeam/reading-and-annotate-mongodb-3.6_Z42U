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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/string_map.h"

namespace mongo {

class CachedDatabaseInfo;
class CachedCollectionRoutingInfo;
class OperationContext;

static constexpr int kMaxNumStaleVersionRetries = 10;

/**
 * This is the root of the "read-only" hierarchy of cached catalog metadata. It is read only
 * in the sense that it only reads from the persistent store, but never writes to it. Instead
 * writes happen through the ShardingCatalogManager and the cache hierarchy needs to be invalidated.
 //MetadataManager��CatalogCache���Բο�https://developer.aliyun.com/article/778536?spm=a2c6h.17698244.wenzhang.9.7b934d126DdIOU

ǿ��·��ˢ��db.adminCommand( { flushRouterConfig: 1 } )��In MongoDB 4.0.5 and earlier (and 3.6.10 and earlier)���ϰ汾��֧��
 */ 
//cfg��ӦConfigServerCatalogCacheLoader��mongod��ӦReadOnlyCatalogCacheLoader(ֻ���ڵ�)����ConfigServerCatalogCacheLoader(mongodʵ��)
//��initializeGlobalShardingStateForMongod��mongos��ӦConfigServerCatalogCacheLoader����runMongosServer

//Grid._catalogCache��ԱΪ����  ·����Ϣ���浽����
class CatalogCache {
    MONGO_DISALLOW_COPYING(CatalogCache);

public:
    CatalogCache(CatalogCacheLoader& cacheLoader);
    ~CatalogCache();

    /**
     * Retrieves the cached metadata for the specified database. The returned value is still owned
     * by the cache and should not be kept elsewhere. I.e., it should only be used as a local
     * variable. The reason for this is so that if the cache gets invalidated, the caller does not
     * miss getting the most up-to-date value.
     *
     * Returns the database cache entry if the database exists or a failed status otherwise.
     */
    StatusWith<CachedDatabaseInfo> getDatabase(OperationContext* opCtx, StringData dbName);

    /**
     * Blocking shortcut method to get a specific sharded collection from a given database using the
     * complete namespace. If the collection is sharded returns a ScopedChunkManager initialized
     * with ChunkManager. If the collection is not sharded, returns a ScopedChunkManager initialized
     * with the primary shard for the specified database. If an error occurs loading the metadata
     * returns a failed status.
     */
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                                     const NamespaceString& nss);
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                                     StringData ns);

    /**
     * Same as getCollectionRoutingInfo above, but in addition causes the namespace to be refreshed.
     */
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfoWithRefresh above, but in addition returns a
     * NamespaceNotSharded error if the collection is not sharded.
     */
    StatusWith<CachedCollectionRoutingInfo> getShardedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);
    StatusWith<CachedCollectionRoutingInfo> getShardedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, StringData ns);

    /**
     * Non-blocking method to be called whenever using the specified routing table has encountered a
     * stale config exception. Returns immediately and causes the routing table to be refreshed the
     * next time getCollectionRoutingInfo is called. Does nothing if the routing table has been
     * refreshed already.
     */
    void onStaleConfigError(CachedCollectionRoutingInfo&&);

    /**
     * Non-blocking method, which indiscriminately causes the routing table for the specified
     * namespace to be refreshed the next time getCollectionRoutingInfo is called.
     */
    void invalidateShardedCollection(const NamespaceString& nss);
    void invalidateShardedCollection(StringData ns);

    /**
     * Non-blocking method, which removes the entire specified database (including its collections)
     * from the cache.
     */
    void purgeDatabase(StringData dbName);

    /**
     * Non-blocking method, which removes all databases (including their collections) from the
     * cache.
     */
    void purgeAllDatabases();

private:
    // Make the cache entries friends so they can access the private classes below
    friend class CachedDatabaseInfo;
    friend class CachedCollectionRoutingInfo;

    /**
     * Cache entry describing a collection.
     * CollectionRoutingInfoEntry�洢���chunk��Ϣ��CachedCollectionRoutingInfo�洢�������Ƭ��Ϣ
     */ 
     //CachedDatabaseInfo._db.collections��ԱΪ������,
     //CatalogCache::_scheduleCollectionRefresh  CatalogCache::_getDatabaseˢ�¼���·����Ϣ
    struct CollectionRoutingInfoEntry {
        // Specifies whether this cache entry needs a refresh (in which case routingInfo should not
        // be relied on) or it doesn't, in which case there should be a non-null routingInfo.
        //�Ƿ���Ҫˢ�¼���·����Ϣ��
        //CatalogCache::onStaleConfigError�� invalidateShardedCollection CatalogCache::_getDatabase������Ϊtrue
        // CatalogCache::_scheduleCollectionRefresh����Ϊfalse

        //CatalogCache::onStaleConfigError��CatalogCache::invalidateShardedCollection����ǿ��·��ˢ�£�
        //  ͨ��CatalogCache::_scheduleCollectionRefresh��ȡ����·�ɺ�����ΪΪfalse
        //���ΪTure��ͨ��CatalogCache::getCollectionRoutingInfo��ȡ����·����Ϣ
        //������������������һ������ˢ��·�ɺ󣬺�������������ˢ·����
        bool needsRefresh{true};

        // Contains a notification to be waited on for the refresh to complete (only available if
        // needsRefresh is true)
        //���needsRefresh=ture����ˢ�º�ص�����
        std::shared_ptr<Notification<Status>> refreshCompletionNotification;

        // Contains the cached routing information (only available if needsRefresh is false)
        //�ñ��·����Ϣ
        std::shared_ptr<ChunkManager> routingInfo;
    };

    /**
     * Cache entry describing a database.
     */ //CachedDatabaseInfo._db  CatalogCache._databasesΪ������
    struct DatabaseInfoEntry {
        //��DB������ƬID
        ShardId primaryShardId;

        bool shardingEnabled;

        //��DB�����collections����Ϣ
        StringMap<CollectionRoutingInfoEntry> collections; 
    };

    using DatabaseInfoMap = StringMap<std::shared_ptr<DatabaseInfoEntry>>;

    /**
     * Ensures that the specified database is in the cache, loading it if necessary. If the database
     * was not in cache, all the sharded collections will be in the 'needsRefresh' state.
     */
    std::shared_ptr<DatabaseInfoEntry> _getDatabase(OperationContext* opCtx, StringData dbName);

    /**
     * Non-blocking call which schedules an asynchronous refresh for the specified namespace. The
     * namespace must be in the 'needRefresh' state.
     */
    void _scheduleCollectionRefresh(WithLock,
                                    std::shared_ptr<DatabaseInfoEntry> dbEntry,
                                    std::shared_ptr<ChunkManager> existingRoutingInfo,
                                    NamespaceString const& nss,
                                    int refreshAttempt);

    // Interface from which chunks will be retrieved
    //CatalogCache::_scheduleCollectionRefresh���ã���ȡchunk��Ϣ
    CatalogCacheLoader& _cacheLoader;

    // Mutex to serialize access to the structures below
    stdx::mutex _mutex;

    // Map from DB name to the info for that database
    //�������е�chunk��Ϣ����¼�������棬һ��DB��Ӧmap�е�һ����Ա
    DatabaseInfoMap _databases; 
};

/**
 * Constructed exclusively by the CatalogCache, contains a reference to the cached information for
 * the specified database.
 */
//����Ϣ����������ÿ�ӵ�ж�Ӧ�ı���Ϣ�����Ӧ��chunk��Ϣ��CachedCollectionRoutingInfo
class CachedDatabaseInfo {
public:
    const ShardId& primaryId() const;

    bool shardingEnabled() const;

private:
    friend class CatalogCache;

    CachedDatabaseInfo(std::shared_ptr<CatalogCache::DatabaseInfoEntry> db);


    //
    std::shared_ptr<CatalogCache::DatabaseInfoEntry> _db;
};

/**
 * Constructed exclusively by the CatalogCache contains a reference to the routing information for
 * the specified collection.
 */
//CatalogCache::getCollectionRoutingInfo�й���ʹ�� CatalogCache::getCollectionRoutingInfo
//ChunkManagerTargeter._routingInfo��ԱΪ�����ͣ�
//CollectionRoutingInfoEntry�洢���chunk��Ϣ��CachedCollectionRoutingInfo�洢�������Ƭ��chunk·����Ϣ

//ע��CollectionMetadata(mongod�ڴ���·����Ϣ)��CachedCollectionRoutingInfo(mongos�ڴ���·����Ϣ)����
class CachedCollectionRoutingInfo { //getShardedCollection
public:
    /**
     * Returns the ID of the primary shard for the database owining this collection, regardless of
     * whether it is sharded or not.
     */
    const ShardId& primaryId() const {
        return _primaryId;
    }

    /**
     * If the collection is sharded, returns a chunk manager for it. Otherwise, nullptr.
     */
    std::shared_ptr<ChunkManager> cm() const {
        return _cm;
    }

    /**
     * If the collection is not sharded, returns its primary shard. Otherwise, nullptr.
     */
    std::shared_ptr<Shard> primary() const {
        return _primary;
    }

private:
    friend class CatalogCache;

    CachedCollectionRoutingInfo(ShardId primaryId, std::shared_ptr<ChunkManager> cm);

    CachedCollectionRoutingInfo(ShardId primaryId,
                                NamespaceString nss,
                                std::shared_ptr<Shard> primary);

    // The id of the primary shard containing the database
    //�����_nss���϶�Ӧ����ƬID
    //���û�����÷�Ƭ�����ֻ������Ƭ��Ϣ����ʱ�������_cmҲû����
    ShardId _primaryId;

    // Reference to the corresponding chunk manager (if sharded) or null
    //�ü��ϵ�chunk�ֲ���Ϣ����CollectionRoutingInfoEntry.routingInfoһ��
    //��cm��Ϣ��˵����chunk��Ϣ��Ҳ�ʹ���ü��������˷�Ƭ���ܣ�����û�����÷�Ƭ���ܣ�ֻ������Ƭ��Ϣ�����Բο�ChunkManagerTargeter::targetCollection
    std::shared_ptr<ChunkManager> _cm;

    // Reference to the primary of the database (if not sharded) or null
    //db.collection����
    NamespaceString _nss;
    //db.collection��Ӧ����Ƭ
    std::shared_ptr<Shard> _primary;
};

}  // namespace mongo

