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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/migration_manager.h"

#include <memory>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/balancer/scoped_migration_request.h"
#include "mongo/db/s/balancer/type_migration.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/move_chunk_request.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::shared_ptr;
using std::vector;
using str::stream;

namespace {

const char kChunkTooBig[] = "chunkTooBig";  // TODO: delete in 3.8
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                Seconds(15));

/**
 * Parses the 'commandResponse' and converts it to a status to use as the outcome of the command.
 * Preserves backwards compatibility with 3.4 and earlier shards that, rather than use a ChunkTooBig
 * error code, include an extra field in the response.
 *
 * TODO: Delete in 3.8
 */
Status extractMigrationStatusFromCommandResponse(const BSONObj& commandResponse) {
    Status commandStatus = getStatusFromCommandResult(commandResponse);

    if (!commandStatus.isOK()) {
        bool chunkTooBig = false;
        bsonExtractBooleanFieldWithDefault(commandResponse, kChunkTooBig, false, &chunkTooBig)
            .transitional_ignore();
        if (chunkTooBig) {
            commandStatus = {ErrorCodes::ChunkTooBig, commandStatus.reason()};
        }
    }

    return commandStatus;
}


/**
 * Returns whether the specified status is an error caused by stepdown of the primary config node
 * currently running the balancer.
 */
bool isErrorDueToConfigStepdown(Status status, bool isStopping) {
    return ((status == ErrorCodes::CallbackCanceled && isStopping) ||
            status == ErrorCodes::BalancerInterrupted ||
            status == ErrorCodes::InterruptedDueToReplStateChange);
}

}  // namespace

MigrationManager::MigrationManager(ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

MigrationManager::~MigrationManager() {
    // The migration manager must be completely quiesced at destruction time
    invariant(_activeMigrations.empty());
}

//Դ��Ƭ�յ�config server���͹�����moveChunk����  
//ע��MoveChunkCmd��MoveChunkCommand������MoveChunkCmdΪ�����յ�mongo shell�ȿͻ��˵Ĵ������̣�
//Ȼ�����configsvr_client::moveChunk������_configsvrMoveChunk��config server,��config serverͳһ
//����movechunk��shardִ��chunk�������Ӷ�ִ��MoveChunkCommand::run�����shard��������shard��Ǩ��

//MoveChunkCommandΪshard�յ�movechunk�������������Ǩ�Ƶ����
//MoveChunkCmdΪmongos�յ��ͻ���movechunk����Ĵ������̣�ת����config server
//ConfigSvrMoveChunkCommandΪconfig server�յ�mongos��������_configsvrMoveChunk����Ĵ�������

//�Զ�balancer����shard������������Ǩ�������Balancer::_moveChunks->MigrationManager::executeMigrationsForAutoBalance
//�ֶ�balance��config�յ�����ConfigSvrMoveChunkCommand�����Ǩ�����Balancer::moveSingleChunk

//Balancer::_moveChunks����
MigrationStatuses MigrationManager::executeMigrationsForAutoBalance(
    OperationContext* opCtx,
    //��ҪǨ�ƵĿ� 
    const vector<MigrateInfo>& migrateInfos,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {

	//balance��chunkǨ��״̬��Ϣ��¼��������
    MigrationStatuses migrationStatuses;

    {	//
        std::map<MigrationIdentifier, ScopedMigrationRequest> scopedMigrationRequests;
        vector<std::pair<shared_ptr<Notification<RemoteCommandResponse>>, MigrateInfo>> responses;

		//������ҪǨ�Ƶ�chunk��Ϣ
        for (const auto& migrateInfo : migrateInfos) {
            // Write a document to the config.migrations collection, in case this migration must be
            // recovered by the Balancer. Fail if the chunk is already moving.
           // д��ҪǨ�Ƶ�chunk��Ϣ��config.migrations������г�ͻ����ʧ��
            auto statusWithScopedMigrationRequest = //ScopedMigrationRequest����
                ScopedMigrationRequest::writeMigration(opCtx, migrateInfo, waitForDelete);
            if (!statusWithScopedMigrationRequest.isOK()) {
				//��chunkд��config.migrations�쳣����¼������˵����chunkǨ����Ϣ�쳣
                migrationStatuses.emplace(migrateInfo.getName(),
                                          std::move(statusWithScopedMigrationRequest.getStatus()));
                continue;
            }
            scopedMigrationRequests.emplace(migrateInfo.getName(),
                                            std::move(statusWithScopedMigrationRequest.getValue()));

			//��Դ��Ƭ����moveChunk���ͬʱ��ȡ���ؽ������responses
            responses.emplace_back(
                _schedule(opCtx, migrateInfo, maxChunkSizeBytes, secondaryThrottle, waitForDelete),
                migrateInfo);
        }

        // Wait for all the scheduled migrations to complete.
        //��ȡ��Ӧresponese���ȴ���ѭ������һ��migrateInfosǨ�����
        for (auto& response : responses) {
            auto notification = std::move(response.first);
            auto migrateInfo = std::move(response.second);

            const auto& remoteCommandResponse = notification->get();

            auto it = scopedMigrationRequests.find(migrateInfo.getName());
            invariant(it != scopedMigrationRequests.end());
			//��ȡmoveChunk�Ľ��
            Status commandStatus =
                _processRemoteCommandResponse(remoteCommandResponse, &it->second);
            migrationStatuses.emplace(migrateInfo.getName(), std::move(commandStatus));
        }
    }

    invariant(migrationStatuses.size() == migrateInfos.size());

    return migrationStatuses;
}

//Balancer::moveSingleChunk  Balancer::rebalanceSingleChunk����
//�ֹ�movechunk������ߵ�����
Status MigrationManager::executeManualMigration(
    OperationContext* opCtx,
    const MigrateInfo& migrateInfo,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {
    _waitForRecovery();

    // Write a document to the config.migrations collection, in case this migration must be
    // recovered by the Balancer. Fail if the chunk is already moving.
    //д��ҪǨ�Ƶ�chunk��Ϣ��config.migrations������ͨ���ñ�鿴��ǰ����Ǩ�Ƶ�chunk��Ϣ
    auto statusWithScopedMigrationRequest =
        ScopedMigrationRequest::writeMigration(opCtx, migrateInfo, waitForDelete);
    if (!statusWithScopedMigrationRequest.isOK()) {
        return statusWithScopedMigrationRequest.getStatus();
    }

	//һֱ�ȵ�Դ��Ƭ��Ⱥ�Ѷ�Ӧ����Ǩ�Ƶ�Ŀ�ķ�Ƭ�ɹ�������Ż᷵��
    RemoteCommandResponse remoteCommandResponse =
        _schedule(opCtx, migrateInfo, maxChunkSizeBytes, secondaryThrottle, waitForDelete)->get();

    auto routingInfoStatus =
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
            opCtx, migrateInfo.ns);
    if (!routingInfoStatus.isOK()) {
        return routingInfoStatus.getStatus();
    }

    auto& routingInfo = routingInfoStatus.getValue();

    auto chunk = routingInfo.cm()->findIntersectingChunkWithSimpleCollation(migrateInfo.minKey);
    invariant(chunk);

    Status commandStatus = _processRemoteCommandResponse(
        remoteCommandResponse, &statusWithScopedMigrationRequest.getValue());

    // Migration calls can be interrupted after the metadata is committed but before the command
    // finishes the waitForDelete stage. Any failovers, therefore, must always cause the moveChunk
    // command to be retried so as to assure that the waitForDelete promise of a successful command
    // has been fulfilled.
    if (chunk->getShardId() == migrateInfo.to && commandStatus != ErrorCodes::BalancerInterrupted) {
        return Status::OK();
    }

    return commandStatus;
}

//Balancer::initiateBalancer����
void MigrationManager::startRecoveryAndAcquireDistLocks(OperationContext* opCtx) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_state == State::kStopped);
        invariant(_migrationRecoveryMap.empty());
        _state = State::kRecovering;
    }

    auto scopedGuard = MakeGuard([&] {
        _migrationRecoveryMap.clear();
        _abandonActiveMigrationsAndEnableManager(opCtx);
    });

    auto distLockManager = Grid::get(opCtx)->catalogClient()->getDistLockManager();

    // Load the active migrations from the config.migrations collection.
    auto statusWithMigrationsQueryResponse =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString(MigrationType::ConfigNS),
            BSONObj(),
            BSONObj(),
            boost::none);

    if (!statusWithMigrationsQueryResponse.isOK()) {
        log() << "Unable to read config.migrations collection documents for balancer migration"
              << " recovery. Abandoning balancer recovery."
              << causedBy(redact(statusWithMigrationsQueryResponse.getStatus()));
        return;
    }

    for (const BSONObj& migration : statusWithMigrationsQueryResponse.getValue().docs) {
        auto statusWithMigrationType = MigrationType::fromBSON(migration);
        if (!statusWithMigrationType.isOK()) {
            // The format of this migration document is incorrect. The balancer holds a distlock for
            // this migration, but without parsing the migration document we cannot identify which
            // distlock must be released. So we must release all distlocks.
            log() << "Unable to parse config.migrations document '" << redact(migration.toString())
                  << "' for balancer migration recovery. Abandoning balancer recovery."
                  << causedBy(redact(statusWithMigrationType.getStatus()));
            return;
        }
        MigrationType migrateType = std::move(statusWithMigrationType.getValue());

        auto it = _migrationRecoveryMap.find(NamespaceString(migrateType.getNss()));
        if (it == _migrationRecoveryMap.end()) {
            std::list<MigrationType> list;
            it = _migrationRecoveryMap.insert(std::make_pair(migrateType.getNss(), list)).first;

            // Reacquire the matching distributed lock for this namespace.
            const std::string whyMessage(stream() << "Migrating chunk(s) in collection "
                                                  << migrateType.getNss().ns());

            auto statusWithDistLockHandle = distLockManager->tryLockWithLocalWriteConcern(
                opCtx, migrateType.getNss().ns(), whyMessage, _lockSessionID);
            if (!statusWithDistLockHandle.isOK()) {
                log() << "Failed to acquire distributed lock for collection '"
                      << migrateType.getNss().ns()
                      << "' during balancer recovery of an active migration. Abandoning"
                      << " balancer recovery."
                      << causedBy(redact(statusWithDistLockHandle.getStatus()));
                return;
            }
        }

        it->second.push_back(std::move(migrateType));
    }

    scopedGuard.Dismiss();
}


//Balancer::_mainThread����
void MigrationManager::finishRecovery(OperationContext* opCtx,
                                      uint64_t maxChunkSizeBytes,
                                      const MigrationSecondaryThrottleOptions& secondaryThrottle) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state == State::kStopping) {
            _migrationRecoveryMap.clear();
            return;
        }

        // If recovery was abandoned in startRecovery, then there is no more to do.
        if (_state == State::kEnabled) {
            invariant(_migrationRecoveryMap.empty());
            return;
        }

        invariant(_state == State::kRecovering);
    }

    auto scopedGuard = MakeGuard([&] {
        _migrationRecoveryMap.clear();
        _abandonActiveMigrationsAndEnableManager(opCtx);
    });

    // Schedule recovered migrations.
    vector<ScopedMigrationRequest> scopedMigrationRequests;
    vector<shared_ptr<Notification<RemoteCommandResponse>>> responses;

    for (auto& nssAndMigrateInfos : _migrationRecoveryMap) {
        auto& nss = nssAndMigrateInfos.first;
        auto& migrateInfos = nssAndMigrateInfos.second;
        invariant(!migrateInfos.empty());

        auto routingInfoStatus =
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss);
        if (!routingInfoStatus.isOK()) {
            // This shouldn't happen because the collection was intact and sharded when the previous
            // config primary was active and the dist locks have been held by the balancer
            // throughout. Abort migration recovery.
            log() << "Unable to reload chunk metadata for collection '" << nss
                  << "' during balancer recovery. Abandoning recovery."
                  << causedBy(redact(routingInfoStatus.getStatus()));
            return;
        }

        auto& routingInfo = routingInfoStatus.getValue();

        int scheduledMigrations = 0;

        while (!migrateInfos.empty()) {
            auto migrationType = std::move(migrateInfos.front());
            const auto migrationInfo = migrationType.toMigrateInfo();
            auto waitForDelete = migrationType.getWaitForDelete();
            migrateInfos.pop_front();

            auto chunk =
                routingInfo.cm()->findIntersectingChunkWithSimpleCollation(migrationInfo.minKey);
            invariant(chunk);

            if (chunk->getShardId() != migrationInfo.from) {
                // Chunk is no longer on the source shard specified by this migration. Erase the
                // migration recovery document associated with it.
                ScopedMigrationRequest::createForRecovery(opCtx, nss, migrationInfo.minKey);
                continue;
            }

            scopedMigrationRequests.emplace_back(
                ScopedMigrationRequest::createForRecovery(opCtx, nss, migrationInfo.minKey));

            scheduledMigrations++;

            responses.emplace_back(_schedule(
                opCtx, migrationInfo, maxChunkSizeBytes, secondaryThrottle, waitForDelete));
        }

        // If no migrations were scheduled for this namespace, free the dist lock
        if (!scheduledMigrations) {
            Grid::get(opCtx)->catalogClient()->getDistLockManager()->unlock(
                opCtx, _lockSessionID, nss.ns());
        }
    }

    _migrationRecoveryMap.clear();
    scopedGuard.Dismiss();

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state == State::kRecovering) {
            _state = State::kEnabled;
            _condVar.notify_all();
        }
    }

    // Wait for each migration to finish, as usual.
    for (auto& response : responses) {
        response->get();
    }
}

//Balancer::interruptBalancer���ã�
//_migrationManagerInterruptThread�߳�ר�Ÿ���Ǩ���쳣����
void MigrationManager::interruptAndDisableMigrations() {
    executor::TaskExecutor* const executor =
        Grid::get(_serviceContext)->getExecutorPool()->getFixedExecutor();

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_state == State::kEnabled || _state == State::kRecovering);
    _state = State::kStopping;

    // Interrupt any active migrations with dist lock
    //���쳣balance�ı�����쳣����
    for (auto& cmsEntry : _activeMigrations) {
        auto& migrations = cmsEntry.second;

        for (auto& migration : migrations) {
            if (migration.callbackHandle) {
                executor->cancel(*migration.callbackHandle);
            }
        }
    }

    _checkDrained(lock);
}

//Balancer::_mainThread�е��ã�˵��balancer�ر���
void MigrationManager::drainActiveMigrations() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    if (_state == State::kStopped)
        return;
    invariant(_state == State::kStopping);
    _condVar.wait(lock, [this] { return _activeMigrations.empty(); });
    _state = State::kStopped;
}

//MigrationManager::executeMigrationsForAutoBalance MigrationManager::finishRecovery
//MigrationManager::executeManualMigration ����
//����ҪǨ�Ƶ�Դ��Ƭ����moveChunk���Ǩ��migrateInfo��Ӧ�Ŀ�
shared_ptr<Notification<RemoteCommandResponse>> 
  MigrationManager::_schedule(
    OperationContext* opCtx,
    const MigrateInfo& migrateInfo,
    uint64_t maxChunkSizeBytes,
    const MigrationSecondaryThrottleOptions& secondaryThrottle,
    bool waitForDelete) {
    const NamespaceString nss(migrateInfo.ns);

    // Ensure we are not stopped in order to avoid doing the extra work
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_state != State::kEnabled && _state != State::kRecovering) {
            return std::make_shared<Notification<RemoteCommandResponse>>(
                Status(ErrorCodes::BalancerInterrupted,
                       "Migration cannot be executed because the balancer is not running"));
        }
    }

	//��ȡǨ�Ƶ�Դ��Ƭ
    const auto fromShardStatus =
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, migrateInfo.from);
    if (!fromShardStatus.isOK()) {
        return std::make_shared<Notification<RemoteCommandResponse>>(
            std::move(fromShardStatus.getStatus()));
    }

    const auto fromShard = fromShardStatus.getValue();
	//��ȡԴ��Ƭ���ڵ�
    auto fromHostStatus = fromShard->getTargeter()->findHost(
        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!fromHostStatus.isOK()) {
        return std::make_shared<Notification<RemoteCommandResponse>>(
            std::move(fromHostStatus.getStatus()));
    }

    BSONObjBuilder builder;
	//����moveChunk��������   moveChunkʵ�����Ƿ��͸�Դ��Ƭ�ģ�ע�ⲻ�Ƿ��͸�cfg��
    MoveChunkRequest::appendAsCommand(
        &builder,
        nss,
        migrateInfo.version,
        repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString(),
        migrateInfo.from,
        migrateInfo.to,
        ChunkRange(migrateInfo.minKey, migrateInfo.maxKey),
        maxChunkSizeBytes,
        secondaryThrottle,
        waitForDelete);

    stdx::lock_guard<stdx::mutex> lock(_mutex);

	//balance disable��
    if (_state != State::kEnabled && _state != State::kRecovering) {
        return std::make_shared<Notification<RemoteCommandResponse>>(
            Status(ErrorCodes::BalancerInterrupted,
                   "Migration cannot be executed because the balancer is not running"));
    }

	//����Migration�� moveChunk�����
    Migration migration(nss, builder.obj());

    auto retVal = migration.completionNotification;

    _schedule(lock, opCtx, fromHostStatus.getValue(), std::move(migration));

    return retVal;
}

/*
���������chunks�ֲ�:
xx_HTZjQeZL_shard_1 571274
xx_HTZjQeZL_shard_2 319536
xx_HTZjQeZL_shard_3 572644
xx_HTZjQeZL_shard_4 707811
xx_HTZjQeZL_shard_QubQcjup	145339
xx_HTZjQeZL_shard_ewMvmPnE	136034
xx_HTZjQeZL_shard_jaAVvOei	129682
xx_HTZjQeZL_shard_kxYilhNF	150150

�����ѡ������ҪǨ�Ƶ�chunk
mongos> db.migrations.find()
{ "_id" : "xx_cold_data_db.xx_cold_data_db-user_id_\"359209050\"module_\"album\"md5_\"992F3FF0DDCB009D1A6CCD8647CEAFA5\"", "ns" : "xx_cold_data_db.xx_cold_data_db", "min" : { "user_id" : "359209050", "module" : "album", "md5" : "992F3FF0DDCB009D1A6CCD8647CEAFA5" }, "max" : { "xx" : "359209058", "module" : "album", "md5" : "49D552BEBEAE7D3CB6B53A7FE384E5A4" }, "fromShard" : "xx_HTZjQeZL_shard_1", "toShard" : "xx_HTZjQeZL_shard_QubQcjup", "chunkVersion" : [ Timestamp(588213, 1), ObjectId("5ec496373f311c50a0185499") ], "waitForDelete" : false }
{ "_id" : "xx_cold_data_db.xx_cold_data_db-user_id_\"278344065\"module_\"album\"md5_\"CAA4B99617A83D0DEE5CE30D4D75829F\"", "ns" : "xx_cold_data_db.xx_cold_data_db", "min" : { "user_id" : "278344065", "module" : "album", "md5" : "CAA4B99617A83D0DEE5CE30D4D75829F" }, "max" : { "xx" : "278344356", "module" : "album", "md5" : "E691E5F8756E723BB44BC2049D624F81" }, "fromShard" : "xx_HTZjQeZL_shard_4", "toShard" : "xx_HTZjQeZL_shard_jaAVvOei", "chunkVersion" : [ Timestamp(588211, 1), ObjectId("5ec496373f311c50a0185499") ], "waitForDelete" : false }
{ "_id" : "xx_cold_data_db.xx_cold_data_db-user_id_\"226060685\"module_\"album\"md5_\"56A0293EAD54C7A1326C91621A7C4664\"", "ns" : "xx_cold_data_db.xx_cold_data_db", "min" : { "user_id" : "226060685", "module" : "album", "md5" : "56A0293EAD54C7A1326C91621A7C4664" }, "max" : { "xx" : "226061085", "module" : "album", "md5" : "6686C24B301C39C3B3585D8602A26F45" }, "fromShard" : "xx_HTZjQeZL_shard_3", "toShard" : "xx_HTZjQeZL_shard_ewMvmPnE", "chunkVersion" : [ Timestamp(588212, 1), ObjectId("5ec496373f311c50a0185499") ], "waitForDelete" : false }
*/

//3.6�汾balancer migrate��config server����   

//�����MigrationManager::_schedule����
//��Դ��Ƭ���ڵ㷢��moveChunk����    splitchunkҲ����������̣�������movechunk�������chunk big�����splite��chunk,Ҳ���ߵ��ýӿ�
void MigrationManager::_schedule(WithLock lock,
                                 OperationContext* opCtx,
                                 const HostAndPort& targetHost,
                                 Migration migration) {
    executor::TaskExecutor* const executor =
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

	//��ȡbalance��ҪǨ�Ƶı�
    const NamespaceString nss(migration.nss);

	//��map���в���nss
    auto it = _activeMigrations.find(nss);
	//û�ҵ�,˵����Ҫ���»�ȡ�ֲ�ʽ����ͬһ������Ƭ����£������4��Ƭ���ݵ�8��Ƭ�����ѡ��4����ҪǨ�Ƶ�chunk
	//������MigrationManager::executeMigrationsForAutoBalance�л�ÿ��chunk��ѡ����_schedule����4��chunkʵ����ֻ
	//���ȡһ�ηֲ�ʽ����Ҳ���ǵ�һ��chunk������ʱ���ȡһ�ηֲ�ʽ��������3��chunk��movechunk���������ٴλ�ȡ�÷ֲ�ʽ��
	//�Ӷ�ʵ����ͬһ����ͬ��Ƭ���ݵĲ���Ǩ��
    if (it == _activeMigrations.end()) { //ֻҪ��ǰ�ñ���chunk��Ǩ�ƣ��򲻻���������棬ֱ������
        const std::string whyMessage(stream() << "Migrating chunk(s) in collection " << nss.ns());

        // Acquire the collection distributed lock (blocking call)
        //��ȡ�ֲ�ʽ��
        auto statusWithDistLockHandle =
        	//ReplSetDistLockManager::lockWithSessionID
            Grid::get(opCtx)->catalogClient()->getDistLockManager()->lockWithSessionID(
                opCtx,
                nss.ns(), //��Ҫ��balance�ı�
                whyMessage,
                _lockSessionID,
                DistLockManager::kSingleLockAttemptTimeout);
		
		//��mongod�Ѿ��и�nss��ķֲ�ʽ����˵����ǰ����Ǩ�Ƹñ�����
        if (!statusWithDistLockHandle.isOK()) { 
            migration.completionNotification->set(
			/*
			���ƴ�ӡ����:
			Failed with error ��could not acquire collection lock for mongodbtest.usertable to migrate chunk [{ : MinKey },{ : MaxKey }) 
			:: caused by :: Lock for migrating chunk [{ : MinKey }, { : MaxKey }) in mongodbtest.usertable is taken.��, from shard0002 to shard0000
			*/ //���chunk�ܴ���ᴥ��splite��spliteҲ��Ҫ��ȡnss��Ӧ�ֲ�ʽ������ʱ��ͻ�����Ǩ��ʧ��
                Status(statusWithDistLockHandle.getStatus().code(),
                       stream() << "Could not acquire collection lock for " << nss.ns()
                                << " to migrate chunks, due to "
                                << statusWithDistLockHandle.getStatus().reason()));
            return;
        }

		//_activeMigrationsΪCollectionMigrationsStateMap�ṹ
		//��¼һ�£���ȡnss���Ӧ���ɹ�����¼����Ϣ��ͬʱ����һ��MigrationsListһ���¼MigrationsList
		//MigrationsList�ں����migrations->push_front(std::move(migration))���
        it = _activeMigrations.insert(std::make_pair(nss, MigrationsList())).first;
    }

	//Ҳ����MigrationsList
    auto migrations = &it->second;

    // Add ourselves to the list of migrations on this collection
    //migration��¼��_activeMigrations������_activeMigrations�м�¼��map��Ϊ<��movechunk��Ϣ>
    migrations->push_front(std::move(migration));
    auto itMigration = migrations->begin();

	//����migration����moveChunk��Ϣ���͵�targetHost�ڵ� 
    const RemoteCommandRequest remoteRequest( 
        targetHost, NamespaceString::kAdminDb.toString(), itMigration->moveChunkCmdObj, opCtx);

    StatusWith<executor::TaskExecutor::CallbackHandle> callbackHandleWithStatus =
        executor->scheduleRemoteCommand(
            remoteRequest,
            [this, itMigration](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                Client::initThread(getThreadName());
                ON_BLOCK_EXIT([&] { Client::destroy(); });
                auto opCtx = cc().makeOperationContext();

                stdx::lock_guard<stdx::mutex> lock(_mutex);
				//��chunk��Ӧmovechunk����ִ����ɺ󣬵���_complete
                _complete(lock, opCtx.get(), itMigration, args.response);
            });

    if (callbackHandleWithStatus.isOK()) {
		//һ��ͨ�����ﷵ��
        itMigration->callbackHandle = std::move(callbackHandleWithStatus.getValue());
        return;
    }

	//��������²����ߵ�����
	//chunkǨ����ɺ�ýӿ������ͷŷֲ�ʽ��
    _complete(lock, opCtx, itMigration, std::move(callbackHandleWithStatus.getStatus()));
}

//�����MigrationManager::_schedule����
void MigrationManager::_complete(WithLock lock,
                                 OperationContext* opCtx,
                                 MigrationsList::iterator itMigration,
                                 const RemoteCommandResponse& remoteCommandResponse) {
    const NamespaceString nss(itMigration->nss);

    // Make sure to signal the notification last, after the distributed lock is freed, so that we
    // don't have the race condition where a subsequently scheduled migration finds the dist lock
    // still acquired.
    auto notificationToSignal = itMigration->completionNotification;

	//map�в���nss
    auto it = _activeMigrations.find(nss);
    invariant(it != _activeMigrations.end());

	//��ȡ�ñ�nss��Ӧ��Ǩ�ƿ���Ϣ
    auto migrations = &it->second;
	//�Ƴ���ӦitMigration
    migrations->erase(itMigration);

	//�����nss�����Ѿ�û��itMigration�����ͷŻ���ķֲ�ʽ����Ϣ
	//balancerһ��ѭ����ѡȡ������ҪǨ�Ƶ�chunk��ֻ�е�����chunk��Ǩ����ɣ��Ż��ͷŷֲ�ʽ��
    if (migrations->empty()) {
		//DistLockManagerMock::unlock
        Grid::get(opCtx)->catalogClient()->getDistLockManager()->unlock(
            opCtx, _lockSessionID, nss.ns());
        _activeMigrations.erase(it);
        _checkDrained(lock);
    }

    notificationToSignal->set(remoteCommandResponse);
}

void MigrationManager::_checkDrained(WithLock) {
    if (_state == State::kEnabled || _state == State::kRecovering) {
        return;
    }
    invariant(_state == State::kStopping);

    if (_activeMigrations.empty()) {
        _condVar.notify_all();
    }
}

void MigrationManager::_waitForRecovery() {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condVar.wait(lock, [this] { return _state != State::kRecovering; });
}

void MigrationManager::_abandonActiveMigrationsAndEnableManager(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (_state == State::kStopping) {
        // The balancer was interrupted. Let the next balancer recover the state.
        return;
    }
    invariant(_state == State::kRecovering);

    auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Unlock all balancer distlocks we aren't using anymore.
    auto distLockManager = catalogClient->getDistLockManager();
    distLockManager->unlockAll(opCtx, distLockManager->getProcessID());

    // Clear the config.migrations collection so that those chunks can be scheduled for migration
    // again.
    catalogClient
        ->removeConfigDocuments(opCtx, MigrationType::ConfigNS, BSONObj(), kMajorityWriteConcern)
        .transitional_ignore();

    _state = State::kEnabled;
    _condVar.notify_all();
}

//��ȡmoveChunk�Ľ����MigrationManager::executeMigrationsForAutoBalance����
Status MigrationManager::_processRemoteCommandResponse(
    const RemoteCommandResponse& remoteCommandResponse,
    ScopedMigrationRequest* scopedMigrationRequest) {

    stdx::lock_guard<stdx::mutex> lock(_mutex);
    Status commandStatus(ErrorCodes::InternalError, "Uninitialized value.");

    // Check for local errors sending the remote command caused by stepdown.
    if (isErrorDueToConfigStepdown(remoteCommandResponse.status,
                                   _state != State::kEnabled && _state != State::kRecovering)) {
        scopedMigrationRequest->keepDocumentOnDestruct();
        return {ErrorCodes::BalancerInterrupted,
                stream() << "Migration interrupted because the balancer is stopping."
                         << " Command status: "
                         << remoteCommandResponse.status.toString()};
    }

    if (!remoteCommandResponse.isOK()) {
        commandStatus = remoteCommandResponse.status;
    } else {
        // TODO: delete in 3.8
        commandStatus = extractMigrationStatusFromCommandResponse(remoteCommandResponse.data);
    }

    if (!Shard::shouldErrorBePropagated(commandStatus.code())) {
        commandStatus = {ErrorCodes::OperationFailed,
                         stream() << "moveChunk command failed on source shard."
                                  << causedBy(commandStatus)};
    }

    // Any failure to remove the migration document should be because the config server is
    // stepping/shutting down. In this case we must fail the moveChunk command with a retryable
    // error so that the caller does not move on to other distlock requiring operations that could
    // fail when the balancer recovers and takes distlocks for migration recovery.
    Status status = scopedMigrationRequest->tryToRemoveMigration();
    if (!status.isOK()) {
        commandStatus = {
            ErrorCodes::BalancerInterrupted,
            stream() << "Migration interrupted because the balancer is stopping"
                     << " and failed to remove the config.migrations document."
                     << " Command status: "
                     << (commandStatus.isOK() ? status.toString() : commandStatus.toString())};
    }

    return commandStatus;
}

MigrationManager::Migration::Migration(NamespaceString inNss, BSONObj inMoveChunkCmdObj)
    : nss(std::move(inNss)),
      moveChunkCmdObj(std::move(inMoveChunkCmdObj)),
      completionNotification(std::make_shared<Notification<RemoteCommandResponse>>()) {}

MigrationManager::Migration::~Migration() {
    invariant(completionNotification);
}

}  // namespace mongo
