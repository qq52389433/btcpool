/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#include "JobMakerBitcoin.h"
#include "CommonBitcoin.h"
#include "StratumBitcoin.h"
#include "BitcoinUtils.h"

#include <iostream>
#include <stdlib.h>

#include <glog/logging.h>
#include <librdkafka/rdkafka.h>

#include <hash.h>
#include <script/script.h>
#include <uint256.h>
#include <util.h>

#ifdef INCLUDE_BTC_KEY_IO_H //
#include <key_io.h> //  IsValidDestinationString for bch is not in this file.
#endif

#include "utilities_js.hpp"
#include "Utils.h"

////////////////////////////////JobMakerHandlerBitcoin//////////////////////////////////
JobMakerHandlerBitcoin::JobMakerHandlerBitcoin()
  : currBestHeight_(0)
  , lastJobSendTime_(0)
  , isLastJobEmptyBlock_(false)
  , latestNmcAuxBlockHeight_(0)
  , previousRskWork_(nullptr)
  , currentRskWork_(nullptr)
  , isMergedMiningUpdate_(false) {}

bool JobMakerHandlerBitcoin::init(shared_ptr<JobMakerDefinition> defPtr) {
  JobMakerHandler::init(defPtr);

  // select chain
  if (def()->testnet_) {
    SelectParams(CBaseChainParams::TESTNET);
    LOG(WARNING) << "using bitcoin testnet3";
  } else {
    SelectParams(CBaseChainParams::MAIN);
  }

  LOG(INFO) << "Block Version: " << std::hex << def()->blockVersion_;
  LOG(INFO) << "Coinbase Info: " << def()->coinbaseInfo_;
  LOG(INFO) << "Payout Address: " << def()->payoutAddr_;

  // check pool payout address
  if (!BitcoinUtils::IsValidDestinationString(def()->payoutAddr_)) {
    LOG(ERROR) << "invalid pool payout address";
    return false;
  }
  poolPayoutAddr_ = BitcoinUtils::DecodeDestination(def()->payoutAddr_);

  return true;
}

bool JobMakerHandlerBitcoin::initConsumerHandlers(
    const string &kafkaBrokers, vector<JobMakerConsumerHandler> &handlers) {

  const int32_t consumeLatestN = 20;
  shared_ptr<KafkaConsumer> kafkaRawGbtConsumer;
  {
    auto messageProcessor = std::bind(
        &JobMakerHandlerBitcoin::processRawGbtMsg, this, std::placeholders::_1);
    auto handler = createConsumerHandler(
        kafkaBrokers,
        def()->rawGbtTopic_,
        consumeLatestN,
        {},
        messageProcessor);
    if (handler.kafkaConsumer_ == nullptr) return false;
    handlers.push_back(handler);
    kafkaRawGbtConsumer = handler.kafkaConsumer_;
  }

  shared_ptr<KafkaConsumer> kafkaAuxPowConsumer;
  {
    auto messageProcessor = std::bind(
        &JobMakerHandlerBitcoin::processAuxPowMsg, this, std::placeholders::_1);
    auto handler = createConsumerHandler(
        kafkaBrokers, def()->auxPowGwTopic_, 1, {}, messageProcessor);
    if (handler.kafkaConsumer_ == nullptr) return false;
    handlers.push_back(handler);
    kafkaAuxPowConsumer = handler.kafkaConsumer_;
  }

  shared_ptr<KafkaConsumer> kafkaRskGwConsumer;
  {
    auto messageProcessor = std::bind(
        &JobMakerHandlerBitcoin::processRskGwMsg, this, std::placeholders::_1);
    auto handler = createConsumerHandler(
        kafkaBrokers, def()->rskRawGwTopic_, 1, {}, messageProcessor);
    if (handler.kafkaConsumer_ == nullptr) return false;
    handlers.push_back(handler);
    kafkaRskGwConsumer = handler.kafkaConsumer_;
  }

  // sleep 3 seconds, wait for the latest N messages transfer from broker to
  // client
  sleep(3);

  /* pre-consume some messages for initialization */

  //
  // consume the latest AuxPow message
  //
  {
    rd_kafka_message_t *rkmessage;
    rkmessage = kafkaAuxPowConsumer->consumer(1000 /* timeout ms */);
    if (rkmessage != nullptr && !rkmessage->err) {
      string msg((const char *)rkmessage->payload, rkmessage->len);
      processAuxPowMsg(msg);
      rd_kafka_message_destroy(rkmessage);
    }
  }

  //
  // consume the latest RSK getwork message
  //
  {
    rd_kafka_message_t *rkmessage;
    rkmessage = kafkaRskGwConsumer->consumer(1000 /* timeout ms */);
    if (rkmessage != nullptr && !rkmessage->err) {
      string msg((const char *)rkmessage->payload, rkmessage->len);
      processRskGwMsg(msg);
      rd_kafka_message_destroy(rkmessage);
    }
  }

  //
  // consume the latest N RawGbt messages
  //
  LOG(INFO) << "consume latest rawgbt messages from kafka...";
  for (int32_t i = 0; i < consumeLatestN; i++) {
    rd_kafka_message_t *rkmessage;
    rkmessage = kafkaRawGbtConsumer->consumer(5000 /* timeout ms */);
    if (rkmessage == nullptr || rkmessage->err) { break; }
    string msg((const char *)rkmessage->payload, rkmessage->len);
    processRawGbtMsg(msg);
    rd_kafka_message_destroy(rkmessage);
  }
  LOG(INFO) << "consume latest rawgbt messages done";

  return true;
}

bool JobMakerHandlerBitcoin::addRawGbt(const string &msg) {
  JsonNode r;
  if (!JsonNode::parse(msg.c_str(), msg.c_str() + msg.size(), r)) {
    LOG(ERROR) << "parse rawgbt message to json fail";
    return false;
  }

  if (r["created_at_ts"].type() != Utilities::JS::type::Int ||
      r["block_template_base64"].type() != Utilities::JS::type::Str ||
      r["gbthash"].type() != Utilities::JS::type::Str) {
    LOG(ERROR) << "invalid rawgbt: missing fields";
    return false;
  }

  const uint256 gbtHash = uint256S(r["gbthash"].str());
  for (const auto &itr : lastestGbtHash_) {
    if (gbtHash == itr) {
      LOG(ERROR) << "duplicate gbt hash: " << gbtHash.ToString();
      return false;
    }
  }

  const uint32_t gbtTime = r["created_at_ts"].uint32();
  const int64_t timeDiff = (int64_t)time(nullptr) - (int64_t)gbtTime;
  if (labs(timeDiff) >= 60) {
    LOG(WARNING) << "rawgbt diff time is more than 60, ignore it";
    return false; // time diff too large, there must be some problems, so ignore
                  // it
  }
  if (labs(timeDiff) >= 3) {
    LOG(WARNING) << "rawgbt diff time is too large: " << timeDiff << " seconds";
  }

  const string gbt = DecodeBase64(r["block_template_base64"].str());
  assert(gbt.length() > 64); // valid gbt string's len at least 64 bytes

  JsonNode nodeGbt;
  if (!JsonNode::parse(gbt.c_str(), gbt.c_str() + gbt.length(), nodeGbt)) {
    LOG(ERROR) << "parse gbt message to json fail";
    return false;
  }
  assert(nodeGbt["result"]["height"].type() == Utilities::JS::type::Int);
  const uint32_t height = nodeGbt["result"]["height"].uint32();

#ifdef CHAIN_TYPE_BCH
  bool isLightVersion =
      nodeGbt["result"]["job_id"].type() == Utilities::JS::type::Str;
  bool isEmptyBlock = false;
  if (isLightVersion) {
    assert(nodeGbt["result"]["merkle"].type() == Utilities::JS::type::Array);
    isEmptyBlock = nodeGbt["result"]["merkle"].array().size() == 0;
  } else {
    assert(
        nodeGbt["result"]["transactions"].type() == Utilities::JS::type::Array);
    isEmptyBlock = nodeGbt["result"]["transactions"].array().size() == 0;
  }
#else
  assert(
      nodeGbt["result"]["transactions"].type() == Utilities::JS::type::Array);
  const bool isEmptyBlock =
      nodeGbt["result"]["transactions"].array().size() == 0;
#endif

  {
    ScopeLock sl(lock_);

    if (rawgbtMap_.size() > 0) {
      const uint64_t bestKey = rawgbtMap_.rbegin()->first;
      const uint32_t bestTime = gbtKeyGetTime(bestKey);
      const uint32_t bestHeight = gbtKeyGetHeight(bestKey);
      const bool bestIsEmpty = gbtKeyIsEmptyBlock(bestKey);

      // To prevent the job's block height ups and downs
      // when the block height of two bitcoind is not synchronized.
      // The block height downs must past twice the time of stratumJobInterval_
      // without the higher height GBT received.
      if (height < bestHeight && !bestIsEmpty &&
          gbtTime - bestTime < 2 * def()->jobInterval_) {
        LOG(WARNING) << "skip low height GBT. height: " << height
                     << ", best height: " << bestHeight
                     << ", elapsed time after best GBT: "
                     << (gbtTime - bestTime) << "s";
        return false;
      }
    }

    const uint64_t key = makeGbtKey(gbtTime, isEmptyBlock, height);
    if (rawgbtMap_.find(key) == rawgbtMap_.end()) {
      rawgbtMap_.insert(std::make_pair(key, gbt));
    } else {
      LOG(ERROR) << "key already exist in rawgbtMap: " << key;
    }
  }

  lastestGbtHash_.push_back(gbtHash);
  while (lastestGbtHash_.size() > 20) { lastestGbtHash_.pop_front(); }

  LOG(INFO) << "add rawgbt, height: " << height
            << ", gbthash: " << r["gbthash"].str().substr(0, 16)
            << "..., gbtTime(UTC): " << date("%F %T", gbtTime)
            << ", isEmpty:" << isEmptyBlock;

  return true;
}

bool JobMakerHandlerBitcoin::findBestRawGbt(string &bestRawGbt) {
  static uint64_t lastSendBestKey = 0;

  ScopeLock sl(lock_);

  // clean expired gbt first
  clearTimeoutGbt();
  clearTimeoutGw();

  if (rawgbtMap_.size() == 0) {
    LOG(WARNING) << "RawGbt Map is empty";
    return false;
  }

  bool isFindNewHeight = false;
  bool needUpdateEmptyBlockJob = false;

  // rawgbtMap_ is sorted gbt by (timestamp + height + emptyFlag),
  // so the last item is the newest/best item.
  // @see makeGbtKey()
  const uint64_t bestKey = rawgbtMap_.rbegin()->first;

  const uint32_t bestHeight = gbtKeyGetHeight(bestKey);
  const bool currentGbtIsEmpty = gbtKeyIsEmptyBlock(bestKey);

  if (bestKey == lastSendBestKey) {
    LOG(WARNING) << "bestKey is the same as last one: " << lastSendBestKey;
  }

  // if last job is an empty block job, we need to
  // send a new non-empty job as quick as possible.
  if (bestHeight == currBestHeight_ && isLastJobEmptyBlock_ &&
      !currentGbtIsEmpty) {
    needUpdateEmptyBlockJob = true;
    LOG(INFO) << "--------update last empty block job--------";
  }

  // The height cannot reduce in normal.
  // However, if there is indeed a height reduce,
  // isReachTimeout() will allow the new job sending.
  if (bestHeight > currBestHeight_) {
    LOG(INFO) << ">>>> found new best height: " << bestHeight
              << ", curr: " << currBestHeight_ << " <<<<";
    isFindNewHeight = true;
  }

  if (isFindNewHeight || needUpdateEmptyBlockJob || isMergedMiningUpdate_ ||
      isReachTimeout()) {
    lastSendBestKey = bestKey;
    currBestHeight_ = bestHeight;

    bestRawGbt = rawgbtMap_.rbegin()->second.c_str();
    return true;
  }

  return false;
}

bool JobMakerHandlerBitcoin::isReachTimeout() {
  uint32_t intervalSeconds = def()->jobInterval_;

  if (lastJobSendTime_ + intervalSeconds <= time(nullptr)) { return true; }
  return false;
}

void JobMakerHandlerBitcoin::clearTimeoutGbt() {
  // Maps (and sets) are sorted, so the first element is the smallest,
  // and the last element is the largest.

  const uint32_t ts_now = time(nullptr);

  // Ensure that rawgbtMap_ has at least one element, even if it expires.
  // So jobmaker can always generate jobs even if blockchain node does not
  // update the response of getblocktemplate for a long time when there is no
  // new transaction. This happens on SBTC v0.17.
  for (auto itr = rawgbtMap_.begin();
       rawgbtMap_.size() > 1 && itr != rawgbtMap_.end();) {
    const uint32_t ts = gbtKeyGetTime(itr->first);
    const bool isEmpty = gbtKeyIsEmptyBlock(itr->first);
    const uint32_t height = gbtKeyGetHeight(itr->first);

    // gbt expired time
    const uint32_t expiredTime =
        ts + (isEmpty ? def()->emptyGbtLifeTime_ : def()->gbtLifeTime_);

    if (expiredTime > ts_now) {
      // not expired
      ++itr;
    } else {
      // remove expired gbt
      LOG(INFO) << "remove timeout rawgbt: " << date("%F %T", ts) << "|" << ts
                << ", height:" << height
                << ", isEmptyBlock:" << (isEmpty ? 1 : 0);

      // c++11: returns an iterator to the next element in the map
      itr = rawgbtMap_.erase(itr);
    }
  }
}

void JobMakerHandlerBitcoin::clearTimeoutGw() {
  RskWork currentRskWork;
  RskWork previousRskWork;
  {
    ScopeLock sl(rskWorkAccessLock_);
    if (previousRskWork_ == nullptr || currentRskWork_ == nullptr) { return; }

    const uint32_t ts_now = time(nullptr);
    currentRskWork = *currentRskWork_;
    if (currentRskWork.getCreatedAt() + 120u < ts_now) {
      delete currentRskWork_;
      currentRskWork_ = nullptr;
    }

    previousRskWork = *previousRskWork_;
    if (previousRskWork.getCreatedAt() + 120u < ts_now) {
      delete previousRskWork_;
      previousRskWork_ = nullptr;
    }
  }
}

bool JobMakerHandlerBitcoin::triggerRskUpdate() {
  RskWork currentRskWork;
  RskWork previousRskWork;
  {
    ScopeLock sl(rskWorkAccessLock_);
    if (previousRskWork_ == nullptr || currentRskWork_ == nullptr) {
      return false;
    }
    currentRskWork = *currentRskWork_;
    previousRskWork = *previousRskWork_;
  }

  bool notifyFlagUpdate =
      def()->mergedMiningNotifyPolicy_ == 1 && currentRskWork.getNotifyFlag();
  bool differentHashUpdate = def()->mergedMiningNotifyPolicy_ == 2 &&
      (currentRskWork.getBlockHash() != previousRskWork.getBlockHash());

  return notifyFlagUpdate || differentHashUpdate;
}

bool JobMakerHandlerBitcoin::processRawGbtMsg(const string &msg) {
  DLOG(INFO) << "JobMakerHandlerBitcoin::processRawGbtMsg: " << msg;
  return addRawGbt(msg);
}

bool JobMakerHandlerBitcoin::processAuxPowMsg(const string &msg) {
  uint32_t currentNmcBlockHeight = 0;
  string currentNmcBlockHash;
  // get block height
  {
    JsonNode r;
    if (!JsonNode::parse(msg.data(), msg.data() + msg.size(), r)) {
      LOG(ERROR) << "parse NmcAuxBlock message to json fail";
      return false;
    }

    if (r["height"].type() != Utilities::JS::type::Int ||
        r["hash"].type() != Utilities::JS::type::Str) {
      LOG(ERROR) << "nmc auxblock fields failure";
      return false;
    }

    currentNmcBlockHeight = r["height"].uint32();
    currentNmcBlockHash = r["hash"].str();
  }

  uint32_t latestNmcAuxBlockHeight = 0;
  string latestNmcAuxBlockHash;
  // set json string
  {
    ScopeLock sl(auxJsonLock_);
    // backup old height / hash
    latestNmcAuxBlockHeight = latestNmcAuxBlockHeight_;
    latestNmcAuxBlockHash = latestNmcAuxBlockHash_;
    // update height / hash
    latestNmcAuxBlockHeight_ = currentNmcBlockHeight;
    latestNmcAuxBlockHash_ = currentNmcBlockHash;
    // update json
    latestNmcAuxBlockJson_ = msg;
    DLOG(INFO) << "latestAuxPowJson: " << latestNmcAuxBlockJson_;
  }

  bool higherHeightUpdate = def()->mergedMiningNotifyPolicy_ == 1 &&
      currentNmcBlockHeight > latestNmcAuxBlockHeight;
  bool differentHashUpdate = def()->mergedMiningNotifyPolicy_ == 2 &&
      currentNmcBlockHash != latestNmcAuxBlockHash;

  isMergedMiningUpdate_ = higherHeightUpdate || differentHashUpdate;
  return isMergedMiningUpdate_;
}

bool JobMakerHandlerBitcoin::processRskGwMsg(const string &rawGetWork) {
  // set json string
  {
    ScopeLock sl(rskWorkAccessLock_);

    RskWork *rskWork = new RskWork();
    if (rskWork->initFromGw(rawGetWork)) {

      if (previousRskWork_ != nullptr) {
        delete previousRskWork_;
        previousRskWork_ = nullptr;
      }

      previousRskWork_ = currentRskWork_;
      currentRskWork_ = rskWork;

      DLOG(INFO) << "currentRskBlockJson: " << rawGetWork;
    } else {
      delete rskWork;
    }
  }

  isMergedMiningUpdate_ = triggerRskUpdate();
  return isMergedMiningUpdate_;
}

string JobMakerHandlerBitcoin::makeStratumJob(const string &gbt) {
  DLOG(INFO) << "JobMakerHandlerBitcoin::makeStratumJob gbt: " << gbt;
  string latestNmcAuxBlockJson;
  {
    ScopeLock sl(auxJsonLock_);
    latestNmcAuxBlockJson = latestNmcAuxBlockJson_;
  }

  RskWork currentRskBlockJson;
  {
    ScopeLock sl(rskWorkAccessLock_);
    if (currentRskWork_ != nullptr) { currentRskBlockJson = *currentRskWork_; }
  }

  StratumJobBitcoin sjob;
  if (!sjob.initFromGbt(
          gbt.c_str(),
          def()->coinbaseInfo_,
          poolPayoutAddr_,
          def()->blockVersion_,
          latestNmcAuxBlockJson,
          currentRskBlockJson,
          def()->serverId_,
          isMergedMiningUpdate_)) {
    LOG(ERROR) << "init stratum job message from gbt str fail";
    return "";
  }
  const string jobMsg = sjob.serializeToJson();

  // set last send time
  // TODO: fix Y2K38 issue
  lastJobSendTime_ = (uint32_t)time(nullptr);

  // is an empty block job
  isLastJobEmptyBlock_ = sjob.isEmptyBlock();

  LOG(INFO) << "--------producer stratum job, jobId: " << sjob.jobId_
            << ", height: " << sjob.height_ << "--------";
  LOG(INFO) << "sjob: " << jobMsg;

  isMergedMiningUpdate_ = false;
  return jobMsg;
}

string JobMakerHandlerBitcoin::makeStratumJobMsg() {
  string bestRawGbt;
  if (!findBestRawGbt(bestRawGbt)) { return ""; }
  return makeStratumJob(bestRawGbt);
}

uint64_t JobMakerHandlerBitcoin::makeGbtKey(
    uint32_t gbtTime, bool isEmptyBlock, uint32_t height) {
  assert(height < 0x7FFFFFFFU);

  //
  // gbtKey:
  //  -----------------------------------------------------------------------------------------
  // |               32 bits               |               31 bits | 1 bit | |
  // xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx | xxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx |
  // x            | |               gbtTime               |               height
  // | nonEmptyFlag |
  //  -----------------------------------------------------------------------------------------
  // use nonEmptyFlag (aka: !isEmptyBlock) so the key of a non-empty block
  // will large than the key of an empty block.
  //
  return (((uint64_t)gbtTime) << 32) | (((uint64_t)height) << 1) |
      ((uint64_t)(!isEmptyBlock));
}

uint32_t JobMakerHandlerBitcoin::gbtKeyGetTime(uint64_t gbtKey) {
  return (uint32_t)(gbtKey >> 32);
}

uint32_t JobMakerHandlerBitcoin::gbtKeyGetHeight(uint64_t gbtKey) {
  return (uint32_t)((gbtKey >> 1) & 0x7FFFFFFFULL);
}

bool JobMakerHandlerBitcoin::gbtKeyIsEmptyBlock(uint64_t gbtKey) {
  return !((bool)(gbtKey & 1ULL));
}
