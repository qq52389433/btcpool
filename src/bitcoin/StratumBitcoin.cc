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
#include "StratumBitcoin.h"

#include "BitcoinUtils.h"

#include <core_io.h>
#include <hash.h>
#include <script/script.h>
#include <uint256.h>
#include <util.h>
#include <pubkey.h>
#include <streams.h>

#include "Utils.h"
#include <glog/logging.h>

#include <boost/endian/buffers.hpp>

static void
makeMerkleBranch(const vector<uint256> &vtxhashs, vector<uint256> &steps) {
  if (vtxhashs.size() == 0) { return; }
  vector<uint256> hashs(vtxhashs.begin(), vtxhashs.end());
  while (hashs.size() > 1) {
    // put first element
    steps.push_back(*hashs.begin());
    if (hashs.size() % 2 == 0) {
      // if even, push_back the end one, size should be an odd number.
      // because we ignore the coinbase tx when make merkle branch.
      hashs.push_back(*hashs.rbegin());
    }
    // ignore the first one than merge two
    for (size_t i = 0; i < (hashs.size() - 1) / 2; i++) {
      // Hash = Double SHA256
      hashs[i] = Hash(
          BEGIN(hashs[i * 2 + 1]),
          END(hashs[i * 2 + 1]),
          BEGIN(hashs[i * 2 + 2]),
          END(hashs[i * 2 + 2]));
    }
    hashs.resize((hashs.size() - 1) / 2);
  }
  assert(hashs.size() == 1);
  steps.push_back(*hashs.begin()); // put the last one
}

static int64_t findExtraNonceStart(
    const vector<char> &coinbaseOriTpl, const vector<char> &placeHolder) {
  // find for the end
  for (int64_t i = coinbaseOriTpl.size() - placeHolder.size(); i >= 0; i--) {
    if (memcmp(&coinbaseOriTpl[i], &placeHolder[0], placeHolder.size()) == 0) {
      return i;
    }
  }
  return -1;
}

StratumJobBitcoin::StratumJobBitcoin()
  : height_(0)
  , nVersion_(0)
  , nBits_(0U)
  , nTime_(0U)
  , minTime_(0U)
  , coinbaseValue_(0)
  , nmcAuxBits_(0u)
  , isMergedMiningCleanJob_(false) {}

string StratumJobBitcoin::serializeToJson() const {
  string merkleBranchStr;
  merkleBranchStr.reserve(merkleBranch_.size() * 64 + 1);
  for (size_t i = 0; i < merkleBranch_.size(); i++) {
    merkleBranchStr.append(merkleBranch_[i].ToString());
  }

  //
  // we use key->value json string, so it's easy to update system
  //
  return Strings::Format(
      "{\"jobId\":%" PRIu64
      ",\"gbtHash\":\"%s\""
      ",\"prevHash\":\"%s\",\"prevHashBeStr\":\"%s\""
      ",\"height\":%d,\"coinbase1\":\"%s\",\"coinbase2\":\"%s\""
      ",\"merkleBranch\":\"%s\""
      ",\"nVersion\":%d,\"nBits\":%u,\"nTime\":%u"
      ",\"minTime\":%u,\"coinbaseValue\":%lld"
      ",\"witnessCommitment\":\"%s\""
#ifdef CHAIN_TYPE_UBTC
      ",\"rootStateHash\":\"%s\""
#endif
      // namecoin, optional
      ",\"nmcBlockHash\":\"%s\",\"nmcBits\":%u,\"nmcHeight\":%d"
      ",\"nmcRpcAddr\":\"%s\",\"nmcRpcUserpass\":\"%s\""
      // RSK, optional
      ",\"rskBlockHashForMergedMining\":\"%s\",\"rskNetworkTarget\":\"0x%s\""
      ",\"rskFeesForMiner\":\"%s\""
      ",\"rskdRpcAddress\":\"%s\",\"rskdRpcUserPwd\":\"%s\""
      // namecoin and RSK
      // TODO: delete isRskCleanJob (keep it for forward compatible).
      ",\"isRskCleanJob\":%s,\"mergedMiningClean\":%s"
      "}",
      jobId_,
      gbtHash_.c_str(),
      prevHash_.ToString().c_str(),
      prevHashBeStr_.c_str(),
      height_,
      coinbase1_.c_str(),
      coinbase2_.c_str(),
      // merkleBranch_ could be empty
      merkleBranchStr.size() ? merkleBranchStr.c_str() : "",
      nVersion_,
      nBits_,
      nTime_,
      minTime_,
      coinbaseValue_,
      witnessCommitment_.size() ? witnessCommitment_.c_str() : "",
#ifdef CHAIN_TYPE_UBTC
      rootStateHash_.size() ? rootStateHash_.c_str() : "",
#endif
      // nmc
      nmcAuxBlockHash_.ToString().c_str(),
      nmcAuxBits_,
      nmcHeight_,
      nmcRpcAddr_.size() ? nmcRpcAddr_.c_str() : "",
      nmcRpcUserpass_.size() ? nmcRpcUserpass_.c_str() : "",
      // rsk
      blockHashForMergedMining_.size() ? blockHashForMergedMining_.c_str() : "",
      rskNetworkTarget_.GetHex().c_str(),
      feesForMiner_.size() ? feesForMiner_.c_str() : "",
      rskdRpcAddress_.size() ? rskdRpcAddress_.c_str() : "",
      rskdRpcUserPwd_.c_str() ? rskdRpcUserPwd_.c_str() : "",
      isMergedMiningCleanJob_ ? "true" : "false",
      isMergedMiningCleanJob_ ? "true" : "false");
}

bool StratumJobBitcoin::unserializeFromJson(const char *s, size_t len) {
  JsonNode j;
  if (!JsonNode::parse(s, s + len, j)) { return false; }
  if (j["jobId"].type() != Utilities::JS::type::Int ||
      j["gbtHash"].type() != Utilities::JS::type::Str ||
      j["prevHash"].type() != Utilities::JS::type::Str ||
      j["prevHashBeStr"].type() != Utilities::JS::type::Str ||
      j["height"].type() != Utilities::JS::type::Int ||
      j["coinbase1"].type() != Utilities::JS::type::Str ||
      j["coinbase2"].type() != Utilities::JS::type::Str ||
      j["merkleBranch"].type() != Utilities::JS::type::Str ||
      j["nVersion"].type() != Utilities::JS::type::Int ||
      j["nBits"].type() != Utilities::JS::type::Int ||
      j["nTime"].type() != Utilities::JS::type::Int ||
      j["minTime"].type() != Utilities::JS::type::Int ||
      j["coinbaseValue"].type() != Utilities::JS::type::Int) {
    LOG(ERROR) << "parse stratum job failure: " << s;
    return false;
  }

  jobId_ = j["jobId"].uint64();
  gbtHash_ = j["gbtHash"].str();
  prevHash_ = uint256S(j["prevHash"].str());
  prevHashBeStr_ = j["prevHashBeStr"].str();
  height_ = j["height"].int32();
  coinbase1_ = j["coinbase1"].str();
  coinbase2_ = j["coinbase2"].str();
  nVersion_ = j["nVersion"].int32();
  nBits_ = j["nBits"].uint32();
  nTime_ = j["nTime"].uint32();
  minTime_ = j["minTime"].uint32();
  coinbaseValue_ = j["coinbaseValue"].int64();

  // witnessCommitment, optional
  // witnessCommitment must be at least 38 bytes
  if (j["witnessCommitment"].type() == Utilities::JS::type::Str &&
      j["witnessCommitment"].str().length() >= 38 * 2) {
    witnessCommitment_ = j["witnessCommitment"].str();
  }

#ifdef CHAIN_TYPE_UBTC
  // rootStateHash, optional
  // rootStateHash must be at least 2 bytes (00f9, empty root state hash)
  if (j["rootStateHash"].type() == Utilities::JS::type::Str &&
      j["rootStateHash"].str().length() >= 2 * 2) {
    rootStateHash_ = j["rootStateHash"].str();
  }
#endif

  // for Namecoin and RSK merged mining, optional
  if (j["mergedMiningClean"].type() == Utilities::JS::type::Bool) {
    isMergedMiningCleanJob_ = j["mergedMiningClean"].boolean();
  }

  //
  // namecoin, optional
  //
  if (j["nmcBlockHash"].type() == Utilities::JS::type::Str &&
      j["nmcBits"].type() == Utilities::JS::type::Int &&
      j["nmcHeight"].type() == Utilities::JS::type::Int &&
      j["nmcRpcAddr"].type() == Utilities::JS::type::Str &&
      j["nmcRpcUserpass"].type() == Utilities::JS::type::Str) {
    nmcAuxBlockHash_ = uint256S(j["nmcBlockHash"].str());
    nmcAuxBits_ = j["nmcBits"].uint32();
    nmcHeight_ = j["nmcHeight"].int32();
    nmcRpcAddr_ = j["nmcRpcAddr"].str();
    nmcRpcUserpass_ = j["nmcRpcUserpass"].str();
    BitsToTarget(nmcAuxBits_, nmcNetworkTarget_);
  }

  //
  // RSK, optional
  //
  if (j["rskBlockHashForMergedMining"].type() == Utilities::JS::type::Str &&
      j["rskNetworkTarget"].type() == Utilities::JS::type::Str &&
      j["rskFeesForMiner"].type() == Utilities::JS::type::Str &&
      j["rskdRpcAddress"].type() == Utilities::JS::type::Str &&
      j["rskdRpcUserPwd"].type() == Utilities::JS::type::Str) {
    blockHashForMergedMining_ = j["rskBlockHashForMergedMining"].str();
    rskNetworkTarget_ = uint256S(j["rskNetworkTarget"].str());
    feesForMiner_ = j["rskFeesForMiner"].str();
    rskdRpcAddress_ = j["rskdRpcAddress"].str();
    rskdRpcUserPwd_ = j["rskdRpcUserPwd"].str();
  }

  const string merkleBranchStr = j["merkleBranch"].str();
  const size_t merkleBranchCount = merkleBranchStr.length() / 64;
  merkleBranch_.resize(merkleBranchCount);
  for (size_t i = 0; i < merkleBranchCount; i++) {
    merkleBranch_[i] = uint256S(merkleBranchStr.substr(i * 64, 64));
  }

  BitsToTarget(nBits_, networkTarget_);

  return true;
}

bool StratumJobBitcoin::initFromGbt(
    const char *gbt,
    const string &poolCoinbaseInfo,
    const CTxDestination &poolPayoutAddr,
    const uint32_t blockVersion,
    const string &nmcAuxBlockJson,
    const RskWork &latestRskBlockJson,
    const uint8_t serverId,
    const bool isMergedMiningUpdate) {
  uint256 gbtHash = Hash(gbt, gbt + strlen(gbt));
  JsonNode r;
  if (!JsonNode::parse(gbt, gbt + strlen(gbt), r)) {
    LOG(ERROR) << "decode gbt json fail: >" << gbt << "<";
    return false;
  }
  JsonNode jgbt = r["result"];

  // jobId: timestamp + gbtHash, we need to make sure jobId is unique in a some
  // time jobId can convert to uint64_t
  auto hash =
      reinterpret_cast<boost::endian::little_uint32_buf_t *>(gbtHash.begin());
  jobId_ = (static_cast<uint64_t>(time(nullptr)) << 32) |
      (hash->value() & 0xFFFFFF00) | serverId;

  gbtHash_ = gbtHash.ToString();

  // height etc.
  // fields in gbt json has already checked by GbtMaker
  prevHash_ = uint256S(jgbt["previousblockhash"].str());
  height_ = jgbt["height"].int32();
  if (blockVersion != 0) {
    nVersion_ = blockVersion;
  } else {
    nVersion_ = jgbt["version"].uint32();
  }
  nBits_ = jgbt["bits"].uint32_hex();
  nTime_ = jgbt["curtime"].uint32();
  minTime_ = jgbt["mintime"].uint32();
  coinbaseValue_ = jgbt["coinbasevalue"].int64();

  // default_witness_commitment must be at least 38 bytes
  if (jgbt["default_witness_commitment"].type() == Utilities::JS::type::Str &&
      jgbt["default_witness_commitment"].str().length() >= 38 * 2) {
    witnessCommitment_ = jgbt["default_witness_commitment"].str();
  }

#ifdef CHAIN_TYPE_UBTC
  // rootStateHash, optional
  // default_root_state_hash must be at least 2 bytes (00f9, empty root state
  // hash)
  if (jgbt["default_root_state_hash"].type() == Utilities::JS::type::Str &&
      jgbt["default_root_state_hash"].str().length() >= 2 * 2) {
    rootStateHash_ = jgbt["default_root_state_hash"].str();
  }
#endif

  BitsToTarget(nBits_, networkTarget_);

  // previous block hash
  // we need to convert to little-endian
  // 00000000000000000328e9fea9914ad83b7404a838aa66aefb970e5689c2f63d
  // 89c2f63dfb970e5638aa66ae3b7404a8a9914ad80328e9fe0000000000000000
  for (int i = 0; i < 8; i++) {
    uint32_t a = *(uint32_t *)(BEGIN(prevHash_) + i * 4);
    a = HToBe(a);
    prevHashBeStr_ += HexStr(BEGIN(a), END(a));
  }

#ifdef CHAIN_TYPE_BCH
  bool isLightVersion = jgbt["job_id"].type() == Utilities::JS::type::Str;
  // merkle branch, merkleBranch_ could be empty
  if (isLightVersion) {
    auto &gbtMerkle = jgbt["merkle"].array();
    for (auto &mHex : gbtMerkle) {
      uint256 m;
      m.SetHex(mHex.str().c_str());
      merkleBranch_.push_back(m);
    }
  } else
#endif
  // merkle branch, merkleBranch_ could be empty
  {
    // read txs hash/data
    vector<uint256> vtxhashs; // txs without coinbase
    for (JsonNode &node : jgbt["transactions"].array()) {
      CMutableTransaction tx;
      DecodeHexTx(tx, node["data"].str());
      vtxhashs.push_back(MakeTransactionRef(std::move(tx))->GetHash());
    }
    // make merkleSteps and merkle branch
    makeMerkleBranch(vtxhashs, merkleBranch_);
  }

  // for Namecoin and RSK merged mining
  isMergedMiningCleanJob_ = isMergedMiningUpdate;

  //
  // namecoin merged mining
  //
  if (!nmcAuxBlockJson.empty()) {
    do {
      JsonNode jNmcAux;
      if (!JsonNode::parse(
              nmcAuxBlockJson.c_str(),
              nmcAuxBlockJson.c_str() + nmcAuxBlockJson.length(),
              jNmcAux)) {
        LOG(ERROR) << "decode nmc auxblock json fail: >" << nmcAuxBlockJson
                   << "<";
        break;
      }
      // check fields created_at_ts
      if (jNmcAux["created_at_ts"].type() != Utilities::JS::type::Int ||
          jNmcAux["hash"].type() != Utilities::JS::type::Str ||
          jNmcAux["merkle_size"].type() != Utilities::JS::type::Int ||
          jNmcAux["merkle_nonce"].type() != Utilities::JS::type::Int ||
          jNmcAux["height"].type() != Utilities::JS::type::Int ||
          jNmcAux["bits"].type() != Utilities::JS::type::Str ||
          jNmcAux["rpc_addr"].type() != Utilities::JS::type::Str ||
          jNmcAux["rpc_userpass"].type() != Utilities::JS::type::Str) {
        LOG(ERROR) << "nmc auxblock fields failure";
        break;
      }
      // check timestamp
      if (jNmcAux["created_at_ts"].uint32() + 60u < time(nullptr)) {
        LOG(ERROR) << "too old nmc auxblock: "
                   << date("%F %T", jNmcAux["created_at_ts"].uint32());
        break;
      }

      // set nmc aux info
      nmcAuxBlockHash_ = uint256S(jNmcAux["hash"].str());
      nmcAuxMerkleSize_ = jNmcAux["merkle_size"].int32();
      nmcAuxMerkleNonce_ = jNmcAux["merkle_nonce"].int32();
      nmcAuxBits_ = jNmcAux["bits"].uint32_hex();
      nmcHeight_ = jNmcAux["height"].int32();
      nmcRpcAddr_ = jNmcAux["rpc_addr"].str();
      nmcRpcUserpass_ = jNmcAux["rpc_userpass"].str();
      BitsToTarget(nmcAuxBits_, nmcNetworkTarget_);
    } while (0);
  }

  //
  // rsk merged mining
  //
  if (latestRskBlockJson.isInitialized()) {

    // set rsk info
    blockHashForMergedMining_ = latestRskBlockJson.getBlockHash();
    rskNetworkTarget_ = uint256S(latestRskBlockJson.getTarget());
    feesForMiner_ = latestRskBlockJson.getFees();
    rskdRpcAddress_ = latestRskBlockJson.getRpcAddress();
    rskdRpcUserPwd_ = latestRskBlockJson.getRpcUserPwd();
  }

  // make coinbase1 & coinbase2
  {
    CTxIn cbIn;
    //
    // block height, 4 bytes in script: 0x03xxxxxx
    // https://github.com/bitcoin/bips/blob/master/bip-0034.mediawiki
    // https://github.com/bitcoin/bitcoin/pull/1526
    //
    cbIn.scriptSig = CScript();
    cbIn.scriptSig << (uint32_t)height_;

    // add current timestamp to coinbase tx input, so if the block's merkle root
    // hash is the same, there's no risk for miners to calc the same space.
    // https://github.com/btccom/btcpool/issues/5
    //
    // 5 bytes in script: 0x04xxxxxxxx.
    // eg. 0x0402363d58 -> 0x583d3602 = 1480406530 = 2016-11-29 16:02:10
    //
    cbIn.scriptSig << CScriptNum((uint32_t)time(nullptr));

    // pool's info
    cbIn.scriptSig.insert(
        cbIn.scriptSig.end(), poolCoinbaseInfo.begin(), poolCoinbaseInfo.end());

    //
    // put namecoin merged mining info, 44 bytes
    // https://en.bitcoin.it/wiki/Merged_mining_specification
    //
    if (nmcAuxBits_ != 0u) {
      string merkleSize, merkleNonce;
      Bin2Hex((uint8_t *)&nmcAuxMerkleSize_, 4, merkleSize);
      Bin2Hex((uint8_t *)&nmcAuxMerkleNonce_, 4, merkleNonce);
      string mergedMiningCoinbase = Strings::Format(
          "%s%s%s%s",
          // magic: 0xfa, 0xbe, 0x6d('m'), 0x6d('m')
          "fabe6d6d",
          // block_hash: Hash of the AuxPOW block header
          nmcAuxBlockHash_.ToString().c_str(),
          merkleSize.c_str(), // merkle_size : 1
          merkleNonce.c_str() // merkle_nonce: 0
      );
      vector<char> mergedMiningBin;
      Hex2Bin(mergedMiningCoinbase.c_str(), mergedMiningBin);
      assert(mergedMiningBin.size() == (12 + 32));
      cbIn.scriptSig.insert(
          cbIn.scriptSig.end(), mergedMiningBin.begin(), mergedMiningBin.end());
    }

#ifdef USER_DEFINED_COINBASE
    // reserved for user defined coinbase info
    string userCoinbaseInfoPadding;
    userCoinbaseInfoPadding.resize(USER_DEFINED_COINBASE_SIZE, '\x20');
    cbIn.scriptSig.insert(
        cbIn.scriptSig.end(),
        userCoinbaseInfoPadding.begin(),
        userCoinbaseInfoPadding.end());
#endif

    //  placeHolder: extra nonce1 (4bytes) + extra nonce2 (8bytes)
    const vector<char> placeHolder(4 + 8, 0xEE);
    // pub extra nonce place holder
    cbIn.scriptSig.insert(
        cbIn.scriptSig.end(), placeHolder.begin(), placeHolder.end());

    // 100: coinbase script sig max len, range: (2, 100).
    //
    // bitcoind/src/main.cpp: CheckTransaction()
    //   if (tx.IsCoinBase())
    //   {
    //     if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() >
    //     100)
    //       return state.DoS(100, false, REJECT_INVALID, "bad-cb-length");
    //   }
    //
    if (cbIn.scriptSig.size() >= 100) {
      LOG(FATAL) << "coinbase input script size over than 100, shold < 100";
      return false;
    }

    // coinbase outputs
    vector<CTxOut> cbOut;

    //
    // output[0]: pool payment address
    //
    {
      CTxOut paymentTxOut;
      paymentTxOut.scriptPubKey = GetScriptForDestination(poolPayoutAddr);

      paymentTxOut.nValue = AMOUNT_TYPE(coinbaseValue_);

      cbOut.push_back(paymentTxOut);
    }

    //
    // output[1] (optional): witness commitment
    //
    if (!witnessCommitment_.empty()) {
      DLOG(INFO) << "witness commitment: " << witnessCommitment_.c_str();
      vector<char> binBuf;
      Hex2Bin(witnessCommitment_.c_str(), binBuf);

      CTxOut witnessTxOut;
      witnessTxOut.scriptPubKey = CScript(
          (unsigned char *)binBuf.data(),
          (unsigned char *)binBuf.data() + binBuf.size());
      witnessTxOut.nValue = AMOUNT_TYPE(0);

      cbOut.push_back(witnessTxOut);
    }

#ifdef CHAIN_TYPE_UBTC
    //
    // output[2] (optional): root state hash of UB smart contract
    //
    if (!rootStateHash_.empty()) {
      DLOG(INFO) << "root state hash: " << rootStateHash_.c_str();
      vector<char> binBuf;
      Hex2Bin(rootStateHash_.c_str(), binBuf);

      CTxOut rootStateTxOut;
      rootStateTxOut.scriptPubKey = CScript(
          (unsigned char *)binBuf.data(),
          (unsigned char *)binBuf.data() + binBuf.size());
      rootStateTxOut.nValue = 0;

      cbOut.push_back(rootStateTxOut);
    }
#endif

    //
    // output[3] (optional): RSK merge mining
    //
    if (latestRskBlockJson.isInitialized()) {
      DLOG(INFO) << "RSK blockhash: " << blockHashForMergedMining_;
      string rskBlockTag =
          "\x52\x53\x4B\x42\x4C\x4F\x43\x4B\x3A"; // "RSKBLOCK:"
      vector<char> rskTag(rskBlockTag.begin(), rskBlockTag.end());
      vector<char> binBuf;

      Hex2Bin(blockHashForMergedMining_.c_str(), binBuf);

      rskTag.insert(std::end(rskTag), std::begin(binBuf), std::end(binBuf));

      CTxOut rskTxOut;
      rskTxOut.scriptPubKey = CScript(
          (unsigned char *)rskTag.data(),
          (unsigned char *)rskTag.data() + rskTag.size());
      rskTxOut.nValue = AMOUNT_TYPE(0);

      cbOut.push_back(rskTxOut);
    }

    CMutableTransaction cbtx;
    cbtx.vin.push_back(cbIn);
    cbtx.vout = cbOut;

    vector<char> coinbaseTpl;
    {
      CSerializeData sdata;
      CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
      ssTx << cbtx; // put coinbase CTransaction to CDataStream
      ssTx.GetAndClear(sdata); // dump coinbase bin to coinbaseTpl
      coinbaseTpl.insert(coinbaseTpl.end(), sdata.begin(), sdata.end());
    }

    // check coinbase tx size
    if (coinbaseTpl.size() >= COINBASE_TX_MAX_SIZE) {
      LOG(FATAL) << "coinbase tx size " << coinbaseTpl.size()
                 << " is over than max " << COINBASE_TX_MAX_SIZE;
      return false;
    }

    const int64_t extraNonceStart =
        findExtraNonceStart(coinbaseTpl, placeHolder);
    coinbase1_ = HexStr(&coinbaseTpl[0], &coinbaseTpl[extraNonceStart]);
    coinbase2_ = HexStr(
        &coinbaseTpl[extraNonceStart + placeHolder.size()],
        &coinbaseTpl[coinbaseTpl.size()]);
  }

  return true;
}

bool StratumJobBitcoin::isEmptyBlock() {
  return merkleBranch_.size() == 0 ? true : false;
}
