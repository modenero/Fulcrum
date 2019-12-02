#include "BlockProc.h"
#include "BTC.h"
#include "Controller.h"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <list>
#include <map>

Controller::Controller(const std::shared_ptr<Options> &o)
    : Mgr(nullptr), options(o)
{
    setObjectName("Controller");
    _thread.setObjectName(objectName());
}

Controller::~Controller() { Debug("%s", __FUNCTION__); cleanup(); }

void Controller::startup()
{
    storage = std::make_unique<Storage>(options);
    storage->startup(); // may throw here

    bitcoindmgr = std::make_unique<BitcoinDMgr>(options->bitcoind.first, options->bitcoind.second, options->rpcuser, options->rpcpassword);
    {
        // some setup code that waits for bitcoind to be ready before kicking off our "process" method
        auto waitForBitcoinD = [this] {
            auto constexpr waitTimer = "wait4bitcoind", callProcessTimer = "callProcess";
            int constexpr msgPeriod = 10000, // 10sec
                          smallDelay = 100;
            stopTimer(pollTimerName);
            stopTimer(callProcessTimer);
            callOnTimerSoon(msgPeriod, waitTimer, []{ Log("Waiting for bitcoind..."); return true; }, false, Qt::TimerType::VeryCoarseTimer);
            // connection to kick off our 'process' method once the first auth is received
            auto connPtr = std::make_shared<QMetaObject::Connection>();
            *connPtr = connect(bitcoindmgr.get(), &BitcoinDMgr::gotFirstGoodConnection, this, [this, connPtr](quint64 id) mutable {
                if (connPtr) {
                    stopTimer(waitTimer);
                    if (!disconnect(*connPtr)) Fatal() << "Failed to disconnect 'authenticated' signal! FIXME!"; // this should never happen but if it does, app quits.
                    connPtr.reset(); // clear connPtr right away to 1. delte it asap and 2. so we are guaranteed not to reenter this block for this connection should there be a spurious signal emitted.
                    Debug() << "Auth recvd from bicoind with id: " << id << ", proceeding with processing ...";
                    callOnTimerSoonNoRepeat(smallDelay, callProcessTimer, [this]{process();}, true);
                }
            });
        };
        waitForBitcoinD();
        conns += connect(bitcoindmgr.get(), &BitcoinDMgr::allConnectionsLost, this, waitForBitcoinD);
        conns += connect(bitcoindmgr.get(), &BitcoinDMgr::inWarmUp, this, [last = -1.0](const QString &msg) mutable {
            // just print a message to the log as to why we keep dropping conn. -- if bitcoind is still warming up
            auto now = Util::getTimeSecs();
            if (now-last >= 1.0) { // throttled to not spam log
                last = now;
                Log() << "bitcoind is still warming up: " << msg;
            }
        });
    }

    bitcoindmgr->startup(); // may throw

    // We defer listening for connections until we hit the "upToDate" state at least once, to prevent problems
    // for clients.
    auto connPtr = std::make_shared<QMetaObject::Connection>();
    *connPtr = connect(this, &Controller::upToDate, this, [this, connPtr] {
        if (connPtr) disconnect(*connPtr);
        if (!srvmgr) {
            if (!origThread) {
                Fatal() << "INTERNAL ERROR: Controller's creation thread is null; cannot start SrvMgr, exiting!";
                return;
            }
            srvmgr = std::make_unique<SrvMgr>(options->interfaces);
            // this object will live on our creation thread (normally the main thread)
            srvmgr->moveToThread(origThread);
            // now, start it up on our creation thread (normally the main thread)
            Util::VoidFuncOnObjectNoThrow(srvmgr.get(), [this]{
                // creation thread (normally the main thread)
                try {
                    srvmgr->startup(); // may throw Exception, waits for servers to bind
                } catch (const Exception & e) {
                    // exit app on bind/listen failure.
                    Fatal() << e.what();
                }
            }); // wait for srvmgr's thread (usually the main thread)
        }
    }, Qt::QueuedConnection);

    start();  // start our thread
}

void Controller::cleanup()
{
    stop();
    tasks.clear(); // deletes all tasks asap
    if (srvmgr) { Log("Stopping SrvMgr ... "); srvmgr->cleanup(); srvmgr.reset(); }
    if (bitcoindmgr) { Log("Stopping BitcoinDMgr ... "); bitcoindmgr->cleanup(); bitcoindmgr.reset(); }
    if (storage) { Log("Closing storage ..."); storage->cleanup(); storage.reset(); }
    sm.reset();
}

/// Encapsulates basically the data returned from bitcoind by the getblockchaininfo RPC method.
/// This has been separated out into its own struct for future use to detect blockchain changes.
/// TODO: Refactor this out to storage, etc to detect when blockchain changed.
struct ChainInfo {
    QString toString() const;

    QString chain = "";
    int blocks = 0, headers = -1;
    QByteArray bestBlockhash; ///< decoded bytes
    double difficulty = 0.0;
    int64_t mtp = 0;
    double verificationProgress = 0.0;
    bool initialBlockDownload = false;
    QByteArray chainWork; ///< decoded bytes
    size_t sizeOnDisk = 0;
    bool pruned = false;
    QString warnings;
};

struct GetChainInfoTask : public CtlTask
{
    GetChainInfoTask(Controller *ctl_) : CtlTask(ctl_, "Task.GetChainInfo") {}
    ~GetChainInfoTask() override { stop(); } // paranoia
    void process() override;

    ChainInfo info;
};

void GetChainInfoTask::process()
{
    submitRequest("getblockchaininfo", {}, [this](const RPC::Message & resp){
        const auto Err = [this, id=resp.id.toInt()](const QString &thing) {
            const auto msg = QString("Failed to parse %1").arg(thing);
            errorCode = id;
            errorMessage = msg;
            throw Exception(msg);
        };
        try {
            bool ok = false;
            const auto map = resp.result().toMap();

            if (map.isEmpty()) Err("response; expected map");

            info.blocks = map.value("blocks").toInt(&ok);
            if (!ok || info.blocks < 0) Err("blocks"); // enforce positive blocks number

            info.chain = map.value("chain").toString();
            if (info.chain.isEmpty()) Err("chain");

            info.headers = map.value("headers").toInt(); // error ignored here

            info.bestBlockhash = Util::ParseHexFast(map.value("bestblockhash").toByteArray());
            if (info.bestBlockhash.size() != bitcoin::uint256::width()) Err("bestblockhash");

            info.difficulty = map.value("difficulty").toDouble(); // error ignored here
            info.mtp = map.value("mediantime").toLongLong(); // error ok
            info.verificationProgress = map.value("verificationprogress").toDouble(); // error ok

            if (auto v = map.value("initialblockdownload"); v.canConvert<bool>())
                info.initialBlockDownload = v.toBool();
            else
                Err("initialblockdownload");

            info.chainWork = Util::ParseHexFast(map.value("chainwork").toByteArray()); // error ok
            info.sizeOnDisk = map.value("size_on_disk").toULongLong(); // error ok
            info.pruned = map.value("pruned").toBool(); // error ok
            info.warnings = map.value("warnings").toString(); // error ok

            if (Trace::isEnabled()) Trace() << info.toString();

            emit success();
        } catch (const Exception & e) {
            Error() << "INTERNAL ERROR: " << e.what();
            emit errored();
        }
    });
}

QString ChainInfo::toString() const
{
    QString ret;
    {
        QTextStream ts(&ret, QIODevice::WriteOnly|QIODevice::Truncate);
        ts << "(ChainInfo"
           << " chain: \"" << chain << "\""
           << " blocks: " << blocks
           << " headers: " << headers
           << " bestBlockHash: " << bestBlockhash.toHex()
           << " difficulty: " << QString::number(difficulty, 'f', 9)
           << " mtp: " << mtp
           << " verificationProgress: " << QString::number(verificationProgress, 'f', 6)
           << " ibd: " << initialBlockDownload
           << " chainWork: " << chainWork.toHex()
           << " sizeOnDisk: " << sizeOnDisk
           << " pruned: " << pruned
           << " warnings: \"" << warnings << "\""
           << ")";
    }
    return ret;
}

struct DownloadBlocksTask : public CtlTask
{
    DownloadBlocksTask(unsigned from, unsigned to, unsigned stride, Controller *ctl);
    ~DownloadBlocksTask() override { stop(); } // paranoia
    void process() override;


    const unsigned from = 0, to = 0, stride = 1, expectedCt = 1;
    unsigned next = 0;
    std::atomic_uint goodCt = 0;
    bool maybeDone = false;

    int q_ct = 0;
    static constexpr int max_q = /*16;*/BitcoinDMgr::N_CLIENTS+1; // todo: tune this

    static const int HEADER_SIZE;

    std::atomic<size_t> nTx = 0, nIns = 0, nOuts = 0;

    void do_get(unsigned height);

    // basically computes expectedCt. Use expectedCt member to get the actual expected ct. this is used only by c'tor as a utility function
    static size_t nToDL(unsigned from, unsigned to, unsigned stride)  { return size_t( (((to-from)+1) + stride-1) / qMax(stride, 1U) ); }
    // thread safe, this is a rough estimate and not 100% accurate
    size_t nSoFar(double prog=-1) const { if (prog<0.) prog = lastProgress; return size_t(qRound(expectedCt * prog)); }
    // given a position in the headers array, return the height
    size_t index2Height(size_t index) { return size_t( from + (index * stride) ); }
    // given a block height, return the index into our array
    size_t height2Index(size_t h) { return size_t( ((h-from) + stride-1) / stride ); }
};


/*static*/ const int DownloadBlocksTask::HEADER_SIZE = int(BTC::GetBlockHeaderSize());

DownloadBlocksTask::DownloadBlocksTask(unsigned from, unsigned to, unsigned stride, Controller *ctl_)
    : CtlTask(ctl_, QString("Task.DL %1 -> %2").arg(from).arg(to)), from(from), to(to), stride(stride), expectedCt(unsigned(nToDL(from, to, stride)))
{
    FatalAssert( (to >= from) && (ctl_) && (stride > 0)) << "Invalid params to DonloadBlocksTask c'tor, FIXME!";

    next = from;
}

void DownloadBlocksTask::process()
{
    if (next > to) {
        if (maybeDone) {
            if (goodCt >= expectedCt)
                emit success();
            else {
                errorCode = int(expectedCt - goodCt);
                errorMessage = QString("missing %1 headers").arg(errorCode);
                emit errored();
            }
        }
        return;
    }

    do_get(next);
    next += stride;
}

void DownloadBlocksTask::do_get(unsigned int bnum)
{
    submitRequest("getblockhash", {bnum}, [this, bnum](const RPC::Message & resp){
        QVariant var = resp.result();
        const auto hash = Util::ParseHexFast(var.toByteArray());
        if (hash.length() == bitcoin::uint256::width()) {
            submitRequest("getblock", {var, false}, [this, bnum, hash](const RPC::Message & resp){
                QVariant var = resp.result();
                const auto rawblock = Util::ParseHexFast(var.toByteArray());
                const auto header = rawblock.left(HEADER_SIZE); // we need a deep copy of this anyway so might as well take it now.
                QByteArray chkHash;
                if (bool sizeOk = header.length() == HEADER_SIZE; sizeOk && (chkHash = BTC::HashRev(header)) == hash) {
                    auto ppb = PreProcessedBlock::makeShared(bnum, size_t(rawblock.size()), BTC::Deserialize<bitcoin::CBlock>(rawblock)); // this is here to test performance
                    // TESTING --
                    //if (bnum == 60000) Debug() << ppb->toDebugString();

                    if (Trace::isEnabled()) Trace() << "block " << bnum << " size: " << rawblock.size() << " nTx: " << ppb->txInfos.size();
                    // update some stats for /stats endpoint
                    nTx += ppb->txInfos.size();
                    nOuts += ppb->outputs.size();
                    nIns += ppb->inputs.size();

                    const size_t index = height2Index(bnum);
                    ++goodCt;
                    q_ct = qMax(q_ct-1, 0);
                    lastProgress = double(index) / double(expectedCt);
                    if (!(bnum % 1000) && bnum) {
                        emit progress(lastProgress);
                    }
                    if (Trace::isEnabled()) Trace() << resp.method << ": header for height: " << bnum << " len: " << header.length();
                    ctl->putBlock(this, ppb); // send the block off to the Controller thread for further processing and for save to db
                    if (goodCt >= expectedCt) {
                        // flag state to maybeDone to do checks when process() called again
                        maybeDone = true;
                        AGAIN();
                        return;
                    }
                    while (goodCt + unsigned(q_ct) < expectedCt && q_ct < max_q) {
                        // queue multiple at once
                        AGAIN();
                        ++q_ct;
                    }
                } else if (!sizeOk) {
                    Warning() << resp.method << ": at height " << bnum << " header not valid (decoded size: " << header.length() << ")";
                    errorCode = int(bnum);
                    errorMessage = QString("bad size for height %1").arg(bnum);
                    emit errored();
                } else {
                    Warning() << resp.method << ": at height " << bnum << " header not valid (expected hash: " << hash.toHex() << ", got hash: " << chkHash.toHex() << ")";
                    errorCode = int(bnum);
                    errorMessage = QString("hash mismatch for height %1").arg(bnum);
                    emit errored();
                }
            });
        } else {
            Warning() << resp.method << ": at height " << bnum << " hash not valid (decoded size: " << hash.length() << ")";
            errorCode = int(bnum);
            errorMessage = QString("invalid hash for height %1").arg(bnum);
            emit errored();
        }
    });
}

struct Controller::StateMachine
{
    enum State {
        Begin=0, GetBlocks, DownloadingBlocks, FinishedDL, End, Failure, IBD
    };
    State state = Begin;
    int ht = -1;

    std::map<unsigned, PreProcessedBlockPtr> ppBlocks; // mapping of height -> PreProcessedBlock
    unsigned ppBlkHtNext = 0,  ///< the next unprocessed block height we need to process in series
             startheight = 0, ///< the height we started at
             endHeight = 0; ///< the final block height we expect to receive to pronounce the synch done

    // todo: tune this
    const size_t DL_CONCURRENCY = qMax(Util::getNPhysicalProcessors()-1, 1U);//size_t(qMin(qMax(int(Util::getNPhysicalProcessors())-BitcoinDMgr::N_CLIENTS, BitcoinDMgr::N_CLIENTS), 32));

    size_t nTx = 0, nIns = 0, nOuts = 0;

    const char * stateStr() const {
        static constexpr const char *stateStrings[] = { "Begin", "GetBlocks", "DownloadingBlocks", "FinishedDL", "End", "Failure", "IBD",
                                                        "Unknown" /* this should always be last */ };
        auto idx = qMin(size_t(state), std::size(stateStrings)-1);
        return stateStrings[idx];
    }
};

void Controller::rmTask(CtlTask *t)
{
    if (auto it = tasks.find(t); it != tasks.end()) {
        tasks.erase(it); // will delete object immediately
        return;
    }
    Error() << __FUNCTION__ << ": Task '" << t->objectName() << "' not found! FIXME!";
}

bool Controller::isTaskDeleted(CtlTask *t) const { return tasks.count(t) == 0; }

void Controller::add_DLHeaderTask(unsigned int from, unsigned int to, size_t nTasks)
{
    DownloadBlocksTask *t = newTask<DownloadBlocksTask>(false, unsigned(from), unsigned(to), unsigned(nTasks), this);
    connect(t, &CtlTask::success, this, [t, this]{
        if (UNLIKELY(!sm || isTaskDeleted(t))) return; // task was stopped from underneath us, this is stale.. abort.
        sm->nTx += t->nTx;
        sm->nIns += t->nIns;
        sm->nOuts += t->nOuts;
        Debug() << "Got all headers from: " << t->objectName() << " headerCt: "  << t->goodCt
                << " nTx,nInp,nOutp: " << t->nTx << "," << t->nIns << "," << t->nOuts << " totals: "
                << sm->nTx << "," << sm->nIns << "," << sm->nOuts;
    });
    connect(t, &CtlTask::errored, this, [t, this]{
        if (UNLIKELY(!sm || isTaskDeleted(t))) return; // task was stopped from underneath us, this is stale.. abort.
        if (sm->state == StateMachine::State::Failure) return; // silently ignore if we are already in failure
        Error() << "Task errored: " << t->objectName() << ", error: " << t->errorMessage;
        genericTaskErrored();
    });
    connect(t, &CtlTask::progress, this, [t, this](double prog){
        if (UNLIKELY(!sm || isTaskDeleted(t))) return; // task was stopped from underneath us, this is stale.. abort.
        Log() << "Downloaded height: " << t->index2Height(unsigned(t->expectedCt*prog)) << ", "
              << QString::number(prog*1e2, 'f', 1) << "%";
    });
}

void Controller::genericTaskErrored()
{
    if (sm && sm->state != StateMachine::State::Failure) {
        sm->state = StateMachine::State::Failure;
        AGAIN();
    }
}

template <typename CtlTaskT, typename ...Args, typename /* enable_if... */>
CtlTaskT *Controller::newTask(bool connectErroredSignal, Args && ...args)
{
    CtlTaskT *task = new CtlTaskT(std::forward<Args>(args)...);
    tasks.emplace(task, task);
    if (connectErroredSignal)
        connect(task, &CtlTask::errored, this, &Controller::genericTaskErrored);
    QTimer::singleShot(0, this, [task, this] {
        if (!isTaskDeleted(task))
            task->start();
    });
    return task;
}

void Controller::process(bool beSilentIfUpToDate)
{
    bool enablePollTimer = false;
    auto polltimeout = polltime_ms;
    stopTimer(pollTimerName);
    //Debug() << "Process called...";
    if (!sm) sm = std::make_unique<StateMachine>();
    using State = StateMachine::State;
    if (sm->state == State::Begin) {
        auto task = newTask<GetChainInfoTask>(true, this);
        connect(task, &CtlTask::success, this, [this, task, beSilentIfUpToDate]{
            if (UNLIKELY(!sm || isTaskDeleted(task))) return; // task was stopped from underneath us, this is stale.. abort.
            if (task->info.initialBlockDownload) {
                sm->state = State::IBD;
                AGAIN();
                return;
            }
            if (const auto dbchain = storage->getChain(); dbchain.isEmpty() && !task->info.chain.isEmpty()) {
                storage->setChain(task->info.chain);
            } else if (dbchain != task->info.chain) {
                Fatal() << "Bitcoind reports chain: \"" << task->info.chain << "\", which differs from our database: \""
                        << dbchain << "\". You may have connected to the wrong bitcoind. To fix this issue either "
                        << "connect to a different bitcoind or delete this program's datadir to resynch.";
                return;
            }
            // TODO: detect reorgs here -- to be implemented later after we figure out data model more, etc.
            const auto old = int(storage->headers().first.size())-1;
            sm->ht = task->info.blocks;
            if (old == sm->ht) {
                if (!beSilentIfUpToDate) {
                    Log() << "Block height " << sm->ht << ", up-to-date";
                    emit upToDate();
                }
                sm->state = State::End;
            } else if (old > sm->ht) {
                Fatal() << "We have height " << old << ", but bitcoind reports height " << sm->ht << ". "
                        << "Possible reasons: A massive reorg, your node is acting funny, you are on the wrong chain "
                        << "(testnet vs mainnet), or there is a bug in this program. Cowardly giving up and exiting...";
                return;
            } else {
                Log() << "Block height " << sm->ht << ", downloading new headers ...";
                emit synchronizing();
                sm->state = State::GetBlocks;
            }
            AGAIN();
        });
    } else if (sm->state == State::GetBlocks) {
        FatalAssert(sm->ht >= 0) << "Inconsistent state -- sm->ht cannot be negative in State::GetBlocks! FIXME!"; // paranoia
        const size_t base = storage->headers().first.size();
        const size_t num = size_t(sm->ht+1) - base;
        FatalAssert(num > 0) << "Cannot download 0 blocks! FIXME!"; // more paranoia
        const size_t nTasks = qMin(num, sm->DL_CONCURRENCY);
        sm->ppBlkHtNext = sm->startheight = unsigned(base);
        sm->endHeight = unsigned(base + num) - 1;
        for (size_t i = 0; i < nTasks; ++i) {
            add_DLHeaderTask(unsigned(base + i), unsigned(sm->ht), nTasks);
        }
        sm->state = State::DownloadingBlocks; // advance state now. we will be called back by download task in putBlock()
    } else if (sm->state == State::DownloadingBlocks) {
        process_DownloadingBlocks();
    } else if (sm->state == State::FinishedDL) {
        size_t N = sm->endHeight - sm->startheight;
        Log() << "Processed " << N << " new " << Util::Pluralize("block", N) << " with " << sm->nTx << " " << Util::Pluralize("tx", sm->nTx)
              << " (" << sm->nIns << " " << Util::Pluralize("input", sm->nIns) << " & " << sm->nOuts << " " << Util::Pluralize("output", sm->nOuts) << ")"
              << ", verified ok.";
        sm.reset(); // go back to "Begin" state to check if any new headers arrived in the meantime
        AGAIN();
        storage->save(Storage::SaveItem::Hdrs); // enqueue a header commit to db ...
    } else if (sm->state == State::Failure) {
        // We will try again later via the pollTimer
        Error() << "Failed to download headers";
        sm.reset();
        enablePollTimer = true;
        emit synchFailure();
    } else if (sm->state == State::End) {
        sm.reset();  // great success!
        enablePollTimer = true;
    } else if (sm->state == State::IBD) {
        sm.reset();
        enablePollTimer = true;
        Warning() << "bitcoind is in initial block download, will try again in 1 minute";
        polltimeout = 60 * 1000; // try again every minute
        emit synchFailure();
    }

    if (enablePollTimer)
        callOnTimerSoonNoRepeat(polltimeout, pollTimerName, [this]{if (!sm) process(true);});
}

void Controller::putBlock(CtlTask *task, PreProcessedBlockPtr p)
{
    // returns right away
    Util::AsyncOnObject(this, [this, task, p] {
        if (!sm || isTaskDeleted(task) || sm->state == StateMachine::State::Failure) {
            Debug() << "Ignoring block " << p->height << " for now-defunct task";
            return;
        } else if (sm->state != StateMachine::State::DownloadingBlocks) {
            Warning() << "Ignoring putBlocks request for block " << p->height << " -- state is not \"DownloadingBlocks\" but rather is: \"" << sm->stateStr() << "\"";
            return;
        }
        sm->ppBlocks[p->height] = p;
        AGAIN(); // queue up, return right away
    });
}

void Controller::process_DownloadingBlocks()
{
    unsigned ct = 0;
    for (auto it = sm->ppBlocks.begin(); it != sm->ppBlocks.end() && it->first == sm->ppBlkHtNext; it = sm->ppBlocks.begin()) {
        auto ppb = it->second;
        FatalAssert(ppb->height == sm->ppBlkHtNext) << "INTERNAL ERROR: Retrieved block has the wrong height! FIXME!"; // paranoia
        ++ct;
        ++sm->ppBlkHtNext;

        sm->ppBlocks.erase(it); // remove immediately from q

        // process & add it if it's good
        if ( ! process_VerifyAndAddBlock(ppb) )
            // error encountered.. abort!
            return;
    }

    // testing debug
    //if (auto backlog = sm->ppBlocks.size(); backlog < 100 || ct > 100) {
    //    Debug() << "ppblk - processed: " << ct << ", backlog: " << backlog;
    //}

    if (sm->ppBlkHtNext >= sm->endHeight) {
        sm->state = StateMachine::State::FinishedDL;
        AGAIN();
    }
}

bool Controller::process_VerifyAndAddBlock(PreProcessedBlockPtr ppb)
{
    assert(sm);
    // Verify header chain makes sense (by checking hashes, using the shared header verifier)
    QByteArray rawHeader;
    {
        auto [verif, lock] = storage->headerVerifier(); // lock needs to be held while we use this shared verifier
        const auto verifUndo = verif; // keep a copy for undo purposes in case this fails

        if (QString verifErr; !verif(ppb->header, &verifErr) ) {
            // XXX possible reorg point. FIXME TODO
            // reorg here? TODO: deal with this better.
            Error() << verifErr;
            sm->state = StateMachine::State::Failure;
            verif = verifUndo; // undo header verifier state
            AGAIN();
            return false;
        }
        // save raw header back to our buffer
        rawHeader = verif.lastHeaderProcessed().second;
    } // end lock context

    FatalAssert(rawHeader.size() == int(BTC::GetBlockHeaderSize())) << "INTERNAL ERROR: raw header has the wrong size!";

    const auto nLeft = qMax(sm->endHeight - (sm->ppBlkHtNext-1), 0U);
    {
        // updated shared headers from storage while holding lock...
        auto [headers, lock] = storage->mutableHeaders(); // write lock held until scope end
        if (const auto size = headers.size(); size + nLeft < headers.capacity())
            headers.reserve(size + nLeft); // reserve space for new headers in 1 go to save on copying
        headers.insert(headers.end(), rawHeader);

    } // end lock context

    // TESTING save every 10000 headers to db -- TODO: tune this or have this be configurable?
    if (!(nLeft % 10000) && nLeft)
        storage->save(Storage::SaveItem::Hdrs);

    return true;
}

// -- CtlTask
CtlTask::CtlTask(Controller *ctl, const QString &name)
    : QObject(nullptr), ctl(ctl)
{
    setObjectName(name);
    _thread.setObjectName(name);
}

CtlTask::~CtlTask() {
    Debug("%s (%s)", __FUNCTION__, objectName().isEmpty() ? "" : objectName().toUtf8().constData());
    stop();
}

void CtlTask::on_started()
{
    ThreadObjectMixin::on_started();
    conns += connect(this, &CtlTask::success, ctl, [this]{stop();});
    conns += connect(this, &CtlTask::errored, ctl, [this]{stop();});
    conns += connect(this, &CtlTask::finished, ctl, [this]{ctl->rmTask(this);});
    process();
    emit started();
}

void CtlTask::on_finished()
{
    ThreadObjectMixin::on_finished();
    emit finished();
}

void CtlTask::on_error(const RPC::Message &resp)
{
    Warning() << resp.method << ": error response: " << resp.toJsonString();
    errorCode = resp.errorCode();
    errorMessage = resp.errorMessage();
    emit errored();
}
void CtlTask::on_failure(const RPC::Message::Id &id, const QString &msg)
{
    Warning() << id.toString() << ": FAIL: " << msg;
    errorCode = id.toInt();
    errorMessage = msg;
    emit errored();
}
quint64 CtlTask::submitRequest(const QString &method, const QVariantList &params, const BitcoinDMgr::ResultsF &resultsFunc)
{
    quint64 id = IdMixin::newId();
    ctl->bitcoindmgr->submitRequest(this, id, method, params,
                                    resultsFunc,
                                    [this](const RPC::Message &r){on_error(r);},
                                    [this](const RPC::Message::Id &id, const QString &msg){on_failure(id, msg);});
    return id;
}



// --- Controller stats
auto Controller::stats() const -> Stats
{
    // "Servers"
    auto st = QVariantMap{{ "Servers", srvmgr ? srvmgr->statsSafe() : QVariant() }};

    // "BitcoinD's"
    st["Bitcoin Daemon"] = bitcoindmgr->statsSafe();

    // "Controller" (self)
    QVariantMap m;
    m["Headers"] = int(storage->headers().first.size());
    if (sm) {
        QVariantMap m2;
        m2["State"] = sm->stateStr();
        m2["Height"] = sm->ht;
        if (const auto nDL = nHeadersDownloadedSoFar(); nDL > 0)
            m2["Headers_Downloaded_This_Run"] = qlonglong(nDL);
        if (const auto [ntx, nin, nout] = nTxInOutSoFar(); ntx > 0) {
            m2["Txs_Seen_This_Run"] = QVariantMap({
                { "nTx" , qlonglong(ntx) },
                { "nIns", qlonglong(nin) },
                { "nOut", qlonglong(nout) }
            });
        }
        const size_t backlogBlocks = sm->ppBlocks.size();
        m2["BackLog_Blocks"] = qulonglong(backlogBlocks);
        if (backlogBlocks) {
            size_t backlogBytes = 0, backlogTxs = 0, backlogInMemoryBytes = 0;
            for (const auto & [height, ppb] : sm->ppBlocks) {
                backlogBytes += ppb->sizeBytes;
                backlogTxs += ppb->txInfos.size();
                backlogInMemoryBytes += ppb->estimatedThisSizeBytes;
            }
            m2["BackLog_RawBlocksDataSize"] = QString("%1 MiB").arg(QString::number(double(backlogBytes) / 1e6, 'f', 3));
            m2["BackLog_InMemoryDataSize"] = QString("%1 MiB").arg(QString::number(double(backlogInMemoryBytes) / 1e6, 'f', 3));
            m2["BackLog_Txs"] = qulonglong(backlogTxs);
        }
        m["StateMachine"] = m2;
    } else
        m["StateMachine"] = QVariant(); // null
    QVariantMap timerMap;
    for (const auto & timer: _timerMap) {
        timerMap.insert(timer->objectName(), timer->interval());
    }
    m["activeTimers"] = timerMap;
    QVariantList l;
    { // task list
        const auto now = Util::getTime();
        for (const auto & [task, ign] : tasks) {
            Q_UNUSED(ign)
            l.push_back(QVariantMap{{ task->objectName(), QVariantMap{
                {"age", QString("%1 sec").arg(double((now-task->ts)/1e3))} ,
                {"progress" , QString("%1%").arg(QString::number(task->lastProgress*100.0, 'f', 1)) }}
            }});
        }
        Util::updateMap(m, QVariantMap{{"tasks" , l}});
    }
    st["Controller"] = m;
    return st;
}

size_t Controller::nHeadersDownloadedSoFar() const
{
    size_t ret = 0;
    for (const auto & [task, ign] : tasks) {
        Q_UNUSED(ign)
        auto t = dynamic_cast<DownloadBlocksTask *>(task);
        if (t)
            ret += t->nSoFar();
    }
    return ret;
}

std::tuple<size_t, size_t, size_t> Controller::nTxInOutSoFar() const
{
    size_t nTx = 0, nIn = 0, nOut = 0;
    for (const auto & [task, ign] : tasks) {
        Q_UNUSED(ign)
        auto t = dynamic_cast<DownloadBlocksTask *>(task);
        if (t) {
            nTx += t->nTx;
            nIn += t->nIns;
            nOut += t->nOuts;
        }
    }
    return {nTx, nIn, nOut};
}
