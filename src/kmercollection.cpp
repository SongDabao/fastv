#include "kmercollection.h"
#include "util.h"
#include <sstream>
#include <string.h>
#include "kmer.h"

const long HASH_LENGTH = (1L<<30);

KmerCollection::KmerCollection(string filename, Options* opt)
{
    mOptions = opt;
    mHashCounts = new uint32[HASH_LENGTH];
    memset(mHashCounts, 0, sizeof(uint32)*HASH_LENGTH);
    mFilename = filename;
    mNumber = 0;
    mIdBits = 0;
    mIdMask = 0;
    mCountMax = 0;
    mStatDone = false;
    init();
}

KmerCollection::~KmerCollection()
{
    if(mHashCounts) {
        delete mHashCounts;
        mHashCounts = NULL;
    }

    if (mZipped){
        if (mZipFile){
            gzclose(mZipFile);
            mZipFile = NULL;
        }
    }
    else {
        if (mFile.is_open()){
            mFile.close();
        }
    }
}

bool KmerCollection::getLine(char* line, int maxLine){
    bool status = true;
    if(mZipped)
        status = gzgets(mZipFile, line, maxLine);
    else {
        mFile.getline(line, maxLine);
        status = !mFile.fail();
    }

    // trim \n, \r or \r\n in the tail
    int readed = strlen(line);
    if(readed >=2 ){
        if(line[readed-1] == '\n' || line[readed-1] == '\r'){
            line[readed-1] = '\0';
            if(line[readed-2] == '\r')
                line[readed-2] = '\0';
        }
    }

    return status;
}

bool desc_comp (int i,int j) { return (i>j); }

void KmerCollection::stat(){
    vector<vector<int>> kmerHits(mNumber);
    for(int i=0; i<HASH_LENGTH; i++) {
        uint32 val = mHashCounts[i];

        if(val == 0 ||  val == COLLISION_FLAG)
            continue;

        uint32 id, count;
        unpackIdCount(val, id, count);

        if(id == 0 || id > mNumber)
            error_exit("Wrong ID");

        if(count>0) {
            mHits[id-1]+=count;
            kmerHits[id-1].push_back(count);
        }
    }

    for(int id=0; id<mNumber; id++){
        if(mKmerCounts[id] ==  0) {
            mMedianHits[id]=0;
            mMeanHits[id]=0.0;
            mCoverage[id]=0.0;
        } else{
            int medianPos = (mKmerCounts[id]+1)/2;
            if(medianPos >= kmerHits[id].size())
                mMedianHits[id] = 0;
            else {
                nth_element(kmerHits[id].begin(), kmerHits[id].begin()+medianPos, kmerHits[id].end(), desc_comp);
                mMedianHits[id] = kmerHits[id][medianPos];
            }
            mMeanHits[id] = (double)mHits[id]/(double)mKmerCounts[id];
            mCoverage[id] = (double)kmerHits[id].size()/(double)mKmerCounts[id];
        }
    }

    mStatDone = true;
}

bool KmerCollection::add(uint64 kmer64) {
    uint64 kmerhash = makeHash(kmer64);
    uint32 hashcount = mHashCounts[kmerhash];
    if(hashcount != 0 &&  hashcount != COLLISION_FLAG) {
        uint32 id;
        uint32 count;
        unpackIdCount(hashcount, id, count);
        if(count == mCountMax)
            return true;
        else
            count++;
        uint32 newHashcount  = packIdCount(id, count);
        mHashCounts[kmerhash] = newHashcount;
        return true;
    } else
        return false;
}

void KmerCollection::init()
{
    if (ends_with(mFilename, ".fasta.gz") || ends_with(mFilename, ".fa.gz")){
        mZipFile = gzopen(mFilename.c_str(), "r");
        mZipped = true;
    }
    else if(ends_with(mFilename, ".fasta") || ends_with(mFilename, ".fa")){
        mFile.open(mFilename.c_str(), ifstream::in);
        mZipped = false;
    } else {
        error_exit("Not a FASTA file: " + mFilename);
    }

    const int maxLine = 1000;
    char line[maxLine];
    if (mZipped){
        if (mZipFile == NULL)
            return ;
    }

    int collision = 0;
    int total = 0;
    bool initialized = false;
    while(true) {
        if(eof())
            break;
        getLine(line, maxLine);
        string linestr(line);
        if(linestr.empty() || line[0]=='#') {
            continue;
        }
        if(line[0]=='>') {
            if(total > 0) {
                //cerr << collision << "/" << total << endl;
                mKmerCounts.push_back(total);
                total=0;
                collision=0;
            }
            mNames.push_back(linestr.substr(1, linestr.length() - 1));
            mHits.push_back(0);
            mMeanHits.push_back(0.0);
            mCoverage.push_back(0.0);
            mMedianHits.push_back(0);
            mNumber++;
            //cerr<<mNumber<<": " << linestr << endl;
            continue;
        }

        string& seq = linestr;
        if(!initialized) {
            initialized = true;
            if(mOptions->kmerKeyLen == 0)
                mOptions->kmerKeyLen = seq.length();

            if(mOptions->kmerKeyLen > 32)
                error_exit("KMER key length cannot be >32: " + seq);
        }

        if(seq.length() != mOptions->kmerKeyLen) {
            cerr << "KMER length must be " << mOptions->kmerKeyLen << ", skipped " << seq << endl;
            continue;
        }

        bool valid = true;
        uint64 kmer64 = Kmer::seq2uint64(seq, 0, seq.length(), valid);
        if(valid) {
            total++;
            uint64 kmerhash = makeHash(kmer64);
            if(mHashCounts[kmerhash] ==0) {
                mHashCounts[kmerhash] = mNumber;
            } else if(mHashCounts[kmerhash] != mNumber && mHashCounts[kmerhash]!= COLLISION_FLAG) {
                mHashCounts[kmerhash] = COLLISION_FLAG;
                collision++;
            }
        }
    }
    mKmerCounts.push_back(total);

    makeBitAndMask();
}

bool KmerCollection::eof() {
    if (mZipped) {
        return gzeof(mZipFile);
    } else {
        return mFile.eof();
    }
}

uint64 KmerCollection::makeHash(uint64 key) {
    return (1713137323 * key + (key>>12)*7341234131 + (key>>24)*371371377) & (HASH_LENGTH-1);
}

void KmerCollection::report() {
    if(!mStatDone)
        stat();
    for(int i=0; i<mNumber; i++) {
        if(mCoverage[i]>0.5) {
            cerr << i+1 << " " << mNames[i] << ", hits: " << mHits[i] << "/ median hits: " << mMedianHits[i] << "/ mean hits: " << mMeanHits[i] << "/ coverage: " << mCoverage[i] << endl;
        }
    }
}

void KmerCollection::reportJSON(ofstream& ofs) {
}

void KmerCollection::makeBitAndMask() {
    int bits=1;
    uint32 mask =0x01;

    while(true){
        if(bits == 32) {
            error_exit("Too many contigs in: " + mFilename);
        }
        if(mask > mNumber)
            break;
        bits++;
        mask= (mask << 1) + 1;
    }
    mIdBits = bits;
    mIdMask = mask;
    mCountMax  = (0xFFFFFFFFFFF >> mIdBits);
}

uint32 KmerCollection::packIdCount(uint32 id, uint32 count) {
    uint32 data = 0;
    data |= (count << mIdBits); // high bits: count
    data |= id; // low bits: id
    return data;
}

void KmerCollection::unpackIdCount(uint32 data, uint32& id, uint32& count) {
    count = data >> mIdBits;
    id = data & mIdMask;
}
