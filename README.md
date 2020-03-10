validation.cpp can be seen as Syscoin's main function
This file contains 5,445 lines, implementing transaction, Block I/O, Condition Check, Chain Connection, Loading and Flushing.
Since we only need to gain insights from transaction, we extract that part into a new file.

We divided it into 6 steps:

### 1)Pre check
Before actually starting our transaction, we should pre-check our transaction states. 
We check if the transaction size is too small, if it is already in the mempool, if there exists any conflict with in-memory transactions, if all the inputs exist and meet standards and other conditions may reduce fee rate.

### 2)Policy Script Check
This part checks input scripts and signatures, and check again against the current block tip's script verification. 
This is done last to help prevent CPU exhaustion denial-of-service attacks.

### 3)Consensus Script Check

### 4)Finalize Check


### 5)Accept Transaction in MemPool 
This part mainly decides when we accept a transaction.


### 6) Mempool update
We need to limit mempool size and update the mempool with time for reorgization.
* Function LimitMempoolSize
The function first checks whether transactions can be removed from the mempool. They should be removed if they have expired. 
Furthermore transactions for which the transaction fee is close to the cost of the transaction itself should be removed as well.
* Function IsCurrentForFeeEstimation()
This function returns false if
1. the block is the initial block
2. the current block time is lower than the MAX-FEE-ESTIMATION-TIP-AGE 
3.  The currenct height is lower than the best height of the blocks

In all other cases true will be returned
* Function UpdateMempoolForReorg
The mempool is updated to remove disconnected transactions blocks from the mempool. It will also remove transactions that are no longer valid given the tip/height. Only not confirmed transactions will be removed. This function comes with an extra parameter fAddToMempool. When filled in as false, the function will not try to add the transactions back into the mempool. Transactions will be added back in order of appearance. Thus, latest transactions will be added back to the mempool earlier than older ones. Resurrected transactions are not validated. If a transaction will not be added back into the mempool, its depending transaction nodes will be removed as well. Immature transactions will be removed as well. The last thing is to update the size of the mempool in case transactions were added.
