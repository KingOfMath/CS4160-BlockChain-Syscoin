## testnet

link: https://github.com/syscoin/GameNetworkingSockets

#### configuration:

regtest = 0

testnet = 0

[test]

rpcuser=u

rpcpassword=p

rpcport=18370

server=1

**gethtestnet=1**

**addnode=54.203.169.179 (key)**

**addnode=54.190.239.153 (key)**

zmqrawmempooltx=tcp://127.0.0.1:28332

zmqpubhashblock=tcp://127.0.0.1:28332

[main]

gethtestnet=0

#### Things to check:

1.incomingListPeers

2.outcomingListPeers

3."rawmempooltx"

4.SyscoinCoreZMQURL



## validation.cpp

This file contains 5,445 lines, implementing transaction, Block I/O, Condition Check, Chain Connection, Loading and Flushing.
Since we only need to gain insights from transaction, we extract that part into a new file.

We divided it into 6 steps:

### 1)Pre check
Before actually starting our transaction, this part runs the policy checks on a given transaction, excluding any script checks.

This pre-check looks up inputs, calculates fee rate, considers replacement, evaluates package limits. 

It checks if the transaction size is too small, if it is already in the mempool, if there exists any conflict with in-memory transactions and other conditions may reduce fee rate.

### 2)Policy Script Check
This part runs the script checks using policy flags. 

As this can be slow, we should only invoke this on transactions that have otherwise passed policy checks. 

It also checks whether the script fits the requirement of the Syscoin input script.

### 3)Consensus Script Check
This part re-runs the script checks using consensus flags, and tries to cache the result in the scriptecache. 

It checks the inputs from mempool and cache and detect duplicate flag.

### 4)Finalize Check
This final check tries to add the transaction to the mempool. 

It removes conflicting transactions from the mempool first, then deletes duplicate inputs from an asset allocation transaction, stores transaction in memory and trims mempool. 

This function returns true if the transaction is in the mempool after any size limiting is performed.

### 5)Accept Transaction in MemPool 
This part mainly decides when we accept a transaction.


### 6) Mempool update
We need to limit mempool size and update the mempool with time for reorgization.

## How to install gateway.cpp file of GameNetworkingSockets
It appears that it is easier to run the file on a linux distribtion

### Instructions
* sudo apt install libssl-dev
* sudo apt install libprotobuf-dev protobuf-compiler libjsonrpccpp-dev libjsonrpccpp-tools libczmq-dev
* sudo apt install meson
* go to directory where you want to place the GameNetworkSocketFiles
* git clone https://github.com/syscoin/GameNetworkingSockets.git
* cd GameNetworkingSockets
* meson . build
* ninja -C build
* cd build/gateway
* ./gateway

### Connection
*  curl ifconfig.me
*  ./gateway
*  /connect
*  /quit

## Improvements

### Pre-check doublespending duplication (validation.cpp row 453-481)
Follow the original mechanism, when the size of actorSet is one, Syscoin locks the conflicts set of asset allocation. It only checks if there exists the first double-spend and falls back to normal policy to not relay and potentially bans on other double-spends. If the double-spends are duplicated, the conflicts set records the hash of ptxConflicting. One potential improvement is that if such a case exists: fault tolerance is improved due to more advanced technology, we ought to be able to tolerate more double-spends. Maybe there is such a solution e.g. storing duplicated double spends but not conflicts original transactions and miners can still avoid these faulty double-spends. Or another research point is if we fake a conflict, is there only possible solution to check if its hash code is in line with Syscoin's standard.

### Lock mechanism (validation.cpp row 87-141)
Syscoin's acceptance of transactions to memory pool is executed with a specified acceptance time, which we can see in the code it is called by function AcceptSingleTransaction. If we dive into this function, we can observe that the memory pool locks its reads during each check procedure. When transaction volume and concurrency are relatively large, is it still possible to lock the memory pool and serve it for only a single transaction? Is there any other feasible schedule solution to deal with the resource competition? How to ensure that acceptance time is well runnable if more conflicts are allowed, which may cause some deadlocks?
