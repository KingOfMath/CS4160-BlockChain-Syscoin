validation.cpp can be seen as Syscoin's main function
This file contains 5,445 lines, implementing transaction, Block I/O, Condition Check, Chain Connection, Loading and Flushing.
Since we only need to gain insights from transaction, we extract that part into a new file.

We divided it into 6 steps:

### 1)Pre check
Before actually starting our transaction, this part runs the policy checks on a given transaction, excluding any script checks. This pre-check looks up inputs, calculates fee rate, considers replacement, evaluates package limits. It checks if the transaction size is too small, if it is already in the mempool, if there exists any conflict with in-memory transactions and other conditions may reduce fee rate.

### 2)Policy Script Check
This part runs the script checks using policy flags. As this can be slow, we should only invoke this on transactions that have otherwise passed policy checks. It also checks whether the script fits the requirement of the Syscoin input script.

### 3)Consensus Script Check
This part re-runs the script checks using consensus flags, and tries to cache the result in the scriptecache. It checks the inputs from mempool and cache and detect duplicate flag.

### 4)Finalize Check
This final check tries to add the transaction to the mempool. It removes conflicting transactions from the mempool first, then deletes duplicate inputs from an asset allocation transaction, stores transaction in memory and trims mempool. This function returns true if the transaction is in the mempool after any size limiting is performed.

### 5)Accept Transaction in MemPool 
This part mainly decides when we accept a transaction.


### 6) Mempool update
We need to limit mempool size and update the mempool with time for reorgization.

