BOOST_AUTO_TEST_CASE(generate_asset_throughput)
{

    int64_t start = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    UniValue r;
    tfm::format(std::cout,"Running generate_asset_throughput...\n");
    GenerateBlocks(5, "node1");
    GenerateBlocks(5, "node3");
    vector<string> vecAssets;
    // setup senders and receiver node addresses
    vector<string> senders;
    vector<string> receivers;
    senders.push_back("node1");
    senders.push_back("node2");
    receivers.push_back("node3");
    BOOST_CHECK(receivers.size() == 1);
    // user modifiable variables

    // for every asset you add numberOfAssetSendsPerBlock tx's effectively
    int numAssets = 10;
    BOOST_CHECK(numAssets >= 1);

    int numberOfAssetSendsPerBlock = 250;
    BOOST_CHECK(numberOfAssetSendsPerBlock >= 1 && numberOfAssetSendsPerBlock <= 250);

      // NOT MEANT TO BE MODIFIED! CALCULATE INSTEAD!
    const int numberOfTransactionToSend = numAssets*numberOfAssetSendsPerBlock;

    // make sure numberOfAssetSendsPerBlock isn't a fraction of numberOfTransactionToSend
    BOOST_CHECK((numberOfTransactionToSend % numberOfAssetSendsPerBlock) == 0);

    vector<string> unfundedAccounts;
    vector<string> rawSignedAssetAllocationSends;
    vector<string> vecFundedAddresses;
    GenerateBlocks((numAssets+1)/ numberOfAssetSendsPerBlock);
    // PHASE 1:  GENERATE UNFUNDED ADDRESSES FOR RECIPIENTS TO ASSETALLOCATIONSEND
    tfm::format(std::cout,"Throughput test: Total transaction count: %d, Receivers Per Asset Allocation Transfer %d, Total Number of Assets needed %d\n\n", numberOfTransactionToSend, numberOfAssetSendsPerBlock, numAssets);
    tfm::format(std::cout,"creating %d unfunded addresses...\n", numberOfAssetSendsPerBlock);
    for(int i =0;i<numberOfAssetSendsPerBlock;i++){
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "getnewaddress"));
        unfundedAccounts.emplace_back(r.get_str());
    }

   // PHASE 2:  GENERATE FUNDED ADDRESSES FOR CREATING AND SENDING ASSETS
    // create address for funding
    BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "getnewaddress"));
    string fundedAccount = r.get_str();
    tfm::format(std::cout,"creating %d funded accounts for using with assetsend/assetallocationsend in subsequent steps...\n", numAssets*numberOfAssetSendsPerBlock);
    string sendManyString = "";
    for(int i =0;i<numAssets;i++){
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "getnewaddress"));
        string fundedAccount = r.get_str();
        if(sendManyString != "")
            sendManyString += ",";
        sendManyString += "\"" + fundedAccount + "\":1";
        if(((i+1)% numberOfAssetSendsPerBlock)==0){
            tfm::format(std::cout,"Sending funds to batch of %d funded accounts, approx. %d batches remaining\n", numberOfAssetSendsPerBlock, (numAssets-i)/ numberOfAssetSendsPerBlock);
            std::string strSendMany = "sendmany \"\" {" + sendManyString + "}";
            CallExtRPC("node1", "sendmany", "\"\",{" + sendManyString + "}");
            sendManyString = "";

        }
        vecFundedAddresses.push_back(fundedAccount);
    }
    if(!sendManyString.empty()){
        std::string strSendMany = "sendmany \"\" {" + sendManyString + "}";
        CallExtRPC("node1", "sendmany", "\"\",{" + sendManyString + "}");
    }
    GenerateBlocks(5);

    // PHASE 3:  SET tpstestsetenabled ON ALL SENDER/RECEIVER NODES
    for (auto &sender : senders)
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "tpstestsetenabled", "true"));
    for (auto &receiver : receivers)
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "tpstestsetenabled", "true"));

     // PHASE 4:  CREATE ASSETS
    // create assets needed
    tfm::format(std::cout,"creating %d sender assets...\n", numAssets);
    for(int i =0;i<numAssets;i++){
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "assetnew" , "\"" +  vecFundedAddresses[i] + "\",\"tps\",\"''\",\"''\",8,250,250,31,{},\"''\""));
        string guid = itostr(find_value(r.get_obj(), "asset_guid").get_uint());
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "signrawtransactionwithwallet", "\"" + find_value(r.get_obj(), "hex").get_str() + "\""));
        string hex_str = find_value(r.get_obj(), "hex").get_str();
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "sendrawtransaction" , "\"" + hex_str + "\""));
        vecAssets.push_back(guid);
    }
    GenerateBlocks(5);
    tfm::format(std::cout,"sending assets with assetsend...\n");
    // PHASE 5:  SEND ASSETS TO NEW ALLOCATIONS
    for(int i =0;i<numAssets;i++){
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "listassetindexassets" , "\"" +  vecFundedAddresses[i] + "\""));
        UniValue indexArray = r.get_array();
        BOOST_CHECK_EQUAL(indexArray.size(), 1);
        uint32_t nAsset = find_value(indexArray[0].get_obj(), "asset_guid").get_uint();
        uint32_t nAssetStored;
        ParseUInt32(vecAssets[i], &nAssetStored);
        BOOST_CHECK_EQUAL(nAsset, nAssetStored);
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "assetsendmany" ,  vecAssets[i] + ",[{\"address\":\"" + vecFundedAddresses[i] + "\",\"amount\":250}],\"''\""));

        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "signrawtransactionwithwallet", "\"" +  find_value(r.get_obj(), "hex").get_str() + "\""));
        string hex_str = find_value(r.get_obj(), "hex").get_str();
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "sendrawtransaction" , "\"" + hex_str + "\""));       
    }

    GenerateBlocks(5);

    // PHASE 6:  SEND ALLOCATIONS TO NEW ALLOCATIONS (UNFUNDED ADDRESSES) USING ZDAG
    tfm::format(std::cout,"Creating assetallocationsend transactions...\n");
    int unfoundedAccountIndex = 0;
    // create vector of signed transactions
    string assetAllocationSendMany = "";
    for(int i =0;i<numAssets;i++){
        // send asset to numberOfAssetSendsPerBlock addresses
        string assetAllocationSendMany = "";
        // +1 to account for change output
        for (int j = 0; j < numberOfAssetSendsPerBlock; j++) {
            if(assetAllocationSendMany != "")
                assetAllocationSendMany += ",";
            assetAllocationSendMany += "{\"address\":\"" + unfundedAccounts[unfoundedAccountIndex++] + "\",\"amount\":1}";
            if(unfoundedAccountIndex >= unfundedAccounts.size())
                unfoundedAccountIndex = 0;
        }

        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "assetallocationsendmany" , vecAssets[i] + ",\"" + vecFundedAddresses[i] + "\",[" + assetAllocationSendMany + "],\"''\""));
        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "signrawtransactionwithwallet" , "\"" + find_value(r.get_obj(), "hex").get_str() + "\""));
        string hex_str = find_value(r.get_obj(), "hex").get_str();
        rawSignedAssetAllocationSends.push_back(hex_str);
    }

    BOOST_CHECK(assetAllocationSendMany.empty());

    // PHASE 7:  DISTRIBUTE LOAD AMONG SENDERS
    // push vector of signed transactions to tpstestadd on every sender node distributed evenly
    int txPerSender = 1;
    tfm::format(std::cout,"Dividing work (%d transactions) between %d senders (%d per sender)...\n", rawSignedAssetAllocationSends.size(), senders.size(), txPerSender);

    unsigned int j=0;
    unsigned int i=0;
    unsigned int senderIndex=0;
    while(j < rawSignedAssetAllocationSends.size()){
        string vecTX = "[";
        unsigned int currentTxIndex = i * txPerSender;
        unsigned int nextTxIndex = (i+1) * txPerSender;
        if((nextTxIndex+txPerSender) > rawSignedAssetAllocationSends.size())
            nextTxIndex += txPerSender;
        for(j=currentTxIndex;j< nextTxIndex;j++){
            if(j >= rawSignedAssetAllocationSends.size())
                break;
            if(vecTX != "[")
                vecTX += ",";
            vecTX += "{\"tx\":\"" + rawSignedAssetAllocationSends[j] + "\"}";
        }
        if(vecTX != "["){
            vecTX += "]";
            BOOST_CHECK_NO_THROW(CallExtRPC(senders[senderIndex++], "tpstestadd", "0," + vecTX));
        }
        if(senderIndex >= senders.size())
            senderIndex = 0;
        i++;
    }

    // PHASE 8:  CALL tpstestadd ON ALL SENDER/RECEIVER NODES WITH A FUTURE TIME
    // set the start time to 1 second from now (this needs to be profiled, if the tpstestadd setting time to every node exceeds say 500ms then this time should be extended to account for the latency).
    // rule of thumb if sender count is high (> 25) then profile how long it takes and multiple by 10 and get ceiling of next second needed to send this rpc to every node to have them sync up

    // this will set a start time to every node which will send the vector of signed txs to the network
    int64_t tpstarttime = GetTimeMicros();
    int microsInSecond =  1000 * 1000;
    tpstarttime = tpstarttime + microsInSecond;
    tfm::format(std::cout,"Adding assetsend transactions to queue on sender nodes...\n");
    // ensure mnsync isn't doing its thing before the test starts
    for (auto &sender : senders){
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "mnsync", "\"next\"", false));
    }
    for (auto &receiver : receivers){
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "mnsync", "\"next\"", false));
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "mnsync", "\"next\"", false));
    }
    for (auto &sender : senders){
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "tpstestadd",  i64tostr(tpstarttime)));
        BOOST_CHECK_NO_THROW(r = CallExtRPC(sender, "tpstestinfo"));
        BOOST_CHECK_EQUAL(find_value(r.get_obj(), "testinitiatetime").get_int64(), tpstarttime);
    }
    for (auto &receiver : receivers){
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "tpstestadd", i64tostr(tpstarttime)));
        BOOST_CHECK_EQUAL(find_value(r.get_obj(), "testinitiatetime").get_int64(), tpstarttime);
    }

    // PHASE 9:  WAIT 10 SECONDS + DELAY SET ABOVE (1 SECOND)
    tfm::format(std::cout,"Waiting 11 seconds as per protocol...\n");
    // start 11 second wait
    MilliSleep(11000);

    // PHASE 10:  CALL tpstestinfo ON SENDERS AND GET AVERAGE START TIME (TIME SENDERS PUSHED TRANSACTIONS TO THE SOCKETS)
    // get the elapsed time of each node on how long it took to push the vector of signed txs to the network
    int64_t avgteststarttime = 0;
    for (auto &sender : senders) {
        BOOST_CHECK_NO_THROW(r = CallExtRPC(sender, "tpstestinfo"));
        avgteststarttime += find_value(r.get_obj(), "testinitiatetime").get_int64();
    }
    avgteststarttime /= senders.size();

    // PHASE 11:  CALL tpstestinfo ON RECEIVERS AND GET AVERAGE RECEIVE TIME, CALCULATE AVERAGE
    // gather received transfers on the receiver, you can query any receiver node here, in general they all should see the same state after the elapsed time.
    BOOST_CHECK_NO_THROW(r = CallExtRPC(receivers[0], "tpstestinfo"));
    UniValue tpsresponse = r.get_obj();
    UniValue tpsresponsereceivers = find_value(tpsresponse, "receivers").get_array();

    float totalTime = 0;
    for (size_t i = 0; i < tpsresponsereceivers.size(); i++) {
        const UniValue &responseObj = tpsresponsereceivers[i].get_obj();
        totalTime += find_value(responseObj, "time").get_int64() - avgteststarttime;
    }
    // average the start time - received time by the number of responses received (usually number of responses should match number of transactions sent beginning of test)
    totalTime /= tpsresponsereceivers.size();


    // PHASE 12:  DISPLAY RESULTS

    tfm::format(std::cout,"tpstarttime %lld avgteststarttime %lld totaltime %.2f, num responses %zu\n", tpstarttime, avgteststarttime, totalTime, tpsresponsereceivers.size());
    for (auto &sender : senders)
        BOOST_CHECK_NO_THROW(CallExtRPC(sender, "tpstestsetenabled", "false"));
    for (auto &receiver : receivers)
        BOOST_CHECK_NO_THROW(CallExtRPC(receiver, "tpstestsetenabled", "false"));
    int64_t end = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t &startblock = GetTimeMicros();
    tfm::format(std::cout,"creating %d blocks\n", (numAssets/(93*4)) + 2);
    GenerateBlocks((numAssets/(93*4)) + 2, receivers[0]);

    const int64_t &endblock = GetTimeMicros();
    tfm::format(std::cout,"elapsed time in block creation: %lld\n", endblock-startblock);
    tfm::format(std::cout,"elapsed time in seconds: %lld\n", end-start);
    tfm::format(std::cout,"checking indexes...\n");
    unfoundedAccountIndex = 0;
    for(int i =0;i<numAssets;i++){
        uint32_t nAssetStored;
        ParseUInt32(vecAssets[i], &nAssetStored);
        for (int j = 0; j < numberOfAssetSendsPerBlock; j++) {
            BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "listassetindexallocations" , "\"" + unfundedAccounts[unfoundedAccountIndex++] + "\""));
            UniValue indexArray = r.get_array();
            BOOST_CHECK_EQUAL(indexArray.size(), numAssets);       
            if(unfoundedAccountIndex >= unfundedAccounts.size())
                unfoundedAccountIndex = 0;
        }

        BOOST_CHECK_NO_THROW(r = CallExtRPC("node1", "listassetindexassets" , "\"" + vecFundedAddresses[i] + "\""));
        UniValue indexArray = r.get_array();
        BOOST_CHECK_EQUAL(indexArray.size(), 1);
        uint32_t nAsset = find_value(indexArray[0].get_obj(), "asset_guid").get_uint();
        BOOST_CHECK_EQUAL(nAsset, nAssetStored);
    }
}
