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

#include <boost/optional/optional.hpp>
#include <set>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/plan_cache_indexability.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

// A PlanCacheKey is a string-ified version of a query's predicate/projection/sort.
//
//���������computeKey(cq)ΪgetPlansByQuery�еĲ�ѯdb.xx.getPlanCache().getPlansByQuery({"query" : {"create_time" : { "$gte" : "2020-12-27 00:00:00","$lte" : "2021-01-26 23:59:59"}},"sort" : { },"projection" : {}})
//�ο�PlanCache::contains
typedef std::string PlanCacheKey;

struct PlanRankingDecision;
struct QuerySolution;
struct QuerySolutionNode;

/**
 * When the CachedPlanStage runs a cached query, it can provide feedback to the cache.  This
 * feedback is available to anyone who retrieves that query in the future.
 */
//CachedPlanStage::updatePlanCache()�й���ʹ��
struct PlanCacheEntryFeedback {
    // How well did the cached plan perform?
    std::unique_ptr<PlanStageStats> stats;

    // The "goodness" score produced by the plan ranker
    // corresponding to 'stats'.
    double score;
};

// TODO: Replace with opaque type.
typedef std::string PlanID;

/**
 * A PlanCacheIndexTree is the meaty component of the data
 * stored in SolutionCacheData. It is a tree structure with
 * index tags that indicates to the access planner which indices
 * it should try to use.
 *
 * How a PlanCacheIndexTree is created:
 *   The query planner tags a match expression with indices. It
 *   then uses the tagged tree to create a PlanCacheIndexTree,
 *   using QueryPlanner::cacheDataFromTaggedTree. The PlanCacheIndexTree
 *   is isomorphic to the tagged match expression, and has matching
 *   index tags.
 *
 * How a PlanCacheIndexTree is used:
 *   When the query planner is planning from the cache, it uses
 *   the PlanCacheIndexTree retrieved from the cache in order to
 *   recreate index assignments. Specifically, a raw MatchExpression
 *   is tagged according to the index tags in the PlanCacheIndexTree.
 *   This is done by QueryPlanner::tagAccordingToCache.
 */
//QueryPlanner::plan->QueryPlanner::cacheDataFromTaggedTree�и�ֵ

//QuerySolution.cacheData.tree�л����Ӧ������tree��Ϣ
//SolutionCacheData.treeΪ�����ͣ�����solution��Ӧ������Ϣ
//cacheDataFromTaggedTree  choosePlanForSubqueries  QueryPlanner::plan�й���ʹ��
struct PlanCacheIndexTree {

    /**
     * An OrPushdown is the cached version of an OrPushdownTag::Destination. It indicates that this
     * node is a predicate that can be used inside of a sibling indexed OR, to tighten index bounds
     * or satisfy the first field in the index.
     */
    struct OrPushdown {
        std::string indexName;
        size_t position;
        bool canCombineBounds;
        std::deque<size_t> route;
    };

    PlanCacheIndexTree() : entry(nullptr), index_pos(0), canCombineBounds(true) {}

    ~PlanCacheIndexTree() {
        for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
             it != children.end();
             ++it) {
            delete *it;
        }
    }

    /**
     * Clone 'ie' and set 'this->entry' to be the clone.
     */
    void setIndexEntry(const IndexEntry& ie);

    /**
     * Make a deep copy.
     */
    PlanCacheIndexTree* clone() const;

    /**
     * For debugging.
     */
    std::string toString(int indents = 0) const;

    // Children owned here.
    //tree��ͨ��children����
    std::vector<PlanCacheIndexTree*> children;

    // Owned here. 
    //cacheDataFromTaggedTree�й���ʹ�ã���¼������Ϣ
    std::unique_ptr<IndexEntry> entry;
    //cacheDataFromTaggedTree�й���ʹ��  
    size_t index_pos;

    // The value for this member is taken from the IndexTag of the corresponding match expression
    // and is used to ensure that bounds are correctly intersected and/or compounded when a query is
    // planned from the plan cache.
    bool canCombineBounds; //cacheDataFromTaggedTree�й���ʹ��

    std::vector<OrPushdown> orPushdowns;
};

/**
 * Data stored inside a QuerySolution which can subsequently be
 * used to create a cache entry. When this data is retrieved
 * from the cache, it is sufficient to reconstruct the original
 * QuerySolution.
 */ 
//QueryPlanner::plan�й���, 
//SubplanStage._branchResults.cachedSolution.plannerDataΪ������  
//CachedSolution.plannerDataΪ������
//QuerySolution.cacheData  PlanCacheEntry.plannerData��ԱΪ������

//����ͨ��PlanCacheListPlans::list�鿴������ݣ�Ҳ����PlanCacheListPlans�����������:
//"(index-tagged expression tree: tree=Leaf name_1_age_1__id_1, pos: 0, can combine? 1\n)"

//�ýṹʵ���ϼ�¼�˸�solution��������Ϣ
struct SolutionCacheData {
    SolutionCacheData()
        : tree(nullptr),
          solnType(USE_INDEX_TAGS_SOLN),
          wholeIXSolnDir(1),
          indexFilterApplied(false) {}

    // Make a deep copy.
    SolutionCacheData* clone() const;

    // For debugging.
    std::string toString() const;

    // Owned here. If 'wholeIXSoln' is false, then 'tree'
    // can be used to tag an isomorphic match expression. If 'wholeIXSoln'
    // is true, then 'tree' is used to store the relevant IndexEntry.
    // If 'collscanSoln' is true, then 'tree' should be NULL.
    //SolutionCacheData::toString()�����tree��Ϣ
    //QueryPlanner::plan�и�ֵ������cacheIndex��Ҳ���ǻ���solution��Ӧ������Ϣ
    //QueryPlanner::plan->QueryPlanner::cacheDataFromTaggedTree�и�ֵ����¼solution��Ӧ��������Ϣ
    std::unique_ptr<PlanCacheIndexTree> tree;   //QuerySolution.cacheData.tree�л����Ӧ������tree��Ϣ

    //��Ӧ��ͬ��������ο�SolutionCacheData::toString()
    enum SolutionType {
        // Indicates that the plan should use
        // the index as a proxy for a collection
        // scan (e.g. using index to provide sort).
        //û�к��ʵ�index��������Ҫsort���򣬻����ж�Ӧindex,���Ǻ��������෴(����������name:1,����������name:-1)
        //���findû��Я����ѯ������������projection���ˣ�����out solutionΪ0����Ĭ������ѡ����������if�����жϵ���������buildWholeIXSoln����
        WHOLE_IXSCAN_SOLN,   //�ο�QueryPlanner::plan

        // The cached plan is a collection scan.
        //ȫ��ɨ��
        COLLSCAN_SOLN,   //�ο�QueryPlanner::plan

        // Build the solution by using 'tree'
        // to tag the match expression.
        //�ߺ�ѡ������SolutionCacheData����ʹ�õ�Ĭ��ֵ
        //ֻ��SubplanStage��ʹ�ã����solnType != USE_INDEX_TAGS_SOLN��˵��û�к��ʵĺ�ѡ�������ο�tagOrChildAccordingToCache
        USE_INDEX_TAGS_SOLN
    } solnType; //Ĭ��USE_INDEX_TAGS_SOLN

    // The direction of the index scan used as
    // a proxy for a collection scan. Used only
    // for WHOLE_IXSCAN_SOLN.
    //�����Ƿ����ѯ���ο�QueryPlanner::plan
    int wholeIXSolnDir;

    // True if index filter was applied.
    bool indexFilterApplied;
};

class PlanCacheEntry;

/**
 * Information returned from a get(...) query.
 */
//PlanCache::get�й���ʹ��
//SubplanStage._branchResults.cachedSolutionΪ������
//QueryPlanner::planFromCache�и���CachedSolution��ȡQuerySolution

//ֻ���ڲ�ѯ�ж����ѡ������ʱ�򣬲ŻỺ���Ӧsolution��Ϣ
class CachedSolution {
private:
    MONGO_DISALLOW_COPYING(CachedSolution);

public:
    CachedSolution(const PlanCacheKey& key, const PlanCacheEntry& entry);
    ~CachedSolution();

    // Owned here. 
    //solution�����������棬�����0����Ա�����ŵģ��Դ����ƣ�����QueryPlanner::planFromCache��ʹ��
    std::vector<SolutionCacheData*> plannerData; 

    // Key used to provide feedback on the entry.
    PlanCacheKey key;

    // For debugging.
    std::string toString() const;

    // We are extracting just enough information from the canonical
    // query. We could clone the canonical query but the following
    // items are all that is displayed to the user.
    BSONObj query;
    BSONObj sort;
    BSONObj projection;
    BSONObj collation;

    // The number of work cycles taken to decide on a winning plan when the plan was first
    // cached.
    size_t decisionWorks;
};

/**
 * Used by the cache to track entries and their performance over time.
 * Also used by the plan cache commands to display plan cache state.
 */ 
/* planCache�������������ļ�ʵ��
listQueryShapes��ȡ�����plancache��Ҳ���ǻ��������
X-X:PRIMARY> db.XResource.getPlanCache().listQueryShapes()
{
        "query" : {
                "status" : 1,
                "likedTimes" : {
                        "$gte" : 1500
                }
        },
        "sort" : {

        },
        "projection" : {

        }
}
getPlansByQuery�鿴cache�е�ִ�мƻ�
xx-xx:PRIMARY> db.xx.getPlanCache().getPlansByQuery({"query" : {"create_time" : { "$gte" : "2020-12-27 00:00:00","$lte" : "2021-01-26 23:59:59"}},"sort" : { },"projection" : {}})
{
        "plans" : [
                {
                        "details" : {
                                "solution" : "(index-tagged expression tree: tree=Node\n---Leaf create_time_1, pos: 0, can combine? 1\n---Leaf create_time_1, pos: 0, can combine? 1\n)"
                        },
                        "reason" : {
                                "score" : 2.0003,
                                "stats" : {
                                        "stage" : "FETCH",
                                        "nReturned" : 101,
                                        "executionTimeMillisEstimate" : 60,
                                        "works" : 101,
                                        "advanced" : 101,
                                        "needTime" : 0,
                                        "needYield" : 0,
                                        "saveState" : 2,
                                        "restoreState" : 2,
                                        "isEOF" : 0,
                                        "invalidates" : 0,
                                        "docsExamined" : 101,
                                        "alreadyHasObj" : 0,
                                        "inputStage" : {
                                                "stage" : "IXSCAN",
                                                "nReturned" : 101,
                                                "executionTimeMillisEstimate" : 0,
                                                "works" : 101,
                                                "advanced" : 101,
                                                "needTime" : 0,
                                                "needYield" : 0,
                                                "saveState" : 2,
                                                "restoreState" : 2,
                                                "isEOF" : 0,
                                                "invalidates" : 0,
                                                "keyPattern" : {
                                                        "create_time" : 1
                                                },
                                                "indexName" : "create_time_1",
                                                "isMultiKey" : false,
                                                "multiKeyPaths" : {
                                                        "create_time" : [ ]
                                                },
                                                "isUnique" : false,
                                                "isSparse" : false,
                                                "isPartial" : false,
                                                "indexVersion" : 2,
                                                "direction" : "forward",
                                                "indexBounds" : {
                                                        "create_time" : [
                                                                "[\"2020-12-27 00:00:00\", \"2021-01-26 23:59:59\"]"
                                                        ]
                                                },
                                                "keysExamined" : 101,
                                                "seeks" : 1,
                                                "dupsTested" : 0,
                                                "dupsDropped" : 0,
                                                "seenInvalidated" : 0
                                        }
                                }
                        },
                        "feedback" : {
                                "nfeedback" : 8,
                                "scores" : [
                                        {
                                                "score" : 2.0003
                                        },
                                        {
                                                "score" : 2.0003
                                        },
                                        {
                                                "score" : 2.0003
                                        },
                                        {
                                                "score" : 2.0003
                                        },
                                        {
                                                "score" : 2.0003
                                        },
                                        {
                                                "score" : 2.0003
                                        },
                                        {
                                                "score" : 2.0003
                                        },
                                        {
                                                "score" : 2.0003
                                        }
                                ]
                        },
                        "filterSet" : false
                },
        {
                "details" : {
                        "solution" : "(index-tagged expression tree: tree=Node\n---Leaf create_time_1_audit_state_1_auto_audit_state_1_manual_state_1, pos: 0, can combine? 1\n---Leaf create_time_1_audit_state_1_auto_audit_state_1_manual_state_1, pos: 0, can combine? 1\n)"
                },
                "reason" : {
                        "score" : 2.0003,
                        "stats" : {
                                "stage" : "FETCH",
                                "nReturned" : 101,
                                "executionTimeMillisEstimate" : 0,
                                "works" : 101,
                                "advanced" : 101,
                                "needTime" : 0,
                                "needYield" : 0,
                                "saveState" : 2,
                                "restoreState" : 2,
                                "isEOF" : 0,
                                "invalidates" : 0,
                                "docsExamined" : 101,
                                "alreadyHasObj" : 0,
                                "inputStage" : {
                                        "stage" : "IXSCAN",
                                        "nReturned" : 101,
                                        "executionTimeMillisEstimate" : 0,
                                        "works" : 101,
                                        "advanced" : 101,
                                        "needTime" : 0,
                                        "needYield" : 0,
                                        "saveState" : 2,
                                        "restoreState" : 2,
                                        "isEOF" : 0,
                                        "invalidates" : 0,
                                        "keyPattern" : {
                                                "create_time" : 1,
                                                "audit_state" : 1,
                                                "auto_audit_state" : 1,
                                                "manual_state" : 1
                                        },
                                        "indexName" : "create_time_1_audit_state_1_auto_audit_state_1_manual_state_1",
                                        "isMultiKey" : false,
                                        "multiKeyPaths" : {
                                                "create_time" : [ ],
                                                "audit_state" : [ ],
                                                "auto_audit_state" : [ ],
                                                "manual_state" : [ ]
                                        },
                                        "isUnique" : false,
                                        "isSparse" : false,
                                        "isPartial" : false,
                                        "indexVersion" : 2,
                                        "direction" : "forward",
                                        "indexBounds" : {
                                                "create_time" : [
                                                        "[\"2020-12-27 00:00:00\", \"2021-01-26 23:59:59\"]"
                                                ],
                                                "audit_state" : [
                                                        "[MinKey, MaxKey]"
                                                ],
                                                "auto_audit_state" : [
                                                        "[MinKey, MaxKey]"
                                                ],
                                                "manual_state" : [
                                                        "[MinKey, MaxKey]"
                                                ]
                                        },
                                        "keysExamined" : 101,
                                        "seeks" : 1,
                                        "dupsTested" : 0,
                                        "dupsDropped" : 0,
                                        "seenInvalidated" : 0
                                }
                        }
                },
                "feedback" : {

                },
                "filterSet" : false
        }
],


......
�ο�querysolution.txt�ļ���<planCache���>�½�
*/

//PlanCache::getAllEntries�л�ȡentry��Ϣ
//PlanCache._cacheΪ�����ͣ�PlanCacheEntry���浽PlanCache._cache�ṹ��lru��
//PlanCacheListQueryShapes  PlanCacheClear  PlanCacheListPlans��������ʹ�ò鿴������Ӧcommandģ��
class PlanCacheEntry {
private:
    MONGO_DISALLOW_COPYING(PlanCacheEntry);

public:
    /**
     * Create a new PlanCacheEntry.
     * Grabs any planner-specific data required from the solutions.
     * Takes ownership of the PlanRankingDecision that placed the plan in the cache.
     */
    PlanCacheEntry(const std::vector<QuerySolution*>& solutions, PlanRankingDecision* why);

    ~PlanCacheEntry();

    /**
     * Make a deep copy.
     */
    PlanCacheEntry* clone() const;

    // For debugging.
    std::string toString() const;

    //
    // Planner data
    //

    // Data provided to the planner to allow it to recreate the solutions this entry
    // represents. Each SolutionCacheData is fully owned here, so in order to return
    // it from the cache a deep copy is made and returned inside CachedSolution.
    std::vector<SolutionCacheData*> plannerData;

    // TODO: Do we really want to just hold a copy of the CanonicalQuery?  For now we just
    // extract the data we need.
    //
    // Used by the plan cache commands to display an example query
    // of the appropriate shape.
    BSONObj query;
    BSONObj sort;
    BSONObj projection;
    BSONObj collation;
    Date_t timeOfCreation;

    //
    // Performance stats
    //

    // Information that went into picking the winning plan and also why
    // the other plans lost. 
    //PlanCacheListPlans::listͨ��PlanCacheListPlans�������
    //��planCache ��ֹ��̼���planstage
    std::unique_ptr<PlanRankingDecision> decision;

    // Annotations from cached runs.  The CachedPlanStage provides these stats about its
    // runs when they complete.
    //PlanCacheListPlans::listͨ��PlanCacheListPlans�������
    //������Դ��CachedPlanStage::updatePlanCache()
    std::vector<PlanCacheEntryFeedback*> feedback;
};

/**
 * Caches the best solution to a query.  Aside from the (CanonicalQuery -> QuerySolution)
 * mapping, the cache contains information on why that mapping was made and statistics on the
 * cache entry's actual performance on subsequent runs.
 *
 */

/*
listQueryShapes��ȡ�����plancache��Ҳ���ǻ��������
X-X:PRIMARY> db.XResource.getPlanCache().listQueryShapes()
{
		"query" : {
				"status" : 1,
				"likedTimes" : {
						"$gte" : 1500
				}
		},
		"sort" : {

		},
		"projection" : {

		}
}
getPlansByQuery�鿴cache�е�ִ�мƻ�
:PRIMARY> db.videoResource.getPlanCache().getPlansByQuery({"query" : {"status" : 1,"likedTimes" : {"$gte" : 1500} },"sort" : {},"projection" : {}})
{
        "plans" : [
                {
                        "details" : {
                                "solution" : "(index-tagged expression tree: tree=Node\n---Leaf status_1_likedTimes_-1_createTime_-1_viewCount_1, pos: 0, can combine? 1\n---Leaf status_1_likedTimes_-1_createTime_-1_viewCount_1, pos: 1, can combine? 1\n)"
                        },
                        "reason" : {
                                "score" : 2.0003,
                                "stats" : {
                                        "stage" : "LIMIT",
                                        "nReturned" : 101,
                                        "executionTimeMillisEstimate" : 0,
                                        "works" : 101,
                                        "advanced" : 101,
                                        "needTime" : 0,
                                        "needYield" : 0,
                                        "saveState" : 3,
                                        "restoreState" : 3,
                                        "isEOF" : 0,
                                        "invalidates" : 0,
                                        "limitAmount" : 180,
                                        "inputStage" : {
                                                "stage" : "FETCH",
                                                "nReturned" : 101,
                                                "executionTimeMillisEstimate" : 0,
                                                "works" : 101,
                                                "advanced" : 101,
                                                "needTime" : 0,
......
�ο�querysolution.txt�ļ���<planCache���>�½�
*/


//plancache���Բο�https://segmentfault.com/a/1190000015236644   CollectionInfoCacheImpl._planCache
//CollectionInfoCacheImpl._planCache��ԱΪ�ýṹ���ͣ�PlanCache���ձ��ֵ��ó�Ա
//PlanCacheListQueryShapes  PlanCacheClear  PlanCacheListPlans��������ʹ�ò鿴������Ӧcommandģ��
class PlanCache {
private:
    MONGO_DISALLOW_COPYING(PlanCache);

public:
    /**
     * We don't want to cache every possible query. This function
     * encapsulates the criteria for what makes a canonical query
     * suitable for lookup/inclusion in the cache.
     */
    static bool shouldCacheQuery(const CanonicalQuery& query);

    /**
     * If omitted, namespace set to empty string.
     */
    PlanCache();

    PlanCache(const std::string& ns);

    ~PlanCache();

    /**
     * Record solutions for query. Best plan is first element in list.
     * Each query in the cache will have more than 1 plan because we only
     * add queries which are considered by the multi plan runner (which happens
     * only when the query planner generates multiple candidate plans). Callers are responsible
     * for passing the current time so that the time the plan cache entry was created is stored
     * in the plan cache.
     *
     * Takes ownership of 'why'.
     *
     * If the mapping was added successfully, returns Status::OK().
     * If the mapping already existed or some other error occurred, returns another Status.
     */
    //MultiPlanStage::pickBestPlan�аѵ÷ָߵĺ�ѡ������ӵ�plancache
    Status add(const CanonicalQuery& query,
               const std::vector<QuerySolution*>& solns,
               PlanRankingDecision* why,
               Date_t now);

    /**
     * Look up the cached data access for the provided 'query'.  Used by the query planner
     * to shortcut planning.
     *
     * If there is no entry in the cache for the 'query', returns an error Status.
     *
     * If there is an entry in the cache, populates 'crOut' and returns Status::OK().  Caller
     * owns '*crOut'.
     */
    Status get(const CanonicalQuery& query, CachedSolution** crOut) const;

    /**
     * When the CachedPlanStage runs a plan out of the cache, we want to record data about the
     * plan's performance.  The CachedPlanStage calls feedback(...) after executing the cached
     * plan for a trial period in order to do this.
     *
     * Cache takes ownership of 'feedback'.
     *
     * If the entry corresponding to 'cq' isn't in the cache anymore, the feedback is ignored
     * and an error Status is returned.
     *
     * If the entry corresponding to 'cq' still exists, 'feedback' is added to the run
     * statistics about the plan.  Status::OK() is returned.
     */
    Status feedback(const CanonicalQuery& cq, PlanCacheEntryFeedback* feedback);

    /**
     * Remove the entry corresponding to 'ck' from the cache.  Returns Status::OK() if the plan
     * was present and removed and an error status otherwise.
     */
    Status remove(const CanonicalQuery& canonicalQuery);

    /**
     * Remove *all* cached plans.  Does not clear index information.
     */
    void clear();

    /**
     * Get the cache key corresponding to the given canonical query.  The query need not already
     * be cached.
     *
     * This is provided in the public API simply as a convenience for consumers who need some
     * description of query shape (e.g. index filters).
     *
     * Callers must hold the collection lock when calling this method.
     */
    PlanCacheKey computeKey(const CanonicalQuery&) const;

    /**
     * Returns a copy of a cache entry.
     * Used by planCacheListPlans to display plan details.
      *
     * If there is no entry in the cache for the 'query', returns an error Status.
     *
     * If there is an entry in the cache, populates 'entryOut' and returns Status::OK().  Caller
     * owns '*entryOut'.
     */
    Status getEntry(const CanonicalQuery& cq, PlanCacheEntry** entryOut) const;

    /**
     * Returns a vector of all cache entries.
     * Caller owns the result vector and is responsible for cleaning up
     * the cache entry copies.
     * Used by planCacheListQueryShapes and index_filter_commands_test.cpp.
     */
    std::vector<PlanCacheEntry*> getAllEntries() const;

    /**
     * Returns true if there is an entry in the cache for the 'query'.
     * Internally calls hasKey() on the LRU cache.
     */
    bool contains(const CanonicalQuery& cq) const;

    /**
     * Returns number of entries in cache.
     * Used for testing.
     */
    size_t size() const;

    /**
     * Updates internal state kept about the collection's indexes.  Must be called when the set
     * of indexes on the associated collection have changed.
     *
     * Callers must hold the collection lock in exclusive mode when calling this method.
     */
    void notifyOfIndexEntries(const std::vector<IndexEntry>& indexEntries);

private:
    void encodeKeyForMatch(const MatchExpression* tree, StringBuilder* keyBuilder) const;
    void encodeKeyForSort(const BSONObj& sortObj, StringBuilder* keyBuilder) const;
    void encodeKeyForProj(const BSONObj& projObj, StringBuilder* keyBuilder) const;
    
    //PlanCacheEntry����PlanCacheKey���浽���֧��LRU
    //����ĳ�������PlanCacheEntry, �ο�PlanCache::get  PlanCache::getAllEntries()
    ////MultiPlanStage::pickBestPlan�аѵ÷ָߵĺ�ѡ������ӵ�plancache
    LRUKeyValue<PlanCacheKey, PlanCacheEntry> _cache;

    // Protects _cache.
    mutable stdx::mutex _cacheMutex;

    // Full namespace of collection.
    std::string _ns;

    // Holds computed information about the collection's indexes.  Used for generating plan
    // cache keys.
    //
    // Concurrent access is synchronized by the collection lock.  Multiple concurrent readers
    // are allowed.
    PlanCacheIndexabilityState _indexabilityState;
};

}  // namespace mongo
