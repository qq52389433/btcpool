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
#ifndef STRATUM_SESSION_ETH_H_
#define STRATUM_SESSION_ETH_H_

#include "StratumSession.h"


class StratumSessionEth : public StratumSession
{
public:
  enum class StratumProtocol {
    ETHPROXY,
    STRATUM,
    // @see https://www.nicehash.com/sw/Ethereum_specification_R1.txt
    NICEHASH_STRATUM
  };
  
  static const char* getProtocolString(const StratumProtocol protocol) {
    switch(protocol) {
      case StratumProtocol::ETHPROXY:
        return "ETHPROXY";
      case StratumProtocol::STRATUM:
        return "STRATUM";
      case StratumProtocol::NICEHASH_STRATUM:
        return "NICEHASH_STRATUM";
    }
    // should not be here
    return "UNKNOWN";
  }

  StratumSessionEth(evutil_socket_t fd, struct bufferevent *bev,
                    Server *server, struct sockaddr *saddr,
                    const int32_t shareAvgSeconds, const uint32_t extraNonce1);
  
protected:
  void responseError(const string &idStr, int code) override;
  void responseTrue(const string &idStr) override;

  void sendMiningNotify(shared_ptr<StratumJobEx> exJobPtr, bool isFirstJob=false) override;  
  void sendMiningNotifyWithId(shared_ptr<StratumJobEx> exJobPtr, const string &idStr);
  void handleRequest_Subscribe        (const string &idStr, const JsonNode &jparams) override;      
  void handleRequest_Submit           (const string &idStr, const JsonNode &jparams) override;         
  void handleRequest_Authorize(const string &idStr, const JsonNode &jparams, const JsonNode &jroot) override;   
  void handleRequest_GetWork(const string &idStr, const JsonNode &jparams) override; 
  void handleRequest_SubmitHashrate(const string &idStr, const JsonNode &jparams) override; 

  // Remove the Ethereum address prefix from worker's full name
  // 0x00d8c82Eb65124Ea3452CaC59B64aCC230AA3482.test.aaa -> test.aaa
  string stripEthAddrFromFullName(const string& fullNameStr);
private: 
  StratumProtocol ethProtocol_;
  // Record the difficulty of the last time sent to the miner in NICEHASH_STRATUM protocol.
  uint64_t nicehashLastSentDiff_;
};


#endif // STRATUM_SESSION_ETH_H_
