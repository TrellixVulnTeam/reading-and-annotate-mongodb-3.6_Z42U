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

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"

namespace mongo {
namespace {

/*
��Ƭ1��Ϣ����:
mongodb_4.0_shard1:PRIMARY> db.runCommand({getShardVersion :"test.test"})
{
        "configServer" : "mongodb_4.0_config/192.168.10.1:10001,192.168.10.1:10002,192.168.10.1:10003",
        "inShardedMode" : false,
        "mine" : Timestamp(0, 0),
        "global" : Timestamp(13, 4),
        "ok" : 1,
        "operationTime" : Timestamp(1650100503, 1),
        "$gleStats" : {
                "lastOpTime" : Timestamp(0, 0),
                "electionId" : ObjectId("7fffffff0000000000000017")
        },
        "lastCommittedOpTime" : Timestamp(1650100503, 1),
        "$configServerState" : {
                "opTime" : {
                        "ts" : Timestamp(1650100505, 3),
                        "t" : NumberLong(13)
                }
        },
        "$clusterTime" : {
                "clusterTime" : Timestamp(1650100505, 3),
                "signature" : {
                        "hash" : BinData(0,"AAAAAAAAAAAAAAAAAAAAAAAAAAA="),
                        "keyId" : NumberLong(0)
                }
        }
}
mongodb_4.0_shard1:PRIMARY> 
switched to db config
mongodb_4.0_shard1:PRIMARY> db.cache.chunks.test.test.find({"shard" : "mongodb_4.0_shard1"}).sort({lastmod:-1}).limit(1)
{ "_id" : { "id" : 8154998 }, "max" : { "id" : 8168354 }, "shard" : "mongodb_4.0_shard1", "lastmod" : Timestamp(13, 4), "history" : [ { "validAfter" : Timestamp(1642842039, 9549), "shard" : "mongodb_4.0_shard1" } ] }
mongodb_4.0_shard1:PRIMARY> 
mongodb_4.0_shard1:PRIMARY> db.cache.chunks.test.test.find({"shard" : "mongodb_4.0_shard2"}).sort({lastmod:-1}).limit(1)
{ "_id" : { "id" : 24872948 }, "max" : { "id" : 32452630 }, "shard" : "mongodb_4.0_shard2", "lastmod" : Timestamp(13, 1), "history" : [ { "validAfter" : Timestamp(1642840304, 1), "shard" : "mongodb_4.0_shard2" } ] }
mongodb_4.0_shard1:PRIMARY> 



��Ƭ2��Ϣ����:

mongodb_4.0_shard2:PRIMARY> use admin
switched to db admin
mongodb_4.0_shard2:PRIMARY> db.runCommand({getShardVersion :"test.test"})
{
        "configServer" : "mongodb_4.0_config/192.168.10.1:10001,192.168.10.1:10002,192.168.10.1:10003",
        "inShardedMode" : false,
        "mine" : Timestamp(0, 0),
        "global" : Timestamp(13, 1),
        "ok" : 1,
        "operationTime" : Timestamp(1650100053, 1),
        "$gleStats" : {
                "lastOpTime" : Timestamp(0, 0),
                "electionId" : ObjectId("7fffffff0000000000000018")
        },
        "lastCommittedOpTime" : Timestamp(1650100053, 1),
        "$configServerState" : {
                "opTime" : {
                        "ts" : Timestamp(1650100055, 3),
                        "t" : NumberLong(13)
                }
        },
        "$clusterTime" : {
                "clusterTime" : Timestamp(1650100055, 3),
                "signature" : {
                        "hash" : BinData(0,"GTITjn7FTQgOb6lVJh6Sz6D4TBE="),
                        "keyId" : NumberLong("7051130195905871878")
                }
        }
}
mongodb_4.0_shard2:PRIMARY> use config
switched to db config
mongodb_4.0_shard2:PRIMARY> 
mongodb_4.0_shard2:PRIMARY> 
mongodb_4.0_shard2:PRIMARY> 
mongodb_4.0_shard2:PRIMARY> db.cache.chunks.test.test.find({"shard" : "mongodb_4.0_shard2"}).sort({lastmod:-1}).limit(1)
{ "_id" : { "id" : 24872948 }, "max" : { "id" : 32452630 }, "shard" : "mongodb_4.0_shard2", "lastmod" : Timestamp(13, 1), "history" : [ { "validAfter" : Timestamp(1642840304, 1), "shard" : "mongodb_4.0_shard2" } ] }
mongodb_4.0_shard2:PRIMARY> 
mongodb_4.0_shard2:PRIMARY> 
mongodb_4.0_shard2:PRIMARY> db.cache.chunks.test.test.find({"shard" : "mongodb_4.0_shard1"}).sort({lastmod:-1}).limit(1)
{ "_id" : { "id" : 8154998 }, "max" : { "id" : 8168354 }, "shard" : "mongodb_4.0_shard1", "lastmod" : Timestamp(13, 4), "history" : [ { "validAfter" : Timestamp(1642842039, 9549), "shard" : "mongodb_4.0_shard1" } ] }
mongodb_4.0_shard2:PRIMARY> 
mongodb_4.0_shard2:PRIMARY> 

��������Կ���cache.chunks.db.collection�м�¼���Ǹñ����е�chunk,��������������Ƭ��������������Ƭ�ģ�shard versionΪ����Ƭ����chunk�汾

//collection version Ϊ sharded collection ������shard����ߵ� chunk version
//shard versionΪ����Ƭ��collection���İ汾��Ϣ��Ҳ���Ǳ���Ƭconfig.cache.chunks.db.collection�����İ汾�ţ�
//collection versionΪ�ñ������з�Ƭ�İ汾��Ϣ�����ֵ��Ҳ����config server�иñ�汾��Ϣ�����ֵ
*/ 
//db.runCommand({getShardVersion :"test.test"})
//db.runCommand({getShardVersion: "test.test", fullMetadata: true});
class GetShardVersion : public BasicCommand {
public:
    GetShardVersion() : BasicCommand("getShardVersion") {}

    void help(std::stringstream& help) const override {
        help << " example: { getShardVersion : 'alleyinsider.foo'  } ";
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(parseNs(dbname, cmdObj))),
                ActionType::getShardVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbname, cmdObj));

        ShardingState* const shardingState = ShardingState::get(opCtx);
        if (shardingState->enabled()) {//�����˷�Ƭ����
            result.append("configServer", shardingState->getConfigServer(opCtx).toString());
        } else {
            result.append("configServer", "");
        }

		//��¼�ǲ��Ǵ�mongos��������
        ShardedConnectionInfo* const sci = ShardedConnectionInfo::get(opCtx->getClient(), false);
        result.appendBool("inShardedMode", sci != nullptr); 
        if (sci) { //client��mongos����������
            result.appendTimestamp("mine", sci->getVersion(nss.ns()).toLong());
        } else {
            result.appendTimestamp("mine", 0);
        }

        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
		
        CollectionShardingState* const css = CollectionShardingState::get(opCtx, nss);


		//CollectionShardingState::getMetadata          mongosͨ��getShardedCollection��ȡchunk���汾��Ϣ
        const auto metadata = css->getMetadata();
        if (metadata) {
			//ScopedCollectionMetadata�е�->()�������ߵ�CollectionMetadata::getShardVersion
            result.appendTimestamp("global", metadata->getShardVersion().toLong());
        } else {
            result.appendTimestamp("global", ChunkVersion(0, 0, OID()).toLong());
        }

        if (cmdObj["fullMetadata"].trueValue()) {
            BSONObjBuilder metadataBuilder(result.subobjStart("metadata"));
            if (metadata) {
                metadata->toBSONBasic(metadataBuilder);

                BSONArrayBuilder chunksArr(metadataBuilder.subarrayStart("chunks"));
                metadata->toBSONChunks(chunksArr);
                chunksArr.doneFast();

                BSONArrayBuilder pendingArr(metadataBuilder.subarrayStart("pending"));
                css->toBSONPending(pendingArr);
                pendingArr.doneFast();
            }
            metadataBuilder.doneFast();
        }

        return true;
    }

} getShardVersionCmd;

}  // namespace
}  // namespace mongo
