// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <txdb.h>

#include <chainparams.h>
#include <hash.h>
#include <random.h>
#include <pow.h>
#include <sidechain.h>
#include <uint256.h>
#include <util.h>
#include <utilstrencodings.h>
#include <ui_interface.h>
#include <init.h>
#include <validation.h>

#include <script/standard.h>
#include <base58.h>

#include <stdint.h>

#include <boost/thread.hpp>

static const char DB_COIN = 'C';
static const char DB_COINS = 'c';
static const char DB_BLOCK_FILES = 'f';
static const char DB_TXINDEX = 't';
static const char DB_BLOCK_INDEX = 'b';

static const char DB_BEST_BLOCK = 'B';
static const char DB_HEAD_BLOCKS = 'H';
static const char DB_FLAG = 'F';
static const char DB_REINDEX_FLAG = 'R';
static const char DB_LAST_BLOCK = 'l';

static const char DB_LOADED_COINS = 'p';

static const char DB_OP_RETURN = 'x';
static const char DB_OP_RETURN_TYPES = 'X';

namespace {

struct CoinEntry {
    COutPoint* outpoint;
    char key;
    explicit CoinEntry(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)), key(DB_COIN)  {}

    template<typename Stream>
    void Serialize(Stream &s) const {
        s << key;
        s << outpoint->hash;
        s << VARINT(outpoint->n);
    }

    template<typename Stream>
    void Unserialize(Stream& s) {
        s >> key;
        s >> outpoint->hash;
        s >> VARINT(outpoint->n);
    }
};

}

CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "chainstate", nCacheSize / 2, fMemory, fWipe, true) , loadedcoindb(GetDataDir() / "loadedcoins", nCacheSize / 2, fMemory, fWipe, true)
{
}

bool CCoinsViewDB::GetCoin(const COutPoint &outpoint, Coin &coin) const {
    if (db.Read(CoinEntry(&outpoint), coin))
        return true;

    LoadedCoin loadedCoin;
    if (GetLoadedCoin(outpoint.GetHash(), loadedCoin)) {
        coin = loadedCoin.coin;
        coin.fLoaded = true;
        return !loadedCoin.fSpent;
    }

    return false;
}

bool CCoinsViewDB::HaveCoin(const COutPoint &outpoint) const {
    if (db.Exists(CoinEntry(&outpoint)))
        return true;
    else
        return HaveLoadedCoin(outpoint.GetHash());
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!db.Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

std::vector<uint256> CCoinsViewDB::GetHeadBlocks() const {
    std::vector<uint256> vhashHeadBlocks;
    if (!db.Read(DB_HEAD_BLOCKS, vhashHeadBlocks)) {
        return std::vector<uint256>();
    }
    return vhashHeadBlocks;
}

bool CCoinsViewDB::BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock) {
    CDBBatch batch(db);
    size_t count = 0;
    size_t changed = 0;
    size_t batch_size = (size_t)gArgs.GetArg("-dbbatchsize", nDefaultDbBatchSize);
    int crash_simulate = gArgs.GetArg("-dbcrashratio", 0);
    assert(!hashBlock.IsNull());

    uint256 old_tip = GetBestBlock();
    if (old_tip.IsNull()) {
        // We may be in the middle of replaying.
        std::vector<uint256> old_heads = GetHeadBlocks();
        if (old_heads.size() == 2) {
            assert(old_heads[0] == hashBlock);
            old_tip = old_heads[1];
        }
    }

    // In the first batch, mark the database as being in the middle of a
    // transition from old_tip to hashBlock.
    // A vector is used for future extensibility, as we may want to support
    // interrupting after partial writes from multiple independent reorgs.
    batch.Erase(DB_BEST_BLOCK);
    batch.Write(DB_HEAD_BLOCKS, std::vector<uint256>{hashBlock, old_tip});

    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        // Skip loaded coins, we don't want them being written to the base
        if (it->second.coin.fLoaded) {
            it++;
            continue;
        }

        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            CoinEntry entry(&it->first);
            if (it->second.coin.IsSpent())
                batch.Erase(entry);
            else
                batch.Write(entry, it->second.coin);
            changed++;
        }
        count++;
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
        if (batch.SizeEstimate() > batch_size) {
            LogPrint(BCLog::COINDB, "Writing partial batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
            db.WriteBatch(batch);
            batch.Clear();
            if (crash_simulate) {
                static FastRandomContext rng;
                if (rng.randrange(crash_simulate) == 0) {
                    LogPrintf("Simulating a crash. Goodbye.\n");
                    _Exit(0);
                }
            }
        }
    }

    // In the last batch, mark the database as consistent with hashBlock again.
    batch.Erase(DB_HEAD_BLOCKS);
    batch.Write(DB_BEST_BLOCK, hashBlock);

    LogPrint(BCLog::COINDB, "Writing final batch of %.2f MiB\n", batch.SizeEstimate() * (1.0 / 1048576.0));
    bool ret = db.WriteBatch(batch);
    LogPrint(BCLog::COINDB, "Committed %u changed transaction outputs (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return ret;
}

size_t CCoinsViewDB::EstimateSize() const
{
    return db.EstimateSize(DB_COIN, (char)(DB_COIN+1));
}

bool CCoinsViewDB::WriteLoadedCoinIndex(const std::vector<LoadedCoin>& vLoadedCoin)
{
    CDBBatch batch(loadedcoindb);

    for (const LoadedCoin& c : vLoadedCoin) {
        std::pair<char, uint256> key = std::make_pair(DB_LOADED_COINS, c.out.GetHash());
        batch.Write(key, c);
    }

    return loadedcoindb.WriteBatch(batch);
}

bool CCoinsViewDB::WriteToLoadedCoinIndex(const LoadedCoin& coin)
{
    return WriteLoadedCoinIndex(std::vector<LoadedCoin>{ coin });
}

bool CCoinsViewDB::GetLoadedCoin(const uint256& hashOutPoint, LoadedCoin& coinOut) const
{
    std::unique_ptr<CDBIterator> pcursor(const_cast<CDBWrapper&>(loadedcoindb).NewIterator());
    pcursor->Seek(std::make_pair(DB_LOADED_COINS, hashOutPoint));
    if (pcursor->Valid()) {
        try {
            std::pair<char, uint256> key;
            pcursor->GetKey(key);

            if (key.second == hashOutPoint) {
                pcursor->GetValue(coinOut);
                return true;
            }
        } catch (const std::exception& e) {
           error("%s: %s", __func__, e.what());
        }
    }
    return false;
}

bool CCoinsViewDB::HaveLoadedCoin(const uint256& hashOutPoint) const
{
    LoadedCoin coin;
    return GetLoadedCoin(hashOutPoint, coin);
}

bool CCoinsViewDB::ReadLoadedCoins()
{
    fs::path path = GetDataDir() / "loaded_coins.dat";
    CAutoFile filein(fsbridge::fopen(path, "r"), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return false;
    }

    // TODO log this
    //uint64_t fileSize = fs::file_size(path);
    //uint64_t dataSize = fileSize - (sizeof(int) * 3);

    int read = 0;
    std::vector<LoadedCoin> vLoadedCoin;
    try {
        int nVersionRequired, nVersionThatWrote;
        filein >> nVersionRequired;
        filein >> nVersionThatWrote;
        if (nVersionRequired > CLIENT_VERSION) {
            LogPrintf("%s: version required greater than client version!\n",  __func__);
            return false;
        }

        int count = 0;
        filein >> count;
        for (int i = 0; i < count; i++) {
            if (i % 4000000 == 0) {
                // Write a batch of loaded coins to the index.
                // Batches are 4000000 coins each, which is around 400MB.
                WriteLoadedCoinIndex(vLoadedCoin);
                vLoadedCoin.clear();
            }

            LoadedCoin loadedCoin;
            filein >> loadedCoin;
            vLoadedCoin.push_back(loadedCoin);
            read++;
        }
        // Write final batch
        WriteLoadedCoinIndex(vLoadedCoin);
    }
    catch (const std::exception& e) {
        LogPrintf("%s: Exception: %s\n",  __func__, e.what());
        return false;
    }


    LogPrintf("%s: read: %u loaded coins.\n", __func__, read);

    return true;
}

std::vector<LoadedCoin> CCoinsViewDB::ReadMyLoadedCoins()
{
    std::vector<LoadedCoin> vLoadedCoin;

    fs::path path = GetDataDir() / "my_loaded_coins.dat";
    CAutoFile filein(fsbridge::fopen(path, "r"), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        return vLoadedCoin;
    }

    // TODO log this
    //uint64_t fileSize = fs::file_size(path);
    //uint64_t dataSize = fileSize - (sizeof(int) * 3);

    try {
        int nVersionRequired, nVersionThatWrote;
        filein >> nVersionRequired;
        filein >> nVersionThatWrote;
        if (nVersionRequired > CLIENT_VERSION) {
            return vLoadedCoin;
        }

        int count = 0;
        filein >> count;
        for (int i = 0; i < count; i++) {
            LoadedCoin loadedCoin;
            filein >> loadedCoin;
            vLoadedCoin.push_back(loadedCoin);
        }
    }
    catch (const std::exception& e) {
        LogPrintf("%s: Exception: %s\n",  __func__, e.what());
        return vLoadedCoin;
    }

    return vLoadedCoin;
}

void CCoinsViewDB::WriteMyLoadedCoins(const std::vector<LoadedCoin>& vLoadedCoin)
{
    if (!vLoadedCoin.size())
        return;
    int count = vLoadedCoin.size();

    // Write the coins
    fs::path path = GetDataDir() / "my_loaded_coins.dat";
    CAutoFile fileout(fsbridge::fopen(path, "w"), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull()) {
        return;
    }

    try {
        fileout << 210000; // version required to read: 0.21.00 or later
        fileout << CLIENT_VERSION; // version that wrote the file
        fileout << count; // Number of coins in file

        for (const LoadedCoin& c : vLoadedCoin) {
            fileout << c;
        }
    }
    catch (const std::exception& e) {
        LogPrintf("%s: Exception: %s\n",  __func__, e.what());
        return;
    }
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe) {
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(std::make_pair(DB_BLOCK_FILES, nFile), info);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write(DB_REINDEX_FLAG, '1');
    else
        return Erase(DB_REINDEX_FLAG);
}

bool CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    fReindexing = Exists(DB_REINDEX_FLAG);
    return true;
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

CCoinsViewCursor *CCoinsViewDB::Cursor() const
{
    CCoinsViewDBCursor *i = new CCoinsViewDBCursor(const_cast<CDBWrapper&>(db).NewIterator(), GetBestBlock());
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    i->pcursor->Seek(DB_COIN);
    // Cache key of first record
    if (i->pcursor->Valid()) {
        CoinEntry entry(&i->keyTmp.second);
        i->pcursor->GetKey(entry);
        i->keyTmp.first = entry.key;
    } else {
        i->keyTmp.first = 0; // Make sure Valid() and GetKey() return false
    }
    return i;
}

CCoinsViewLoadedCursor *CCoinsViewDB::LoadedCursor() const
{
    CCoinsViewLoadedDBCursor *i = new CCoinsViewLoadedDBCursor(const_cast<CDBWrapper&>(loadedcoindb).NewIterator());
    i->pcursor->Seek(DB_LOADED_COINS);
    return i;
}

bool CCoinsViewDBCursor::GetKey(COutPoint &key) const
{
    // Return cached key
    if (keyTmp.first == DB_COIN) {
        key = keyTmp.second;
        return true;
    }
    return false;
}

bool CCoinsViewDBCursor::GetValue(Coin &coin) const
{
    return pcursor->GetValue(coin);
}

unsigned int CCoinsViewDBCursor::GetValueSize() const
{
    return pcursor->GetValueSize();
}

bool CCoinsViewDBCursor::Valid() const
{
    return keyTmp.first == DB_COIN;
}

void CCoinsViewDBCursor::Next()
{
    pcursor->Next();
    CoinEntry entry(&keyTmp.second);
    if (!pcursor->Valid() || !pcursor->GetKey(entry)) {
        keyTmp.first = 0; // Invalidate cached key after last record so that Valid() and GetKey() return false
    } else {
        keyTmp.first = entry.key;
    }
}

bool CCoinsViewLoadedDBCursor::GetKey(std::pair<char, uint256>& key) const
{
    return pcursor->GetKey(key);
}

bool CCoinsViewLoadedDBCursor::GetValue(LoadedCoin &coin) const
{
    return pcursor->GetValue(coin);
}

bool CCoinsViewLoadedDBCursor::Valid() const
{
    return pcursor->Valid();
}

void CCoinsViewLoadedDBCursor::Next()
{
    pcursor->Next();
}

bool CBlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(std::make_pair(DB_TXINDEX, txid), pos);
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(std::make_pair(DB_TXINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? '1' : '0');
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

bool CBlockTreeDB::LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex)
{
    std::unique_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(std::make_pair(DB_BLOCK_INDEX, uint256()));

    // Load mapBlockIndex
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = insertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = insertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;

                // Copy Litecoin, skip PoW check when reading our own data for
                // performance reasons. This can be re-enabled but each block on
                // disk will need to be SHAndwich hashed again when read as we
                // only use the SHAndwich hash for PoW and then forget about it.
                //
                //if (!CheckProofOfWork(pindexNew->GetBlockHash(), pindexNew->nBits, consensusParams))
                //    return error("%s: CheckProofOfWork failed: %s", __func__, pindexNew->ToString());

                pcursor->Next();
            } else {
                return error("%s: failed to read value", __func__);
            }
        } else {
            break;
        }
    }

    return true;
}

CSidechainTreeDB::CSidechainTreeDB(size_t nCacheSize, bool fMemory, bool fWipe)
    : CDBWrapper(GetDataDir() / "blocks" / "sidechain", nCacheSize, fMemory, fWipe) { }

bool CSidechainTreeDB::WriteSidechainIndex(const std::vector<std::pair<uint256, const SidechainObj *> > &list)
{
    CDBBatch batch(*this);
    for (std::vector<std::pair<uint256, const SidechainObj *> >::const_iterator it=list.begin(); it!=list.end(); it++) {
        const uint256 &objid = it->first;
        const SidechainObj *obj = it->second;
        std::pair<char, uint256> key = std::make_pair(obj->sidechainop, objid);

        if (obj->sidechainop == DB_SIDECHAIN_BLOCK_OP) {
            const SidechainBlockData *ptr = (const SidechainBlockData *) obj;
            batch.Write(key, *ptr);
        }
    }

    return WriteBatch(batch, true);
}

bool CSidechainTreeDB::WriteSidechainBlockData(const std::pair<uint256, const SidechainBlockData>& data)
{
    CDBBatch batch(*this);
    std::pair<char, uint256> key = std::make_pair(data.second.sidechainop, data.first);
    batch.Write(key, data.second);

    return WriteBatch(batch, true);
}

bool CSidechainTreeDB::GetBlockData(const uint256& hashBlock, SidechainBlockData& data) const
{
    if (ReadSidechain(std::make_pair(DB_SIDECHAIN_BLOCK_OP, hashBlock), data))
        return true;

    return false;
}

bool CSidechainTreeDB::HaveBlockData(const uint256& hashBlock) const
{
    SidechainBlockData data;
    return GetBlockData(hashBlock, data);
}

OPReturnDB::OPReturnDB(size_t nCacheSize, bool fMemory, bool fWipe)
    : CDBWrapper(GetDataDir() / "blocks" / "opreturn", nCacheSize, fMemory, fWipe) { }

bool OPReturnDB::WriteBlockData(const std::pair<uint256, const std::vector<OPReturnData>>& data)
{
    CDBBatch batch(*this);
    std::pair<char, uint256> key = std::make_pair(DB_OP_RETURN, data.first);
    batch.Write(key, data.second);

    return WriteBatch(batch, true);
}

bool OPReturnDB::GetBlockData(const uint256& hashBlock, std::vector<OPReturnData>& vData) const
{
    return Read(std::make_pair(DB_OP_RETURN, hashBlock), vData);
}

bool OPReturnDB::HaveBlockData(const uint256& hashBlock) const
{
    std::vector<OPReturnData> vData;
    return GetBlockData(hashBlock, vData);
}

void OPReturnDB::GetNewsTypes(std::vector<NewsType>& vType)
{
    std::pair<char, uint256> key = std::make_pair(DB_OP_RETURN_TYPES, uint256());

    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(key);

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();

        NewsType type;
        if (pcursor->GetKey(key) && key.first == DB_OP_RETURN_TYPES) {
            if (pcursor->GetValue(type))
                vType.push_back(type);
        }

        pcursor->Next();
    }
}

void OPReturnDB::WriteNewsType(NewsType type)
{
    // Maybe in the future there will be different categories
    // of news types. If so, the pair.second can be used.
    CDBBatch batch(*this);
    batch.Write(std::make_pair(DB_OP_RETURN_TYPES, type.GetHash()), type);

    WriteBatch(batch, true);
}

void OPReturnDB::EraseNewsType(uint256 hash)
{
    Erase(std::make_pair(DB_OP_RETURN_TYPES, hash));
}

std::string NewsType::GetShareURL() const
{
    std::string str =
            std::to_string(nDays)
            + "{" +
            HexStr(header.begin(), header.end())
            + "}"
            + title;
    return str;
}

bool NewsType::SetURL(const std::string& strURL)
{
    if (strURL.size() < 12)
        return false;

    const size_t nFirst = strURL.find("{");
    const size_t nSecond = strURL.find("}");

    if (nFirst == std::string::npos || nSecond == std::string::npos)
        return false;
    if (nFirst == 0 || nFirst >= strURL.size())
        return false;
    if (nSecond >= strURL.size())
        return false;
    if (nFirst == nSecond)
        return false;

    // Get number of days
    try {
        nDays = std::stoi(strURL.substr(0, nFirst));
    } catch (...) {
        return false;
    }

    if (nDays == 0)
        return false;

    std::string strBytes = strURL.substr(nFirst + 1, 8);
    if (!IsHexNumber(strBytes))
        return false;

    // Get header bytes
    std::vector<unsigned char> vBytes = ParseHex(strBytes);
    header = CScript(vBytes.begin(), vBytes.end());
    if (header.size() != 4)
        return false;

    // Get title
    title = strURL.substr(nSecond + 1);
    if (title.empty())
        return false;

    return true;
}

namespace {

//! Legacy class to deserialize pre-pertxout database entries without reindex.
class CCoins
{
public:
    //! whether transaction is a coinbase
    bool fCoinBase;

    //! unspent transaction outputs; spent outputs are .IsNull(); spent outputs at the end of the array are dropped
    std::vector<CTxOut> vout;

    //! at which height this transaction was included in the active block chain
    int nHeight;

    //! empty constructor
    CCoins() : fCoinBase(false), vout(0), nHeight(0) { }

    template<typename Stream>
    void Unserialize(Stream &s) {
        unsigned int nCode = 0;
        // version
        int nVersionDummy;
        ::Unserialize(s, VARINT(nVersionDummy));
        // header code
        ::Unserialize(s, VARINT(nCode));
        fCoinBase = nCode & 1;
        std::vector<bool> vAvail(2, false);
        vAvail[0] = (nCode & 2) != 0;
        vAvail[1] = (nCode & 4) != 0;
        unsigned int nMaskCode = (nCode / 8) + ((nCode & 6) != 0 ? 0 : 1);
        // spentness bitmask
        while (nMaskCode > 0) {
            unsigned char chAvail = 0;
            ::Unserialize(s, chAvail);
            for (unsigned int p = 0; p < 8; p++) {
                bool f = (chAvail & (1 << p)) != 0;
                vAvail.push_back(f);
            }
            if (chAvail != 0)
                nMaskCode--;
        }
        // txouts themself
        vout.assign(vAvail.size(), CTxOut());
        for (unsigned int i = 0; i < vAvail.size(); i++) {
            if (vAvail[i])
                ::Unserialize(s, REF(CTxOutCompressor(vout[i])));
        }
        // coinbase height
        ::Unserialize(s, VARINT(nHeight));
    }
};

}

/** Upgrade the database from older formats.
 *
 * Currently implemented: from the per-tx utxo model (0.8..0.14.x) to per-txout.
 */
bool CCoinsViewDB::Upgrade() {
    std::unique_ptr<CDBIterator> pcursor(db.NewIterator());
    pcursor->Seek(std::make_pair(DB_COINS, uint256()));
    if (!pcursor->Valid()) {
        return true;
    }

    int64_t count = 0;
    LogPrintf("Upgrading utxo-set database...\n");
    LogPrintf("[0%%]...");
    uiInterface.ShowProgress(_("Upgrading UTXO database"), 0, true);
    size_t batch_size = 1 << 24;
    CDBBatch batch(db);
    int reportDone = 0;
    std::pair<unsigned char, uint256> key;
    std::pair<unsigned char, uint256> prev_key = {DB_COINS, uint256()};
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        if (ShutdownRequested()) {
            break;
        }
        if (pcursor->GetKey(key) && key.first == DB_COINS) {
            if (count++ % 256 == 0) {
                uint32_t high = 0x100 * *key.second.begin() + *(key.second.begin() + 1);
                int percentageDone = (int)(high * 100.0 / 65536.0 + 0.5);
                uiInterface.ShowProgress(_("Upgrading UTXO database"), percentageDone, true);
                if (reportDone < percentageDone/10) {
                    // report max. every 10% step
                    LogPrintf("[%d%%]...", percentageDone);
                    reportDone = percentageDone/10;
                }
            }
            CCoins old_coins;
            if (!pcursor->GetValue(old_coins)) {
                return error("%s: cannot parse CCoins record", __func__);
            }
            COutPoint outpoint(key.second, 0);
            for (size_t i = 0; i < old_coins.vout.size(); ++i) {
                if (!old_coins.vout[i].IsNull() && !old_coins.vout[i].scriptPubKey.IsUnspendable()) {
                    Coin newcoin(std::move(old_coins.vout[i]), old_coins.nHeight, old_coins.fCoinBase, false, false);
                    outpoint.n = i;
                    CoinEntry entry(&outpoint);
                    batch.Write(entry, newcoin);
                }
            }
            batch.Erase(key);
            if (batch.SizeEstimate() > batch_size) {
                db.WriteBatch(batch);
                batch.Clear();
                db.CompactRange(prev_key, key);
                prev_key = key;
            }
            pcursor->Next();
        } else {
            break;
        }
    }
    db.WriteBatch(batch);
    db.CompactRange({DB_COINS, uint256()}, key);
    uiInterface.ShowProgress("", 100, false);
    LogPrintf("[%s].\n", ShutdownRequested() ? "CANCELLED" : "DONE");
    return !ShutdownRequested();
}


