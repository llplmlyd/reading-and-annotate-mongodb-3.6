/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/s/catalog/dist_lock_manager_mock.h"

#include <algorithm>

#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

void NoLockFuncSet(StringData name, StringData whyMessage, Milliseconds waitFor) {
    FAIL(str::stream() << "Lock not expected to be called. "
                       << "Name: "
                       << name
                       << ", whyMessage: "
                       << whyMessage
                       << ", waitFor: "
                       << waitFor);
}

}  // namespace

DistLockManagerMock::DistLockManagerMock(std::unique_ptr<DistLockCatalog> catalog)
    : _catalog(std::move(catalog)), _lockReturnStatus{Status::OK()}, _lockChecker{NoLockFuncSet} {}

void DistLockManagerMock::startUp() {}

void DistLockManagerMock::shutDown(OperationContext* opCtx) {
    uassert(28659, "DistLockManagerMock shut down with outstanding locks present", _locks.empty());
}

std::string DistLockManagerMock::getProcessID() {
    return "Mock dist lock manager process id";
}

//MigrationManager::_schedule调用
//获取name这个锁
StatusWith<DistLockHandle> DistLockManagerMock::lockWithSessionID(OperationContext* opCtx,
                                                                  StringData name,
                                                                  StringData whyMessage,
                                                                  const OID& lockSessionID,
                                                                  Milliseconds waitFor) {
    _lockChecker(name, whyMessage, waitFor);
    _lockChecker = NoLockFuncSet;

    if (!_lockReturnStatus.isOK()) {
        return _lockReturnStatus;
    }

	//遍历查找  _locks记录了本mongos上面缓存的分布式锁，一个锁对应一个name，实际上就是一个表信息
    if (_locks.end() != std::find_if(_locks.begin(), _locks.end(), [name](LockInfo info) -> bool {
            return info.name == name;
        })) {
        return Status(ErrorCodes::LockBusy,
                      str::stream() << "Lock \"" << name << "\" is already taken");
    }

	//locks中没有，则添加进去，同时获取这个锁ID
    LockInfo info;
    info.name = name.toString();
    info.lockID = lockSessionID;
    _locks.push_back(info);

    return info.lockID;
}

StatusWith<DistLockHandle> DistLockManagerMock::tryLockWithLocalWriteConcern(
    OperationContext* opCtx, StringData name, StringData whyMessage, const OID& lockSessionID) {
    // Not yet implemented
    MONGO_UNREACHABLE;
}

void DistLockManagerMock::unlockAll(OperationContext* opCtx, const std::string& processID) {
    // Not yet implemented
    MONGO_UNREACHABLE;
}

//从队列中去除缓存的锁
void DistLockManagerMock::unlock(OperationContext* opCtx, const DistLockHandle& lockHandle) {
    std::vector<LockInfo>::iterator it =
        std::find_if(_locks.begin(), _locks.end(), [&lockHandle](LockInfo info) -> bool {
            return info.lockID == lockHandle;
        });
    if (it == _locks.end()) {
        return;
    }
    _locks.erase(it);
}

void DistLockManagerMock::unlock(OperationContext* opCtx,
                                 const DistLockHandle& lockHandle,
                                 StringData name) {
    std::vector<LockInfo>::iterator it =
        std::find_if(_locks.begin(), _locks.end(), [&lockHandle, &name](LockInfo info) -> bool {
            return ((info.lockID == lockHandle) && (info.name == name));
        });
    if (it == _locks.end()) {
        return;
    }
    _locks.erase(it);
}

Status DistLockManagerMock::checkStatus(OperationContext* opCtx, const DistLockHandle& lockHandle) {
    return Status::OK();
}

void DistLockManagerMock::expectLock(LockFunc checker, Status status) {
    _lockReturnStatus = std::move(status);
    _lockChecker = checker;
}

}  // namespace mongo
