/**
*    Copyright (C) 2008 10gen Inc.
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
*/

#include "mongo/pch.h"

#include "mongo/db/repl/rs_sync.h"

#include <vector>

#include "third_party/murmurhash3/MurmurHash3.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/d_concurrency.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/repl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/rs_sync.h"

namespace mongo {

    using namespace bson;
    extern unsigned replSetForceInitialSyncFailure;

namespace replset {

    SyncTail::SyncTail(BackgroundSyncInterface *q) :
        Sync(""), _networkQueue(q)
    {}

    SyncTail::~SyncTail() {}

    bool SyncTail::peek(BSONObj* op) {
        return _networkQueue->peek(op);
    }
    /* apply the log op that is in param o
       @return bool success (true) or failure (false)
    */
    bool SyncTail::syncApply(const BSONObj &op) {
        const char *ns = op.getStringField("ns");
        verify(ns);

        if ( (*ns == '\0') || (*ns == '.') ) {
            // this is ugly
            // this is often a no-op
            // but can't be 100% sure
            if( *op.getStringField("op") != 'n' ) {
                error() << "replSet skipping bad op in oplog: " << op.toString() << rsLog;
            }
            return true;
        }

        bool isCommand(op["op"].valuestrsafe()[0] == 'c');

        boost::scoped_ptr<Lock::ScopedLock> lk;

        if(isCommand) {
            // a command may need a global write lock. so we will conservatively go 
            // ahead and grab one here. suboptimal. :-(
            lk.reset(new Lock::GlobalWrite());
        } else {
            // DB level lock for this operation
            lk.reset(new Lock::DBWrite(ns)); 
        }

        Client::Context ctx(ns, dbpath, false);
        ctx.getClient()->curop()->reset();
        bool ok = !applyOperation_inlock(op);
        getDur().commitIfNeeded();

        return ok;
    }

    void initializePrefetchThread() {
        if (!ClientBasic::getCurrent()) {
            Client::initThread("repl prefetch worker");
            replLocalAuth();
        }
    }

    static AtomicUInt32 replWriterWorkerId;
    void initializeWriterThread() {
        // Only do this once per thread
        if (!ClientBasic::getCurrent()) {
            string threadName = str::stream() << "repl writer worker " << replWriterWorkerId.addAndFetch(1);
            Client::initThread( threadName.c_str() );
            // allow us to get through the magic barrier
            Lock::ParallelBatchWriterMode::iAmABatchParticipant();
            replLocalAuth();
        }
    }

    // This free function is used by the writer threads to apply each op
    void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
        initializeWriterThread();
        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                fassert(16359, st->syncApply(*it));
            } catch (DBException& e) {
                error() << "writer worker caught exception: " << e.what() 
                        << " on: " << it->toString() << endl;
                fassertFailed(16360);
            }
        }
    }

    // This free function is used by the initial sync writer threads to apply each op
    void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
        initializeWriterThread();
        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                if (!st->syncApply(*it)) {
                    bool status;
                    {
                        Lock::GlobalWrite lk;
                        status = st->shouldRetry(*it);
                    }
                    if (status) {
                        // retry
                        fassert(15915, st->syncApply(*it));
                    }
                    // If shouldRetry() returns false, fall through.
                    // This can happen if the document that was moved and missed by Cloner
                    // subsequently got deleted and no longer exists on the Sync Target at all
                }
            }
            catch (DBException& e) {
                // Skip duplicate key exceptions.
                // These are relatively common on initial sync: if a document is inserted
                // early in the clone step, the insert will be replayed but the document
                // will probably already have been cloned over.
                if( e.getCode() == 11000 || e.getCode() == 11001 || e.getCode() == 12582) {
                    return; // ignore
                }
                error() << "exception: " << e.what() << " on: " << it->toString() << endl;
                fassertFailed(16361);
            }
        }
    }


    // The pool threads call this to prefetch each op
    void SyncTail::prefetchOp(const BSONObj& op) {
        initializePrefetchThread();

        const char *ns = op.getStringField("ns");
        if (ns && (ns[0] != '\0')) {
            try {
                Client::ReadContext ctx(ns);
                prefetchPagesForReplicatedOp(op);
            }
            catch (const DBException& e) {
                LOG(2) << "ignoring exception in prefetchOp(): " << e.what() << endl;
            }
            catch (const std::exception& e) {
                log() << "Unhandled std::exception in prefetchOp(): " << e.what() << endl;
                fassertFailed(16397);
            }
        }
    }

    // Doles out all the work to the reader pool threads and waits for them to complete
    void SyncTail::prefetchOps(const std::deque<BSONObj>& ops) {
        threadpool::ThreadPool& prefetcherPool = theReplSet->getPrefetchPool();
        for (std::deque<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            prefetcherPool.schedule(&prefetchOp, *it);
        }
        prefetcherPool.join();
    }
    
    // Doles out all the work to the writer pool threads and waits for them to complete
    void SyncTail::applyOps(const std::vector< std::vector<BSONObj> >& writerVectors, 
                                     MultiSyncApplyFunc applyFunc) {
        ThreadPool& writerPool = theReplSet->getWriterPool();
        for (std::vector< std::vector<BSONObj> >::const_iterator it = writerVectors.begin();
             it != writerVectors.end();
             ++it) {
            if (!it->empty()) {
                writerPool.schedule(applyFunc, boost::cref(*it), this);
            }
        }
        writerPool.join();
    }

    // Doles out all the work to the writer pool threads and waits for them to complete
    void SyncTail::multiApply( std::deque<BSONObj>& ops, MultiSyncApplyFunc applyFunc ) {

        // Use a ThreadPool to prefetch all the operations in a batch.
        prefetchOps(ops);
        
        std::vector< std::vector<BSONObj> > writerVectors(theReplSet->replWriterThreadCount);
        fillWriterVectors(ops, &writerVectors);
        LOG(1) << "replication batch size is " << ops.size() << endl;
        // We must grab this because we're going to grab write locks later.
        // We hold this mutex the entire time we're writing; it doesn't matter
        // because all readers are blocked anyway.
        SimpleMutex::scoped_lock fsynclk(filesLockedFsync);

        // stop all readers until we're done
        Lock::ParallelBatchWriterMode pbwm;

        applyOps(writerVectors, applyFunc);
    }


    void SyncTail::fillWriterVectors(const std::deque<BSONObj>& ops, 
                                              std::vector< std::vector<BSONObj> >* writerVectors) {
        for (std::deque<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            const BSONElement e = it->getField("ns");
            verify(e.type() == String);
            const char* ns = e.valuestr();
            int len = e.valuestrsize();
            uint32_t hash = 0;
            MurmurHash3_x86_32( ns, len, 0, &hash);

            (*writerVectors)[hash % writerVectors->size()].push_back(*it);
        }
    }


    InitialSync::InitialSync(BackgroundSyncInterface *q) : 
        SyncTail(q) {}

    InitialSync::~InitialSync() {}


    /* initial oplog application, during initial sync, after cloning.
    */
    void InitialSync::oplogApplication(const BSONObj& applyGTEObj, const BSONObj& minValidObj) {
        OpTime applyGTE = applyGTEObj["ts"]._opTime();
        OpTime minValid = minValidObj["ts"]._opTime();

        if (replSetForceInitialSyncFailure > 0) {
            log() << "replSet test code invoked, forced InitialSync failure: " << replSetForceInitialSyncFailure << rsLog;
            replSetForceInitialSyncFailure--;
            throw DBException("forced error",0);
        }

        syncApply(applyGTEObj);
        _logOpObjRS(applyGTEObj);


        // if there were no writes during the initial sync, there will be nothing in the queue so
        // just go live
        if (minValid == applyGTE) {
            return;
        }

        OpTime ts;
        time_t start = time(0);
        unsigned long long n = 0, lastN = 0;
        
        while( ts < minValid ) {
            OpQueue ops;

            while (ops.getSize() < replBatchSizeBytes) {
                if (tryPopAndWaitForMore(&ops)) {
                    break;
                }
            }

            
            multiApply(ops.getDeque(), multiInitialSyncApply);

            n += ops.getDeque().size();

            if ( n > lastN + 1000 ) {
                time_t now = time(0);
                if (now - start > 10) {
                    // simple progress metering
                    log() << "replSet initialSyncOplogApplication applied " << n << " operations, synced to "
                          << ts.toStringPretty() << rsLog;
                    start = now;
                    lastN = n;
                }
            }

            // we want to keep a record of the last op applied, to compare with minvalid
            const BSONObj& lastOp = ops.getDeque().back();
            OpTime tempTs = lastOp["ts"]._opTime();
            applyOpsToOplog(&ops.getDeque());

            ts = tempTs;
        }
    }

    /* tail an oplog.  ok to return, will be re-called. */
    void SyncTail::oplogApplication() {
        while( 1 ) {
            OpQueue ops;
            time_t lastTimeChecked = time(0);

            verify( !Lock::isLocked() );

            // always fetch a few ops first
            
            // tryPopAndWaitForMore returns true when we need to end a batch early
            while (!tryPopAndWaitForMore(&ops) && 
                   (ops.getSize() < replBatchSizeBytes)) {

                if (theReplSet->isPrimary()) {
                    return;
                }
                time_t now = time(0);
                // occasionally check some things
                if (ops.empty() || now > lastTimeChecked) {
                    lastTimeChecked = now;
                    // can we become secondary?
                    // we have to check this before calling mgr, as we must be a secondary to
                    // become primary
                    if (!theReplSet->isSecondary()) {
                        OpTime minvalid;
                        theReplSet->tryToGoLiveAsASecondary(minvalid);
                    }

                    // normally msgCheckNewState gets called periodically, but in a single node repl set
                    // there are no heartbeat threads, so we do it here to be sure.  this is relevant if the
                    // singleton member has done a stepDown() and needs to come back up.
                    if (theReplSet->config().members.size() == 1 &&
                        theReplSet->myConfig().potentiallyHot()) {
                        Manager* mgr = theReplSet->mgr;
                        // When would mgr be null?  During replsettest'ing.
                        if (mgr) mgr->send(boost::bind(&Manager::msgCheckNewState, theReplSet->mgr));
                        sleepsecs(1);
                        return;
                    }
                }
            }
            const BSONObj& lastOp = ops.getDeque().back();
            handleSlaveDelay(lastOp);

            // Set minValid to the last op to be applied in this next batch.
            // This will cause this node to go into RECOVERING state
            // if we should crash and restart before updating the oplog
            { 
                Client::WriteContext cx( "local" );
                Helpers::putSingleton("local.replset.minvalid", lastOp);
            }
            multiApply(ops.getDeque(), multiSyncApply);

            applyOpsToOplog(&ops.getDeque());
        }
    }

    // Copies ops out of the bgsync queue into the deque passed in as a parameter.
    // Returns true if the batch should be ended early.
    // Batch should end early if we encounter a command, or if
    // there are no further ops in the bgsync queue to read.
    // This function also blocks 1 second waiting for new ops to appear in the bgsync
    // queue.  We can't block forever because there are maintenance things we need
    // to periodically check in the loop.
    bool SyncTail::tryPopAndWaitForMore(SyncTail::OpQueue* ops) {
        BSONObj op;
        // Check to see if there are ops waiting in the bgsync queue
        bool peek_success = peek(&op);

        if (!peek_success) {
            // if we don't have anything in the queue, wait a bit for something to appear
            if (ops->empty()) {
                // block 1 second
                _networkQueue->waitForMore();
                return false;
            }

            // otherwise, apply what we have
            return true;
        }
        // check for commands
        if ((op["op"].valuestrsafe()[0] == 'c') ||
            // Index builds are acheived through the use of an insert op, not a command op.
            // The following line is the same as what the insert code uses to detect an index build.
            (strstr(op["ns"].valuestrsafe(), ".system.indexes"))) {
            if (ops->empty()) {
                // apply commands one-at-a-time
                ops->push_back(op);
                _networkQueue->consume();
            }

            // otherwise, apply what we have so far and come back for the command
            return true;
        }

        // Copy the op to the deque and remove it from the bgsync queue.
        ops->push_back(op);
        _networkQueue->consume();

        // Go back for more ops
        return false;
    }

    void SyncTail::applyOpsToOplog(std::deque<BSONObj>* ops) {
        {
            Lock::DBWrite lk("local");
            while (!ops->empty()) {
                const BSONObj& op = ops->front();
                // this updates theReplSet->lastOpTimeWritten
                _logOpObjRS(op);
                ops->pop_front();
             }
        }

        // Update write concern on primary
        BackgroundSync::notify();
    }

    void SyncTail::handleSlaveDelay(const BSONObj& lastOp) {
        int sd = theReplSet->myConfig().slaveDelay;

        // ignore slaveDelay if the box is still initializing. once
        // it becomes secondary we can worry about it.
        if( sd && theReplSet->isSecondary() ) {
            const OpTime ts = lastOp["ts"]._opTime();
            long long a = ts.getSecs();
            long long b = time(0);
            long long lag = b - a;
            long long sleeptime = sd - lag;
            if( sleeptime > 0 ) {
                uassert(12000, "rs slaveDelay differential too big check clocks and systems", sleeptime < 0x40000000);
                if( sleeptime < 60 ) {
                    sleepsecs((int) sleeptime);
                }
                else {
                    log() << "replSet slavedelay sleep long time: " << sleeptime << rsLog;
                    // sleep(hours) would prevent reconfigs from taking effect & such!
                    long long waitUntil = b + sleeptime;
                    while( 1 ) {
                        sleepsecs(6);
                        if( time(0) >= waitUntil )
                            break;

                        if( theReplSet->myConfig().slaveDelay != sd ) // reconf
                            break;
                    }
                }
            }
        } // endif slaveDelay
    }

} // namespace replset

    /* should be in RECOVERING state on arrival here.
       readlocks
       @return true if transitioned to SECONDARY
    */
    bool ReplSetImpl::tryToGoLiveAsASecondary(OpTime& /*out*/ minvalid) {
        bool golive = false;

        // make sure we're not primary or secondary already
        if (box.getState().primary() || box.getState().secondary()) {
            return false;
        }

        {
            lock lk( this );

            if (_maintenanceMode > 0) {
                // we're not actually going live
                return true;
            }

            // if we're blocking sync, don't change state
            if (_blockSync) {
                return false;
            }
        }

        {
            Lock::DBRead lk("local.replset.minvalid");
            BSONObj mv;
            if( Helpers::getSingleton("local.replset.minvalid", mv) ) {
                minvalid = mv["ts"]._opTime();
                if( minvalid <= lastOpTimeWritten ) {
                    golive=true;
                }
                else {
                    sethbmsg(str::stream() << "still syncing, not yet to minValid optime " << minvalid.toString());
                }
            }
            else
                golive = true; /* must have been the original member */
        }
        if( golive ) {
            sethbmsg("");
            changeState(MemberState::RS_SECONDARY);
        }
        return golive;
    }


    bool ReplSetImpl::forceSyncFrom(const string& host, string& errmsg, BSONObjBuilder& result) {
        lock lk(this);

        // initial sanity check
        if (iAmArbiterOnly()) {
            errmsg = "arbiters don't sync";
            return false;
        }

        // find the member we want to sync from
        Member *newTarget = 0;
        for (Member *m = _members.head(); m; m = m->next()) {
            if (m->fullName() == host) {
                newTarget = m;
                break;
            }
        }

        // do some more sanity checks
        if (!newTarget) {
            // this will also catch if someone tries to sync a member from itself, as _self is not
            // included in the _members list.
            errmsg = "could not find member in replica set";
            return false;
        }
        if (newTarget->config().arbiterOnly) {
            errmsg = "I cannot sync from an arbiter";
            return false;
        }
        if (!newTarget->config().buildIndexes && myConfig().buildIndexes) {
            errmsg = "I cannot sync from a member who does not build indexes";
            return false;
        }
        if (newTarget->hbinfo().authIssue) {
            errmsg = "I cannot authenticate against the requested member";
            return false;
        }
        if (newTarget->hbinfo().health == 0) {
            errmsg = "I cannot reach the requested member";
            return false;
        }
        if (newTarget->hbinfo().opTime.getSecs()+10 < lastOpTimeWritten.getSecs()) {
            log() << "attempting to sync from " << newTarget->fullName()
                  << ", but its latest opTime is " << newTarget->hbinfo().opTime.getSecs()
                  << " and ours is " << lastOpTimeWritten.getSecs() << " so this may not work"
                  << rsLog;
            result.append("warning", "requested member is more than 10 seconds behind us");
            // not returning false, just warning
        }

        // record the previous member we were syncing from
        Member *prev = replset::BackgroundSync::get()->getSyncTarget();
        if (prev) {
            result.append("prevSyncTarget", prev->fullName());
        }

        // finally, set the new target
        _forceSyncTarget = newTarget;
        return true;
    }

    bool ReplSetImpl::gotForceSync() {
        lock lk(this);
        return _forceSyncTarget != 0;
    }

    void ReplSetImpl::_syncThread() {
        StateBox::SP sp = box.get();
        if( sp.state.primary() ) {
            sleepsecs(1);
            return;
        }
        if( _blockSync || sp.state.fatal() || sp.state.startup() ) {
            sleepsecs(5);
            return;
        }

        /* do we have anything at all? */
        if( lastOpTimeWritten.isNull() ) {
            syncDoInitialSync();
            return; // _syncThread will be recalled, starts from top again in case sync failed.
        }

        /* we have some data.  continue tailing. */
        replset::SyncTail tail(replset::BackgroundSync::get());
        tail.oplogApplication();
    }

    void ReplSetImpl::syncThread() {
        while( 1 ) {
            // After a reconfig, we may not be in the replica set anymore, so
            // check that we are in the set (and not an arbiter) before
            // trying to sync with other replicas.
            if( ! _self ) {
                log() << "replSet warning did not receive a valid config yet, sleeping 20 seconds " << rsLog;
                sleepsecs(20);
                continue;
            }
            if( myConfig().arbiterOnly ) {
                return;
            }

            fassert(16113, !Lock::isLocked());

            try {
                _syncThread();
            }
            catch(DBException& e) {
                sethbmsg(str::stream() << "syncThread: " << e.toString());
                sleepsecs(10);
            }
            catch(...) {
                sethbmsg("unexpected exception in syncThread()");
                // TODO : SET NOT SECONDARY here?
                sleepsecs(60);
            }
            sleepsecs(1);
        }
    }

    void startSyncThread() {
        static int n;
        if( n != 0 ) {
            log() << "replSet ERROR : more than one sync thread?" << rsLog;
            verify( n == 0 );
        }
        n++;

        Client::initThread("rsSync");
        cc().iAmSyncThread(); // for isSyncThread() (which is used not used much, is used in secondary create index code
        replLocalAuth();
        theReplSet->syncThread();
        cc().shutdown();
    }

    void GhostSync::starting() {
        Client::initThread("rsGhostSync");
        replLocalAuth();
    }

    void ReplSetImpl::blockSync(bool block) {
        _blockSync = block;
        if (_blockSync) {
            // syncing is how we get into SECONDARY state, so we'll be stuck in
            // RECOVERING until we unblock
            changeState(MemberState::RS_RECOVERING);
        }
    }

    void GhostSync::associateSlave(const BSONObj& id, const int memberId) {
        const OID rid = id["_id"].OID();
        rwlock lk( _lock , true );
        shared_ptr<GhostSlave> &g = _ghostCache[rid];
        if( g.get() == 0 ) {
            g.reset( new GhostSlave() );
            wassert( _ghostCache.size() < 10000 );
        }
        GhostSlave &slave = *g;
        if (slave.init) {
            LOG(1) << "tracking " << slave.slave->h().toString() << " as " << rid << rsLog;
            return;
        }

        slave.slave = (Member*)rs->findById(memberId);
        if (slave.slave != 0) {
            slave.init = true;
        }
        else {
            log() << "replset couldn't find a slave with id " << memberId
                  << ", not tracking " << rid << rsLog;
        }
    }

    void GhostSync::updateSlave(const mongo::OID& rid, const OpTime& last) {
        rwlock lk( _lock , false );
        MAP::iterator i = _ghostCache.find( rid );
        if ( i == _ghostCache.end() ) {
            OCCASIONALLY warning() << "couldn't update slave " << rid << " no entry" << rsLog;
            return;
        }

        GhostSlave& slave = *(i->second);
        if (!slave.init) {
            OCCASIONALLY log() << "couldn't update slave " << rid << " not init" << rsLog;
            return;
        }

        ((ReplSetConfig::MemberCfg)slave.slave->config()).updateGroups(last);
    }

    void GhostSync::percolate(const BSONObj& id, const OpTime& last) {
        const OID rid = id["_id"].OID();
        GhostSlave* slave;
        {
            rwlock lk( _lock , false );

            MAP::iterator i = _ghostCache.find( rid );
            if ( i == _ghostCache.end() ) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " no entry" << rsLog;
                return;
            }

            slave = i->second.get();
            if (!slave->init) {
                OCCASIONALLY log() << "couldn't percolate slave " << rid << " not init" << rsLog;
                return;
            }
        }

        verify(slave->slave);

        const Member *target = replset::BackgroundSync::get()->getSyncTarget();
        if (!target || rs->box.getState().primary()
            // we are currently syncing from someone who's syncing from us
            // the target might end up with a new Member, but s.slave never
            // changes so we'll compare the names
            || target == slave->slave || target->fullName() == slave->slave->fullName()) {
            LOG(1) << "replica set ghost target no good" << endl;
            return;
        }

        try {
            if (!slave->reader.haveCursor()) {
                if (!slave->reader.connect(id, slave->slave->id(), target->fullName())) {
                    // error message logged in OplogReader::connect
                    return;
                }
                slave->reader.ghostQueryGTE(rsoplog, last);
            }

            LOG(1) << "replSet last: " << slave->last.toString() << " to " << last.toString() << rsLog;
            if (slave->last > last) {
                return;
            }

            while (slave->last <= last) {
                if (!slave->reader.more()) {
                    // we'll be back
                    return;
                }

                BSONObj o = slave->reader.nextSafe();
                slave->last = o["ts"]._opTime();
            }
            LOG(2) << "now last is " << slave->last.toString() << rsLog;
        }
        catch (DBException& e) {
            // we'll be back
            LOG(2) << "replSet ghost sync error: " << e.what() << " for "
                   << slave->slave->fullName() << rsLog;
            slave->reader.resetConnection();
        }
    }
}
