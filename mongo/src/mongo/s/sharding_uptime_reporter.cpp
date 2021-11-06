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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_uptime_reporter.h"

#include "mongo/db/client.h"
#include "mongo/db/server_options.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_mongos.h"
#include "mongo/s/grid.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/version.h"

namespace mongo {
namespace {

//10S
const Seconds kUptimeReportInterval(10);

//hostname:port ���
std::string constructInstanceIdString() {
    return str::stream() << getHostNameCached() << ":" << serverGlobalParams.port;
}

/**
 * Reports the uptime status of the current instance to the config.pings collection. This method
 * is best-effort and never throws.

 mongos> db.mongos.find()
{ "_id" : "bjhtxxx1:20003", "advisoryHostFQDNs" : [ ], "mongoVersion" : "3.6.10", "ping" : ISODate("2020-08-13T09:19:30.154Z"), "up" : NumberLong(15653743), "waiting" : true }
{ "_id" : "bjhtxxx2:20003", "advisoryHostFQDNs" : [ ], "mongoVersion" : "3.6.13", "ping" : ISODate("2020-08-13T09:19:31.911Z"), "up" : NumberLong(18239828), "waiting" : true }
{ "_id" : "bjhtxxx3:20002", "advisoryHostFQDNs" : [ ], "mongoVersion" : "3.6.13", "ping" : ISODate("2020-08-13T09:19:24.496Z"), "up" : NumberLong(18320414), "waiting" : true }
mongos> 


 bjhtxxx2:20022��kill������pingʱ���upʱ�䲻�����ӣ�ping��up����10s����
 { "_id" : "bjhtxxx1:20009", "advisoryHostFQDNs" : [ ], "mongoVersion" : "3.6.10", "ping" : ISODate("2020-08-13T11:10:21.458Z"), "up" : NumberLong(14227), "waiting" : true }
 { "_id" : "bjhtxxx2:20022", "advisoryHostFQDNs" : [ ], "mongoVersion" : "3.6.10", "ping" : ISODate("2020-08-13T11:08:37.637Z"), "up" : NumberLong(14256), "waiting" : true }
 { "_id" : "bjhtxxx3:20009", "advisoryHostFQDNs" : [ ], "mongoVersion" : "3.6.10", "ping" : ISODate("2020-08-13T11:10:21.323Z"), "up" : NumberLong(14307), "waiting" : true }

 */
//ShardingUptimeReporter::startPeriodicThread�߳�ѭ������  10sִ��һ��
void reportStatus(OperationContext* opCtx,
                  const std::string& instanceId,
                  const Timer& upTimeTimer) {
    MongosType mType;
    mType.setName(instanceId);
	//ʱ�������
    mType.setPing(jsTime());
    mType.setUptime(upTimeTimer.seconds());
    // balancer is never active in mongos. Here for backwards compatibility only.
    mType.setWaiting(true);
	//version��Ϣ
    mType.setMongoVersion(VersionInfoInterface::instance().version().toString());

	//db.pings.update({ _id : "bjhtxxx1:20003" }, { $set : {mType } })
    try { //Զ�̸���cfg��config.pings��
        Grid::get(opCtx)
            ->catalogClient()
            ->updateConfigDocument(opCtx,
                                   MongosType::ConfigNS,
                                   BSON(MongosType::name(instanceId)),
                                   BSON("$set" << mType.toBSON()),
                                   true,
                                   ShardingCatalogClient::kMajorityWriteConcern)
            .status_with_transitional_ignore();
    } catch (const std::exception& e) {
        log() << "Caught exception while reporting uptime: " << e.what();
    }
}

}  // namespace

ShardingUptimeReporter::ShardingUptimeReporter() = default;

ShardingUptimeReporter::~ShardingUptimeReporter() {
    // The thread must not be running when this object is destroyed
    invariant(!_thread.joinable());
}

void ShardingUptimeReporter::startPeriodicThread() {
    invariant(!_thread.joinable());

    _thread = stdx::thread([this] {
        Client::initThread("Uptime reporter");

		//hostname:port ���
        const std::string instanceId(constructInstanceIdString());
        const Timer upTimeTimer;

		//ÿ��10s��CFG��config.pings�������£���config.pings�����mongos�����򲻻����
		//"ping"��"up"��10s����
        while (!globalInShutdownDeprecated()) {
            {
                auto opCtx = cc().makeOperationContext();
                reportStatus(opCtx.get(), instanceId, upTimeTimer);

                auto status = Grid::get(opCtx.get())
                                  ->getBalancerConfiguration()
                                  ->refreshAndCheck(opCtx.get());
                if (!status.isOK()) {
                    warning() << "failed to refresh mongos settings" << causedBy(status);
                }
            }

            MONGO_IDLE_THREAD_BLOCK;
			//��ʱ10s��Ҳ����ÿ10s��config.pings������
            sleepFor(kUptimeReportInterval);
        }
    });
}


}  // namespace mongo
