/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/system_tick_source.h"

namespace mongo {
namespace {
//全局变量 service_context_d.cpp:    makeMongoDServiceContext中初始化赋值
//mongos赋值在mongoSMain上面的函数中，编译的时候已经确定了,对应ServiceContextNoop
ServiceContext* globalServiceContext = nullptr;
stdx::mutex globalServiceContextMutex;
stdx::condition_variable globalServiceContextCV;

}  // namespace

bool hasGlobalServiceContext() {
    return globalServiceContext;
}

//fassert赋值见 assert_util.h #define fassert MONGO_fassert
//_initAndListen中调用该函数接口获取context
ServiceContext* getGlobalServiceContext() {
    fassert(17508, globalServiceContext);
    return globalServiceContext;
}

ServiceContext* waitAndGetGlobalServiceContext() {
    stdx::unique_lock<stdx::mutex> lk(globalServiceContextMutex);
    globalServiceContextCV.wait(lk, [] { return globalServiceContext; });
    fassert(40549, globalServiceContext);
    return globalServiceContext;
}

void setGlobalServiceContext(std::unique_ptr<ServiceContext>&& serviceContext) {
    fassert(17509, serviceContext.get());

    delete globalServiceContext;

    stdx::lock_guard<stdx::mutex> lk(globalServiceContextMutex);

    if (!globalServiceContext) {
        globalServiceContextCV.notify_all();
    }

    globalServiceContext = serviceContext.release();
}

//wiredtiger是支持的，见 WiredTigerKVEngine::supportsDocLocking
bool _supportsDocLocking = false;

bool supportsDocLocking() {
    return _supportsDocLocking;
}

bool isMMAPV1() {
    StorageEngine* globalStorageEngine = getGlobalServiceContext()->getGlobalStorageEngine();

    invariant(globalStorageEngine);
    return globalStorageEngine->isMmapV1();
}

//mongo::userCreateNSImpl中调用，建表的时候存储引擎相关检查
//存储引擎相关参数检测
Status validateStorageOptions(
    const BSONObj& storageEngineOptions,
    stdx::function<Status(const StorageEngine::Factory* const, const BSONObj&)> validateFunc) {
    BSONObjIterator storageIt(storageEngineOptions);
    while (storageIt.more()) {
        BSONElement storageElement = storageIt.next();
        StringData storageEngineName = storageElement.fieldNameStringData();
        if (storageElement.type() != mongo::Object) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "'storageEngine." << storageElement.fieldNameStringData()
                                        << "' has to be an embedded document.");
        }

        std::unique_ptr<StorageFactoriesIterator> sfi(
            getGlobalServiceContext()->makeStorageFactoriesIterator());
        invariant(sfi);
        bool found = false;
        while (sfi->more()) {
            const StorageEngine::Factory* const& factory = sfi->next();
            if (storageEngineName != factory->getCanonicalName()) {
                continue;
            }
            Status status = validateFunc(factory, storageElement.Obj());
            if (!status.isOK()) {
                return status;
            }
            found = true;
        }
        if (!found) {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << storageEngineName
                                        << " is not a registered storage engine for this server");
        }
    }
    return Status::OK();
}

ServiceContext::ServiceContext()
    : _tickSource(stdx::make_unique<SystemTickSource>()),
      _fastClockSource(stdx::make_unique<SystemClockSource>()),
      _preciseClockSource(stdx::make_unique<SystemClockSource>()) {}

ServiceContext::~ServiceContext() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_clients.empty());
}

//根据desc线程名和session信息构造一个唯一UniqueClient
ServiceContext::UniqueClient ServiceContext::makeClient(std::string desc,
                                                        transport::SessionHandle session) {
    std::unique_ptr<Client> client(new Client(std::move(desc), this, std::move(session)));
    auto observer = _clientObservers.cbegin();
    try {
        for (; observer != _clientObservers.cend(); ++observer) {
            observer->get()->onCreateClient(client.get());
        }
    } catch (...) {
        try {
            while (observer != _clientObservers.cbegin()) {
                --observer;
                observer->get()->onDestroyClient(client.get());
            }
        } catch (...) {
            std::terminate();
        }
        throw;
    }
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
		//client加入到_clients集合中
        invariant(_clients.insert(client.get()).second);
    }

	//根据client生成一个唯一UniqueClient
    return UniqueClient(client.release());
}

void ServiceContext::setPeriodicRunner(std::unique_ptr<PeriodicRunner> runner) {
    invariant(!_runner);
    _runner = std::move(runner);
}

PeriodicRunner* ServiceContext::getPeriodicRunner() const {
    return _runner.get();
}

transport::TransportLayer* ServiceContext::getTransportLayer() const {
    return _transportLayer.get();//对应TransportLayerManager._tls  即transportLayerASIO
}

//ServiceEntryPointMongod或者ServiceEntryPointMongos
ServiceEntryPoint* ServiceContext::getServiceEntryPoint() const {
    return _serviceEntryPoint.get();
}

/*
"adaptive") : <ServiceExecutorAdaptive>( 
"synchronous"): <ServiceExecutorSynchronous>(ctx));
}
*/
transport::ServiceExecutor* ServiceContext::getServiceExecutor() const {
    return _serviceExecutor.get();
}

//_initAndListen中有调用 赋值为OpObserverImpl类
void ServiceContext::setOpObserver(std::unique_ptr<OpObserver> opObserver) {
    _opObserver = std::move(opObserver);
}

//makeMongoDServiceContext ->ServiceContext::setTickSource中调用
void ServiceContext::setTickSource(std::unique_ptr<TickSource> newSource) {
    _tickSource = std::move(newSource);
}

//makeMongoDServiceContext _initAndListen  _main中调用，默认10ms
void ServiceContext::setFastClockSource(std::unique_ptr<ClockSource> newSource) {
    _fastClockSource = std::move(newSource);
}
//makeMongoDServiceContext 中调用
void ServiceContext::setPreciseClockSource(std::unique_ptr<ClockSource> newSource) {
    _preciseClockSource = std::move(newSource);
}

//makeMongoDServiceContext  ServiceEntryPointMongos  _initAndListen  runMongosServer中调用
void ServiceContext::setServiceEntryPoint(std::unique_ptr<ServiceEntryPoint> sep) {
    _serviceEntryPoint = std::move(sep);
}

//_initAndListen中执行 mongos在runMongosServer
void ServiceContext::setTransportLayer(std::unique_ptr<transport::TransportLayer> tl) {
	//对应transportLayerASIO
	_transportLayer = std::move(tl);
}

/*
if (config->serviceExecutor == "adaptive") { //异步方式
    ctx->setServiceExecutor(stdx::make_unique<ServiceExecutorAdaptive>(
        ctx, transportLayerASIO->getIOContext()));
} else if (config->serviceExecutor == "synchronous") { //同步方式
    ctx->setServiceExecutor(stdx::make_unique<ServiceExecutorSynchronous>(ctx));
}
*/
//TransportLayerManager::createWithConfig中执行
void ServiceContext::setServiceExecutor(std::unique_ptr<transport::ServiceExecutor> exec) {
    _serviceExecutor = std::move(exec); 
}

void ServiceContext::ClientDeleter::operator()(Client* client) const {
    ServiceContext* const service = client->getServiceContext();
    {
        stdx::lock_guard<stdx::mutex> lk(service->_mutex);
        invariant(service->_clients.erase(client));
    }
    try {
        for (const auto& observer : service->_clientObservers) {
            observer->onDestroyClient(client);
        }
    } catch (...) {
        std::terminate();
    }
    delete client;
}

//Client::makeOperationContext()  _initAndListen中调用
ServiceContext::UniqueOperationContext ServiceContext::makeOperationContext(Client* client) {
	//获取一个OperationContext类   
	//ServiceContextMongoD::_newOpCtx 
    auto opCtx = _newOpCtx(client, _nextOpId.fetchAndAdd(1));
    auto observer = _clientObservers.begin();
    try {
        for (; observer != _clientObservers.cend(); ++observer) {
            observer->get()->onCreateOperationContext(opCtx.get());
        }
    } catch (...) {
        try {
            while (observer != _clientObservers.cbegin()) {
                --observer;
                observer->get()->onDestroyOperationContext(opCtx.get());
            }
        } catch (...) {
            std::terminate();
        }
        throw;
    }
    {
        stdx::lock_guard<Client> lk(*client);
        client->setOperationContext(opCtx.get());
    }
    return UniqueOperationContext(opCtx.release());
};

//消耗OperationContext
void ServiceContext::OperationContextDeleter::operator()(OperationContext* opCtx) const {
    auto client = opCtx->getClient();
    auto service = client->getServiceContext();
    {
        stdx::lock_guard<Client> lk(*client);
        client->resetOperationContext();
    }
    try {
        for (const auto& observer : service->_clientObservers) {
            observer->onDestroyOperationContext(opCtx);
        }
    } catch (...) {
        std::terminate();
    }
    delete opCtx;
}

void ServiceContext::registerClientObserver(std::unique_ptr<ClientObserver> observer) {
    _clientObservers.push_back(std::move(observer));
}

ServiceContext::LockedClientsCursor::LockedClientsCursor(ServiceContext* service)
    : _lock(service->_mutex), _curr(service->_clients.cbegin()), _end(service->_clients.cend()) {}

Client* ServiceContext::LockedClientsCursor::next() {
    if (_curr == _end)
        return nullptr;
    Client* result = *_curr;
    ++_curr;
    return result;
}

BSONArray storageEngineList() {
    if (!hasGlobalServiceContext())
        return BSONArray();

    std::unique_ptr<StorageFactoriesIterator> sfi(
        getGlobalServiceContext()->makeStorageFactoriesIterator());

    if (!sfi)
        return BSONArray();

    BSONArrayBuilder engineArrayBuilder;

    while (sfi->more()) {
        engineArrayBuilder.append(sfi->next()->getCanonicalName());
    }

    return engineArrayBuilder.arr();
}

void appendStorageEngineList(BSONObjBuilder* result) {
    result->append("storageEngines", storageEngineList());
}

void ServiceContext::setKillAllOperations() {
    stdx::lock_guard<stdx::mutex> clientLock(_mutex);

    // Ensure that all newly created operation contexts will immediately be in the interrupted state
    _globalKill.store(true);

    // Interrupt all active operations
    for (auto&& client : _clients) {
        stdx::lock_guard<Client> lk(*client);
        auto opCtxToKill = client->getOperationContext();
        if (opCtxToKill) {
            killOperation(opCtxToKill, ErrorCodes::InterruptedAtShutdown);
        }
    }

    // Notify any listeners who need to reach to the server shutting down
    for (const auto listener : _killOpListeners) {
        try {
            listener->interruptAll();
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::killOperation(OperationContext* opCtx, ErrorCodes::Error killCode) {
    opCtx->markKilled(killCode);

    for (const auto listener : _killOpListeners) {
        try {
            listener->interrupt(opCtx->getOpID());
        } catch (...) {
            std::terminate();
        }
    }
}

void ServiceContext::killAllUserOperations(const OperationContext* opCtx,
                                           ErrorCodes::Error killCode) {
    for (LockedClientsCursor cursor(this); Client* client = cursor.next();) {
        if (!client->isFromUserConnection()) {
            // Don't kill system operations.
            continue;
        }

        stdx::lock_guard<Client> lk(*client);
        OperationContext* toKill = client->getOperationContext();

        // Don't kill ourself.
        if (toKill && toKill->getOpID() != opCtx->getOpID()) {
            killOperation(toKill, killCode);
        }
    }
}

void ServiceContext::unsetKillAllOperations() {
    _globalKill.store(false);
}

void ServiceContext::registerKillOpListener(KillOpListenerInterface* listener) {
    stdx::lock_guard<stdx::mutex> clientLock(_mutex);
    _killOpListeners.push_back(listener);
}

//waitForStartupComplete和notifyStartupComplete配合
void ServiceContext::waitForStartupComplete() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _startupCompleteCondVar.wait(lk, [this] { return _startupComplete; });
}

//waitForStartupComplete和notifyStartupComplete配合
void ServiceContext::notifyStartupComplete() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _startupComplete = true;
    lk.unlock();
    _startupCompleteCondVar.notify_all();
}

}  // namespace mongo
