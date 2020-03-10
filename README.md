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
This part finalizes the transaction. Conflicting transactions are removed. The fee is determined here as well after which the transaction is stored into the memory. The finalization ends with a clean up

### 5)Accept Transaction in MemPool 
This part mainly decides when we accept a transaction.


### 6) Mempool update
We need to limit mempool size and update the mempool with time for reorganization. Invalid transaction blocks will be removed from the mempool. Furthermore this function will try to add back lost transaction blocks. The size of the mempool is updated accordingly
