// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/transaction.h>

#include <hash.h>
#include <sidechain.h>
#include <tinyformat.h>
#include <utilstrencodings.h>

std::string COutPoint::ToString() const
{
    return strprintf("COutPoint(%s, %u)", hash.ToString().substr(0,10), n);
}

uint256 COutPoint::GetHash() const
{
    return SerializeHash(*this);
}

CTxIn::CTxIn(COutPoint prevoutIn, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = prevoutIn;
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

CTxIn::CTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn, uint32_t nSequenceIn)
{
    prevout = COutPoint(hashPrevTx, nOut);
    scriptSig = scriptSigIn;
    nSequence = nSequenceIn;
}

std::string CTxIn::ToString() const
{
    std::string str;
    str += "CTxIn(";
    str += prevout.ToString();
    if (prevout.IsNull())
        str += strprintf(", coinbase %s", HexStr(scriptSig));
    else
        str += strprintf(", scriptSig=%s", HexStr(scriptSig).substr(0, 24));
    if (nSequence != SEQUENCE_FINAL)
        str += strprintf(", nSequence=%u", nSequence);
    str += ")";
    return str;
}

CTxOut::CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn)
{
    nValue = nValueIn;
    scriptPubKey = scriptPubKeyIn;
}

std::string CTxOut::ToString() const
{
    return strprintf("CTxOut(nValue=%d.%08d, scriptPubKey=%s)", nValue / COIN, nValue % COIN, HexStr(scriptPubKey).substr(0, 30));
}

CMutableTransaction::CMutableTransaction() : nVersion(CTransaction::CURRENT_VERSION), nLockTime(0) {}
CMutableTransaction::CMutableTransaction(const CTransaction& tx) : vin(tx.vin), vout(tx.vout), criticalData(tx.criticalData), nVersion(tx.nVersion), nLockTime(tx.nLockTime) {}
uint256 CMutableTransaction::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, SERIALIZE_TRANSACTION_NO_WITNESS | SERIALIZE_TRANSACTION_NO_DRIVECHAIN);
}

uint256 CTransaction::ComputeHash() const
{
    return SerializeHash(*this, SER_GETHASH, SERIALIZE_TRANSACTION_NO_WITNESS | SERIALIZE_TRANSACTION_NO_DRIVECHAIN);
}

uint256 CTransaction::GetWitnessHash() const
{
    if (!HasWitness() && nVersion != 3) {
        return GetHash();
    } else {
        return SerializeHash(*this, SER_GETHASH, SERIALIZE_TRANSACTION_NO_DRIVECHAIN);
    }
}

bool CTransaction::GetBlindHash(uint256& hashRet) const
{
    CMutableTransaction mtx(*this);
    if (!mtx.vin.size() || !mtx.vout.size())
        return false;

    // Remove the CTIP scriptSig (set to OP_0 as the sidechain must orignally)
    mtx.vin.clear();
    mtx.vin.resize(1);
    mtx.vin[0].scriptSig = CScript() << OP_0;

    // Remove the sidechain change return
    mtx.vout.pop_back();

    // We now have the blind withdrawal hash
    hashRet = mtx.GetHash();

    return true;
}

CAmount CTransaction::GetBlindValueOut() const
{
    CMutableTransaction mtx(*this);
    if (!mtx.vin.size() || !mtx.vout.size())
        return false;

    // Remove the CTIP scriptSig (set to OP_0 as the sidechain must orignally)
    mtx.vin.clear();
    mtx.vin.resize(1);
    mtx.vin[0].scriptSig = CScript() << OP_0;

    // Remove the sidechain change return
    mtx.vout.pop_back();

    return CTransaction(mtx).GetValueOut();
}

/* For backward compatibility, the hash is initialized to 0. TODO: remove the need for this default constructor entirely. */
CTransaction::CTransaction() : vin(), vout(), criticalData(), nVersion(CTransaction::CURRENT_VERSION), nLockTime(0), hash() {}
CTransaction::CTransaction(const CMutableTransaction &tx) : vin(tx.vin), vout(tx.vout), criticalData(tx.criticalData), nVersion(tx.nVersion), nLockTime(tx.nLockTime), hash(ComputeHash()) {}
CTransaction::CTransaction(CMutableTransaction &&tx) : vin(std::move(tx.vin)), vout(std::move(tx.vout)), criticalData(tx.criticalData), nVersion(tx.nVersion), nLockTime(tx.nLockTime), hash(ComputeHash()) {}

CAmount CTransaction::GetValueOut() const
{
    CAmount nValueOut = 0;
    for (const auto& tx_out : vout) {
        nValueOut += tx_out.nValue;
        if (!MoneyRange(tx_out.nValue) || !MoneyRange(nValueOut))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nValueOut;
}

unsigned int CTransaction::GetTotalSize() const
{
    return ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
}

std::string CTransaction::ToString() const
{
    std::string str;
    str += strprintf("CTransaction(hash=%s, ver=%d, vin.size=%u, vout.size=%u, nLockTime=%u)\n",
        GetHash().ToString().substr(0,10),
        nVersion,
        vin.size(),
        vout.size(),
        nLockTime);
    for (unsigned int i = 0; i < vin.size(); i++)
        str += "    " + vin[i].ToString() + "\n";
    for (unsigned int i = 0; i < vin.size(); i++)
        str += "    " + vin[i].scriptWitness.ToString() + "\n";
    for (unsigned int i = 0; i < vout.size(); i++)
        str += "    " + vout[i].ToString() + "\n";
    if (!criticalData.IsNull()) {
        str += strprintf("Critical Data:\nvBytes.size=%s\nhashCritical=%s",
        criticalData.vBytes.size(),
        criticalData.hashCritical.ToString());
    }
    return str;
}

bool CCriticalData::IsBMMRequest() const
{
    uint8_t nSidechain;
    std::string strPrevBytes = "";

    return IsBMMRequest(nSidechain, strPrevBytes);
}

bool CCriticalData::IsBMMRequest(uint8_t& nSidechain, std::string& strPrevBlock) const
{
    if (IsNull())
        return false;
    if (hashCritical.IsNull())
        return false;
    if (vBytes.size() != 8)
        return false;

    if (vBytes[0] != 0x00 || vBytes[1] != 0xbf || vBytes[2] != 0x00)
        return false;

    nSidechain = vBytes[3];

    // Read prev block bytes
    std::vector<unsigned char> vPrevBytes;
    vPrevBytes = std::vector<unsigned char>(vBytes.begin() + 4, vBytes.end());
    if (vPrevBytes.size() != 4)
        return false;

    strPrevBlock = HexStr(vPrevBytes);
    if (strPrevBlock.size() != 8)
        return false;

    return true;
}
