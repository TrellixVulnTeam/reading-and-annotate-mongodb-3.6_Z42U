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

#include "mongo/db/s/balancer/balancer.h"

#include <algorithm>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy_impl.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

namespace mongo {

using std::map;
using std::string;
using std::vector;

namespace {

const Seconds kBalanceRoundDefaultInterval(10);

// Sleep between balancer rounds in the case where the last round found some chunks which needed to
// be balanced. This value should be set sufficiently low so that imbalanced clusters will quickly
// reach balanced state, but setting it too low may cause CRUD operations to start failing due to
// not being able to establish a stable shard version.
const Seconds kShortBalanceRoundInterval(1);

const auto getBalancer = ServiceContext::declareDecoration<std::unique_ptr<Balancer>>();

/**
 * Utility class to generate timing and statistics for a single balancer round.
 */ //balancer统计信息   
class BalanceRoundDetails {
public:
    BalanceRoundDetails() : _executionTimer() {}

    void setSucceeded(int candidateChunks, int chunksMoved) {
        invariant(!_errMsg);
        _candidateChunks = candidateChunks;
        _chunksMoved = chunksMoved;
    }

    void setFailed(const string& errMsg) {
        _errMsg = errMsg;
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("executionTimeMillis", _executionTimer.millis());
        builder.append("errorOccured", _errMsg.is_initialized());

        if (_errMsg) {
            builder.append("errmsg", *_errMsg);
        } else {
            builder.append("candidateChunks", _candidateChunks);
            builder.append("chunksMoved", _chunksMoved);
        }

        return builder.obj();
    }

private:
    const Timer _executionTimer;

    // Set only on success
    int _candidateChunks{0};
    int _chunksMoved{0};

    // Set only on failure
    boost::optional<string> _errMsg;
};

/**
 * Occasionally prints a log message with shard versions if the versions are not the same
 * in the cluster.
 */
//检查每个分片的主节点版本是否一致，不一致则打印提示
void warnOnMultiVersion(const vector<ClusterStatistics::ShardStatistics>& clusterStats) {
    auto&& vii = VersionInfoInterface::instance();

    bool isMultiVersion = false;
    for (const auto& stat : clusterStats) {
        if (!vii.isSameMajorVersion(stat.mongoVersion.c_str())) {
            isMultiVersion = true;
            break;
        }
    }

    // If we're all the same version, don't message
    if (!isMultiVersion)
        return;

    StringBuilder sb;
    sb << "Multi version cluster detected. Local version: " << vii.version()
       << ", shard versions: ";

    for (const auto& stat : clusterStats) {
        sb << stat.shardId << " is at " << stat.mongoVersion << "; ";
    }

    warning() << sb.str();
}

}  // namespace

Balancer::Balancer(ServiceContext* serviceContext)
    : _balancedLastTime(0),
      _clusterStats(stdx::make_unique<ClusterStatisticsImpl>()),
      //chunk选择策略
      _chunkSelectionPolicy(
          stdx::make_unique<BalancerChunkSelectionPolicyImpl>(_clusterStats.get())),
	 //迁移管理
	 _migrationManager(serviceContext) {} //Balancer._migrationManager

Balancer::~Balancer() {
    // The balancer thread must have been stopped
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    invariant(_state == kStopped);
}

void Balancer::create(ServiceContext* serviceContext) {
    invariant(!getBalancer(serviceContext));
    getBalancer(serviceContext) = stdx::make_unique<Balancer>(serviceContext);

    // Register a shutdown task to terminate the balancer thread so that it doesn't leak memory.
    //balancer thread退出适合的资源销毁操作
    registerShutdownTask([serviceContext] {
        auto balancer = Balancer::get(serviceContext);
        // Make sure that the balancer thread has been interrupted.
        balancer->interruptBalancer();
        // Make sure the balancer thread has terminated.
        balancer->waitForBalancerToStop();
    });
}

//根据ServiceContext获取对应balancer
Balancer* Balancer::get(ServiceContext* serviceContext) {
    return getBalancer(serviceContext).get();
}

//根据operationContext获取对应balancer
Balancer* Balancer::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

//Balancer初始化
void Balancer::initiateBalancer(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    invariant(_state == kStopped);
    _state = kRunning;

    _migrationManager.startRecoveryAndAcquireDistLocks(opCtx);

    invariant(!_thread.joinable());
    invariant(!_threadOperationContext);
    _thread = stdx::thread([this] { _mainThread(); });
}

//balancer异常处理
void Balancer::interruptBalancer() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (_state != kRunning)
        return;

    _state = kStopping;

    // Interrupt the balancer thread if it has been started. We are guaranteed that the operation
    // context of that thread is still alive, because we hold the balancer mutex.
    if (_threadOperationContext) {
        stdx::lock_guard<Client> scopedClientLock(*_threadOperationContext->getClient());
        _threadOperationContext->markKilled(ErrorCodes::InterruptedDueToReplStateChange);
    }

    // Schedule a separate thread to shutdown the migration manager in order to avoid deadlock with
    // replication step down
    invariant(!_migrationManagerInterruptThread.joinable());
	//_migrationManagerInterruptThread线程专门负责迁移异常处理
    _migrationManagerInterruptThread =
        stdx::thread([this] { _migrationManager.interruptAndDisableMigrations(); });

    _condVar.notify_all();
}

void Balancer::waitForBalancerToStop() {
    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        if (_state == kStopped)
            return;

        invariant(_state == kStopping);
        invariant(_thread.joinable());
    }

    _thread.join();

    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    _state = kStopped;
    _thread = {};

    LOG(1) << "Balancer thread terminated";
}

void Balancer::joinCurrentRound(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> scopedLock(_mutex);
    const auto numRoundsAtStart = _numBalancerRounds;
    opCtx->waitForConditionOrInterrupt(_condVar, scopedLock, [&] {
        return !_inBalancerRound || _numBalancerRounds != numRoundsAtStart;
    });
}

//ConfigSvrMoveChunkCommand::run调用
Status Balancer::rebalanceSingleChunk(OperationContext* opCtx, const ChunkType& chunk) {
    auto migrateStatus = _chunkSelectionPolicy->selectSpecificChunkToMove(opCtx, chunk);
    if (!migrateStatus.isOK()) {
        return migrateStatus.getStatus();
    }

    auto migrateInfo = std::move(migrateStatus.getValue());
    if (!migrateInfo) {
        LOG(1) << "Unable to find more appropriate location for chunk " << redact(chunk.toString());
        return Status::OK();
    }

    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    Status refreshStatus = balancerConfig->refreshAndCheck(opCtx);
    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    return _migrationManager.executeManualMigration(opCtx,
                                                    *migrateInfo,
                                                    balancerConfig->getMaxChunkSizeBytes(),
                                                    balancerConfig->getSecondaryThrottle(),
                                                    balancerConfig->waitForDelete());
}
//源分片收到config server发送过来的moveChunk命令  
//注意MoveChunkCmd和MoveChunkCommand的区别，MoveChunkCmd为代理收到mongo shell等客户端的处理流程，
//然后调用configsvr_client::moveChunk，发送_configsvrMoveChunk给config server,由config server统一
//发送movechunk给shard执行chunk操作，从而执行MoveChunkCommand::run来完成shard见真正的shard间迁移

//MoveChunkCommand为shard收到movechunk命令的真正数据迁移的入口
//MoveChunkCmd为mongos收到客户端movechunk命令的处理流程，转发给config server
//ConfigSvrMoveChunkCommand为config server收到mongos发送来的_configsvrMoveChunk命令的处理流程

//自动balancer触发shard做真正的数据迁移入口在Balancer::_moveChunks->MigrationManager::executeMigrationsForAutoBalance
//手动balance，config收到代理ConfigSvrMoveChunkCommand命令后迁移入口Balancer::moveSingleChunk

//ConfigSvrMoveChunkCommand::run调用,congfig server触发shard迁移
Status Balancer::moveSingleChunk(OperationContext* opCtx,
                                 const ChunkType& chunk,
                                 const ShardId& newShardId,
                                 uint64_t maxChunkSizeBytes,
                                 const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                 bool waitForDelete) {
    auto moveAllowedStatus = _chunkSelectionPolicy->checkMoveAllowed(opCtx, chunk, newShardId);
    if (!moveAllowedStatus.isOK()) {
        return moveAllowedStatus;
    }

	//MigrationManager::executeManualMigration调用
    return _migrationManager.executeManualMigration(
        opCtx, MigrateInfo(newShardId, chunk), maxChunkSizeBytes, secondaryThrottle, waitForDelete);
}

void Balancer::report(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    balancerConfig->refreshAndCheck(opCtx).transitional_ignore();

    const auto mode = balancerConfig->getBalancerMode();

    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    builder->append("mode", BalancerSettingsType::kBalancerModes[mode]);
    builder->append("inBalancerRound", _inBalancerRound);
    builder->append("numBalancerRounds", _numBalancerRounds);
}

/*
例如下面的chunks分布:
xx_HTZjQeZL_shard_1 571274
xx_HTZjQeZL_shard_2 319536
xx_HTZjQeZL_shard_3 572644
xx_HTZjQeZL_shard_4 707811
xx_HTZjQeZL_shard_QubQcjup	145339
xx_HTZjQeZL_shard_ewMvmPnE	136034
xx_HTZjQeZL_shard_jaAVvOei	129682
xx_HTZjQeZL_shard_kxYilhNF	150150

这就是选出的需要迁移的chunk
mongos> db.migrations.find()
{ "_id" : "xx_cold_data_db.xx_cold_data_db-user_id_\"359209050\"module_\"album\"md5_\"992F3FF0DDCB009D1A6CCD8647CEAFA5\"", "ns" : "xx_cold_data_db.xx_cold_data_db", "min" : { "user_id" : "359209050", "module" : "album", "md5" : "992F3FF0DDCB009D1A6CCD8647CEAFA5" }, "max" : { "xx" : "359209058", "module" : "album", "md5" : "49D552BEBEAE7D3CB6B53A7FE384E5A4" }, "fromShard" : "xx_HTZjQeZL_shard_1", "toShard" : "xx_HTZjQeZL_shard_QubQcjup", "chunkVersion" : [ Timestamp(588213, 1), ObjectId("5ec496373f311c50a0185499") ], "waitForDelete" : false }
{ "_id" : "xx_cold_data_db.xx_cold_data_db-user_id_\"278344065\"module_\"album\"md5_\"CAA4B99617A83D0DEE5CE30D4D75829F\"", "ns" : "xx_cold_data_db.xx_cold_data_db", "min" : { "user_id" : "278344065", "module" : "album", "md5" : "CAA4B99617A83D0DEE5CE30D4D75829F" }, "max" : { "xx" : "278344356", "module" : "album", "md5" : "E691E5F8756E723BB44BC2049D624F81" }, "fromShard" : "xx_HTZjQeZL_shard_4", "toShard" : "xx_HTZjQeZL_shard_jaAVvOei", "chunkVersion" : [ Timestamp(588211, 1), ObjectId("5ec496373f311c50a0185499") ], "waitForDelete" : false }
{ "_id" : "xx_cold_data_db.xx_cold_data_db-user_id_\"226060685\"module_\"album\"md5_\"56A0293EAD54C7A1326C91621A7C4664\"", "ns" : "xx_cold_data_db.xx_cold_data_db", "min" : { "user_id" : "226060685", "module" : "album", "md5" : "56A0293EAD54C7A1326C91621A7C4664" }, "max" : { "xx" : "226061085", "module" : "album", "md5" : "6686C24B301C39C3B3585D8602A26F45" }, "fromShard" : "xx_HTZjQeZL_shard_3", "toShard" : "xx_HTZjQeZL_shard_ewMvmPnE", "chunkVersion" : [ Timestamp(588212, 1), ObjectId("5ec496373f311c50a0185499") ], "waitForDelete" : false }
*/

//balancer线程  
//新版本由config server控制,只有主节点才会由该线程，如果发生主从切换，则原主节点会消耗balancer线程，
//从节点变为新主后，会生成新的balancer线程，这样可以保证一个集群做balancer只会由一个节点触发，解决了早期
//mongos版本，多个mongos获取分布式锁的问题
void Balancer::_mainThread() {
    Client::initThread("Balancer");

	//初始化opctx
    auto opCtx = cc().makeOperationContext();
    auto shardingContext = Grid::get(opCtx.get());

    log() << "CSRS balancer is starting";

    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        _threadOperationContext = opCtx.get();
    }

    const Seconds kInitBackoffInterval(10);

	//获取balancer配置信息
    auto balancerConfig = shardingContext->getBalancerConfiguration();
    while (!_stopRequested()) {
		//配置检查
        Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
        if (!refreshStatus.isOK()) {
            warning() << "Balancer settings could not be loaded and will be retried in "
                      << durationCount<Seconds>(kInitBackoffInterval) << " seconds"
                      << causedBy(refreshStatus);

            _sleepFor(opCtx.get(), kInitBackoffInterval);
            continue;
        }

        break;
    }

    log() << "CSRS balancer thread is recovering";

    _migrationManager.finishRecovery(opCtx.get(),
                                     balancerConfig->getMaxChunkSizeBytes(),
                                     balancerConfig->getSecondaryThrottle());

    log() << "CSRS balancer thread is recovered";

    // Main balancer loop
    while (!_stopRequested()) {
        BalanceRoundDetails roundDetails;

        _beginRound(opCtx.get());

        try {
            shardingContext->shardRegistry()->reload(opCtx.get());

            uassert(13258, "oids broken after resetting!", _checkOIDs(opCtx.get()));

			//配置检查
            Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
            if (!refreshStatus.isOK()) {
                warning() << "Skipping balancing round" << causedBy(refreshStatus);
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }

			//balance没启用
            if (!balancerConfig->shouldBalance()) {
                LOG(1) << "Skipping balancing round because balancing is disabled";
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            {
                LOG(1) << "*** start balancing round. "
                       << "waitForDelete: " << balancerConfig->waitForDelete()
                       << ", secondaryThrottle: "
                       << balancerConfig->getSecondaryThrottle().toBSON();

                OCCASIONALLY warnOnMultiVersion(
                    uassertStatusOK(_clusterStats->getStats(opCtx.get())));

				//标签处理
                Status status = _enforceTagRanges(opCtx.get());
                if (!status.isOK()) {
                    warning() << "Failed to enforce tag ranges" << causedBy(status);
                } else {
                    LOG(1) << "Done enforcing tag range boundaries.";
                }

				//选出需要迁移的chunks    candidateChunks为数组类型MigrateInfoVector
                const auto candidateChunks = uassertStatusOK(
                //BalancerChunkSelectionPolicyImpl::selectChunksToMove
                    _chunkSelectionPolicy->selectChunksToMove(opCtx.get(), _balancedLastTime));

				//没有需要迁移的块
                if (candidateChunks.empty()) {
                    LOG(1) << "no need to move any chunk";
					//说明本次循环没有迁移chunk
                    _balancedLastTime = false;
                } else { //迁移chunk在这里
                    _balancedLastTime = _moveChunks(opCtx.get(), candidateChunks);

                    roundDetails.setSucceeded(static_cast<int>(candidateChunks.size()),
                                              _balancedLastTime);

                    shardingContext->catalogClient()
                        ->logAction(opCtx.get(), "balancer.round", "", roundDetails.toBSON())
                        .transitional_ignore();
                }

                LOG(1) << "*** End of balancing round";
            }

			//上一次迁移块成功，这里延时kBalanceRoundDefaultInterval，
			//如果上一次迁移失败，这里延时kShortBalanceRoundInterval
            _endRound(opCtx.get(),
                      _balancedLastTime ? kShortBalanceRoundInterval
                                        : kBalanceRoundDefaultInterval);
        } catch (const std::exception& e) {
            log() << "caught exception while doing balance: " << e.what();

            // Just to match the opening statement if in log level 1
            LOG(1) << "*** End of balancing round";

            // This round failed, tell the world!
            roundDetails.setFailed(e.what());

            shardingContext->catalogClient()
                ->logAction(opCtx.get(), "balancer.round", "", roundDetails.toBSON())
                .transitional_ignore();

            // Sleep a fair amount before retrying because of the error
            _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
        }
    }

    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        invariant(_state == kStopping);
        invariant(_migrationManagerInterruptThread.joinable());
    }

	//balancer关闭后的异常迁移处理
    _migrationManagerInterruptThread.join();
    _migrationManager.drainActiveMigrations();

    {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        _migrationManagerInterruptThread = {};
        _threadOperationContext = nullptr;
    }

    log() << "CSRS balancer is now stopped";
}

bool Balancer::_stopRequested() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    return (_state != kRunning);
}

void Balancer::_beginRound(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _inBalancerRound = true;
    _condVar.notify_all();
}

void Balancer::_endRound(OperationContext* opCtx, Seconds waitTimeout) {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _inBalancerRound = false;
        _numBalancerRounds++;
        _condVar.notify_all();
    }

    MONGO_IDLE_THREAD_BLOCK;
    _sleepFor(opCtx, waitTimeout);
}

void Balancer::_sleepFor(OperationContext* opCtx, Seconds waitTimeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    _condVar.wait_for(lock, waitTimeout.toSystemDuration(), [&] { return _state != kRunning; });
}

bool Balancer::_checkOIDs(OperationContext* opCtx) {
    auto shardingContext = Grid::get(opCtx);

    vector<ShardId> all;
    shardingContext->shardRegistry()->getAllShardIds(&all);

    // map of OID machine ID => shardId
    map<int, ShardId> oids;

    for (const ShardId& shardId : all) {
        if (_stopRequested()) {
            return false;
        }

        auto shardStatus = shardingContext->shardRegistry()->getShard(opCtx, shardId);
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto s = shardStatus.getValue();

        auto result = uassertStatusOK(
            s->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                "admin",
                                                BSON("features" << 1),
                                                Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(result.commandStatus);
        BSONObj f = std::move(result.response);

        if (f["oidMachine"].isNumber()) {
            int x = f["oidMachine"].numberInt();
            if (oids.count(x) == 0) {
                oids[x] = shardId;
            } else {
                log() << "error: 2 machines have " << x << " as oid machine piece: " << shardId
                      << " and " << oids[x];

                result = uassertStatusOK(s->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    "admin",
                    BSON("features" << 1 << "oidReset" << 1),
                    Shard::RetryPolicy::kIdempotent));
                uassertStatusOK(result.commandStatus);

                auto otherShardStatus = shardingContext->shardRegistry()->getShard(opCtx, oids[x]);
                if (otherShardStatus.isOK()) {
                    result = uassertStatusOK(
                        otherShardStatus.getValue()->runCommandWithFixedRetryAttempts(
                            opCtx,
                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                            "admin",
                            BSON("features" << 1 << "oidReset" << 1),
                            Shard::RetryPolicy::kIdempotent));
                    uassertStatusOK(result.commandStatus);
                }

                return false;
            }
        } else {
            log() << "warning: oidMachine not set on: " << s->toString();
        }
    }

    return true;
}

Status Balancer::_enforceTagRanges(OperationContext* opCtx) {
    auto chunksToSplitStatus = _chunkSelectionPolicy->selectChunksToSplit(opCtx);
    if (!chunksToSplitStatus.isOK()) {
        return chunksToSplitStatus.getStatus();
    }

    for (const auto& splitInfo : chunksToSplitStatus.getValue()) {
        auto routingInfoStatus =
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
                opCtx, splitInfo.nss);
        if (!routingInfoStatus.isOK()) {
            return routingInfoStatus.getStatus();
        }

        auto cm = routingInfoStatus.getValue().cm();

        auto splitStatus =
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  splitInfo.shardId,
                                                  splitInfo.nss,
                                                  cm->getShardKeyPattern(),
                                                  splitInfo.collectionVersion,
                                                  ChunkRange(splitInfo.minKey, splitInfo.maxKey),
                                                  splitInfo.splitKeys);
        if (!splitStatus.isOK()) {
            warning() << "Failed to enforce tag range for chunk " << redact(splitInfo.toString())
                      << causedBy(redact(splitStatus.getStatus()));
        }
    }

    return Status::OK();
}

//源分片收到config server发送过来的moveChunk命令  
//注意MoveChunkCmd和MoveChunkCommand的区别，MoveChunkCmd为代理收到mongo shell等客户端的处理流程，
//然后调用configsvr_client::moveChunk，发送_configsvrMoveChunk给config server,由config server统一
//发送movechunk给shard执行chunk操作，从而执行MoveChunkCommand::run来完成shard见真正的shard间迁移

//MoveChunkCommand为shard收到movechunk命令的真正数据迁移的入口
//MoveChunkCmd为mongos收到客户端movechunk命令的处理流程，转发给config server
//ConfigSvrMoveChunkCommand为config server收到mongos发送来的_configsvrMoveChunk命令的处理流程

//自动balancer触发shard做真正的数据迁移入口在Balancer::_moveChunks->MigrationManager::executeMigrationsForAutoBalance
//手动balance，config收到代理ConfigSvrMoveChunkCommand命令后迁移入口Balancer::moveSingleChunk

//Balancer::_mainThread()调用
int Balancer::_moveChunks(OperationContext* opCtx,
						  //需要迁移的chunk记录到这里
                          const BalancerChunkSelectionPolicy::MigrateInfoVector& candidateChunks) {
	//获取balancer配置信息
	auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

    // If the balancer was disabled since we started this round, don't start new chunk moves
    //balance关闭了
    if (_stopRequested() || !balancerConfig->shouldBalance()) {
        LOG(1) << "Skipping balancing round because balancer was stopped";
        return 0;
    }
	
    auto migrationStatuses =
		//MigrationManager::executeMigrationsForAutoBalance 把挑选出来需要迁移的chunk迁移到目的分片
        _migrationManager.executeMigrationsForAutoBalance(opCtx,
                                                          candidateChunks,
                                                          //chunk最大大小
                                                          balancerConfig->getMaxChunkSizeBytes(),
                                                          //迁移到目的分片的时候，需要写入几个从节点
                                                          balancerConfig->getSecondaryThrottle(),
                                                          //源节点迁移走的chunk是同步删除还是异步删除
                                                          balancerConfig->waitForDelete());

    int numChunksProcessed = 0;

    for (const auto& migrationStatusEntry : migrationStatuses) {
        const Status& status = migrationStatusEntry.second;
        if (status.isOK()) {
			//成功迁移的chunk数自增计数
            numChunksProcessed++;
            continue;
        }

        const MigrationIdentifier& migrationId = migrationStatusEntry.first;

        const auto requestIt = std::find_if(candidateChunks.begin(),
                                            candidateChunks.end(),
                                            [&migrationId](const MigrateInfo& migrateInfo) {
                                                return migrateInfo.getName() == migrationId;
                                            });
        invariant(requestIt != candidateChunks.end());

        if (status == ErrorCodes::ChunkTooBig) {
			//遇到big chunk mongos会splite拆分这个chunk，下次可以继续迁移拆分后得块，所以可以算着本次成功
            numChunksProcessed++;

            log() << "Performing a split because migration " << redact(requestIt->toString())
                  << " failed for size reasons" << causedBy(redact(status));
			//遇到大chunk，则splite
            _splitOrMarkJumbo(opCtx, NamespaceString(requestIt->ns), requestIt->minKey);
            continue;
        }

        log() << "Balancer move " << redact(requestIt->toString()) << " failed"
              << causedBy(redact(status));
    }

    return numChunksProcessed;
}

//Balancer::_moveChunks->Balancer::_splitOrMarkJumbo
void Balancer::_splitOrMarkJumbo(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& minKey) {
    auto routingInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss));
    const auto cm = routingInfo.cm().get();

    auto chunk = cm->findIntersectingChunkWithSimpleCollation(minKey);

    try {
        const auto splitPoints = uassertStatusOK(shardutil::selectChunkSplitPoints(
            opCtx,
            chunk->getShardId(),
            nss,
            cm->getShardKeyPattern(),
            ChunkRange(chunk->getMin(), chunk->getMax()),
            Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes(),
            boost::none));

        uassert(ErrorCodes::CannotSplit, "No split points found", !splitPoints.empty());

        uassertStatusOK(
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  chunk->getShardId(),
                                                  nss,
                                                  cm->getShardKeyPattern(),
                                                  cm->getVersion(),
                                                  ChunkRange(chunk->getMin(), chunk->getMax()),
                                                  splitPoints));
    } catch (const DBException&) {
        log() << "Marking chunk " << redact(chunk->toString()) << " as jumbo.";

        chunk->markAsJumbo();

        const std::string chunkName = ChunkType::genID(nss.ns(), chunk->getMin());

        auto status = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
            opCtx,
            ChunkType::ConfigNS,
            BSON(ChunkType::name(chunkName)),
            BSON("$set" << BSON(ChunkType::jumbo(true))),
            false,
            ShardingCatalogClient::kMajorityWriteConcern);
        if (!status.isOK()) {
            log() << "Couldn't set jumbo for chunk: " << redact(chunkName)
                  << causedBy(redact(status.getStatus()));
        }
    }
}

}  // namespace mongo
