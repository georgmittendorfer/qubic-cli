#include <cstring>
#include <vector>
#include <cstdlib>
#include "structs.h"
#include "connection.h"
#include "nodeUtils.h"
#include "logger.h"
#include "K12AndKeyUtil.h"
#include "keyUtils.h"
#include "walletUtils.h"

static CurrentTickInfo getTickInfoFromNode(const char* nodeIp, int nodePort)
{
	CurrentTickInfo result;
	memset(&result, 0, sizeof(CurrentTickInfo));
	auto qc = new QubicConnection(nodeIp, nodePort);
	struct {
        RequestResponseHeader header;
    } packet;
    packet.header.setSize(sizeof(packet));
    packet.header.randomizeDejavu();
    packet.header.setType(REQUEST_CURRENT_TICK_INFO);
    qc->sendData((uint8_t *) &packet, packet.header.size());
    std::vector<uint8_t> buffer;
    qc->receiveDataAll(buffer);
    uint8_t* data = buffer.data();
    int recvByte = buffer.size();
    int ptr = 0;
    while (ptr < recvByte)
    {
    	auto header = (RequestResponseHeader*)(data+ptr);
    	if (header->type() == RESPOND_CURRENT_TICK_INFO){
    		auto curTickInfo = (CurrentTickInfo*)(data + ptr + sizeof(RequestResponseHeader));
    		result = *curTickInfo;
    	}
        ptr+= header->size();
    }
    delete qc;
	return result;
}
uint32_t getTickNumberFromNode(const char* nodeIp, int nodePort)
{
    auto curTickInfo = getTickInfoFromNode(nodeIp, nodePort);
    return curTickInfo.tick;
}
void printTickInfoFromNode(const char* nodeIp, int nodePort)
{
	auto curTickInfo = getTickInfoFromNode(nodeIp, nodePort);
	if (curTickInfo.epoch != 0){
		LOG("Tick: %u\n", curTickInfo.tick);
		LOG("Epoch: %u\n", curTickInfo.epoch);
		LOG("Number Of Aligned Votes: %u\n", curTickInfo.numberOfAlignedVotes);
		LOG("Number Of Misaligned Votes: %u\n", curTickInfo.numberOfMisalignedVotes);	
	} else {
		LOG("Error while getting tick info from %s:%d\n", nodeIp, nodePort);
	}
}
static void getTickTransactions(const char* nodeIp, const int nodePort, const uint32_t requestedTick, int nTx,
                                std::vector<Transaction>& txs, //out
                                std::vector<TxhashStruct>* hashes, //out
                                std::vector<extraDataStruct>* extraData, // out
                                std::vector<SignatureStruct>* sigs // out
                                )
{
    txs.resize(0);
    if (hashes != nullptr)
    {
        hashes->resize(0);
    }
    if (extraData != nullptr)
    {
        extraData->resize(0);
    }
    if (sigs != nullptr)
    {
        sigs->resize(0);
    }

    struct {
        RequestResponseHeader header;
        RequestedTickTransactions txs;
    } packet;
    packet.header.setSize(sizeof(packet));
    packet.header.randomizeDejavu();
    packet.header.setType(REQUEST_TICK_TRANSACTIONS); // REQUEST_TICK_TRANSACTIONS
    packet.txs.tick = requestedTick;
    for (int i = 0; i < (nTx+7)/8; i++) packet.txs.transactionFlags[i] = 0;
    for (int i = (nTx+7)/8; i < NUMBER_OF_TRANSACTIONS_PER_TICK/8; i++) packet.txs.transactionFlags[i] = 1;
    auto qc = new QubicConnection(nodeIp, nodePort);
    qc->sendData((uint8_t *) &packet, packet.header.size());
    std::vector<uint8_t> buffer;
    qc->receiveDataAll(buffer);
    uint8_t* data = buffer.data();
    int recvByte = buffer.size();
    int ptr = 0;
    while (ptr < recvByte)
    {
        auto header = (RequestResponseHeader*)(data+ptr);
        if (header->type() == BROADCAST_TRANSACTION){
            auto tx = (Transaction *)(data + ptr + sizeof(RequestResponseHeader));
            txs.push_back(*tx);
            if (hashes != nullptr){
                TxhashStruct hash;
                uint8_t digest[32] = {0};
                char txHash[128] = {0};
                KangarooTwelve(reinterpret_cast<const uint8_t *>(tx),
                               sizeof(Transaction) + tx->inputSize + SIGNATURE_SIZE,
                               digest,
                               32);
                getTxHashFromDigest(digest, txHash);
                memcpy(hash.hash, txHash, 60);
                hashes->push_back(hash);
            }
            if (extraData != nullptr){
                extraDataStruct ed;
                ed.vecU8.resize(tx->inputSize);
                if (tx->inputSize != 0){
                    memcpy(ed.vecU8.data(), reinterpret_cast<const uint8_t *>(tx) + sizeof(Transaction), tx->inputSize);
                }
                extraData->push_back(ed);
            }
            if (sigs != nullptr){
                SignatureStruct sig;
                memcpy(sig.sig, reinterpret_cast<const uint8_t *>(tx) + sizeof(Transaction) + tx->inputSize, 64);
                sigs->push_back(sig);
            }
        }
        ptr+= header->size();
    }
    delete qc;
}
static void getTickData(const char* nodeIp, const int nodePort, const uint32_t tick, TickData& result)
{
    memset(&result, 0, sizeof(TickData));
    static struct
    {
        RequestResponseHeader header;
        RequestTickData requestTickData;
    } packet;
    packet.header.setSize(sizeof(packet));
    packet.header.randomizeDejavu();
    packet.header.setType(REQUEST_TICK_DATA);
    packet.requestTickData.requestedTickData.tick = tick;
    auto qc = new QubicConnection(nodeIp, nodePort);
    qc->sendData((uint8_t *) &packet, packet.header.size());
    std::vector<uint8_t> buffer;
    qc->receiveDataAll(buffer);
    uint8_t* data = buffer.data();
    int recvByte = buffer.size();
    int ptr = 0;
    while (ptr < recvByte)
    {
        auto header = (RequestResponseHeader*)(data+ptr);
        if (header->type() == BROADCAST_FUTURE_TICK_DATA){
            auto curTickData = (TickData*)(data + ptr + sizeof(RequestResponseHeader));
            result = *curTickData;
        }
        ptr+= header->size();
    }
    delete qc;
}
bool checkTxOnTick(const char* nodeIp, const int nodePort, const char* txHash, uint32_t requestedTick)
{
    // conditions:
    // - current Tick is higher than requested tick
    // - has tick data
    // - has txHash in tick transactions
    uint32_t currenTick = getTickNumberFromNode(nodeIp, nodePort);
    if (currenTick <= requestedTick)
    {
        LOG("Please wait a bit more. Requested tick %u, current tick %u\n", requestedTick, currenTick);
        return false;
    }
    TickData td;
    getTickData(nodeIp, nodePort, requestedTick, td);
    if (td.epoch == 0)
    {
        LOG("Tick %u is empty\n", requestedTick);
        return false;
    }
    int numTx = 0;
    uint8_t all_zero[32] = {0};
    for (int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++){
        if (memcmp(all_zero, td.transactionDigests[i], 32) != 0) numTx++;
    }
    std::vector<Transaction> txs;
    std::vector<TxhashStruct> txHashesFromTick;
    std::vector<extraDataStruct> extraData;
    std::vector<SignatureStruct> signatureStruct;
    getTickTransactions(nodeIp, nodePort, requestedTick, numTx, txs, &txHashesFromTick, &extraData, &signatureStruct);
    for (int i = 0; i < txHashesFromTick.size(); i++)
    {
        if (memcmp(txHashesFromTick[i].hash, txHash, 60) == 0)
        {
            LOG("Found tx %s on tick %u\n", txHash, requestedTick);
            printReceipt(txs[i], txHash, extraData[i].vecU8.data());
            return true;
        }
    }
    LOG("Can NOT find tx %s on tick %u\n", txHash, requestedTick);
    return false;
}

void getTickDataToFile(const char* nodeIp, const int nodePort, uint32_t requestedTick, const char* fileName)
{
    uint32_t currenTick = getTickNumberFromNode(nodeIp, nodePort);
    if (currenTick <= requestedTick)
    {
        LOG("Please wait a bit more. Requested tick %u, current tick %u\n", requestedTick, currenTick);
        return;
    }
    TickData td;
    getTickData(nodeIp, nodePort, requestedTick, td);
    if (td.epoch == 0)
    {
        LOG("Tick %u is empty\n", requestedTick);
        return;
    }
    int numTx = 0;
    uint8_t all_zero[32] = {0};
    for (int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++){
        if (memcmp(all_zero, td.transactionDigests[i], 32) != 0) numTx++;
    }
    std::vector<Transaction> txs;
    std::vector<extraDataStruct> extraData;
    std::vector<SignatureStruct> signatures;
    getTickTransactions(nodeIp, nodePort, requestedTick, numTx, txs, nullptr, &extraData, &signatures);

    FILE* f = fopen(fileName, "wb");
    fwrite(&td, 1, sizeof(TickData), f);
    for (int i = 0; i < txs.size(); i++)
    {
        fwrite(&txs[i], 1, sizeof(Transaction), f);
        int extraDataSize = txs[i].inputSize;
        if (extraDataSize != 0){
            fwrite(extraData[i].vecU8.data(), 1, extraDataSize, f);
        }
        fwrite(signatures[i].sig, 1, SIGNATURE_SIZE, f);
    }
    fclose(f);
    LOG("Tick data and tick transactions have been written to %s\n", fileName);
}

void readTickDataFromFile(const char* fileName, TickData& td,
                          std::vector<Transaction>& txs,
                          std::vector<extraDataStruct>* extraData,
                          std::vector<SignatureStruct>* signatures,
                          std::vector<TxhashStruct>* txHashes)
{
    uint8_t extraDataBuffer[1024] = {0};
    uint8_t signatureBuffer[128] = {0};
    char txHashBuffer[128] = {0};
    uint8_t digest[32] = {0};

    FILE* f = fopen(fileName, "rb");
    fread(&td, 1, sizeof(TickData), f);
    int numTx = 0;
    uint8_t all_zero[32] = {0};
    for (int i = 0; i < NUMBER_OF_TRANSACTIONS_PER_TICK; i++){
        if (memcmp(all_zero, td.transactionDigests[i], 32) != 0) numTx++;
    }
    for (int i = 0; i < numTx; i++){
        Transaction tx;
        fread(&tx, 1, sizeof(Transaction), f);
        int extraDataSize = tx.inputSize;
        if (extraDataSize != 0){
            fread(extraDataBuffer, 1, extraDataSize, f);
            if (extraData != nullptr){
                extraDataStruct eds;
                eds.vecU8.resize(extraDataSize);
                memcpy(eds.vecU8.data(), extraDataBuffer, extraDataSize);
                extraData->push_back(eds);
            }
        }
        fread(signatureBuffer, 1, SIGNATURE_SIZE, f);
        if (signatures != nullptr){
            SignatureStruct sig;
            memcpy(sig.sig, signatureBuffer, SIGNATURE_SIZE);
            signatures->push_back(sig);
        }
        if (txHashes != nullptr){
            std::vector<uint8_t> raw_data;
            raw_data.resize(sizeof(Transaction) + tx.inputSize + SIGNATURE_SIZE);
            auto ptr = raw_data.data();
            memcpy(ptr, &tx, sizeof(Transaction));
            memcpy(ptr + sizeof(Transaction), extraDataBuffer, tx.inputSize);
            memcpy(ptr + sizeof(Transaction) + tx.inputSize, signatureBuffer, SIGNATURE_SIZE);
            KangarooTwelve(ptr,
                           raw_data.size(),
                           digest,
                           32);
            TxhashStruct tx_hash;
            getTxHashFromDigest(digest, txHashBuffer);
            memcpy(tx_hash.hash, txHashBuffer, 60);
            txHashes->push_back(tx_hash);
        }
        txs.push_back(tx);
    }
}

BroadcastComputors readComputorListFromFile(const char* fileName);

void printTickDataFromFile(const char* fileName, const char* compFile)
{
    TickData td;
    std::vector<Transaction> txs;
    std::vector<extraDataStruct> extraData;
    std::vector<SignatureStruct> signatures;
    std::vector<TxhashStruct> txHashes;
    uint8_t digest[32];
    readTickDataFromFile(fileName, td, txs, &extraData, &signatures, &txHashes);
    //verifying everything
    BroadcastComputors bc;
    bc = readComputorListFromFile(compFile);
    int computorIndex = td.computorIndex;
    td.computorIndex ^= BROADCAST_FUTURE_TICK_DATA;
    KangarooTwelve(reinterpret_cast<const uint8_t *>(&td),
                   sizeof(TickData) - SIGNATURE_SIZE,
                   digest,
                   32);
    uint8_t* computorOfThisTick = bc.computors.publicKeys[computorIndex];
    if (verify(computorOfThisTick, digest, td.signature)){
        LOG("Tick is VERIFIED (signed by correct computor).\n");
    } else {
        LOG("Tick is NOT verified (not signed by correct computor).\n");
    }
    LOG("Epoch: %u\n", td.epoch);
    LOG("Tick: %u\n", td.tick);
    LOG("Computor index: %u\n", computorIndex);
    LOG("Datetime: %u-%u-%u %u:%u:%u.%u\n", td.day, td.month, td.year, td.hour, td.minute, td.second, td.millisecond);

    for (int i = 0; i < txs.size(); i++)
    {
        printReceipt(txs[i], txHashes[i].hash, extraData[i].vecU8.data());
        if (verifyTx(txs[i], extraData[i].vecU8.data(), signatures[i].sig))
        {
            LOG("Transaction is VERIFIED\n");
        } else {
            LOG("Transaction is NOT VERIFIED. Incorrect signature\n");
        }
    }
}

bool checkTxOnFile(const char* txHash, const char* fileName)
{
    TickData td;
    std::vector<Transaction> txs;
    std::vector<extraDataStruct> extraData;
    std::vector<SignatureStruct> signatures;
    std::vector<TxhashStruct> txHashes;

    readTickDataFromFile(fileName, td, txs, &extraData, &signatures, &txHashes);

    for (int i = 0; i < txs.size(); i++)
    {
        if (memcmp(txHashes[i].hash, txHash, 60) == 0){
            LOG("Found tx %s on file %s\n", txHash, fileName);
            printReceipt(txs[i], txHash, extraData[i].vecU8.data());
            return true;
        }
    }
    LOG("Can NOT find tx %s on file %s\n", txHash, fileName);
    return false;
}

void sendRawPacket(const char* nodeIp, const int nodePort, int rawPacketSize, uint8_t* rawPacket)
{
    std::vector<uint8_t> buffer;
    auto qc = new QubicConnection(nodeIp, nodePort);
    qc->sendData(rawPacket, rawPacketSize);
    LOG("Sent %d bytes\n", rawPacketSize);
    qc->receiveDataAll(buffer);
    LOG("Received %d bytes\n", buffer.size());
    for (int i = 0; i < buffer.size(); i++){
        LOG("%02x", buffer[i]);
    }
    LOG("\n");
    delete qc;
}

void sendSpecialCommand(const char* nodeIp, const int nodePort, const char* seed, int command)
{
    uint8_t privateKey[32] = {0};
    uint8_t sourcePublicKey[32] = {0};
    uint8_t subseed[32] = {0};
    uint8_t digest[32] = {0};
    uint8_t signature[64] = {0};

    struct {
        RequestResponseHeader header;
        SpecialCommand cmd;
        uint8_t signature[64];
    } packet;
    packet.header.setSize(sizeof(packet));
    packet.header.randomizeDejavu();
    packet.header.setType(PROCESS_SPECIAL_COMMAND);
    uint64_t curTime = time(NULL);
    uint64_t commandByte = (uint64_t)(command) << 56;
    packet.cmd.everIncreasingNonceAndCommandType = commandByte | curTime;

    getSubseedFromSeed((uint8_t*)seed, subseed);
    getPrivateKeyFromSubSeed(subseed, privateKey);
    getPublicKeyFromPrivateKey(privateKey, sourcePublicKey);
    KangarooTwelve((unsigned char*)&packet.cmd,
                   sizeof(packet.cmd),
                   digest,
                   32);
    sign(subseed, sourcePublicKey, digest, signature);
    memcpy(packet.signature, signature, 64);
    auto qc = new QubicConnection(nodeIp, nodePort);
    qc->sendData((uint8_t *) &packet, packet.header.size());
    delete qc;
    LOG("Send command %d to node %s\n", command, nodeIp);
}

BroadcastComputors readComputorListFromFile(const char* fileName)
{
    BroadcastComputors result;
    FILE* f = fopen(fileName, "rb");
    fread(&result, 1, sizeof(BroadcastComputors), f);
    fclose(f);
    uint8_t digest[32] = {0};
    uint8_t arbPubkey[32] = {0};
    // verify with arb
    getPublicKeyFromIdentity(ARBITRATOR, arbPubkey);
    KangarooTwelve(reinterpret_cast<const uint8_t *>(&result),
                   sizeof(BroadcastComputors) - SIGNATURE_SIZE,
                   digest,
                   32);
    if (verify(arbPubkey, digest, result.computors.signature)){
        LOG("Computor list is VERIFIED (signed by ARBITRATOR)\n");
    } else {
        LOG("Computor list is NOT verified\n");
    }
    return result;
}

BroadcastComputors getComputorFromNode(const char* nodeIp, const int nodePort)
{
    BroadcastComputors result;
    static struct
    {
        RequestResponseHeader header;
    } packet;
    packet.header.setSize(sizeof(packet));
    packet.header.setType(REQUEST_COMPUTORS);
    auto qc = new QubicConnection(nodeIp, nodePort);
    qc->sendData((uint8_t *) &packet, packet.header.size());
    std::vector<uint8_t> buffer;
    qc->receiveDataAll(buffer);
    uint8_t* data = buffer.data();
    int recvByte = buffer.size();
    int ptr = 0;
    while (ptr < recvByte)
    {
        auto header = (RequestResponseHeader*)(data+ptr);
        if (header->type() == BROADCAST_COMPUTORS){
            auto bc = (BroadcastComputors*)(data + ptr + sizeof(RequestResponseHeader));
            result = *bc;
        }
        ptr+= header->size();
    }
    delete qc;
    return result;
}

void getComputorListToFile(const char* nodeIp, const int nodePort, const char* fileName)
{
    auto bc = getComputorFromNode(nodeIp, nodePort);
    uint8_t digest[32] = {0};
    uint8_t arbPubkey[32] = {0};
    // verify with arb, dump data
    getPublicKeyFromIdentity(ARBITRATOR, arbPubkey);
    for (int i = 0; i < NUMBER_OF_COMPUTORS; i++)
    {
        char identity[128] = {0};
        const bool isLowerCase = false;
        getIdentityFromPublicKey(bc.computors.publicKeys[i], identity, isLowerCase);
        LOG("%d %s\n", i, identity);
    }
    LOG("Epoch: %u\n", bc.computors.epoch);
    KangarooTwelve(reinterpret_cast<const uint8_t *>(&bc),
                  sizeof(BroadcastComputors) - SIGNATURE_SIZE,
                  digest,
                  32);
    if (verify(arbPubkey, digest, bc.computors.signature)){
        LOG("Computor list is VERIFIED (signed by ARBITRATOR)\n");
    } else {
        LOG("Computor list is NOT verified\n");
    }
    FILE* f = fopen(fileName, "wb");
    fwrite(&bc, 1, sizeof(BroadcastComputors), f);
    fclose(f);
}
