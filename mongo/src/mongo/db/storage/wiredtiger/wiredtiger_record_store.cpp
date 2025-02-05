// wiredtiger_record_store.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/static_assert.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/mongod_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/oplog_hack.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_oplog_stones.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

//该文件接口实现底层WT引擎KV相关操作
namespace mongo {

using std::unique_ptr;
using std::string;

namespace {

static const int kMinimumRecordStoreVersion = 1;
static const int kCurrentRecordStoreVersion = 1;  // New record stores use this by default.
static const int kMaximumRecordStoreVersion = 1;
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion >= kMinimumRecordStoreVersion);
MONGO_STATIC_ASSERT(kCurrentRecordStoreVersion <= kMaximumRecordStoreVersion);

void checkOplogFormatVersion(OperationContext* opCtx, const std::string& uri) {
    StatusWith<BSONObj> appMetadata = WiredTigerUtil::getApplicationMetadata(opCtx, uri);
    fassertStatusOK(39999, appMetadata);

    fassertNoTrace(39998, appMetadata.getValue().getIntField("oplogKeyExtractionVersion") == 1);
}
}  // namespace

MONGO_FP_DECLARE(WTWriteConflictException);
MONGO_FP_DECLARE(WTWriteConflictExceptionForReads);

//WiredTigerEngineInit中注册wt引擎
const std::string kWiredTigerEngineName = "wiredTiger";

class WiredTigerRecordStore::OplogStones::InsertChange final : public RecoveryUnit::Change {
public:
    InsertChange(OplogStones* oplogStones,
                 int64_t bytesInserted,
                 RecordId highestInserted,
                 int64_t countInserted)
        : _oplogStones(oplogStones),
          _bytesInserted(bytesInserted),
          _highestInserted(highestInserted),
          _countInserted(countInserted) {}

    void commit() final {
        invariant(_bytesInserted >= 0);
        invariant(_highestInserted.isNormal());

        _oplogStones->_currentRecords.addAndFetch(_countInserted);
        int64_t newCurrentBytes = _oplogStones->_currentBytes.addAndFetch(_bytesInserted);
        if (newCurrentBytes >= _oplogStones->_minBytesPerStone) {
            _oplogStones->createNewStoneIfNeeded(_highestInserted);
        }
    }

    void rollback() final {}

private:
    OplogStones* _oplogStones;
    int64_t _bytesInserted;
    RecordId _highestInserted;
    int64_t _countInserted;
};

class WiredTigerRecordStore::OplogStones::TruncateChange final : public RecoveryUnit::Change {
public:
    TruncateChange(OplogStones* oplogStones) : _oplogStones(oplogStones) {}

    void commit() final {
        _oplogStones->_currentRecords.store(0);
        _oplogStones->_currentBytes.store(0);

        stdx::lock_guard<stdx::mutex> lk(_oplogStones->_mutex);
        _oplogStones->_stones.clear();
    }

    void rollback() final {}

private:
    OplogStones* _oplogStones;
};

WiredTigerRecordStore::OplogStones::OplogStones(OperationContext* opCtx, WiredTigerRecordStore* rs)
    : _rs(rs) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    invariant(rs->isCapped());
    invariant(rs->cappedMaxSize() > 0);
    unsigned long long maxSize = rs->cappedMaxSize();

    const unsigned long long kMinStonesToKeep = 10ULL;
    const unsigned long long kMaxStonesToKeep = 100ULL;

    unsigned long long numStones = maxSize / BSONObjMaxInternalSize;
    size_t numStonesToKeep = std::min(kMaxStonesToKeep, std::max(kMinStonesToKeep, numStones));
    _minBytesPerStone = maxSize / numStonesToKeep;
    invariant(_minBytesPerStone > 0);

    _calculateStones(opCtx, numStonesToKeep);
    _pokeReclaimThreadIfNeeded();  // Reclaim stones if over the limit.
}

bool WiredTigerRecordStore::OplogStones::isDead() {
    stdx::lock_guard<stdx::mutex> lk(_oplogReclaimMutex);
    return _isDead;
}

void WiredTigerRecordStore::OplogStones::kill() {
    {
        stdx::lock_guard<stdx::mutex> lk(_oplogReclaimMutex);
        _isDead = true;
    }
    _oplogReclaimCv.notify_one();
}

void WiredTigerRecordStore::OplogStones::awaitHasExcessStonesOrDead() {
    // Wait until kill() is called or there are too many oplog stones.
    stdx::unique_lock<stdx::mutex> lock(_oplogReclaimMutex);
    while (!_isDead) {
        {
            MONGO_IDLE_THREAD_BLOCK;
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (hasExcessStones_inlock()) {
                break;
            }
        }
        _oplogReclaimCv.wait(lock);
    }
}

boost::optional<WiredTigerRecordStore::OplogStones::Stone>
WiredTigerRecordStore::OplogStones::peekOldestStoneIfNeeded() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (!hasExcessStones_inlock()) {
        return {};
    }

    return _stones.front();
}

void WiredTigerRecordStore::OplogStones::popOldestStone() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stones.pop_front();
}

void WiredTigerRecordStore::OplogStones::createNewStoneIfNeeded(RecordId lastRecord) {
    stdx::unique_lock<stdx::mutex> lk(_mutex, stdx::try_to_lock);
    if (!lk) {
        // Someone else is either already creating a new stone or popping the oldest one. In the
        // latter case, we let the next insert trigger the new stone's creation.
        return;
    }

    if (_currentBytes.load() < _minBytesPerStone) {
        // Must have raced to create a new stone, someone else already triggered it.
        return;
    }

    if (!_stones.empty() && lastRecord < _stones.back().lastRecord) {
        // Skip creating a new stone when the record's position comes before the most recently
        // created stone. We likely raced with another batch of inserts that caused us to try and
        // make multiples stones.
        return;
    }

    LOG(2) << "create new oplogStone, current stones:" << _stones.size();
    OplogStones::Stone stone = {_currentRecords.swap(0), _currentBytes.swap(0), lastRecord};
    _stones.push_back(stone);

    _pokeReclaimThreadIfNeeded();
}

void WiredTigerRecordStore::OplogStones::updateCurrentStoneAfterInsertOnCommit(
    OperationContext* opCtx,
    int64_t bytesInserted,
    RecordId highestInserted,
    int64_t countInserted) {
    opCtx->recoveryUnit()->registerChange(
        new InsertChange(this, bytesInserted, highestInserted, countInserted));
}

void WiredTigerRecordStore::OplogStones::clearStonesOnCommit(OperationContext* opCtx) {
    opCtx->recoveryUnit()->registerChange(new TruncateChange(this));
}

void WiredTigerRecordStore::OplogStones::updateStonesAfterCappedTruncateAfter(
    int64_t recordsRemoved, int64_t bytesRemoved, RecordId firstRemovedId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    int64_t numStonesToRemove = 0;
    int64_t recordsInStonesToRemove = 0;
    int64_t bytesInStonesToRemove = 0;

    // Compute the number and associated sizes of the records from stones that are either fully or
    // partially truncated.
    for (auto it = _stones.rbegin(); it != _stones.rend(); ++it) {
        if (it->lastRecord < firstRemovedId) {
            break;
        }
        numStonesToRemove++;
        recordsInStonesToRemove += it->records;
        bytesInStonesToRemove += it->bytes;
    }

    // Remove the stones corresponding to the records that were deleted.
    int64_t offset = _stones.size() - numStonesToRemove;
    _stones.erase(_stones.begin() + offset, _stones.end());

    // Account for any remaining records from a partially truncated stone in the stone currently
    // being filled.
    _currentRecords.addAndFetch(recordsInStonesToRemove - recordsRemoved);
    _currentBytes.addAndFetch(bytesInStonesToRemove - bytesRemoved);
}

void WiredTigerRecordStore::OplogStones::setMinBytesPerStone(int64_t size) {
    invariant(size > 0);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Only allow changing the minimum bytes per stone if no data has been inserted.
    invariant(_stones.size() == 0 && _currentRecords.load() == 0);
    _minBytesPerStone = size;
}

void WiredTigerRecordStore::OplogStones::_calculateStones(OperationContext* opCtx,
                                                          size_t numStonesToKeep) {
    long long numRecords = _rs->numRecords(opCtx);
    long long dataSize = _rs->dataSize(opCtx);

    log() << "The size storer reports that the oplog contains " << numRecords
          << " records totaling to " << dataSize << " bytes";

    // Only use sampling to estimate where to place the oplog stones if the number of samples drawn
    // is less than 5% of the collection.
    const uint64_t kMinSampleRatioForRandCursor = 20;

    // If the oplog doesn't contain enough records to make sampling more efficient, then scan the
    // oplog to determine where to put down stones.
    if (numRecords <= 0 || dataSize <= 0 ||
        uint64_t(numRecords) <
            kMinSampleRatioForRandCursor * kRandomSamplesPerStone * numStonesToKeep) {
        _calculateStonesByScanning(opCtx);
        return;
    }

    // Use the oplog's average record size to estimate the number of records in each stone, and thus
    // estimate the combined size of the records.
    double avgRecordSize = double(dataSize) / double(numRecords);
    double estRecordsPerStone = std::ceil(_minBytesPerStone / avgRecordSize);
    double estBytesPerStone = estRecordsPerStone * avgRecordSize;

    _calculateStonesBySampling(opCtx, int64_t(estRecordsPerStone), int64_t(estBytesPerStone));
}

void WiredTigerRecordStore::OplogStones::_calculateStonesByScanning(OperationContext* opCtx) {
    log() << "Scanning the oplog to determine where to place markers for truncation";

    long long numRecords = 0;
    long long dataSize = 0;

    auto cursor = _rs->getCursor(opCtx, true);
    while (auto record = cursor->next()) {
        _currentRecords.addAndFetch(1);
        int64_t newCurrentBytes = _currentBytes.addAndFetch(record->data.size());
        if (newCurrentBytes >= _minBytesPerStone) {
            LOG(1) << "Placing a marker at optime "
                   << Timestamp(record->id.repr()).toStringPretty();

            OplogStones::Stone stone = {_currentRecords.swap(0), _currentBytes.swap(0), record->id};
            _stones.push_back(stone);
        }

        numRecords++;
        dataSize += record->data.size();
    }

    _rs->updateStatsAfterRepair(opCtx, numRecords, dataSize);
}

void WiredTigerRecordStore::OplogStones::_calculateStonesBySampling(OperationContext* opCtx,
                                                                    int64_t estRecordsPerStone,
                                                                    int64_t estBytesPerStone) {
    Timestamp earliestOpTime;
    Timestamp latestOpTime;

    {
        const bool forward = true;
        auto cursor = _rs->getCursor(opCtx, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to determine the earliest optime, falling back to scanning the oplog";
            _calculateStonesByScanning(opCtx);
            return;
        }
        earliestOpTime = Timestamp(record->id.repr());
    }

    {
        const bool forward = false;
        auto cursor = _rs->getCursor(opCtx, forward);
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to determine the latest optime, falling back to scanning the oplog";
            _calculateStonesByScanning(opCtx);
            return;
        }
        latestOpTime = Timestamp(record->id.repr());
    }

    log() << "Sampling from the oplog between " << earliestOpTime.toStringPretty() << " and "
          << latestOpTime.toStringPretty() << " to determine where to place markers for truncation";

    int64_t wholeStones = _rs->numRecords(opCtx) / estRecordsPerStone;
    int64_t numSamples = kRandomSamplesPerStone * _rs->numRecords(opCtx) / estRecordsPerStone;

    log() << "Taking " << numSamples << " samples and assuming that each section of oplog contains"
          << " approximately " << estRecordsPerStone << " records totaling to " << estBytesPerStone
          << " bytes";

    // Inform the random cursor of the number of samples we intend to take. This allows it to
    // account for skew in the tree shape.
    const std::string extraConfig = str::stream() << "next_random_sample_size=" << numSamples;

    // Divide the oplog into 'wholeStones' logical sections, with each section containing
    // approximately 'estRecordsPerStone'. Do so by oversampling the oplog, sorting the samples in
    // order of their RecordId, and then choosing the samples expected to be near the right edge of
    // each logical section.
    auto cursor = _rs->getRandomCursorWithOptions(opCtx, extraConfig);
    std::vector<RecordId> oplogEstimates;
    for (int i = 0; i < numSamples; ++i) {
        auto record = cursor->next();
        if (!record) {
            // This shouldn't really happen unless the size storer values are far off from reality.
            // The collection is probably empty, but fall back to scanning the oplog just in case.
            log() << "Failed to get enough random samples, falling back to scanning the oplog";
            _calculateStonesByScanning(opCtx);
            return;
        }
        oplogEstimates.push_back(record->id);
    }
    std::sort(oplogEstimates.begin(), oplogEstimates.end());

    for (int i = 1; i <= wholeStones; ++i) {
        // Use every (kRandomSamplesPerStone)th sample, starting with the
        // (kRandomSamplesPerStone - 1)th, as the last record for each stone.
        int sampleIndex = kRandomSamplesPerStone * i - 1;
        RecordId lastRecord = oplogEstimates[sampleIndex];

        log() << "Placing a marker at optime " << Timestamp(lastRecord.repr()).toStringPretty();
        OplogStones::Stone stone = {estRecordsPerStone, estBytesPerStone, lastRecord};
        _stones.push_back(stone);
    }

    // Account for the partially filled chunk.
    _currentRecords.store(_rs->numRecords(opCtx) - estRecordsPerStone * wholeStones);
    _currentBytes.store(_rs->dataSize(opCtx) - estBytesPerStone * wholeStones);
}

void WiredTigerRecordStore::OplogStones::_pokeReclaimThreadIfNeeded() {
    if (hasExcessStones_inlock()) {
        _oplogReclaimCv.notify_one();
    }
}

void WiredTigerRecordStore::OplogStones::adjust(int64_t maxSize) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    const unsigned long long kMinStonesToKeep = 10ULL;
    const unsigned long long kMaxStonesToKeep = 100ULL;

    unsigned long long numStones = maxSize / BSONObjMaxInternalSize;
    size_t numStonesToKeep = std::min(kMaxStonesToKeep, std::max(kMinStonesToKeep, numStones));
    _minBytesPerStone = maxSize / numStonesToKeep;
    invariant(_minBytesPerStone > 0);
    _pokeReclaimThreadIfNeeded();
}

//wiredtiger存储引擎的配置参数检查，见wiredtiger_config_validate    validateCollectionStorageOptions调用
StatusWith<std::string> WiredTigerRecordStore::parseOptionsField(const BSONObj options) {
    StringBuilder ss;
    BSONForEach(elem, options) {
        if (elem.fieldNameStringData() == "configString") {
			//调用wiredtiger_config_validate做检查
            Status status = WiredTigerUtil::checkTableCreationOptions(elem);
            if (!status.isOK()) {
                return status;
            }
            ss << elem.valueStringData() << ',';
        } else {
            // Return error on first unrecognized field.
            return StatusWith<std::string>(ErrorCodes::InvalidOptions,
                                           str::stream() << '\'' << elem.fieldNameStringData()
                                                         << '\''
                                                         << " is not a supported option.");
        }
    }
    return StatusWith<std::string>(ss.str());
}

class WiredTigerRecordStore::RandomCursor final : public RecordCursor {
public:
    RandomCursor(OperationContext* opCtx, const WiredTigerRecordStore& rs, StringData config)
        : _cursor(nullptr), _rs(&rs), _opCtx(opCtx), _config(config.toString() + ",next_random") {
        restore();
    }

    ~RandomCursor() {
        if (_cursor)
            detachFromOperationContext();
    }

    boost::optional<Record> next() final {
        int advanceRet = WT_READ_CHECK(_cursor->next(_cursor));
        if (advanceRet == WT_NOTFOUND)
            return {};
        invariantWTOK(advanceRet);

        int64_t key;
        invariantWTOK(_cursor->get_key(_cursor, &key));
        const RecordId id = RecordId(key);

        WT_ITEM value;
        invariantWTOK(_cursor->get_value(_cursor, &value));

        return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
    }

    void save() final {
        if (_cursor && !wt_keeptxnopen()) {
            try {
                _cursor->reset(_cursor);
            } catch (const WriteConflictException&) {
                // Ignore since this is only called when we are about to kill our transaction
                // anyway.
            }
        }
    }

    bool restore() final {
        // We can't use the CursorCache since this cursor needs a special config string.
        WT_SESSION* session = WiredTigerRecoveryUnit::get(_opCtx)->getSession()->getSession();

        if (!_cursor) {
            invariantWTOK(session->open_cursor(
                session, _rs->_uri.c_str(), nullptr, _config.c_str(), &_cursor));
            invariant(_cursor);
        }
        return true;
    }

    void detachFromOperationContext() final {
        invariant(_opCtx);
        _opCtx = nullptr;
        if (_cursor) {
            invariantWTOK(_cursor->close(_cursor));
        }
        _cursor = nullptr;
    }

    void reattachToOperationContext(OperationContext* opCtx) final {
        invariant(!_opCtx);
        _opCtx = opCtx;
    }

private:
    WT_CURSOR* _cursor;
    const WiredTigerRecordStore* _rs;
    OperationContext* _opCtx;
    const std::string _config;
};

//WiredTigerRecordStore::generateCreateString获取创建wiredtiger表的uri
//WiredTigerIndex::generateCreateString获取创建wiredtiger索引的uri
//WiredTigerKVEngine::createGroupedRecordStore调用  
// static 
StatusWith<std::string> WiredTigerRecordStore::generateCreateString(
    const std::string& engineName,
    StringData ns,
    const CollectionOptions& options,
    StringData extraStrings,
    const bool prefixed) {
    // Separate out a prefix and suffix in the default string. User configuration will
    // override values in the prefix, but not values in the suffix.
    str::stream ss;
    ss << "type=file,";
    // Setting this larger than 10m can hurt latencies and throughput degradation if this
    // is the oplog.  See SERVER-16247
    ss << "memory_page_max=10m,";
    // Choose a higher split percent, since most usage is append only. Allow some space
    // for workloads where updates increase the size of documents.
    ss << "split_pct=90,";
    ss << "leaf_value_max=64MB,";
    ss << "checksum=on,";
    if (wiredTigerGlobalOptions.useCollectionPrefixCompression) {
        ss << "prefix_compression,";
    }

    ss << "block_compressor=" << wiredTigerGlobalOptions.collectionBlockCompressor << ",";

    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())->getTableCreateConfig(ns);

    ss << extraStrings << ",";

    StatusWith<std::string> customOptions =
        parseOptionsField(options.storageEngine.getObjectField(engineName));
    if (!customOptions.isOK())
        return customOptions;

    ss << customOptions.getValue();

    if (NamespaceString::oplog(ns)) {
        // force file for oplog
        ss << "type=file,";
        // Tune down to 10m.  See SERVER-16247
        ss << "memory_page_max=10m,";
    }
	
    // WARNING: No user-specified config can appear below this line. These options are required
    // for correct behavior of the server.
    //参考http://source.wiredtiger.com/3.0.0/schema.html#schema_column_types，确定key value的类型
    if (prefixed) {
        ss << "key_format=qq";
    } else {
        ss << "key_format=q";  //key类型为int64_t类型
    }
    ss << ",value_format=u";  //value类型为WT_ITEM *类型

    // Record store metadata
    ss << ",app_metadata=(formatVersion=" << kCurrentRecordStoreVersion;
    if (NamespaceString::oplog(ns)) {
        ss << ",oplogKeyExtractionVersion=1";
    }
    ss << ")";

    const bool keepOldLoggingSettings = true;
    if (keepOldLoggingSettings ||
        WiredTigerUtil::useTableLogging(NamespaceString(ns),
                                        getGlobalReplSettings().usingReplSets())) {
        ss << ",log=(enabled=true)";
    } else {
        ss << ",log=(enabled=false)";
    }

    return StatusWith<std::string>(ss);
}

//WiredTigerKVEngine::getGroupedRecordStore->WiredTigerRecordStore::WiredTigerRecordStore中构造使用

//StandardWiredTigerRecordStore::StandardWiredTigerRecordStore
//PrefixedWiredTigerRecordStore::PrefixedWiredTigerRecordStore中调用
WiredTigerRecordStore::WiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                             OperationContext* ctx,
                                             Params params)
    : RecordStore(params.ns),
      _uri(params.uri),
      _tableId(WiredTigerSession::genTableId()),
      _engineName(params.engineName),
      _isCapped(params.isCapped),
      _isEphemeral(params.isEphemeral),
      _isOplog(NamespaceString::oplog(params.ns)),
      _cappedMaxSize(params.cappedMaxSize),
      _cappedMaxSizeSlack(std::min(params.cappedMaxSize / 10, int64_t(16 * 1024 * 1024))),
      _cappedMaxDocs(params.cappedMaxDocs),
      _cappedSleep(0),
      _cappedSleepMS(0),
      _cappedCallback(params.cappedCallback),
      _shuttingDown(false),
      _cappedDeleteCheckCount(0),
      _sizeStorer(params.sizeStorer),
      _sizeStorerCounter(0),
      _kvEngine(kvEngine) {
    Status versionStatus = WiredTigerUtil::checkApplicationMetadataFormatVersion(
                               ctx, _uri, kMinimumRecordStoreVersion, kMaximumRecordStoreVersion)
                               .getStatus();
    if (!versionStatus.isOK()) {
        std::cout << " Version: " << versionStatus.reason() << std::endl;
        if (versionStatus.code() == ErrorCodes::FailedToParse) {
            uasserted(28548, versionStatus.reason());
        } else {
            fassertFailedNoTrace(34433);
        }
    }

    if (_isCapped) {
        invariant(_cappedMaxSize > 0);
        invariant(_cappedMaxDocs == -1 || _cappedMaxDocs > 0);
    } else {
        invariant(_cappedMaxSize == -1);
        invariant(_cappedMaxDocs == -1);
    }

    if (!params.isReadOnly) {
        uassertStatusOK(WiredTigerUtil::setTableLogging(
            ctx,
            _uri,
            WiredTigerUtil::useTableLogging(NamespaceString(ns()),
                                            getGlobalReplSettings().usingReplSets())));
    }

    if (_isOplog) {
        checkOplogFormatVersion(ctx, _uri);
    }
}

WiredTigerRecordStore::~WiredTigerRecordStore() {
    {
        stdx::lock_guard<stdx::timed_mutex> lk(_cappedDeleterMutex);
        _shuttingDown = true;
    }

    LOG(1) << "~WiredTigerRecordStore for: " << ns();
    if (_sizeStorer) {
        _sizeStorer->onDestroy(this);
    }

    if (_oplogStones) {
        _oplogStones->kill();
    }

    if (_isOplog) {
        // Delete oplog visibility manager on KV engine.
        _kvEngine->haltOplogManager();
    }
}

//初始化获取当前table中最大RecordId，最大数据量 数据行数，启动startOplogManager OplogBackgroundThread等
void WiredTigerRecordStore::postConstructorInit(OperationContext* opCtx) {
    // Find the largest RecordId currently in use and estimate the number of records.
    //获取该表table最大的recordID
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, /*forward=*/false);
    if (auto record = cursor->next()) {
        int64_t max = record->id.repr();
        _nextIdNum.store(1 + max);

        if (_sizeStorer) {
            long long numRecords;
            long long dataSize;
			//获取当前最大数据量和数据行数
            _sizeStorer->loadFromCache(_uri, &numRecords, &dataSize);
            _numRecords.store(numRecords);
            _dataSize.store(dataSize);
            _sizeStorer->onCreate(this, numRecords, dataSize);
        } else {
            LOG(1) << "Doing scan of collection " << ns() << " to get size and count info";

            _numRecords.store(0);
            _dataSize.store(0);

            do {
                _numRecords.fetchAndAdd(1);
                _dataSize.fetchAndAdd(record->data.size());
            } while ((record = cursor->next()));
        }
    } else {
        _dataSize.store(0);
        _numRecords.store(0);
        // Need to start at 1 so we are always higher than RecordId::min()
        _nextIdNum.store(1);
        if (_sizeStorer)
            _sizeStorer->onCreate(this, 0, 0);
    }

    if (WiredTigerKVEngine::initRsOplogBackgroundThread(ns())) {
        _oplogStones = std::make_shared<OplogStones>(opCtx, this);
    }

    if (_isOplog) {
        invariant(_kvEngine);
        _kvEngine->startOplogManager(opCtx, _uri, this);
    }
}

const char* WiredTigerRecordStore::name() const {
    return _engineName.c_str();
}

bool WiredTigerRecordStore::inShutdown() const {
    stdx::lock_guard<stdx::timed_mutex> lk(_cappedDeleterMutex);
    return _shuttingDown;
}

long long WiredTigerRecordStore::dataSize(OperationContext* opCtx) const {
    return _dataSize.load();
}

long long WiredTigerRecordStore::numRecords(OperationContext* opCtx) const {
    return _numRecords.load();
}

bool WiredTigerRecordStore::isCapped() const {
    return _isCapped;
}

int64_t WiredTigerRecordStore::cappedMaxDocs() const {
    invariant(_isCapped);
    return _cappedMaxDocs;
}

int64_t WiredTigerRecordStore::cappedMaxSize() const {
    invariant(_isCapped);
    return _cappedMaxSize;
}

//获取WiredTigerRecordStore对应表的磁盘空间占用大小
int64_t WiredTigerRecordStore::storageSize(OperationContext* opCtx,
                                           BSONObjBuilder* extraInfo,
                                           int infoLevel) const {
    if (_isEphemeral) {
        return dataSize(opCtx);
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession();
    StatusWith<int64_t> result =
        WiredTigerUtil::getStatisticsValueAs<int64_t>(session->getSession(),
                                                      "statistics:" + getURI(),
                                                      "statistics=(size)",
                                                      WT_STAT_DSRC_BLOCK_SIZE);
    uassertStatusOK(result.getStatus());

    int64_t size = result.getValue();

    if (size == 0 && _isCapped) {
        // Many things assume an empty capped collection still takes up space.
        return 1;
    }
    return size;
}

//WiredTigerRecordStore::dataFor    WiredTigerRecordStore::findRecord
// Retrieve the value from a positioned cursor.
//通过游标获取数据信息
RecordData WiredTigerRecordStore::_getData(const WiredTigerCursor& cursor) const {
    WT_ITEM value;
    invariantWTOK(cursor->get_value(cursor.get(), &value));

    return RecordData(static_cast<const char*>(value.data), value.size).getOwned();
}

//根据ID获取数据信息
RecordData WiredTigerRecordStore::dataFor(OperationContext* opCtx, const RecordId& id) const {
    // ownership passes to the shared_array created below
    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    setKey(c, id);
    int ret = WT_READ_CHECK(c->search(c));
    massert(28556, "Didn't find RecordId in WiredTigerRecordStore", ret != WT_NOTFOUND);
    invariantWTOK(ret);
    return _getData(curwrap);
}

//根据ID获取数据信息
bool WiredTigerRecordStore::findRecord(OperationContext* opCtx,
                                       const RecordId& id,
                                       RecordData* out) const {
    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx); //WiredTigerCursor::WiredTigerCursor
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    setKey(c, id);
    int ret = WT_READ_CHECK(c->search(c));
    if (ret == WT_NOTFOUND) {
        return false;
    }
    invariantWTOK(ret);
    *out = _getData(curwrap);
    return true;
}

//删除key=id的数据
void WiredTigerRecordStore::deleteRecord(OperationContext* opCtx, const RecordId& id) {
    // Deletes should never occur on a capped collection because truncation uses
    // WT_SESSION::truncate().
    invariant(!isCapped());

    WiredTigerCursor cursor(_uri, _tableId, true, opCtx);
    cursor.assertInActiveTxn();
    WT_CURSOR* c = cursor.get();
    setKey(c, id);
    int ret = WT_READ_CHECK(c->search(c));
    invariantWTOK(ret);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret);

    int64_t old_length = old_value.size;

    ret = WT_OP_CHECK(c->remove(c));
    invariantWTOK(ret);

    _changeNumRecords(opCtx, -1);
    _increaseDataSize(opCtx, -old_length);
}

//capped collection集合大小不能超过现在，否则需要清除
bool WiredTigerRecordStore::cappedAndNeedDelete() const {
    if (!_isCapped)
        return false;

    if (_dataSize.load() >= _cappedMaxSize)
        return true;

    if ((_cappedMaxDocs != -1) && (_numRecords.load() > _cappedMaxDocs))
        return true;

    return false;
}

//capped collection集合大小不能超过现在，否则需要清除  cappedAndNeedDelete
int64_t WiredTigerRecordStore::cappedDeleteAsNeeded(OperationContext* opCtx,
                                                    const RecordId& justInserted) {
    invariant(!_oplogStones);

    // We only want to do the checks occasionally as they are expensive.
    // This variable isn't thread safe, but has loose semantics anyway.
    dassert(!_isOplog || _cappedMaxDocs == -1);

    if (!cappedAndNeedDelete())
        return 0;

    // ensure only one thread at a time can do deletes, otherwise they'll conflict.
    stdx::unique_lock<stdx::timed_mutex> lock(_cappedDeleterMutex, stdx::defer_lock);

    if (_cappedMaxDocs != -1) {
        lock.lock();  // Max docs has to be exact, so have to check every time.
    } else {
        if (!lock.try_lock()) {
            // Someone else is deleting old records. Apply back-pressure if too far behind,
            // otherwise continue.
            if ((_dataSize.load() - _cappedMaxSize) < _cappedMaxSizeSlack)
                return 0;

            // Don't wait forever: we're in a transaction, we could block eviction.
            Date_t before = Date_t::now();
            bool gotLock = lock.try_lock_for(stdx::chrono::milliseconds(200));
            auto delay =
                stdx::chrono::milliseconds(durationCount<Milliseconds>(Date_t::now() - before));
            _cappedSleep.fetchAndAdd(1);
            _cappedSleepMS.fetchAndAdd(delay.count());
            if (!gotLock)
                return 0;

            // If we already waited, let someone else do cleanup unless we are significantly
            // over the limit.
            if ((_dataSize.load() - _cappedMaxSize) < (2 * _cappedMaxSizeSlack))
                return 0;
        }
    }

    return cappedDeleteAsNeeded_inlock(opCtx, justInserted);
}

//wiredtiger  固定collection如果超限了，需要做清理操作
int64_t WiredTigerRecordStore::cappedDeleteAsNeeded_inlock(OperationContext* opCtx,
                                                           const RecordId& justInserted) {
    // we do this in a side transaction in case it aborts
    WiredTigerRecoveryUnit* realRecoveryUnit =
        checked_cast<WiredTigerRecoveryUnit*>(opCtx->releaseRecoveryUnit());
    invariant(realRecoveryUnit);
    WiredTigerSessionCache* sc = realRecoveryUnit->getSessionCache();
    OperationContext::RecoveryUnitState const realRUstate =
        opCtx->setRecoveryUnit(new WiredTigerRecoveryUnit(sc), OperationContext::kNotInUnitOfWork);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();

    int64_t dataSize = _dataSize.load();
    int64_t numRecords = _numRecords.load();

    int64_t sizeOverCap = (dataSize > _cappedMaxSize) ? dataSize - _cappedMaxSize : 0;
    int64_t sizeSaved = 0;
    int64_t docsOverCap = 0, docsRemoved = 0;
    if (_cappedMaxDocs != -1 && numRecords > _cappedMaxDocs)
        docsOverCap = numRecords - _cappedMaxDocs;

    try {
        WriteUnitOfWork wuow(opCtx);

        WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
        WT_CURSOR* truncateEnd = curwrap.get();
        RecordId newestIdToDelete;
        int ret = 0;
        bool positioned = false;  // Mark if the cursor is on the first key
        int64_t savedFirstKey = 0;

        // If we know where the first record is, go to it
        if (_cappedFirstRecord != RecordId()) {
            setKey(truncateEnd, _cappedFirstRecord);
            ret = WT_READ_CHECK(truncateEnd->search(truncateEnd));
            if (ret == 0) {
                positioned = true;
                savedFirstKey = _cappedFirstRecord.repr();
            }
        }

        // Advance the cursor truncateEnd until we find a suitable end point for our truncate
        while ((sizeSaved < sizeOverCap || docsRemoved < docsOverCap) && (docsRemoved < 20000) &&
               (positioned || (ret = WT_READ_CHECK(truncateEnd->next(truncateEnd))) == 0)) {
            positioned = false;

            newestIdToDelete = getKey(truncateEnd);
            // don't go past the record we just inserted
            if (newestIdToDelete >= justInserted)  // TODO: use oldest uncommitted instead
                break;

            WT_ITEM old_value;
            invariantWTOK(truncateEnd->get_value(truncateEnd, &old_value));

            ++docsRemoved;
            sizeSaved += old_value.size;

            stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
            if (_shuttingDown)
                break;

            if (_cappedCallback) {
                uassertStatusOK(_cappedCallback->aboutToDeleteCapped(
                    opCtx,
                    newestIdToDelete,
                    RecordData(static_cast<const char*>(old_value.data), old_value.size)));
            }
        }

        if (ret != WT_NOTFOUND) {
            invariantWTOK(ret);
        }

        if (docsRemoved > 0) {
            // if we scanned to the end of the collection or past our insert, go back one
            if (ret == WT_NOTFOUND || newestIdToDelete >= justInserted) {
                ret = WT_READ_CHECK(truncateEnd->prev(truncateEnd));
            }
            invariantWTOK(ret);

            RecordId firstRemainingId;
            ret = truncateEnd->next(truncateEnd);
            if (ret != WT_NOTFOUND) {
                invariantWTOK(ret);
                firstRemainingId = getKey(truncateEnd);
            }
            invariantWTOK(truncateEnd->prev(truncateEnd));  // put the cursor back where it was

            WiredTigerCursor startWrap(_uri, _tableId, true, opCtx);
            WT_CURSOR* truncateStart = startWrap.get();

            // If we know where the start point is, set it for the truncate
            if (savedFirstKey != 0) {
                setKey(truncateStart, RecordId(savedFirstKey));
            } else {
                truncateStart = NULL;
            }
            ret = session->truncate(session, NULL, truncateStart, truncateEnd, NULL);

            if (ret == ENOENT || ret == WT_NOTFOUND) {
                // TODO we should remove this case once SERVER-17141 is resolved
                log() << "Soft failure truncating capped collection. Will try again later.";
                docsRemoved = 0;
            } else {
                invariantWTOK(ret);
                _changeNumRecords(opCtx, -docsRemoved);
                _increaseDataSize(opCtx, -sizeSaved);
                wuow.commit();
                // Save the key for the next round
                _cappedFirstRecord = firstRemainingId;
            }
        }
    } catch (const WriteConflictException&) {
        delete opCtx->releaseRecoveryUnit();
        opCtx->setRecoveryUnit(realRecoveryUnit, realRUstate);
        log() << "got conflict truncating capped, ignoring";
        return 0;
    } catch (...) {
        delete opCtx->releaseRecoveryUnit();
        opCtx->setRecoveryUnit(realRecoveryUnit, realRUstate);
        throw;
    }

    delete opCtx->releaseRecoveryUnit();
    opCtx->setRecoveryUnit(realRecoveryUnit, realRUstate);
    return docsRemoved;
}

bool WiredTigerRecordStore::yieldAndAwaitOplogDeletionRequest(OperationContext* opCtx) {
    // Create another reference to the oplog stones while holding a lock on the collection to
    // prevent it from being destructed.
    std::shared_ptr<OplogStones> oplogStones = _oplogStones;

    Locker* locker = opCtx->lockState();
    Locker::LockSnapshot snapshot;

    // Release any locks before waiting on the condition variable. It is illegal to access any
    // methods or members of this record store after this line because it could be deleted.
    bool releasedAnyLocks = locker->saveLockStateAndUnlock(&snapshot);
    invariant(releasedAnyLocks);

    // The top-level locks were freed, so also release any potential low-level (storage engine)
    // locks that might be held.
    WiredTigerRecoveryUnit* recoveryUnit = (WiredTigerRecoveryUnit*)opCtx->recoveryUnit();
    recoveryUnit->abandonSnapshot();
    recoveryUnit->beginIdle();

    // Wait for an oplog deletion request, or for this record store to have been destroyed.
    oplogStones->awaitHasExcessStonesOrDead();

    // Reacquire the locks that were released.
    locker->restoreLockState(snapshot);

    return !oplogStones->isDead();
}

//WiredTigerRecordStoreThread::_deleteExcessDocuments调用
//local库replset.oplogTruncateAfterPoint表相关内容处理，清理该表中指定时间范围得数据
//oplogTruncateAfterPoint 只用于备库，用来保证 oplog batch 应用的原子性，最终目的是确保主备数据的一致性
void WiredTigerRecordStore::reclaimOplog(OperationContext* opCtx) {
    while (auto stone = _oplogStones->peekOldestStoneIfNeeded()) {
        invariant(stone->lastRecord.isNormal());

        LOG(1) << "Truncating the oplog between " << _oplogStones->firstRecord << " and "
               << stone->lastRecord << " to remove approximately " << stone->records
               << " records totaling to " << stone->bytes << " bytes";

        WiredTigerRecoveryUnit* ru = WiredTigerRecoveryUnit::get(opCtx);
        WT_SESSION* session = ru->getSession()->getSession();

        try {
            WriteUnitOfWork wuow(opCtx);

            WiredTigerCursor startwrap(_uri, _tableId, true, opCtx);
            WT_CURSOR* start = startwrap.get();
            setKey(start, _oplogStones->firstRecord);

            WiredTigerCursor endwrap(_uri, _tableId, true, opCtx);
            WT_CURSOR* end = endwrap.get();
            setKey(end, stone->lastRecord);

            invariantWTOK(session->truncate(session, nullptr, start, end, nullptr));
            _changeNumRecords(opCtx, -stone->records);
            _increaseDataSize(opCtx, -stone->bytes);

            wuow.commit();

            // Remove the stone after a successful truncation.
            _oplogStones->popOldestStone();

            // Stash the truncate point for next time to cleanly skip over tombstones, etc.
            _oplogStones->firstRecord = stone->lastRecord;
        } catch (const WriteConflictException&) {
            LOG(1) << "Caught WriteConflictException while truncating oplog entries, retrying";
        }
    }

    LOG(1) << "Finished truncating the oplog, it now contains approximately " << _numRecords.load()
           << " records totaling to " << _dataSize.load() << " bytes";
}

/*
(gdb) bt
#0  mongo::WiredTigerRecordStore::_insertRecords (this=0x7f863ccbdb00, opCtx=opCtx@entry=0x7f8640572640, records=0x7f8640bbd260, timestamps=0x7f863ccda1c0, nRecords=1) at src/mongo/db/storage/wiredtiger/wiredtiger_record_store.cpp:1121
#1  0x00007f8639821154 in mongo::WiredTigerRecordStore::insertRecords (this=<optimized out>, opCtx=opCtx@entry=0x7f8640572640, records=records@entry=0x7f8638e3eae0, timestamps=timestamps@entry=0x7f8638e3eb00, 
    enforceQuota=<optimized out>) at src/mongo/db/storage/wiredtiger/wiredtiger_record_store.cpp:1100
#2  0x00007f8639b30a97 in mongo::CollectionImpl::_insertDocuments (this=this@entry=0x7f863cac7a40, opCtx=opCtx@entry=0x7f8640572640, begin=..., begin@entry=..., end=end@entry=..., enforceQuota=enforceQuota@entry=true, 
    opDebug=0x7f8640645138) at src/mongo/db/catalog/collection_impl.cpp:525
#3  0x00007f8639b311cc in mongo::CollectionImpl::insertDocuments (this=0x7f863cac7a40, opCtx=0x7f8640572640, begin=..., end=..., opDebug=0x7f8640645138, enforceQuota=true, fromMigrate=false)
    at src/mongo/db/catalog/collection_impl.cpp:377
#4  0x00007f8639ac44d2 in insertDocuments (fromMigrate=false, enforceQuota=true, opDebug=<optimized out>, end=..., begin=..., opCtx=0x7f8640572640, this=<optimized out>) at src/mongo/db/catalog/collection.h:498
#5  mongo::(anonymous namespace)::insertDocuments (opCtx=0x7f8640572640, collection=<optimized out>, begin=begin@entry=..., end=end@entry=...) at src/mongo/db/ops/write_ops_exec.cpp:329
#6  0x00007f8639aca1a6 in operator() (__closure=<optimized out>) at src/mongo/db/ops/write_ops_exec.cpp:406
#7  writeConflictRetry<mongo::(anonymous namespace)::insertBatchAndHandleErrors(mongo::OperationContext*, const mongo::write_ops::Insert&, std::vector<mongo::InsertStatement>&, mongo::(anonymous namespace)::LastOpFixer*, mongo::WriteResult*)::<lambda()> > (f=<optimized out>, ns=..., opStr=..., opCtx=0x7f8640572640) at src/mongo/db/concurrency/write_conflict_exception.h:91
#8  insertBatchAndHandleErrors (out=0x7f8638e3ef20, lastOpFixer=0x7f8638e3ef00, batch=..., wholeOp=..., opCtx=0x7f8640572640) at src/mongo/db/ops/write_ops_exec.cpp:418
#9  mongo::performInserts (opCtx=opCtx@entry=0x7f8640572640, wholeOp=...) at src/mongo/db/ops/write_ops_exec.cpp:527
#10 0x00007f8639ab064e in mongo::(anonymous namespace)::CmdInsert::runImpl (this=<optimized out>, opCtx=0x7f8640572640, request=..., result=...) at src/mongo/db/commands/write_commands/write_commands.cpp:255
#11 0x00007f8639aaa1e8 in mongo::(anonymous namespace)::WriteCommand::enhancedRun (this=<optimized out>, opCtx=0x7f8640572640, request=..., result=...) at src/mongo/db/commands/write_commands/write_commands.cpp:221
#12 0x00007f863aa7272f in mongo::Command::publicRun (this=0x7f863bd223a0 <mongo::(anonymous namespace)::cmdInsert>, opCtx=0x7f8640572640, request=..., result=...) at src/mongo/db/commands.cpp:355
#13 0x00007f86399ee834 in runCommandImpl (startOperationTime=..., replyBuilder=0x7f864056f950, request=..., command=0x7f863bd223a0 <mongo::(anonymous namespace)::cmdInsert>, opCtx=0x7f8640572640)
    at src/mongo/db/service_entry_point_mongod.cpp:506
#14 mongo::(anonymous namespace)::execCommandDatabase (opCtx=0x7f8640572640, command=command@entry=0x7f863bd223a0 <mongo::(anonymous namespace)::cmdInsert>, request=..., replyBuilder=<optimized out>)
    at src/mongo/db/service_entry_point_mongod.cpp:759
#15 0x00007f86399ef39f in mongo::(anonymous namespace)::<lambda()>::operator()(void) const (__closure=__closure@entry=0x7f8638e3f400) at src/mongo/db/service_entry_point_mongod.cpp:880
#16 0x00007f86399ef39f in mongo::ServiceEntryPointMongod::handleRequest (this=<optimized out>, opCtx=<optimized out>, m=...)
#17 0x00007f86399f0201 in runCommands (message=..., opCtx=0x7f8640572640) at src/mongo/db/service_entry_point_mongod.cpp:890
#18 mongo::ServiceEntryPointMongod::handleRequest (this=<optimized out>, opCtx=0x7f8640572640, m=...) at src/mongo/db/service_entry_point_mongod.cpp:1163
#19 0x00007f86399fcb3a in mongo::ServiceStateMachine::_processMessage (this=this@entry=0x7f864050c510, guard=...) at src/mongo/transport/service_state_machine.cpp:414
#20 0x00007f86399f7c7f in mongo::ServiceStateMachine::_runNextInGuard (this=0x7f864050c510, guard=...) at src/mongo/transport/service_state_machine.cpp:474
#21 0x00007f86399fb6be in operator() (__closure=0x7f8640bbe060) at src/mongo/transport/service_state_machine.cpp:515
#22 std::_Function_handler<void(), mongo::ServiceStateMachine::_scheduleNextWithGuard(mongo::ServiceStateMachine::ThreadGuard, mongo::transport::ServiceExecutor::ScheduleFlags, mongo::ServiceStateMachine::Ownership)::<lambda()> >::_M_invoke(const std::_Any_data &) (__functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#23 0x00007f863a937c32 in operator() (this=0x7f8638e41550) at /usr/local/include/c++/5.4.0/functional:2267
#24 mongo::transport::ServiceExecutorSynchronous::schedule(std::function<void ()>, mongo::transport::ServiceExecutor::ScheduleFlags) (this=this@entry=0x7f863ccb2480, task=..., 
    flags=flags@entry=mongo::transport::ServiceExecutor::kMayRecurse) at src/mongo/transport/service_executor_synchronous.cpp:125
#25 0x00007f86399f687d in mongo::ServiceStateMachine::_scheduleNextWithGuard (this=this@entry=0x7f864050c510, guard=..., flags=flags@entry=mongo::transport::ServiceExecutor::kMayRecurse, 
    ownershipModel=ownershipModel@entry=mongo::ServiceStateMachine::kOwned) at src/mongo/transport/service_state_machine.cpp:519
#26 0x00007f86399f9211 in mongo::ServiceStateMachine::_sourceCallback (this=this@entry=0x7f864050c510, status=...) at src/mongo/transport/service_state_machine.cpp:318
#27 0x00007f86399f9e0b in mongo::ServiceStateMachine::_sourceMessage (this=this@entry=0x7f864050c510, guard=...) at src/mongo/transport/service_state_machine.cpp:276
#28 0x00007f86399f7d11 in mongo::ServiceStateMachine::_runNextInGuard (this=0x7f864050c510, guard=...) at src/mongo/transport/service_state_machine.cpp:471
#29 0x00007f86399fb6be in operator() (__closure=0x7f863ccb9a60) at src/mongo/transport/service_state_machine.cpp:515
#30 std::_Function_handler<void(), mongo::ServiceStateMachine::_scheduleNextWithGuard(mongo::ServiceStateMachine::ThreadGuard, mongo::transport::ServiceExecutor::ScheduleFlags, mongo::ServiceStateMachine::Ownership)::<lambda()> >::_M_invoke(const std::_Any_data &) (__functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#31 0x00007f863a938195 in operator() (this=<optimized out>) at /usr/local/include/c++/5.4.0/functional:2267
#32 operator() (__closure=0x7f864052c1b0) at src/mongo/transport/service_executor_synchronous.cpp:143
#33 std::_Function_handler<void(), mongo::transport::ServiceExecutorSynchronous::schedule(mongo::transport::ServiceExecutor::Task, mongo::transport::ServiceExecutor::ScheduleFlags)::<lambda()> >::_M_invoke(const std::_Any_data &) (
    __functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#34 0x00007f863ae87d64 in operator() (this=<optimized out>) at /usr/local/include/c++/5.4.0/functional:2267
#35 mongo::(anonymous namespace)::runFunc (ctx=0x7f863ccb9c40) at src/mongo/transport/service_entry_point_utils.cpp:55
#36 0x00007f8637b5ce25 in start_thread () from /lib64/libpthread.so.0
#37 0x00007f863788a34d in clone () from /lib64/libc.so.6

*/

//CollectionImpl::_insertDocuments(插入数据)   KVCatalog::newCollection(记录集合元数据到_mdb_catalog.wt)中执行   
//WiredTigerRecordStore::insertRecords 普通插入   IndexCatalogImpl::indexRecords 索引插入
Status WiredTigerRecordStore::insertRecords(OperationContext* opCtx,
                                            std::vector<Record>* records,
                                            std::vector<Timestamp>* timestamps,
                                            bool enforceQuota) {
    return _insertRecords(opCtx, records->data(), timestamps->data(), records->size());
}

//唯一索引WiredTigerIndexUnique::_insert   普通索引WiredTigerIndexStandard::_insert
//数据插入WiredTigerRecordStore::_insertRecords

//WiredTigerRecordStore::insertRecords

//为数据生成id，并存入存储引擎走WiredTigerRecordStore::_insertRecords，索引插入走WiredTigerIndex::insert
//oplog写入在WiredTigerRecordStore::insertRecordsWithDocWriter
//写数据文件 索引文件
Status WiredTigerRecordStore::_insertRecords(OperationContext* opCtx,
                                             Record* records,
                                             const Timestamp* timestamps,
                                             size_t nRecords) {
    // We are kind of cheating on capped collections since we write all of them at once ....
    // Simplest way out would be to just block vector writes for everything except oplog ?
    int64_t totalLength = 0;
    for (size_t i = 0; i < nRecords; i++)
        totalLength += records[i].data.size();

    // caller will retry one element at a time
    if (_isCapped && totalLength > _cappedMaxSize)
        return Status(ErrorCodes::BadValue, "object to insert exceeds cappedMaxSize");


	//以下是写入wiredtiger存储引擎
    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    curwrap.assertInActiveTxn(); //头文件中的void assertInActiveTxn() const->WiredTigerRecoveryUnit::assertInActiveTxn()
    WT_CURSOR* c = curwrap.get(); //WiredTigerCursor._cursor成员
    invariant(c);

	//该表中最后一个写入到存储引擎的数据id行号记录到这里面的，下次新数据进来，则继续自增
    RecordId highestId = RecordId();
    dassert(nRecords != 0);
	//为该数据生成一个存储到KV引擎中的id key，是个自增的数字
    for (size_t i = 0; i < nRecords; i++) { //只有固定集合才会一次性多条文档进来，参考insertBatchAndHandleErrors
        auto& record = records[i];
        if (_isOplog) {
            StatusWith<RecordId> status =
                oploghack::extractKey(record.data.data(), record.data.size());
            if (!status.isOK())
                return status.getStatus();
            record.id = status.getValue();
        } else if (_isCapped) { //固定集合
            record.id = _nextId();
        } else {
        	//RecordId 自增，在写索引表的时候会用到，见CollectionImpl::_insertDocuments
            record.id = _nextId();
        }
        dassert(record.id > highestId);
        highestId = record.id;
    }

    for (size_t i = 0; i < nRecords; i++) {
		/*
		struct Record {
		    RecordId id;
		    RecordData data;
		};
		*/
        auto& record = records[i];
        Timestamp ts;
        if (timestamps[i].isNull() && _isOplog) {
            // If the timestamp is 0, that probably means someone inserted a document directly
            // into the oplog.  In this case, use the RecordId as the timestamp, since they are
            // one and the same.
            ts = Timestamp(record.id.repr());
        } else {
            ts = timestamps[i];
        }
		//保障了ts和存储引擎层面的时间戳一致性
		//MongoDB 在提交事务时，会将 oplog 时间戳跟事务关联，从而达到 MongoDB Server 层时序与 WiredTiger 层时序一致的目的。
        if (!ts.isNull()) {
            LOG(4) << "inserting record with timestamp " << ts;
            fassertStatusOK(39001, opCtx->recoveryUnit()->setTimestamp(ts));
        }

		//KV插入wiredtiger
        setKey(c, record.id);
        WiredTigerItem value(record.data.data(), record.data.size());
		//2021-03-24T10:58:46.762+0800 I STORAGE  [conn-1] yang test ...WiredTigerRecordStore::_insertRecords . _uri:table:test/collection/7-380857198902467499 key:RecordId(15) value:{ _id: ObjectId('605aaae6cd83d63c5a6bb264'), name1: 2223.0 }
		log() << "yang test ...WiredTigerRecordStore::_insertRecords . _uri:" << _uri <<" key:" << record.id << " value:" << redact(record.data.toBson());
        c->set_value(c, value.Get());
        int ret = WT_OP_CHECK(c->insert(c));
        if (ret)
            return wtRCToStatus(ret, "WiredTigerRecordStore::insertRecord");
    }

	//记录该表中的数据总条数和总数据大小
    _changeNumRecords(opCtx, nRecords);
    _increaseDataSize(opCtx, totalLength);

    if (_oplogStones) {
        _oplogStones->updateCurrentStoneAfterInsertOnCommit(
            opCtx, totalLength, highestId, nRecords);
    } else {
    	//capped collection集合大小不能超过现在，否则需要清除  cappedAndNeedDelete
        cappedDeleteAsNeeded(opCtx, highestId);
    }

    return Status::OK();
}

StatusWith<RecordId> WiredTigerRecordStore::insertRecord(
    OperationContext* opCtx, const char* data, int len, Timestamp timestamp, bool enforceQuota) {
    Record record = {RecordId(), RecordData(data, len)};
    Status status = _insertRecords(opCtx, &record, &timestamp, 1);
    if (!status.isOK())
        return StatusWith<RecordId>(status);
    return StatusWith<RecordId>(record.id);
}

bool WiredTigerRecordStore::isOpHidden_forTest(const RecordId& id) const {
    invariant(id.repr() > 0);
    invariant(_kvEngine->getOplogManager()->isRunning());
    return _kvEngine->getOplogManager()->getOplogReadTimestamp() <
        static_cast<std::uint64_t>(id.repr());
}

void WiredTigerRecordStore::notifyCappedWaitersIfNeeded() {
    stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
    // This wakes up cursors blocking in await_data.
    if (_cappedCallback) {
        _cappedCallback->notifyCappedWaitersIfNeeded();
    }
}

//写操作写oplog流程OpObserverImpl::onInserts->logInsertOps->_logOpsInner
//->CollectionImpl::insertDocumentsForOplog->WiredTigerRecordStore::insertRecordsWithDocWriter

//CollectionImpl::insertDocumentsForOplog调用
Status WiredTigerRecordStore::insertRecordsWithDocWriter(OperationContext* opCtx,
                                                         const DocWriter* const* docs,
                                                         const Timestamp* timestamps,
                                                         size_t nDocs,
                                                         RecordId* idsOut) {
    std::unique_ptr<Record[]> records(new Record[nDocs]);

    // First get all the sizes so we can allocate a single buffer for all documents. Eventually it
    // would be nice if we could either hand off the buffers to WT without copying or write them
    // in-place as we do with MMAPv1, but for now this is the best we can do.
    size_t totalSize = 0;
    for (size_t i = 0; i < nDocs; i++) {
        const size_t docSize = docs[i]->documentSize();
        records[i].data = RecordData(nullptr, docSize);  // We fill in the real ptr in next loop.
        totalSize += docSize;
    }

    std::unique_ptr<char[]> buffer(new char[totalSize]);
    char* pos = buffer.get();
    for (size_t i = 0; i < nDocs; i++) {
        docs[i]->writeDocument(pos);
        const size_t size = records[i].data.size();
        records[i].data = RecordData(pos, size);
        pos += size;
    }
    invariant(pos == (buffer.get() + totalSize));

    Status s = _insertRecords(opCtx, records.get(), timestamps, nDocs);
    if (!s.isOK())
        return s;

    if (idsOut) {
        for (size_t i = 0; i < nDocs; i++) {
            idsOut[i] = records[i].id;
        }
    }

    return s;
}

Status WiredTigerRecordStore::updateRecord(OperationContext* opCtx,
                                           const RecordId& id,
                                           const char* data,
                                           int len,
                                           bool enforceQuota,
                                           UpdateNotifier* notifier) {
    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    setKey(c, id);
    int ret = WT_READ_CHECK(c->search(c));
    invariantWTOK(ret);

    WT_ITEM old_value;
    ret = c->get_value(c, &old_value);
    invariantWTOK(ret);

    int64_t old_length = old_value.size;

    if (_oplogStones && len != old_length) {
        return {ErrorCodes::IllegalOperation, "Cannot change the size of a document in the oplog"};
    }

    WiredTigerItem value(data, len);
    c->set_value(c, value.Get());
    ret = WT_OP_CHECK(c->insert(c));
    invariantWTOK(ret);

    _increaseDataSize(opCtx, len - old_length);
    if (!_oplogStones) {
        cappedDeleteAsNeeded(opCtx, id);
    }

    return Status::OK();
}

bool WiredTigerRecordStore::updateWithDamagesSupported() const {
    return true;
}

StatusWith<RecordData> WiredTigerRecordStore::updateWithDamages(
    OperationContext* opCtx,
    const RecordId& id,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {

    const int nentries = damages.size();
    mutablebson::DamageVector::const_iterator where = damages.begin();
    const mutablebson::DamageVector::const_iterator end = damages.cend();
    std::vector<WT_MODIFY> entries(nentries);
    for (u_int i = 0; where != end; ++i, ++where) {
        entries[i].data.data = damageSource + where->sourceOffset;
        entries[i].data.size = where->size;
        entries[i].offset = where->targetOffset;
        entries[i].size = where->size;
    }

    WiredTigerCursor curwrap(_uri, _tableId, true, opCtx);
    curwrap.assertInActiveTxn();
    WT_CURSOR* c = curwrap.get();
    invariant(c);
    setKey(c, id);

    // The test harness calls us with empty damage vectors which WiredTiger doesn't allow.
    if (nentries == 0)
        invariantWTOK(WT_OP_CHECK(c->search(c)));
    else
        invariantWTOK(WT_OP_CHECK(c->modify(c, entries.data(), nentries)));

    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));

    return RecordData(static_cast<const char*>(value.data), value.size).getOwned();
}

std::unique_ptr<RecordCursor> WiredTigerRecordStore::getRandomCursor(
    OperationContext* opCtx) const {
    const char* extraConfig = "";
    return getRandomCursorWithOptions(opCtx, extraConfig);
}

std::vector<std::unique_ptr<RecordCursor>> WiredTigerRecordStore::getManyCursors(
    OperationContext* opCtx) const {
    std::vector<std::unique_ptr<RecordCursor>> cursors(1);
    cursors[0] = getCursor(opCtx, /*forward=*/true);
    return cursors;
}

Status WiredTigerRecordStore::truncate(OperationContext* opCtx) {
    WiredTigerCursor startWrap(_uri, _tableId, true, opCtx);
    WT_CURSOR* start = startWrap.get();
    int ret = WT_READ_CHECK(start->next(start));
    // Empty collections don't have anything to truncate.
    if (ret == WT_NOTFOUND) {
        return Status::OK();
    }
    invariantWTOK(ret);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    invariantWTOK(WT_OP_CHECK(session->truncate(session, NULL, start, NULL, NULL)));
    _changeNumRecords(opCtx, -numRecords(opCtx));
    _increaseDataSize(opCtx, -dataSize(opCtx));

    if (_oplogStones) {
        _oplogStones->clearStonesOnCommit(opCtx);
    }

    return Status::OK();
}

Status WiredTigerRecordStore::compact(OperationContext* opCtx,
                                      RecordStoreCompactAdaptor* adaptor,
                                      const CompactOptions* options,
                                      CompactStats* stats) {
    WiredTigerSessionCache* cache = WiredTigerRecoveryUnit::get(opCtx)->getSessionCache();
    if (!cache->isEphemeral()) {
        WT_SESSION* s = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
        opCtx->recoveryUnit()->abandonSnapshot();
        int ret = s->compact(s, getURI().c_str(), "timeout=0");
        invariantWTOK(ret);
    }
    return Status::OK();
}

Status WiredTigerRecordStore::validate(OperationContext* opCtx,
                                       ValidateCmdLevel level,
                                       ValidateAdaptor* adaptor,
                                       ValidateResults* results,
                                       BSONObjBuilder* output) {
    if (!_isEphemeral && level == kValidateFull) {
        int err = WiredTigerUtil::verifyTable(opCtx, _uri, &results->errors);
        if (err == EBUSY) {
            std::string msg = str::stream()
                << "Could not complete validation of " << _uri << ". "
                << "This is a transient issue as the collection was actively "
                   "in use by other operations.";

            warning() << msg;
            results->warnings.push_back(msg);
        } else if (err) {
            std::string msg = str::stream() << "verify() returned " << wiredtiger_strerror(err)
                                            << ". "
                                            << "This indicates structural damage. "
                                            << "Not examining individual documents.";
            error() << msg;
            results->errors.push_back(msg);
            results->valid = false;
            return Status::OK();
        }
    }

    long long nrecords = 0;
    long long dataSizeTotal = 0;
    long long nInvalid = 0;

    results->valid = true;
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, true);
    int interruptInterval = 4096;

    while (auto record = cursor->next()) {
        if (!(nrecords % interruptInterval))
            opCtx->checkForInterrupt();
        ++nrecords;
        auto dataSize = record->data.size();
        dataSizeTotal += dataSize;
        size_t validatedSize;
        Status status = adaptor->validate(record->id, record->data, &validatedSize);

        // The validatedSize equals dataSize below is not a general requirement, but must be
        // true for WT today because we never pad records.
        if (!status.isOK() || validatedSize != static_cast<size_t>(dataSize)) {
            if (results->valid) {
                // Only log once.
                results->errors.push_back("detected one or more invalid documents (see logs)");
            }
            nInvalid++;
            results->valid = false;
            log() << "document at location: " << record->id << " is corrupted";
        }
    }

    if (results->valid) {
        updateStatsAfterRepair(opCtx, nrecords, dataSizeTotal);
    }

    output->append("nInvalidDocuments", nInvalid);
    output->appendNumber("nrecords", nrecords);
    return Status::OK();
}

void WiredTigerRecordStore::appendCustomStats(OperationContext* opCtx,
                                              BSONObjBuilder* result,
                                              double scale) const {
    result->appendBool("capped", _isCapped); //capped collection集合大小不能超过现在，否则需要清除  cappedAndNeedDelete
    if (_isCapped) {
        result->appendIntOrLL("max", _cappedMaxDocs);
        result->appendIntOrLL("maxSize", static_cast<long long>(_cappedMaxSize / scale));
        result->appendIntOrLL("sleepCount", _cappedSleep.load());
        result->appendIntOrLL("sleepMS", _cappedSleepMS.load());
    }
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession();
    WT_SESSION* s = session->getSession();
    BSONObjBuilder bob(result->subobjStart(_engineName));
    {
        BSONObjBuilder metadata(bob.subobjStart("metadata"));
        Status status = WiredTigerUtil::getApplicationMetadata(opCtx, getURI(), &metadata);
        if (!status.isOK()) {
            metadata.append("error", "unable to retrieve metadata");
            metadata.append("code", static_cast<int>(status.code()));
            metadata.append("reason", status.reason());
        }
    }

    std::string type, sourceURI;
    WiredTigerUtil::fetchTypeAndSourceURI(opCtx, _uri, &type, &sourceURI);
    StatusWith<std::string> metadataResult = WiredTigerUtil::getMetadata(opCtx, sourceURI);
    StringData creationStringName("creationString");
    if (!metadataResult.isOK()) {
        BSONObjBuilder creationString(bob.subobjStart(creationStringName));
        creationString.append("error", "unable to retrieve creation config");
        creationString.append("code", static_cast<int>(metadataResult.getStatus().code()));
        creationString.append("reason", metadataResult.getStatus().reason());
    } else {
        bob.append("creationString", metadataResult.getValue());
        // Type can be "lsm" or "file"
        bob.append("type", type);
    }

    Status status =
        WiredTigerUtil::exportTableToBSON(s, "statistics:" + getURI(), "statistics=(fast)", &bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }
}

Status WiredTigerRecordStore::touch(OperationContext* opCtx, BSONObjBuilder* output) const {
    if (_isEphemeral) {
        // Everything is already in memory.
        return Status::OK();
    }
    return Status(ErrorCodes::CommandNotSupported, "this storage engine does not support touch");
}

void WiredTigerRecordStore::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const {
    // Make sure that callers do not hold an active snapshot so it will be able to see the oplog
    // entries it waited for afterwards.
    invariant(!_getRecoveryUnit(opCtx)->inActiveTxn());

    auto oplogManager = _kvEngine->getOplogManager();
    if (oplogManager->isRunning()) {
        oplogManager->waitForAllEarlierOplogWritesToBeVisible(this, opCtx);
    }
}

//oplogHack 优化 参考https://mongoing.com/archives/27285
//从指定位置拉取oplog   getOplogStartHack  getExecutorFind中调用
boost::optional<RecordId> WiredTigerRecordStore::oplogStartHack(
    OperationContext* opCtx, const RecordId& startingPosition) const {
    if (!_isOplog)
        return boost::none;

    if (_isOplog) {
        WiredTigerRecoveryUnit::get(opCtx)->setIsOplogReader();
    }

    WiredTigerCursor cursor(_uri, _tableId, true, opCtx);
    WT_CURSOR* c = cursor.get();

    int cmp;
    setKey(c, startingPosition);
    int ret = WT_READ_CHECK(c->search_near(c, &cmp));
    if (ret == 0 && cmp > 0)
        ret = c->prev(c);  // landed one higher than startingPosition
    if (ret == WT_NOTFOUND)
        return RecordId();  // nothing <= startingPosition
    invariantWTOK(ret);

    return getKey(c);
}

void WiredTigerRecordStore::updateStatsAfterRepair(OperationContext* opCtx,
                                                   long long numRecords,
                                                   long long dataSize) {
    _numRecords.store(numRecords);
    _dataSize.store(dataSize);

    if (_sizeStorer) {
        _sizeStorer->storeToCache(_uri, numRecords, dataSize);
    }
}

RecordId WiredTigerRecordStore::_nextId() {
    invariant(!_isOplog);
    RecordId out = RecordId(_nextIdNum.fetchAndAdd(1));
    invariant(out.isNormal());
    return out;
}

WiredTigerRecoveryUnit* WiredTigerRecordStore::_getRecoveryUnit(OperationContext* opCtx) {
    return checked_cast<WiredTigerRecoveryUnit*>(opCtx->recoveryUnit());
}

class WiredTigerRecordStore::NumRecordsChange : public RecoveryUnit::Change {
public:
    NumRecordsChange(WiredTigerRecordStore* rs, int64_t diff) : _rs(rs), _diff(diff) {}
    virtual void commit() {}
    virtual void rollback() {
        _rs->_numRecords.fetchAndAdd(-_diff);
    }

private:
    WiredTigerRecordStore* _rs;
    int64_t _diff;
};

//WiredTigerRecordStore::deleteRecord  WiredTigerRecordStore::_insertRecords
void WiredTigerRecordStore::_changeNumRecords(OperationContext* opCtx, int64_t diff) { //diff可能为负数
    opCtx->recoveryUnit()->registerChange(new NumRecordsChange(this, diff));
    if (_numRecords.fetchAndAdd(diff) < 0) //_numRecords小于0，则赋值为0
        _numRecords.store(std::max(diff, int64_t(0)));
}

class WiredTigerRecordStore::DataSizeChange : public RecoveryUnit::Change {
public:
    DataSizeChange(WiredTigerRecordStore* rs, int64_t amount) : _rs(rs), _amount(amount) {}
    virtual void commit() {}
    virtual void rollback() {
        _rs->_increaseDataSize(NULL, -_amount);
    }

private:
    WiredTigerRecordStore* _rs;
    int64_t _amount;
};

//WiredTigerRecordStore::deleteRecord  WiredTigerRecordStore::_insertRecords WiredTigerRecordStore::updateRecord
void WiredTigerRecordStore::_increaseDataSize(OperationContext* opCtx, int64_t amount) {
    if (opCtx)
        opCtx->recoveryUnit()->registerChange(new DataSizeChange(this, amount));

    if (_dataSize.fetchAndAdd(amount) < 0)
        _dataSize.store(std::max(amount, int64_t(0)));

    if (_sizeStorer && _sizeStorerCounter++ % 1000 == 0) {
        _sizeStorer->storeToCache(_uri, _numRecords.load(), _dataSize.load());
    }
}

void WiredTigerRecordStore::cappedTruncateAfter(OperationContext* opCtx,
                                                RecordId end,
                                                bool inclusive) {
    std::unique_ptr<SeekableRecordCursor> cursor = getCursor(opCtx, true);

    auto record = cursor->seekExact(end);
    massert(28807, str::stream() << "Failed to seek to the record located at " << end, record);

    int64_t recordsRemoved = 0;
    int64_t bytesRemoved = 0;
    RecordId lastKeptId;
    RecordId firstRemovedId;

    if (inclusive) {
        std::unique_ptr<SeekableRecordCursor> reverseCursor = getCursor(opCtx, false);
        invariant(reverseCursor->seekExact(end));
        auto prev = reverseCursor->next();
        lastKeptId = prev ? prev->id : RecordId();
        firstRemovedId = end;
    } else {
        // If not deleting the record located at 'end', then advance the cursor to the first record
        // that is being deleted.
        record = cursor->next();
        if (!record) {
            return;  // No records to delete.
        }
        lastKeptId = end;
        firstRemovedId = record->id;
    }

    // Compute the number and associated sizes of the records to delete.
    {
        stdx::lock_guard<stdx::mutex> cappedCallbackLock(_cappedCallbackMutex);
        do {
            if (_cappedCallback) {
                uassertStatusOK(
                    _cappedCallback->aboutToDeleteCapped(opCtx, record->id, record->data));
            }
            recordsRemoved++;
            bytesRemoved += record->data.size();
        } while ((record = cursor->next()));
    }

    // Truncate the collection starting from the record located at 'firstRemovedId' to the end of
    // the collection.
    WriteUnitOfWork wuow(opCtx);

    WiredTigerCursor startwrap(_uri, _tableId, true, opCtx);
    WT_CURSOR* start = startwrap.get();
    setKey(start, firstRemovedId);

    WT_SESSION* session = WiredTigerRecoveryUnit::get(opCtx)->getSession()->getSession();
    invariantWTOK(session->truncate(session, nullptr, start, nullptr, nullptr));

    _changeNumRecords(opCtx, -recordsRemoved);
    _increaseDataSize(opCtx, -bytesRemoved);

    wuow.commit();

    if (_isOplog) {
        // Immediately rewind visibility to our truncation point, to prevent new
        // transactions from appearing.
        Timestamp truncTs(lastKeptId.repr());
        _kvEngine->setOldestTimestamp(truncTs);
    }

    if (_oplogStones) {
        _oplogStones->updateStonesAfterCappedTruncateAfter(
            recordsRemoved, bytesRemoved, firstRemovedId);
    }
}

Status WiredTigerRecordStore::oplogDiskLocRegister(OperationContext* opCtx,
                                                   const Timestamp& opTime) {
    // This labels the current transaction with a timestamp.
    // This is required for oplog visibility to work correctly, as WiredTiger uses the transaction
    // list to determine where there are holes in the oplog.
    return opCtx->recoveryUnit()->setTimestamp(opTime);
}

// Cursor Base:

WiredTigerRecordStoreCursorBase::WiredTigerRecordStoreCursorBase(OperationContext* opCtx,
                                                                 const WiredTigerRecordStore& rs,
                                                                 bool forward)
    : _rs(rs), _opCtx(opCtx), _forward(forward) {
    _cursor.emplace(rs.getURI(), rs.tableId(), true, opCtx);
}

boost::optional<Record> WiredTigerRecordStoreCursorBase::next() {
    if (_eof)
        return {};

    WT_CURSOR* c = _cursor->get();

    RecordId id;
    if (!_skipNextAdvance) {
        // Nothing after the next line can throw WCEs.
        // Note that an unpositioned (or eof) WT_CURSOR returns the first/last entry in the
        // table when you call next/prev.
        int advanceRet = WT_READ_CHECK(_forward ? c->next(c) : c->prev(c));
        if (advanceRet == WT_NOTFOUND) {
            _eof = true;
            return {};
        }
        invariantWTOK(advanceRet);
        if (hasWrongPrefix(c, &id)) {
            _eof = true;
            return {};
        }
    }

    _skipNextAdvance = false;
    if (!id.isNormal()) {
        id = getKey(c);
    }

    if (_forward && _lastReturnedId >= id) {
        log() << "WTCursor::next -- c->next_key ( " << id
              << ") was not greater than _lastReturnedId (" << _lastReturnedId
              << ") which is a bug.";
        // Force a retry of the operation from our last known position by acting as-if
        // we received a WT_ROLLBACK error.
        throw WriteConflictException();
    }

    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));

    _lastReturnedId = id;
    return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
}

//获取ID对应的一行记录   WorkingSetCommon::fetch调用
boost::optional<Record> WiredTigerRecordStoreCursorBase::seekExact(const RecordId& id) {
    _skipNextAdvance = false;
    WT_CURSOR* c = _cursor->get();
    setKey(c, id);
    // Nothing after the next line can throw WCEs.
    int seekRet = WT_READ_CHECK(c->search(c)); //curfile_search  没有该id对应的value记录，直接置位_eof
    if (seekRet == WT_NOTFOUND) {
        // hasWrongPrefix check not needed for a precise 'WT_CURSOR::search'.
        _eof = true;
        return {};
    }
    invariantWTOK(seekRet);

    WT_ITEM value;
    invariantWTOK(c->get_value(c, &value));//__wt_cursor_get_value

    _lastReturnedId = id;
    _eof = false;
    return {{id, {static_cast<const char*>(value.data), static_cast<int>(value.size)}}};
}


void WiredTigerRecordStoreCursorBase::save() {
    try {
        if (_cursor)
            _cursor->reset();
    } catch (const WriteConflictException&) {
        // Ignore since this is only called when we are about to kill our transaction
        // anyway.
    }
}

void WiredTigerRecordStoreCursorBase::saveUnpositioned() {
    save();
    _lastReturnedId = RecordId();
}

bool WiredTigerRecordStoreCursorBase::restore() {
    if (_rs._isOplog && _forward) {
        WiredTigerRecoveryUnit::get(_opCtx)->setIsOplogReader();
    }

    if (!_cursor)
        _cursor.emplace(_rs.getURI(), _rs.tableId(), true, _opCtx);

    // This will ensure an active session exists, so any restored cursors will bind to it
    invariant(WiredTigerRecoveryUnit::get(_opCtx)->getSession() == _cursor->getSession());
    _skipNextAdvance = false;

    // If we've hit EOF, then this iterator is done and need not be restored.
    if (_eof)
        return true;

    if (_lastReturnedId.isNull()) {
        initCursorToBeginning();
        return true;
    }

    WT_CURSOR* c = _cursor->get();
    setKey(c, _lastReturnedId);

    int cmp;
    int ret = WT_READ_CHECK(c->search_near(c, &cmp));
    RecordId id;
    if (ret == WT_NOTFOUND) {
        _eof = true;
        return !_rs._isCapped;
    }
    invariantWTOK(ret);
    if (hasWrongPrefix(c, &id)) {
        _eof = true;
        return !_rs._isCapped;
    }

    if (cmp == 0)
        return true;  // Landed right where we left off.

    if (_rs._isCapped) {
        // Doc was deleted either by cappedDeleteAsNeeded() or cappedTruncateAfter().
        // It is important that we error out in this case so that consumers don't
        // silently get 'holes' when scanning capped collections. We don't make
        // this guarantee for normal collections so it is ok to skip ahead in that case.
        _eof = true;
        return false;
    }

    if (_forward && cmp > 0) {
        // We landed after where we were. Return our new location on the next call to next().
        _skipNextAdvance = true;
    } else if (!_forward && cmp < 0) {
        _skipNextAdvance = true;
    }

    return true;
}

void WiredTigerRecordStoreCursorBase::detachFromOperationContext() {
    _opCtx = nullptr;
    _cursor = boost::none;
}

void WiredTigerRecordStoreCursorBase::reattachToOperationContext(OperationContext* opCtx) {
    _opCtx = opCtx;
    // _cursor recreated in restore() to avoid risk of WT_ROLLBACK issues.
}

// Standard Implementations:

//建表KVDatabaseCatalogEntryBase::createCollection->WiredTigerKVEngine::getGroupedRecordStore中调用
StandardWiredTigerRecordStore::StandardWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                                             OperationContext* opCtx,
                                                             Params params)
    : WiredTigerRecordStore(kvEngine, opCtx, params) {}

RecordId StandardWiredTigerRecordStore::getKey(WT_CURSOR* cursor) const {
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &recordId));
    return RecordId(recordId);
}

void StandardWiredTigerRecordStore::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, id.repr());
}

std::unique_ptr<SeekableRecordCursor> StandardWiredTigerRecordStore::getCursor(
    OperationContext* opCtx, bool forward) const {
    if (_isOplog && forward) {
        WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(opCtx);
        // If we already have a snapshot we don't know what it can see, unless we know no one
        // else could be writing (because we hold an exclusive lock).
        if (wru->inActiveTxn() && !opCtx->lockState()->isNoop() &&
            !opCtx->lockState()->isCollectionLockedForMode(_ns, MODE_X)) {
            throw WriteConflictException();
        }
        wru->setIsOplogReader();
    }

    return stdx::make_unique<WiredTigerRecordStoreStandardCursor>(opCtx, *this, forward);
}

std::unique_ptr<RecordCursor> StandardWiredTigerRecordStore::getRandomCursorWithOptions(
    OperationContext* opCtx, StringData extraConfig) const {
    return stdx::make_unique<RandomCursor>(opCtx, *this, extraConfig);
}

WiredTigerRecordStoreStandardCursor::WiredTigerRecordStoreStandardCursor(
    OperationContext* opCtx, const WiredTigerRecordStore& rs, bool forward)
    : WiredTigerRecordStoreCursorBase(opCtx, rs, forward) {}

void WiredTigerRecordStoreStandardCursor::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, id.repr());
}

RecordId WiredTigerRecordStoreStandardCursor::getKey(WT_CURSOR* cursor) const {
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &recordId));

    return RecordId(recordId);
}

bool WiredTigerRecordStoreStandardCursor::hasWrongPrefix(WT_CURSOR* cursor,
                                                         RecordId* recordId) const {
    invariantWTOK(cursor->get_key(cursor, recordId));
    return false;
}


// Prefixed Implementations:

PrefixedWiredTigerRecordStore::PrefixedWiredTigerRecordStore(WiredTigerKVEngine* kvEngine,
                                                             OperationContext* opCtx,
                                                             Params params,
                                                             KVPrefix prefix)
    : WiredTigerRecordStore(kvEngine, opCtx, params), _prefix(prefix) {}

std::unique_ptr<SeekableRecordCursor> PrefixedWiredTigerRecordStore::getCursor(
    OperationContext* opCtx, bool forward) const {
    if (_isOplog && forward) {
        WiredTigerRecoveryUnit* wru = WiredTigerRecoveryUnit::get(opCtx);
        // If we already have a snapshot we don't know what it can see, unless we know no one
        // else could be writing (because we hold an exclusive lock).
        if (wru->inActiveTxn() && !opCtx->lockState()->isNoop() &&
            !opCtx->lockState()->isCollectionLockedForMode(_ns, MODE_X)) {
            throw WriteConflictException();
        }
        wru->setIsOplogReader();
    }

    return stdx::make_unique<WiredTigerRecordStorePrefixedCursor>(opCtx, *this, _prefix, forward);
}

std::unique_ptr<RecordCursor> PrefixedWiredTigerRecordStore::getRandomCursorWithOptions(
    OperationContext* opCtx, StringData extraConfig) const {
    return {};
}

RecordId PrefixedWiredTigerRecordStore::getKey(WT_CURSOR* cursor) const {
    std::int64_t prefix;
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &prefix, &recordId));
    invariant(prefix == _prefix.repr());
    return RecordId(recordId);
}

void PrefixedWiredTigerRecordStore::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, _prefix.repr(), id.repr());
}

WiredTigerRecordStorePrefixedCursor::WiredTigerRecordStorePrefixedCursor(
    OperationContext* opCtx, const WiredTigerRecordStore& rs, KVPrefix prefix, bool forward)
    : WiredTigerRecordStoreCursorBase(opCtx, rs, forward), _prefix(prefix) {
    initCursorToBeginning();
}

void WiredTigerRecordStorePrefixedCursor::setKey(WT_CURSOR* cursor, RecordId id) const {
    cursor->set_key(cursor, _prefix.repr(), id.repr());
}

RecordId WiredTigerRecordStorePrefixedCursor::getKey(WT_CURSOR* cursor) const {
    std::int64_t prefix;
    std::int64_t recordId;
    invariantWTOK(cursor->get_key(cursor, &prefix, &recordId));
    invariant(prefix == _prefix.repr());

    return RecordId(recordId);
}

bool WiredTigerRecordStorePrefixedCursor::hasWrongPrefix(WT_CURSOR* cursor,
                                                         RecordId* recordId) const {
    std::int64_t prefix;
    invariantWTOK(cursor->get_key(cursor, &prefix, recordId));

    return prefix != _prefix.repr();
}

void WiredTigerRecordStorePrefixedCursor::initCursorToBeginning() {
    WT_CURSOR* cursor = _cursor->get();
    if (_forward) {
        cursor->set_key(cursor, _prefix.repr(), RecordId::min());
    } else {
        cursor->set_key(cursor, _prefix.repr(), RecordId::max());
    }

    int exact;
    int err = cursor->search_near(cursor, &exact);
    if (err == WT_NOTFOUND) {
        _eof = true;
        return;
    }
    invariantWTOK(err);

    RecordId recordId;
    if (_forward) {
        invariant(exact != 0);  // `RecordId::min` cannot exist.
        if (exact > 0) {
            // Cursor is positioned after <Prefix, RecordId::min>. It may be the first record of
            // this collection or a following collection with a larger prefix.
            //
            // In the case the cursor is positioned a matching prefix, `_skipNextAdvance` must
            // be set to true. However, `WiredTigerRecordStore::Cursor::next` does not check
            // for EOF if `_skipNextAdvance` is true. Eagerly check and set `_eof` if
            // necessary.
            if (hasWrongPrefix(cursor, &recordId)) {
                _eof = true;
                return;
            }

            _skipNextAdvance = true;
        } else {
            _eof = true;
        }
    } else {                    // Backwards.
        invariant(exact != 0);  // `RecordId::min` cannot exist.
        if (exact > 0) {
            // Cursor is positioned after <Prefix, RecordId::max>. This implies it is
            // positioned at the first record for a collection with a larger
            // prefix. `_skipNextAdvance` should remain false and a following call to
            // `WiredTigerRecordStore::Cursor::next` will advance the cursor and appropriately
            // check for EOF.
            _skipNextAdvance = false;  // Simply for clarity and symmetry to the `forward` case.
        } else {
            // Cursor is positioned before <Prefix, RecordId::max>. This is a symmetric case
            // to `forward: true, exact > 0`. It may be positioned at the last document of
            // this collection or the last document of a collection with a smaller prefix.
            if (hasWrongPrefix(cursor, &recordId)) {
                _eof = true;
                return;
            }

            _skipNextAdvance = true;
        }
    }
}

Status WiredTigerRecordStore::updateCappedSize(OperationContext* opCtx, long long cappedSize) {
    if (_cappedMaxSize == cappedSize) {
        return Status::OK();
    }
    _cappedMaxSize = cappedSize;
    if (_oplogStones) {
        _oplogStones->adjust(cappedSize);
    }
    return Status::OK();
}

}  // namespace mongo

