/**

	part 1
	Policy and Consensus Script

**/
bool MemPoolAccept::PolicyScriptChecks(ATMPArgs& args, Workspace& ws, PrecomputedTransactionData& txdata)
{
    const CTransaction& tx = *ws.m_ptx;

    TxValidationState &state = args.m_state;

    constexpr unsigned int scriptVerifyFlags = STANDARD_SCRIPT_VERIFY_FLAGS;

    // Check input scripts and signatures.
    // This is done last to help prevent CPU exhaustion denial-of-service attacks.
    if (!CheckInputScripts(tx, state, m_view, scriptVerifyFlags, true, false, txdata)) {
        // SCRIPT_VERIFY_CLEANSTACK requires SCRIPT_VERIFY_WITNESS, so we
        // need to turn both off, and compare against just turning off CLEANSTACK
        // to see if the failure is specifically due to witness validation.
        TxValidationState state_dummy; // Want reported failures to be from first CheckInputScripts
        if (!tx.HasWitness() && CheckInputScripts(tx, state_dummy, m_view, scriptVerifyFlags & ~(SCRIPT_VERIFY_WITNESS | SCRIPT_VERIFY_CLEANSTACK), true, false, txdata) &&
                !CheckInputScripts(tx, state_dummy, m_view, scriptVerifyFlags & ~SCRIPT_VERIFY_CLEANSTACK, true, false, txdata)) {
            // Only the witness is missing, so the transaction itself may be fine.
            state.Invalid(TxValidationResult::TX_WITNESS_MUTATED,
                    state.GetRejectReason(), state.GetDebugMessage());
        }
        return false; // state filled in by CheckInputScripts
    }

    return true;
}

bool MemPoolAccept::ConsensusScriptChecks(ATMPArgs& args, Workspace& ws, PrecomputedTransactionData& txdata)
{
    const CTransaction& tx = *ws.m_ptx;
    const uint256& hash = ws.m_hash;

    TxValidationState &state = args.m_state;
    const CChainParams& chainparams = args.m_chainparams;

    // Check again against the current block tip's script verification
    // flags to cache our script execution flags. This is, of course,
    // useless if the next block has different script flags from the
    // previous one, but because the cache tracks script flags for us it
    // will auto-invalidate and we'll just have a few blocks of extra
    // misses on soft-fork activation.
    //
    // This is also useful in case of bugs in the standard flags that cause
    // transactions to pass as valid when they're actually invalid. For
    // instance the STRICTENC flag was incorrectly allowing certain
    // CHECKSIG NOT scripts to pass, even though they were invalid.
    //
    // There is a similar check in CreateNewBlock() to prevent creating
    // invalid blocks (using TestBlockValidity), however allowing such
    // transactions into the mempool can be exploited as a DoS attack.
    unsigned int currentBlockScriptVerifyFlags = GetBlockScriptFlags(::ChainActive().Tip(), chainparams.GetConsensus());
    if (!CheckInputsFromMempoolAndCache(tx, state, m_view, m_pool, currentBlockScriptVerifyFlags, txdata)) {
        return error("%s: BUG! PLEASE REPORT THIS! CheckInputScripts failed against latest-block but not STANDARD flags %s, %s",
                __func__, hash.ToString(), FormatStateMessage(state));
    }
    // SYSCOIN
    if (IsSyscoinTx(tx.nVersion) && !CheckSyscoinInputs(tx, hash, state, m_view, true, ::ChainActive().Height(), ::ChainActive().Tip()->GetMedianTimePast(), args.m_test_accept || args.m_bypass_limits)) {
        // mark to remove from mempool, because if we remove right away then the transaction data cannot be relayed most of the time
        if(!args.m_test_accept && state.IsError()){
            LogPrint(BCLog::SYS, "Double spend detected on tx %s! %s\n", hash.GetHex(), FormatStateMessage(state));
            LOCK(cs_assetallocationmempoolremovetx);
            vecToRemoveFromMempool.emplace_back(hash, ::ChainActive().Tip()->GetMedianTimePast());
        }
        else
            return false;
    }else if(args.m_duplicate){
        return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "txn-mempool-conflict");
    }
    return true;
}



/**

	part 2
	Accept Transaction in MemPool 

**/

bool MemPoolAccept::AcceptSingleTransaction(const CTransactionRef& ptx, ATMPArgs& args)
{
    AssertLockHeld(cs_main);
	
    LOCK(m_pool.cs); // mempool "read lock" (held through GetMainSignals().TransactionAddedToMempool())

    Workspace workspace(ptx);
    if (!PreChecks(args, workspace)) return false;
    // Only compute the precomputed transaction data if we need to verify
    // scripts (ie, other policy checks pass). We perform the inexpensive
    // checks first and avoid hashing and signature verification unless those
    // checks pass, to mitigate CPU exhaustion denial-of-service attacks.
    PrecomputedTransactionData txdata(*ptx);
	// Because script checks take little time, we focus on the PreChecks
	// Check the condition of the mempool, if it is resource conflicted, unlock the mempool
	bool isConflicted = detectMempool(m_pool);
	if(isConflicted) UNLOCK(m_pool.cs);
    if (!PolicyScriptChecks(args, workspace, txdata)) return false;
    if (!ConsensusScriptChecks(args, workspace, txdata)) return false;
    // Tx was accepted, but not added
    if (args.m_test_accept) return true;
    if (!Finalize(args, workspace)) return false;
    GetMainSignals().TransactionAddedToMempool(ptx);
    return true;
}

} // anon namespace

/** (try to) add transaction to memory pool with a specified acceptance time **/
static bool AcceptToMemoryPoolWithTime(const CChainParams& chainparams, CTxMemPool& pool, TxValidationState &state, const CTransactionRef &tx,
                        int64_t nAcceptTime, std::list<CTransactionRef>* plTxnReplaced,
                        bool bypass_limits, const CAmount nAbsurdFee, bool test_accept) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    std::vector<COutPoint> coins_to_uncache;
    // SYSCOIN
    bool bDuplicate = false;
    MemPoolAccept::ATMPArgs args { chainparams, state, nAcceptTime, plTxnReplaced, bypass_limits, nAbsurdFee, coins_to_uncache, test_accept, bDuplicate };
	
	// If taskNumber is within a specified range and acceptTime is runnable, we perform accept single transaction
	int taskNumber = pool.size;
	int schedulableNumber = isScheduable(pool);
	int64_t runnableTime = isRunnable(pool,state);
	bool res = false;
	if( taskNumber < schedulableNumber && nAcceptTime < runnableTime )
		res = MemPoolAccept(pool).AcceptSingleTransaction(tx, args);
    if (!res) {
        // Remove coins that were not present in the coins cache before calling ATMPW;
        // this is to prevent memory DoS in case we receive a large number of
        // invalid transactions that attempt to overrun the in-memory coins cache
        // (`CCoinsViewCache::cacheCoins`).

        for (const COutPoint& hashTx : coins_to_uncache)
            ::ChainstateActive().CoinsTip().Uncache(hashTx);
    }
    // After we've (potentially) uncached entries, ensure our coins cache is still within its size limits
    BlockValidationState state_dummy;
    ::ChainstateActive().FlushStateToDisk(chainparams, state_dummy, FlushStateMode::PERIODIC);
    return res;
}

bool AcceptToMemoryPool(CTxMemPool& pool, TxValidationState &state, const CTransactionRef &tx,
                        std::list<CTransactionRef>* plTxnReplaced,
                        bool bypass_limits, const CAmount nAbsurdFee, bool test_accept)
{
    const CChainParams& chainparams = Params();
    return AcceptToMemoryPoolWithTime(chainparams, pool, state, tx, GetTime(), plTxnReplaced, bypass_limits, nAbsurdFee, test_accept);
}



/**

	part 3
	main class

**/
namespace {

class MemPoolAccept
{
public:
    // SYSCOIN rearrange m_view and m_pool
    MemPoolAccept(CTxMemPool& mempool) : m_view(&m_dummy), m_pool(mempool), m_viewmempool(&::ChainstateActive().CoinsTip(), m_pool),
        m_limit_ancestors(gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT)),
        m_limit_ancestor_size(gArgs.GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT)*1000),
        m_limit_descendants(gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT)),
        m_limit_descendant_size(gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT)*1000) {}

    // We put the arguments we're handed into a struct, so we can pass them
    // around easier.
    struct ATMPArgs {
        const CChainParams& m_chainparams;
        TxValidationState &m_state;
        const int64_t m_accept_time;
        std::list<CTransactionRef>* m_replaced_transactions;
        const bool m_bypass_limits;
        const CAmount& m_absurd_fee;
        /*
         * Return any outpoints which were not previously present in the coins
         * cache, but were added as a result of validating the tx for mempool
         * acceptance. This allows the caller to optionally remove the cache
         * additions if the associated transaction ends up being rejected by
         * the mempool.
         */
        std::vector<COutPoint>& m_coins_to_uncache;
        const bool m_test_accept;
        // SYSCOIN
        bool &m_duplicate;
    };
    
    // Single transaction acceptance
    bool AcceptSingleTransaction(const CTransactionRef& ptx, ATMPArgs& args) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    // SYSCOIN
    CCoinsViewCache m_view;
private:
    // All the intermediate state that gets passed between the various levels
    // of checking a given transaction.
    struct Workspace {
        Workspace(const CTransactionRef& ptx) : m_ptx(ptx), m_hash(ptx->GetHash()) {}
        std::set<uint256> m_conflicts;
        CTxMemPool::setEntries m_all_conflicting;
        CTxMemPool::setEntries m_ancestors;
        std::unique_ptr<CTxMemPoolEntry> m_entry;

        bool m_replacement_transaction;
        CAmount m_modified_fees;
        CAmount m_conflicting_fees;
        size_t m_conflicting_size;

        const CTransactionRef& m_ptx;
        const uint256& m_hash;
    };

    // Run the policy checks on a given transaction, excluding any script checks.
    // Looks up inputs, calculates feerate, considers replacement, evaluates
    // package limits, etc. As this function can be invoked for "free" by a peer,
    // only tests that are fast should be done here (to avoid CPU DoS).
    bool PreChecks(ATMPArgs& args, Workspace& ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Run the script checks using our policy flags. As this can be slow, we should
    // only invoke this on transactions that have otherwise passed policy checks.
    bool PolicyScriptChecks(ATMPArgs& args, Workspace& ws, PrecomputedTransactionData& txdata) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    // Re-run the script checks, using consensus flags, and try to cache the
    // result in the scriptcache. This should be done after
    // PolicyScriptChecks(). This requires that all inputs either be in our
    // utxo set or in the mempool.
    bool ConsensusScriptChecks(ATMPArgs& args, Workspace& ws, PrecomputedTransactionData &txdata) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    // Try to add the transaction to the mempool, removing any conflicts first.
    // Returns true if the transaction is in the mempool after any size
    // limiting is performed, false otherwise.
    bool Finalize(ATMPArgs& args, Workspace& ws) EXCLUSIVE_LOCKS_REQUIRED(cs_main, m_pool.cs);

    // Compare a package's feerate against minimum allowed.
    // SYSCOIN
    bool CheckFeeRate(size_t package_size, CAmount package_fee, TxValidationState& state, const bool& IsAssetAllocation)
    {
        CAmount mempoolRejectFee = m_pool.GetMinFee(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFee(package_size);
        if (mempoolRejectFee > 0 && package_fee < mempoolRejectFee) {
            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "mempool min fee not met", strprintf("%d < %d", package_fee, mempoolRejectFee));
        }
        // SYSCOIN
        if (package_fee < ::minRelayTxFee.GetFee(IsAssetAllocation? package_size*2: package_size)) {
            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "min relay fee not met", strprintf("%d < %d", package_fee, ::minRelayTxFee.GetFee(IsAssetAllocation? package_size*2: package_size)));
        }
        return true;
    }

private:
    CTxMemPool& m_pool;
    CCoinsViewMemPool m_viewmempool;
    CCoinsView m_dummy;

    // The package limits in effect at the time of invocation.
    const size_t m_limit_ancestors;
    const size_t m_limit_ancestor_size;
    // These may be modified while evaluating a transaction (eg to account for
    // in-mempool conflicts; see below).
    size_t m_limit_descendants;
    size_t m_limit_descendant_size;
};



/**

	part 4
	Mempool update

**/
static void LimitMempoolSize(CTxMemPool& pool, size_t limit, std::chrono::seconds age)
    EXCLUSIVE_LOCKS_REQUIRED(pool.cs, ::cs_main)
{
    int expired = pool.Expire(GetTime<std::chrono::seconds>() - age);
    if (expired != 0) {
        LogPrint(BCLog::MEMPOOL, "Expired %i transactions from the memory pool\n", expired);
    }

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    for (const COutPoint& removed : vNoSpendsRemaining)
        ::ChainstateActive().CoinsTip().Uncache(removed);
}

static bool IsCurrentForFeeEstimation() EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    AssertLockHeld(cs_main);
    if (::ChainstateActive().IsInitialBlockDownload())
        return false;
    if (::ChainActive().Tip()->GetBlockTime() < (GetTime() - MAX_FEE_ESTIMATION_TIP_AGE))
        return false;
    if (::ChainActive().Height() < pindexBestHeader->nHeight - 1)
        return false;
    return true;
}

/* Make mempool consistent after a reorg, by re-adding or recursively erasing
 * disconnected block transactions from the mempool, and also removing any
 * other transactions from the mempool that are no longer valid given the new
 * tip/height.
 *
 * Note: we assume that disconnectpool only contains transactions that are NOT
 * confirmed in the current chain nor already in the mempool (otherwise,
 * in-mempool descendants of such transactions would be removed).
 *
 * Passing fAddToMempool=false will skip trying to add the transactions back,
 * and instead just erase from the mempool as needed.
 */

static void UpdateMempoolForReorg(DisconnectedBlockTransactions& disconnectpool, bool fAddToMempool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, ::mempool.cs)
{
    AssertLockHeld(cs_main);
    std::vector<uint256> vHashUpdate;
    // disconnectpool's insertion_order index sorts the entries from
    // oldest to newest, but the oldest entry will be the last tx from the
    // latest mined block that was disconnected.
    // Iterate disconnectpool in reverse, so that we add transactions
    // back to the mempool starting with the earliest transaction that had
    // been previously seen in a block.
    auto it = disconnectpool.queuedTx.get<insertion_order>().rbegin();
    while (it != disconnectpool.queuedTx.get<insertion_order>().rend()) {
        // ignore validation errors in resurrected transactions
        TxValidationState stateDummy;
        if (!fAddToMempool || (*it)->IsCoinBase() ||
            !AcceptToMemoryPool(mempool, stateDummy, *it,
                                nullptr /* plTxnReplaced */, true /* bypass_limits */, 0 /* nAbsurdFee */)) {
            // If the transaction doesn't make it in to the mempool, remove any
            // transactions that depend on it (which would now be orphans).
            mempool.removeRecursive(**it, MemPoolRemovalReason::REORG);
        } else if (mempool.exists((*it)->GetHash())) {
            vHashUpdate.push_back((*it)->GetHash());
        }
        ++it;
    }
    disconnectpool.queuedTx.clear();
    // AcceptToMemoryPool/addUnchecked all assume that new mempool entries have
    // no in-mempool children, which is generally not true when adding
    // previously-confirmed transactions back to the mempool.
    // UpdateTransactionsFromBlock finds descendants of any transactions in
    // the disconnectpool that were added back and cleans up the mempool state.
    mempool.UpdateTransactionsFromBlock(vHashUpdate);

    // We also need to remove any now-immature transactions
    mempool.removeForReorg(&::ChainstateActive().CoinsTip(), ::ChainActive().Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    // Re-limit mempool size, in case we added any transactions
    LimitMempoolSize(mempool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, std::chrono::hours{gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY)});
}



/**

	part 5
	Pre check and final reception

**/
bool MemPoolAccept::PreChecks(ATMPArgs& args, Workspace& ws)
{
    const CTransactionRef& ptx = ws.m_ptx;
    const CTransaction& tx = *ws.m_ptx;
    const uint256& hash = ws.m_hash;

    // Copy/alias what we need out of args
    TxValidationState &state = args.m_state;
    const int64_t nAcceptTime = args.m_accept_time;
    const bool bypass_limits = args.m_bypass_limits;
    const CAmount& nAbsurdFee = args.m_absurd_fee;
    std::vector<COutPoint>& coins_to_uncache = args.m_coins_to_uncache;

    // Alias what we need out of ws
    std::set<uint256>& setConflicts = ws.m_conflicts;
    CTxMemPool::setEntries& allConflicting = ws.m_all_conflicting;
    CTxMemPool::setEntries& setAncestors = ws.m_ancestors;
    std::unique_ptr<CTxMemPoolEntry>& entry = ws.m_entry;
    bool& fReplacementTransaction = ws.m_replacement_transaction;
    CAmount& nModifiedFees = ws.m_modified_fees;
    CAmount& nConflictingFees = ws.m_conflicting_fees;
    size_t& nConflictingSize = ws.m_conflicting_size;

    if (nTPSTestingStartTime > 0)
    {
        const int64_t &currentTime = GetTimeMicros();
        if(currentTime >= nTPSTestingStartTime)
            vecTPSTestReceivedTimesMempool.emplace_back(hash, currentTime);
    }
    if (!CheckTransaction(tx, state))
        return false; // state filled in by CheckTransaction
        
    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "coinbase");

    // Rather not work on nonstandard transactions (unless -testnet/-regtest)
    std::string reason;
    if (fRequireStandard && !IsStandardTx(tx, reason))
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD, reason);

    // Do not work on transactions that are too small.
    // A transaction with 1 segwit input and 1 P2WPHK output has non-witness size of 82 bytes.
    // Transactions smaller than this are not relayed to mitigate CVE-2017-12842 by not relaying
    // 64-byte transactions.
    if (::GetSerializeSize(tx, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS) < MIN_STANDARD_TX_NONWITNESS_SIZE)
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD, "tx-size-small");

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
        return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "non-final");

    // is it already in the memory pool?
    if (m_pool.exists(hash)) {
        return state.Invalid(TxValidationResult::TX_CONFLICT, "txn-already-in-mempool");
    }
    
    // SYSCOIN
    bool bDuplicate = false;
	int tolerance = 0;
	queue doublespendingQueue;
    // Check for conflicts with in-memory transactions
    const bool& IsAssetAllocation = IsAssetAllocationTx(tx.nVersion);
    for (const CTxIn &txin : tx.vin)
    {
        const CTransaction* ptxConflicting = m_pool.GetConflictTx(txin.prevout);
        if (ptxConflicting) {
            if (!setConflicts.count(ptxConflicting->GetHash()))
            {
                // Allow opt-out of transaction replacement by setting
                // nSequence > MAX_BIP125_RBF_SEQUENCE (SEQUENCE_FINAL-2) on all inputs.
                //
                // SEQUENCE_FINAL-1 is picked to still allow use of nLockTime by
                // non-replaceable transactions. All inputs rather than just one
                // is for the sake of multi-party protocols, where we don't
                // want a single party to be able to disable replacement.
                //
                // The opt-out ignores descendants as anyone relying on
                // first-seen mempool behavior should be checking all
                // unconfirmed ancestors anyway; doing otherwise is hopelessly
                // insecure.
                bool fReplacementOptOut = true;
                
                for (const CTxIn &_txin : ptxConflicting->vin)
                {
                    if (_txin.nSequence <= MAX_BIP125_RBF_SEQUENCE)
                    {
                        fReplacementOptOut = false;
                        break;
                    }
                }
                
                if (fReplacementOptOut) {
                    if(!args.m_test_accept && IsAssetAllocation){
                        CAssetAllocation theAssetAlloction(tx);
                        if(theAssetAlloction.assetAllocationTuple.IsNull()){
                            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "txn-mempool-conflict");
                        }
                        ActorSet actorSet;
                        GetActorsFromAssetAllocationTx(theAssetAlloction, tx.nVersion, true, false, actorSet);
                        if(actorSet.size() == 1)
                        {
                            LOCK(cs_assetallocationconflicts);
							if(assetAllocationConflicts.find(*actorSet.begin()) == assetAllocationConflicts.end())
                            {
                                // Add conflicting sender, control the number of doublespendings
                                args.m_duplicate = true;
								doublespendingQueue.add(txin);
								tolerance++;
								if(tolerance>MAX_DOUBLE_SPENDING_LIMITATION)
									break;
                            }
                            else
                                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "txn-mempool-conflict");
                        }
                        else
                            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "txn-mempool-conflict");
                    }
                    else
                        return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "txn-mempool-conflict");
                }
                if(!bDuplicate){
					// Detect fake conflicts: two possible secure hash functions 
					// 1. SHA-256: A family of two similar hash functions, with different block sizes.
					// 2. Collision Resistant: It should be hard to find two messages with the same hash value
					if(isValidConflict(ptxConflicting->GetHash())){
						setConflicts.insert(ptxConflicting->GetHash());
					}
				}
            }
        }
    }

    LockPoints lp;
    m_view.SetBackend(m_viewmempool);

    CCoinsViewCache& coins_cache = ::ChainstateActive().CoinsTip();
    // do all inputs exist?
    for (const CTxIn& txin : tx.vin) {
        if (!coins_cache.HaveCoinInCache(txin.prevout)) {
            coins_to_uncache.push_back(txin.prevout);
        }

        // Note: this call may add txin.prevout to the coins cache
        // (coins_cache.cacheCoins) by way of FetchCoin(). It should be removed
        // later (via coins_to_uncache) if this tx turns out to be invalid.
        if (!m_view.HaveCoin(txin.prevout)) {
            // Are inputs missing because we already have the tx?
            for (size_t out = 0; out < tx.vout.size(); out++) {
                // Optimistically just do efficient check of cache for outputs
                if (coins_cache.HaveCoinInCache(COutPoint(hash, out))) {
                    return state.Invalid(TxValidationResult::TX_CONFLICT, "txn-already-known");
                }
            }
            // Otherwise assume this might be an orphan tx for which we just haven't seen parents yet
            return state.Invalid(TxValidationResult::TX_MISSING_INPUTS, "bad-txns-inputs-missingorspent");
        }
    }

    // Bring the best block into scope
    m_view.GetBestBlock();

    // we have all inputs cached now, so switch back to dummy (to protect
    // against bugs where we pull more inputs from disk that miss being added
    // to coins_to_uncache)
    m_view.SetBackend(m_dummy);

    // Only accept BIP68 sequence locked transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    // Must keep pool.cs for this unless we change CheckSequenceLocks to take a
    // CoinsViewCache instead of create its own
    if (!CheckSequenceLocks(m_pool, tx, STANDARD_LOCKTIME_VERIFY_FLAGS, &lp))
        return state.Invalid(TxValidationResult::TX_PREMATURE_SPEND, "non-BIP68-final");

    CAmount nFees = 0;
    if (!Consensus::CheckTxInputs(tx, state, m_view, GetSpendHeight(m_view), nFees)) {
        return error("%s: Consensus::CheckTxInputs: %s, %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));
    }

    // Check for non-standard pay-to-script-hash in inputs
    if (fRequireStandard && !AreInputsStandard(tx, m_view))
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD, "bad-txns-nonstandard-inputs");

    // Check for non-standard witness in P2WSH
    if (tx.HasWitness() && fRequireStandard && !IsWitnessStandard(tx, m_view))
        return state.Invalid(TxValidationResult::TX_WITNESS_MUTATED, "bad-witness-nonstandard");

    int64_t nSigOpsCost = GetTransactionSigOpCost(tx, m_view, STANDARD_SCRIPT_VERIFY_FLAGS);

    // nModifiedFees includes any fee deltas from PrioritiseTransaction
    nModifiedFees = nFees;
    m_pool.ApplyDelta(hash, nModifiedFees);

    // Keep track of transactions that spend a coinbase, which we re-scan
    // during reorgs to ensure COINBASE_MATURITY is still met.
    bool fSpendsCoinbase = false;
    for (const CTxIn &txin : tx.vin) {
        const Coin &coin = m_view.AccessCoin(txin.prevout);
        if (coin.IsCoinBase()) {
            fSpendsCoinbase = true;
            break;
        }
    }

    entry.reset(new CTxMemPoolEntry(ptx, nFees, nAcceptTime, ::ChainActive().Height(),
            fSpendsCoinbase, nSigOpsCost, lp));
    unsigned int nSize = entry->GetTxSize();

    if (nSigOpsCost > MAX_STANDARD_TX_SIGOPS_COST)
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD, "bad-txns-too-many-sigops",
                strprintf("%d", nSigOpsCost));

    // No transactions are allowed below minRelayTxFee except from disconnected
    // blocks
    if (!bypass_limits && !CheckFeeRate(nSize, nModifiedFees, state, IsAssetAllocation)) return false;

    if (nAbsurdFee && nFees > nAbsurdFee)
        return state.Invalid(TxValidationResult::TX_NOT_STANDARD,
                "absurdly-high-fee", strprintf("%d > %d", nFees, nAbsurdFee));

    const CTxMemPool::setEntries setIterConflicting = m_pool.GetIterSet(setConflicts);
    // Calculate in-mempool ancestors, up to a limit.
    if (setConflicts.size() == 1) {
        // In general, when we receive an RBF transaction with mempool conflicts, we want to know whether we
        // would meet the chain limits after the conflicts have been removed. However, there isn't a practical
        // way to do this short of calculating the ancestor and descendant sets with an overlay cache of
        // changed mempool entries. Due to both implementation and runtime complexity concerns, this isn't
        // very realistic, thus we only ensure a limited set of transactions are RBF'able despite mempool
        // conflicts here. Importantly, we need to ensure that some transactions which were accepted using
        // the below carve-out are able to be RBF'ed, without impacting the security the carve-out provides
        // for off-chain contract systems (see link in the comment below).
        //
        // Specifically, the subset of RBF transactions which we allow despite chain limits are those which
        // conflict directly with exactly one other transaction (but may evict children of said transaction),
        // and which are not adding any new mempool dependencies. Note that the "no new mempool dependencies"
        // check is accomplished later, so we don't bother doing anything about it here, but if BIP 125 is
        // amended, we may need to move that check to here instead of removing it wholesale.
        //
        // Such transactions are clearly not merging any existing packages, so we are only concerned with
        // ensuring that (a) no package is growing past the package size (not count) limits and (b) we are
        // not allowing something to effectively use the (below) carve-out spot when it shouldn't be allowed
        // to.
        //
        // To check these we first check if we meet the RBF criteria, above, and increment the descendant
        // limits by the direct conflict and its descendants (as these are recalculated in
        // CalculateMempoolAncestors by assuming the new transaction being added is a new descendant, with no
        // removals, of each parent's existing dependent set). The ancestor count limits are unmodified (as
        // the ancestor limits should be the same for both our new transaction and any conflicts).
        // We don't bother incrementing m_limit_descendants by the full removal count as that limit never comes
        // into force here (as we're only adding a single transaction).
        assert(setIterConflicting.size() == 1);
        CTxMemPool::txiter conflict = *setIterConflicting.begin();

        m_limit_descendants += 1;
        m_limit_descendant_size += conflict->GetSizeWithDescendants();
    }

    std::string errString;
    if (!m_pool.CalculateMemPoolAncestors(*entry, setAncestors, m_limit_ancestors, m_limit_ancestor_size, m_limit_descendants, m_limit_descendant_size, errString)) {
        setAncestors.clear();
        // If CalculateMemPoolAncestors fails second time, we want the original error string.
        std::string dummy_err_string;
        // Contracting/payment channels CPFP carve-out:
        // If the new transaction is relatively small (up to 40k weight)
        // and has at most one ancestor (ie ancestor limit of 2, including
        // the new transaction), allow it if its parent has exactly the
        // descendant limit descendants.
        //
        // This allows protocols which rely on distrusting counterparties
        // being able to broadcast descendants of an unconfirmed transaction
        // to be secure by simply only having two immediately-spendable
        // outputs - one for each counterparty. For more info on the uses for
        // this, see https://lists.linuxfoundation.org/pipermail/bitcoin-dev/2018-November/016518.html
        if (nSize >  EXTRA_DESCENDANT_TX_SIZE_LIMIT ||
                !m_pool.CalculateMemPoolAncestors(*entry, setAncestors, 2, m_limit_ancestor_size, m_limit_descendants + 1, m_limit_descendant_size + EXTRA_DESCENDANT_TX_SIZE_LIMIT, dummy_err_string)) {
            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "too-long-mempool-chain", errString);
        }
    }

    // A transaction that spends outputs that would be replaced by it is invalid. Now
    // that we have the set of all ancestors we can detect this
    // pathological case by making sure setConflicts and setAncestors don't
    // intersect.
    for (CTxMemPool::txiter ancestorIt : setAncestors)
    {
        const uint256 &hashAncestor = ancestorIt->GetTx().GetHash();
        if (setConflicts.count(hashAncestor))
        {
            return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-txns-spends-conflicting-tx",
                    strprintf("%s spends conflicting transaction %s",
                        hash.ToString(),
                        hashAncestor.ToString()));
        }
    }

    // Check if it's economically rational to mine this transaction rather
    // than the ones it replaces.
    nConflictingFees = 0;
    nConflictingSize = 0;
    uint64_t nConflictingCount = 0;

    // If we don't hold the lock allConflicting might be incomplete; the
    // subsequent RemoveStaged() and addUnchecked() calls don't guarantee
    // mempool consistency for us.
    fReplacementTransaction = setConflicts.size();
    if (fReplacementTransaction)
    {
        CFeeRate newFeeRate(nModifiedFees, nSize);
        std::set<uint256> setConflictsParents;
        const int maxDescendantsToVisit = 100;
        for (const auto& mi : setIterConflicting) {
            // Don't allow the replacement to reduce the feerate of the
            // mempool.
            //
            // We usually don't want to accept replacements with lower
            // feerates than what they replaced as that would lower the
            // feerate of the next block. Requiring that the feerate always
            // be increased is also an easy-to-reason about way to prevent
            // DoS attacks via replacements.
            //
            // We only consider the feerates of transactions being directly
            // replaced, not their indirect descendants. While that does
            // mean high feerate children are ignored when deciding whether
            // or not to replace, we do require the replacement to pay more
            // overall fees too, mitigating most cases.
            CFeeRate oldFeeRate(mi->GetModifiedFee(), mi->GetTxSize());
            if (newFeeRate <= oldFeeRate)
            {
                return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "insufficient fee",
                        strprintf("rejecting replacement %s; new feerate %s <= old feerate %s",
                            hash.ToString(),
                            newFeeRate.ToString(),
                            oldFeeRate.ToString()));
            }

            for (const CTxIn &txin : mi->GetTx().vin)
            {
                setConflictsParents.insert(txin.prevout.hash);
            }

            nConflictingCount += mi->GetCountWithDescendants();
        }
        // This potentially overestimates the number of actual descendants
        // but we just want to be conservative to avoid doing too much
        // work.
        if (nConflictingCount <= maxDescendantsToVisit) {
            // If not too many to replace, then calculate the set of
            // transactions that would have to be evicted
            for (CTxMemPool::txiter it : setIterConflicting) {
                m_pool.CalculateDescendants(it, allConflicting);
            }
            for (CTxMemPool::txiter it : allConflicting) {
                nConflictingFees += it->GetModifiedFee();
                nConflictingSize += it->GetTxSize();
            }
        } else {
            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "too many potential replacements",
                    strprintf("rejecting replacement %s; too many potential replacements (%d > %d)\n",
                        hash.ToString(),
                        nConflictingCount,
                        maxDescendantsToVisit));
        }

        for (unsigned int j = 0; j < tx.vin.size(); j++)
        {
            // We don't want to accept replacements that require low
            // feerate junk to be mined first. Ideally we'd keep track of
            // the ancestor feerates and make the decision based on that,
            // but for now requiring all new inputs to be confirmed works.
            //
            // Note that if you relax this to make RBF a little more useful,
            // this may break the CalculateMempoolAncestors RBF relaxation,
            // above. See the comment above the first CalculateMempoolAncestors
            // call for more info.
            if (!setConflictsParents.count(tx.vin[j].prevout.hash))
            {
                // Rather than check the UTXO set - potentially expensive -
                // it's cheaper to just check if the new input refers to a
                // tx that's in the mempool.
                if (m_pool.exists(tx.vin[j].prevout.hash)) {
                    return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "replacement-adds-unconfirmed",
                            strprintf("replacement %s adds unconfirmed input, idx %d",
                                hash.ToString(), j));
                }
            }
        }

        // The replacement must pay greater fees than the transactions it
        // replaces - if we did the bandwidth used by those conflicting
        // transactions would not be paid for.
        if (nModifiedFees < nConflictingFees)
        {
            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "insufficient fee",
                    strprintf("rejecting replacement %s, less fees than conflicting txs; %s < %s",
                        hash.ToString(), FormatMoney(nModifiedFees), FormatMoney(nConflictingFees)));
        }

        // Finally in addition to paying more fees than the conflicts the
        // new transaction must pay for its own bandwidth.
        CAmount nDeltaFees = nModifiedFees - nConflictingFees;
        if (nDeltaFees < ::incrementalRelayFee.GetFee(nSize))
        {
            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "insufficient fee",
                    strprintf("rejecting replacement %s, not enough additional fees to relay; %s < %s",
                        hash.ToString(),
                        FormatMoney(nDeltaFees),
                        FormatMoney(::incrementalRelayFee.GetFee(nSize))));
        }
    }
    return true;
}

bool MemPoolAccept::Finalize(ATMPArgs& args, Workspace& ws)
{
    const CTransaction& tx = *ws.m_ptx;
    const uint256& hash = ws.m_hash;
    TxValidationState &state = args.m_state;
    const bool bypass_limits = args.m_bypass_limits;

    CTxMemPool::setEntries& allConflicting = ws.m_all_conflicting;
    CTxMemPool::setEntries& setAncestors = ws.m_ancestors;
    const CAmount& nModifiedFees = ws.m_modified_fees;
    const CAmount& nConflictingFees = ws.m_conflicting_fees;
    const size_t& nConflictingSize = ws.m_conflicting_size;
    const bool fReplacementTransaction = ws.m_replacement_transaction;
    std::unique_ptr<CTxMemPoolEntry>& entry = ws.m_entry;

    // Remove conflicting transactions from the mempool
    for (CTxMemPool::txiter it : allConflicting)
    {
        LogPrint(BCLog::MEMPOOL, "replacing tx %s with %s for %s BTC additional fees, %d delta bytes\n",
                it->GetTx().GetHash().ToString(),
                hash.ToString(),
                FormatMoney(nModifiedFees - nConflictingFees),
                (int)entry->GetTxSize() - (int)nConflictingSize);
        if (args.m_replaced_transactions)
            args.m_replaced_transactions->push_back(it->GetSharedTx());
    }
    m_pool.RemoveStaged(allConflicting, false, MemPoolRemovalReason::REPLACED);

    // This transaction should only count for fee estimation if:
    // - it isn't a BIP 125 replacement transaction (may not be widely supported)
    // - it's not being re-added during a reorg which bypasses typical mempool fee limits
    // - the node is not behind
    // - the transaction is not dependent on any other transactions in the mempool
    // SYSCOIN
    // - the transaction does not have a duplicate input from an asset allocation transaction
    bool validForFeeEstimation = !args.m_duplicate && !fReplacementTransaction && !bypass_limits && IsCurrentForFeeEstimation() && m_pool.HasNoInputsOf(tx);

    // Store transaction in memory
    m_pool.addUnchecked(*entry, setAncestors, validForFeeEstimation);

    // trim mempool and check if tx was trimmed
    if (!bypass_limits) {
        LimitMempoolSize(m_pool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, std::chrono::hours{gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY)});
        if (!m_pool.exists(hash))
            return state.Invalid(TxValidationResult::TX_MEMPOOL_POLICY, "mempool full");
    }
    return true;
}

/**
 * Return transaction in txOut, and if it was found inside a block, its hash is placed in hashBlock.
 * If blockIndex is provided, the transaction is fetched from the corresponding block.
 */
bool GetTransaction(const uint256& hash, CTransactionRef& txOut, const Consensus::Params& consensusParams, uint256& hashBlock, const CBlockIndex* const block_index)
{
    LOCK(cs_main);

    if (!block_index) {
        CTransactionRef ptx = mempool.get(hash);
        if (ptx) {
            txOut = ptx;
            return true;
        }

        if (g_txindex) {
            return g_txindex->FindTx(hash, hashBlock, txOut);
        }
    } else {
        CBlock block;
        if (ReadBlockFromDisk(block, block_index, consensusParams)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    txOut = tx;
                    hashBlock = block_index->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}