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
#ifndef JOB_MAKER_BITCOIN_H_
#define JOB_MAKER_BITCOIN_H_

#include "JobMaker.h"

#include "rsk/RskWork.h"

#include <uint256.h>
#include <base58.h>

class JobMakerHandlerBitcoin : public JobMakerHandler {
  mutex lock_; // lock when update rawgbtMap_
  mutex auxJsonLock_;
  mutex rskWorkAccessLock_;

  // mining bitcoin blocks
  CTxDestination poolPayoutAddr_;
  uint32_t currBestHeight_;
  uint32_t lastJobSendTime_;
  bool isLastJobEmptyBlock_;
  std::map<uint64_t /* @see makeGbtKey() */, string>
      rawgbtMap_; // sorted gbt by timestamp
  deque<uint256> lastestGbtHash_;

  // merged mining for AuxPow blocks (example: Namecoin, ElastOS)
  string latestNmcAuxBlockJson_;
  string latestNmcAuxBlockHash_;
  uint32_t latestNmcAuxBlockHeight_;

  // merged mining for RSK
  RskWork *previousRskWork_;
  RskWork *currentRskWork_;
  bool isMergedMiningUpdate_; // a flag to mark RSK has an update

  bool addRawGbt(const string &msg);
  void clearTimeoutGbt();
  bool isReachTimeout();

  void clearTimeoutGw();
  bool triggerRskUpdate();

  // return false if there is no best rawGbt or
  // doesn't need to send a stratum job at current.
  bool findBestRawGbt(string &bestRawGbt);
  string makeStratumJob(const string &gbt);

  inline uint64_t
  makeGbtKey(uint32_t gbtTime, bool isEmptyBlock, uint32_t height);
  inline uint32_t gbtKeyGetTime(uint64_t gbtKey);
  inline uint32_t gbtKeyGetHeight(uint64_t gbtKey);
  inline bool gbtKeyIsEmptyBlock(uint64_t gbtKey);

public:
  JobMakerHandlerBitcoin();
  virtual ~JobMakerHandlerBitcoin() {}

  bool init(shared_ptr<JobMakerDefinition> def) override;
  virtual bool initConsumerHandlers(
      const string &kafkaBrokers,
      vector<JobMakerConsumerHandler> &handlers) override;

  bool processRawGbtMsg(const string &msg);
  bool processAuxPowMsg(const string &msg);
  bool processRskGwMsg(const string &msg);

  virtual string makeStratumJobMsg() override;

  // read-only definition
  inline shared_ptr<const GbtJobMakerDefinition> def() {
    return std::dynamic_pointer_cast<const GbtJobMakerDefinition>(def_);
  }
};

#endif
