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

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/s/chunk_move_write_concern_options.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

namespace {

//源分片收到mongos得moveChunk后，会发送该命令，通知目的分片从原分片拉取chunk数据，见MoveChunkCommand::_runImpl
class RecvChunkStartCommand : public ErrmsgCommandDeprecated {
public:
    RecvChunkStartCommand() : ErrmsgCommandDeprecated("_recvChunkStart") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        // This is required to be true to support moveChunk.
        return true;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

	//RecvChunkStartCommand::errmsgRun
    bool errmsgRun(OperationContext* opCtx,
                   const string&,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        auto shardingState = ShardingState::get(opCtx);
        uassertStatusOK(shardingState->canAcceptShardedCommands());

        const ShardId toShard(cmdObj["toShardName"].String());
        const ShardId fromShard(cmdObj["fromShardName"].String());

        const NamespaceString nss(cmdObj.firstElement().String());

        const auto chunkRange = uassertStatusOK(ChunkRange::fromBSON(cmdObj));

        // Refresh our collection manager from the config server, we need a collection manager to
        // start registering pending chunks. We force the remote refresh here to make the behavior
        // consistent and predictable, generally we'd refresh anyway, and to be paranoid.
        ChunkVersion shardVersion;
        Status status = shardingState->refreshMetadataNow(opCtx, nss, &shardVersion);
        if (!status.isOK()) {
            errmsg = str::stream() << "cannot start receiving chunk "
                                   << redact(chunkRange.toString()) << causedBy(redact(status));
            warning() << errmsg;
            return false;
        }

        // Process secondary throttle settings and assign defaults if necessary.
        const auto secondaryThrottle =
            uassertStatusOK(MigrationSecondaryThrottleOptions::createFromCommand(cmdObj));
        const auto writeConcern = uassertStatusOK(
            ChunkMoveWriteConcernOptions::getEffectiveWriteConcern(opCtx, secondaryThrottle));


		//获取片建
        BSONObj shardKeyPattern = cmdObj["shardKeyPattern"].Obj().getOwned();
		//源分片地址信息
        auto statusWithFromShardConnectionString = ConnectionString::parse(cmdObj["from"].String());
        if (!statusWithFromShardConnectionString.isOK()) {
            errmsg = str::stream()
                << "cannot start receiving chunk " << redact(chunkRange.toString())
                << causedBy(redact(statusWithFromShardConnectionString.getStatus()));

            warning() << errmsg;
            return false;
        }

		//获取sessionId信息,该id记录迁移的源和目的shard id信息，和源集群的MigrationChunkClonerSourceLegacy._sessionId对应
        const MigrationSessionId migrationSessionId(
            uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj)));

        // Ensure this shard is not currently receiving or donating any chunks.
        //注册一下，保证目的分片同一个表不会同时接受几个块迁移
        auto scopedRegisterReceiveChunk(
            uassertStatusOK(shardingState->registerReceiveChunk(nss, chunkRange, fromShard)));

		//MigrationDestinationManager::start
        uassertStatusOK(shardingState->migrationDestinationManager()->start(
            nss,
            std::move(scopedRegisterReceiveChunk),
            migrationSessionId,
            statusWithFromShardConnectionString.getValue(),
            fromShard,
            toShard,
            chunkRange.getMin(),
            chunkRange.getMax(),
            shardKeyPattern,
            shardVersion.epoch(),
            writeConcern));

        result.appendBool("started", true);
        return true;
    }

} recvChunkStartCmd;

//chunk迁移流程中目的shard收到的报文信息，参考https://blog.csdn.net/dreamdaye123/article/details/105278247
class RecvChunkStatusCommand : public BasicCommand {
public:
    RecvChunkStatusCommand() : BasicCommand("_recvChunkStatus") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        ShardingState::get(opCtx)->migrationDestinationManager()->report(result);
        return true;
    }

} recvChunkStatusCommand;

class RecvChunkCommitCommand : public BasicCommand {
public:
    RecvChunkCommitCommand() : BasicCommand("_recvChunkCommit") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auto const sessionId = uassertStatusOK(MigrationSessionId::extractFromBSON(cmdObj));
        auto mdm = ShardingState::get(opCtx)->migrationDestinationManager();
        Status const status = mdm->startCommit(sessionId);
        mdm->report(result);
        if (!status.isOK()) {
            log() << status.reason();
            return appendCommandStatus(result, status);
        }
        return true;
    }

} recvChunkCommitCommand;

class RecvChunkAbortCommand : public BasicCommand {
public:
    RecvChunkAbortCommand() : BasicCommand("_recvChunkAbort") {}

    void help(std::stringstream& h) const {
        h << "internal";
    }

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::internal);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }

    bool run(OperationContext* opCtx,
             const string&,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        auto const mdm = ShardingState::get(opCtx)->migrationDestinationManager();

        auto migrationSessionIdStatus(MigrationSessionId::extractFromBSON(cmdObj));

        if (migrationSessionIdStatus.isOK()) {
            Status const status = mdm->abort(migrationSessionIdStatus.getValue());
            mdm->report(result);
            if (!status.isOK()) {
                log() << status.reason();
                return appendCommandStatus(result, status);
            }
        } else if (migrationSessionIdStatus == ErrorCodes::NoSuchKey) {
            mdm->abortWithoutSessionIdCheck();
            mdm->report(result);
        }
        uassertStatusOK(migrationSessionIdStatus.getStatus());
        return true;
    }

} recvChunkAbortCommand;

}  // namespace
}  // namespace mongo
