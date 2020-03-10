validation.cpp can be seen as Syscoin's main function
This file contains 5,445 lines, implementing transaction, Block I/O, Condition Check, Chain Connection, Loading and Flushing.
Since we only need to gain insights from transaction, we extract that part into a new file.

We divided it into 5 parts:

### 1)Policy and Consensus Script Check
This part checks input scripts and signatures, and check again against the current block tip's script verification. This is done last to help prevent CPU exhaustion denial-of-service attacks.

### 2)MemPoolAccept
This is the main class with two structs ATMPArgs and Workspace, storing transaction information.

### 3)Accept Transaction in MemPool 
This part mainly decides when we accept a transaction.

### 4)Mempool update
We need to limit mempool size and update the mempool with time for reorgization.

### 5)Pre check and final reception
Before actually starting our transaction, we should pre-check our transaction states. When we get our transaction and mempool accepts it, then we end the transaction.
