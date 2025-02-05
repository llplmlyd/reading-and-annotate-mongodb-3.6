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

#pragma once

#include <map>
#include <set>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class CanonicalQuery;
struct QuerySolutionNode;
class OperationContext;

// Ordered map from the max for each chunk to an entry describing the chunk
//map表的 key为chunk.getMax()，value为chunk，参考ChunkManager::makeUpdated
using ChunkMap = BSONObjIndexedMap<std::shared_ptr<Chunk>>;

// Map from a shard is to the max chunk version on that shard
using ShardVersionMap = std::map<ShardId, ChunkVersion>;

/**
 * In-memory representation of the routing table for a single sharded collection.
 */ 
//ChunkManager chunk管理     balance负载均衡管理    
//CachedCollectionRoutingInfo._cm成员为该类型，通过CatalogCache::getCollectionRoutingInfo获取CachedCollectionRoutingInfo，然后得到_cm
//分片chunk块相关 mongoDB 的chunk分裂只会发生在 mongos 写入数据时， 当写入的数据超过一定量时， 就会触发 chunk 的分裂

//CollectionMetadata._cm为该类型

//注意ChunkManager和CollectionMetadata的关系，参考ShardingState::_refreshMetadata

//CollectionMetadata._cm为该类型，该表chunk信息存在集合元数据CollectionMetadata._cm中


//一个表对应一个ChunkManager，也就是该表的routingInfo(参考CatalogCache::_scheduleCollectionRefresh)
//每个表的路由信息存储再CatalogCache::CollectionRoutingInfoEntry.routingInfo成员中
class ChunkManager : public std::enable_shared_from_this<ChunkManager> {
    MONGO_DISALLOW_COPYING(ChunkManager);

public:
    //迭代器
    class ConstChunkIterator {
    public:
        ConstChunkIterator() = default;
        explicit ConstChunkIterator(ChunkMap::const_iterator iter) : _iter{iter} {}

        //迭代器自增
        ConstChunkIterator& operator++() {
            ++_iter;
            return *this;
        }
        ConstChunkIterator operator++(int) {
            return ConstChunkIterator{_iter++};
        }
        bool operator==(const ConstChunkIterator& other) const {
            return _iter == other._iter;
        }
        bool operator!=(const ConstChunkIterator& other) const {
            return !(*this == other);
        }
        const ChunkMap::mapped_type& operator*() const {
            return _iter->second;
        }

    private:
        ChunkMap::const_iterator _iter;
    };

    class ConstRangeOfChunks {
    public:
        ConstRangeOfChunks(ConstChunkIterator begin, ConstChunkIterator end)
            : _begin{std::move(begin)}, _end{std::move(end)} {}

        ConstChunkIterator begin() const {
            return _begin;
        }
        ConstChunkIterator end() const {
            return _end;
        }

    private:
        ConstChunkIterator _begin;
        ConstChunkIterator _end;
    };

    /**
     * Makes an instance with a routing table for collection "nss", sharded on
     * "shardKeyPattern".
     *
     * "defaultCollator" is the default collation for the collection, "unique" indicates whether
     * or not the shard key for each document will be globally unique, and "epoch" is the globally
     * unique identifier for this version of the collection.
     *
     * The "chunks" vector must contain the chunk routing information sorted in ascending order by
     * chunk version, and adhere to the requirements of the routing table update algorithm.
     */
    static std::shared_ptr<ChunkManager> makeNew(NamespaceString nss,
                                                 boost::optional<UUID>,
                                                 KeyPattern shardKeyPattern,
                                                 std::unique_ptr<CollatorInterface> defaultCollator,
                                                 bool unique,
                                                 OID epoch,
                                                 const std::vector<ChunkType>& chunks);

    /**
     * Constructs a new instance with a routing table updated according to the changes described
     * in "changedChunks".
     *
     * The changes in "changedChunks" must be sorted in ascending order by chunk version, and adhere
     * to the requirements of the routing table update algorithm.
     */
    std::shared_ptr<ChunkManager> makeUpdated(const std::vector<ChunkType>& changedChunks);

    /**
     * Returns an increasing number of the reload sequence number of this chunk manager.
     */
    unsigned long long getSequenceNumber() const {
        return _sequenceNumber;
    }

    const std::string& getns() const {
        return _nss.ns();
    }

    const ShardKeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const CollatorInterface* getDefaultCollator() const {
        return _defaultCollator.get();
    }

    bool isUnique() const {
        return _unique;
    }

    ChunkVersion getVersion() const {
        return _collectionVersion;
    }

    ChunkVersion getVersion(const ShardId& shardId) const;

    ConstRangeOfChunks chunks() const {
        return {ConstChunkIterator{_chunkMap.cbegin()}, ConstChunkIterator{_chunkMap.cend()}};
    }

    int numChunks() const {
        return _chunkMap.size();
    }

    /**
     * Given a shard key (or a prefix) that has been extracted from a document, returns the chunk
     * that contains that key.
     *
     * Example: findIntersectingChunk({a : hash('foo')}) locates the chunk for document
     *          {a: 'foo', b: 'bar'} if the shard key is {a : 'hashed'}.
     *
     * If 'collation' is empty, we use the collection default collation for targeting.
     *
     * Throws a DBException with the ShardKeyNotFound code if unable to target a single shard due to
     * collation or due to the key not matching the shard key pattern.
     */
    std::shared_ptr<Chunk> findIntersectingChunk(const BSONObj& shardKey,
                                                 const BSONObj& collation) const;

    /**
     * Same as findIntersectingChunk, but assumes the simple collation.
     */
    std::shared_ptr<Chunk> findIntersectingChunkWithSimpleCollation(const BSONObj& shardKey) const;

    /**
     * Finds the shard IDs for a given filter and collation. If collation is empty, we use the
     * collection default collation for targeting.
     */
    void getShardIdsForQuery(OperationContext* opCtx,
                             const BSONObj& query,
                             const BSONObj& collation,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns all shard ids which contain chunks overlapping the range [min, max]. Please note the
     * inclusive bounds on both sides (SERVER-20768).
     */
    void getShardIdsForRange(const BSONObj& min,
                             const BSONObj& max,
                             std::set<ShardId>* shardIds) const;

    /**
     * Returns the ids of all shards on which the collection has any chunks.
     */
    void getAllShardIds(std::set<ShardId>* all) const;

    // Transforms query into bounds for each field in the shard key
    // for example :
    //   Key { a: 1, b: 1 },
    //   Query { a : { $gte : 1, $lt : 2 },
    //            b : { $gte : 3, $lt : 4 } }
    //   => Bounds { a : [1, 2), b : [3, 4) }
    static IndexBounds getIndexBoundsForQuery(const BSONObj& key,
                                              const CanonicalQuery& canonicalQuery);

    // Collapse query solution tree.
    //
    // If it has OR node, the result could be a superset of the index bounds generated.
    // Since to give a single IndexBounds, this gives the union of bounds on each field.
    // for example:
    //   OR: { a: (0, 1), b: (0, 1) },
    //       { a: (2, 3), b: (2, 3) }
    //   =>  { a: (0, 1), (2, 3), b: (0, 1), (2, 3) }
    static IndexBounds collapseQuerySolution(const QuerySolutionNode* node);

    /**
     * Returns true if, for this shard, the chunks are identical in both chunk managers
     */
    bool compatibleWith(const ChunkManager& other, const ShardId& shard) const;

    std::string toString() const;

    bool uuidMatches(UUID uuid) const {
        return _uuid && *_uuid == uuid;
    }

private:
    /**
     * Represents a range of chunk keys [getMin(), getMax()) and the id of the shard on which they
     * reside according to the metadata.
     */
    struct ShardAndChunkRange {
        const BSONObj& min() const {
            return range.getMin();
        }

        const BSONObj& max() const {
            return range.getMax();
        }

        ChunkRange range;
        ShardId shardId;
    };

    //参考ChunkManager::_constructChunkMapViews
    using ChunkRangeMap = BSONObjIndexedMap<ShardAndChunkRange>;

    /**
     * Contains different transformations of the chunk map for efficient querying
     */
    //ChunkManager::_constructChunkMapViews
    struct ChunkMapViews {
        // Transformation of the chunk map containing what range of keys reside on which shard. The
        // index is the max key of the respective range and the union of all ranges in a such
        // constructed map must cover the complete space from [MinKey, MaxKey).
         
        const ChunkRangeMap chunkRangeMap;

        // Map from shard id to the maximum chunk version for that shard. If a shard contains no
        // chunks, it won't be present in this map.
        //每个shard的版本信息，取值为该shard最大的chunk版本信息
        //ChunkManager::getVersion中获取
        const ShardVersionMap shardVersions;
    };

    /**
     * Does a single pass over the chunkMap and constructs the ChunkMapViews object.
     */
    static ChunkMapViews _constructChunkMapViews(const OID& epoch, const ChunkMap& chunkMap);

    ChunkManager(NamespaceString nss,
                 boost::optional<UUID>,
                 KeyPattern shardKeyPattern,
                 std::unique_ptr<CollatorInterface> defaultCollator,
                 bool unique,
                 ChunkMap chunkMap,
                 ChunkVersion collectionVersion);

    // The shard versioning mechanism hinges on keeping track of the number of times we reload
    // ChunkManagers.
    const unsigned long long _sequenceNumber;

    // Namespace to which this routing information corresponds
    const NamespaceString _nss;

    // The invariant UUID of the collection.  This is optional in 3.6, except in change streams.
    const boost::optional<UUID> _uuid;

    // The key pattern used to shard the collection
    const ShardKeyPattern _shardKeyPattern;

    // Default collation to use for routing data queries for this collection
    const std::unique_ptr<CollatorInterface> _defaultCollator;

    // Whether the sharding key is unique
    const bool _unique;

    // Map from the max for each chunk to an entry describing the chunk. The union of all chunks'
    // ranges must cover the complete space from [MinKey, MaxKey).
    //路由表缓存在这里  ChunkManager::toString可以打印mongos缓存得路由表
    ////map表的 key为chunk.getMax()，value为chunk，参考ChunkManager::makeUpdated
    const ChunkMap _chunkMap;

    // Different transformations of the chunk map for efficient querying
    const ChunkMapViews _chunkMapViews;

    // Max version across all chunks
    //collection version 为 sharded collection 在所有shard上最高的 chunk version
    const ChunkVersion _collectionVersion;

    // Auto-split throttling state (state mutable by write commands)
    struct AutoSplitThrottle {
    public:
        AutoSplitThrottle() : _splitTickets(maxParallelSplits) {}

        TicketHolder _splitTickets;

        // Maximum number of parallel threads requesting a split
        static const int maxParallelSplits = 5;

    } _autoSplitThrottle;

    // This function needs to be able to access the auto-split throttle
    friend void updateChunkWriteStatsAndSplitIfNeeded(OperationContext*,
                                                      ChunkManager*,
                                                      Chunk*,
                                                      long);
};

}  // namespace mongo
