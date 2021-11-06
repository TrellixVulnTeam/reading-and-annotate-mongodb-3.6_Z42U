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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/balancer.h"
#include "mongo/s/request_types/balance_chunk_request_type.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

using std::string;
using str::stream;

/*
mongosִ��:db.runCommand({movePrimary:"test", to:"XX_gQmJGvRW_shard_2"})

{
ԭ��Ƭ��ӡ
2020-09-10T20:41:10.672+0800 I COMMAND  [conn378169] dropDatabase test - starting
2020-09-10T20:41:10.672+0800 I COMMAND  [conn378169] dropDatabase test - dropping 2 collections
2020-09-10T20:41:10.672+0800 I COMMAND  [conn378169] dropDatabase test - dropping collection: test.item_commit_info
2020-09-10T20:41:10.672+0800 I STORAGE  [conn378169] dropCollection: test.item_commit_info (cf56fa2d-6d8b-4320-8a4b-0119aa13125a) - renaming to drop-pending collection: test.system.drop.1599741670i2990t13.item_commit_info with drop optime { ts: Timestamp(1599741670, 2990), t: 13 }
2020-09-10T20:41:10.674+0800 I COMMAND  [conn378169] dropDatabase test - dropping collection: test.test1
2020-09-10T20:41:10.674+0800 I STORAGE  [conn378169] dropCollection: test.test1 (44fb57d7-b804-424f-9695-c4d8aac078f9) - renaming to drop-pending collection: test.system.drop.1599741670i3000t13.test1 with drop optime { ts: Timestamp(1599741670, 3000), t: 13 }
2020-09-10T20:41:10.681+0800 I REPL     [replication-8766] Completing collection drop for test.system.drop.1599741670i2990t13.item_commit_info with drop optime { ts: Timestamp(1599741670, 2990), t: 13 } (notification optime: { ts: Timestamp(1599741670, 2990), t: 13 })
2020-09-10T20:41:10.681+0800 I STORAGE  [replication-8766] Finishing collection drop for test.system.drop.1599741670i2990t13.item_commit_info (cf56fa2d-6d8b-4320-8a4b-0119aa13125a).
2020-09-10T20:41:10.682+0800 I REPL     [replication-8768] Completing collection drop for test.system.drop.1599741670i2990t13.item_commit_info with drop optime { ts: Timestamp(1599741670, 2990), t: 13 } (notification optime: { ts: Timestamp(1599741670, 3000), t: 13 })
2020-09-10T20:41:10.682+0800 I COMMAND  [conn378169] dropDatabase test - successfully dropped 2 collections (most recent drop optime: { ts: Timestamp(1599741670, 3000), t: 13 }) after 7ms. dropping database
2020-09-10T20:41:10.710+0800 I REPL     [replication-8768] Completing collection drop for test.system.drop.1599741670i3000t13.test1 with drop optime { ts: Timestamp(1599741670, 3000), t: 13 } (notification optime: { ts: Timestamp(1599741670, 3000), t: 13 })
2020-09-10T20:41:10.728+0800 I REPL     [replication-8766] Completing collection drop for test.system.drop.1599741670i3000t13.test1 with drop optime { ts: Timestamp(1599741670, 3000), t: 13 } (notification optime: { ts: Timestamp(1599741670, 3011), t: 13 })
2020-09-10T20:41:10.728+0800 I REPL     [replication-8769] Completing collection drop for test.system.drop.1599741670i3000t13.test1 with drop optime { ts: Timestamp(1599741670, 3000), t: 13 } (notification optime: { ts: Timestamp(1599741670, 3012), t: 13 })
2020-09-10T20:41:10.730+0800 I REPL     [replication-8770] Completing collection drop for test.system.drop.1599741670i3000t13.test1 with drop optime { ts: Timestamp(1599741670, 3000), t: 13 } (notification optime: { ts: Timestamp(1599741670, 3048), t: 13 })
2020-09-10T20:41:10.731+0800 I COMMAND  [conn378169] dropDatabase test - finished

Ŀ�ķ�Ƭ���ڵ��ӡ��
2020-09-10T20:41:10.559+0800 I STORAGE  [conn2055546] createCollection: test.item_commit_info with generated UUID: 5d22309b-ef17-4d01-adf2-b9f494ee143a
2020-09-10T20:41:10.579+0800 I STORAGE  [conn2055546] createCollection: test.test1 with generated UUID: 649b14a7-7a75-4ef1-a528-91f89adc046e
2020-09-10T20:41:10.636+0800 I STORAGE  [conn2055546] copying indexes for: { name: "item_commit_info", type: "collection", options: {}, info: { readOnly: false, uuid: UUID("cf56fa2d-6d8b-4320-8a4b-0119aa13125a") }, idIndex: { v: 2, key: { _id: 1 }, name: "_id_", ns: "test.item_commit_info" } }
2020-09-10T20:41:10.645+0800 I INDEX    [conn2055546] build index on: test.item_commit_info properties: { v: 2, unique: true, key: { tag: 1.0 }, name: "tag_1", ns: "test.item_commit_info" }
2020-09-10T20:41:10.645+0800 I INDEX    [conn2055546]    building index using bulk method; build may temporarily use up to 250 megabytes of RAM
2020-09-10T20:41:10.652+0800 I INDEX    [conn2055546] build index on: test.item_commit_info properties: { v: 2, unique: true, key: { tag2: 1.0 }, name: "testindex", ns: "test.item_commit_info" }
2020-09-10T20:41:10.652+0800 I INDEX    [conn2055546]    building index using bulk method; build may temporarily use up to 250 megabytes of RAM
2020-09-10T20:41:10.655+0800 I INDEX    [conn2055546] build index done.  scanned 1 total records. 0 secs
2020-09-10T20:41:10.655+0800 I STORAGE  [conn2055546] copying indexes for: { name: "test1", type: "collection", options: {}, info: { readOnly: false, uuid: UUID("44fb57d7-b804-424f-9695-c4d8aac078f9") }, idIndex: { v: 2, key: { _id: 1 }, name: "_id_", ns: "test.test1" } }
2020-09-10T20:41:10.668+0800 I COMMAND  [conn2055546] command test.$cmd appName: "MongoDB Shell" command: clone { clone: "opush_gQmJGvRW_shard_1/10.36.116.42:20001,10.37.72.102:20001,10.37.76.22:20001", collsToIgnore: [], bypassDocumentValidation: true, writeConcern: { w: "majority", wtimeout: 60000 }, $db: "test", $clusterTime: { clusterTime: Timestamp(1599741670, 2347), signature: { hash: BinData(0, C29F3B6CBB7BB2A931E07D1CFA6E71953A464885), keyId: 6829778851464216577 } }, $client: { application: { name: "MongoDB Shell" }, driver: { name: "MongoDB Internal Client", version: "3.6.14" }, os: { type: "Linux", name: "CentOS release 6.8 (Final)", architecture: "x86_64", version: "Kernel 2.6.32-642.el6.x86_64" }, mongos: { host: "bjht7266:20003", client: "10.35.150.17:44094", version: "3.6.10" } }, $configServerState: { opTime: { ts: Timestamp(1599741670, 2347), t: 7 } } } numYields:0 reslen:353 locks:{ Global: { acquireCount: { r: 17, w: 15, W: 2 }, acquireWaitCount: { W: 2 }, timeAcquiringMicros: { W: 6420 } }, Database: { acquireCount: { w: 10, W: 5 } }, oplog: { acquireCount: { w: 10 } } } protocol:op_msg 123ms

*/
//Դ��Ƭ�յ�config server���͹�����moveChunk����  
//ע��MoveChunkCmd��MoveChunkCommand������MoveChunkCmdΪ�����յ�mongo shell�ȿͻ��˵Ĵ������̣�
//Ȼ�����configsvr_client::moveChunk������_configsvrMoveChunk��config server,��config serverͳһ
//����movechunk��shardִ��chunk�������Ӷ�ִ��MoveChunkCommand::run�����shard��������shard��Ǩ��

//MoveChunkCommandΪshard�յ�movechunk�������������Ǩ�Ƶ����
//MoveChunkCmdΪmongos�յ��ͻ���movechunk����Ĵ������̣�ת����config server
//ConfigSvrMoveChunkCommandΪconfig server�յ�mongos��������_configsvrMoveChunk����Ĵ�������

//�Զ�balancer����shard������������Ǩ�������Balancer::_moveChunks->MigrationManager::executeMigrationsForAutoBalance
//�ֶ�balance��config�յ�����ConfigSvrMoveChunkCommand�����Ǩ�����Balancer::moveSingleChunk


//mongos��Ӧ�Ľӿ�����:
//BalanceChunkRequest::serializeToMoveCommandForConfig      
//BalanceChunkRequest::serializeToRebalanceCommandForConfig
class ConfigSvrMoveChunkCommand : public BasicCommand {
public:
    ConfigSvrMoveChunkCommand() : BasicCommand("_configsvrMoveChunk") {}

    void help(std::stringstream& help) const override {
        help << "Internal command, which is exported by the sharding config server. Do not call "
                "directly. Requests the balancer to move or rebalance a single chunk.";
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& unusedDbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        auto request = uassertStatusOK(BalanceChunkRequest::parseFromConfigCommand(cmdObj));

		//�ֶ�movechunk
        if (request.hasToShardId()) {
			//Balancer::moveSingleChunk
            uassertStatusOK(Balancer::get(opCtx)->moveSingleChunk(opCtx,
                                                                  request.getChunk(),
                                                                  request.getToShardId(),
                                                                  request.getMaxChunkSizeBytes(),
                                                                  request.getSecondaryThrottle(),
                                                                  request.getWaitForDelete()));
        } else { //�Զ�movechunk
        	//Balancer::rebalanceSingleChunk
            uassertStatusOK(Balancer::get(opCtx)->rebalanceSingleChunk(opCtx, request.getChunk()));
        }

        return true;
    }

} configSvrMoveChunk;

}  // namespace
}  // namespace mongo
