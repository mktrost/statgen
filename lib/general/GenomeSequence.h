/*
 *  Copyright (C) 2010  Regents of the University of Michigan
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GENOME_SEQUENCE_H
#define _GENOME_SEQUENCE_H

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>              // for size_t, which md5.h needs (for redhat)
#include <stddef.h>             // for size_t, which md5.h needs (for correct systems)
#include <sys/types.h>
#include <sys/stat.h>
#if defined(WIN32)
#define MD5_DIGEST_LENGTH 16
#else
#include <openssl/md5.h>        // for MD5_DIGEST_LENGTH
#endif
#include <string>
#include <vector>
#include "MemoryMapArray.h"

// Goncalo's String class
#include "StringArray.h"

#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#include <stdint.h>

typedef uint32_t    genomeIndex_t;
#define INVALID_GENOME_INDEX UINT32_MAX

// chromosome index is just a signed int, so this is ok here:
#define INVALID_CHROMOSOME_INDEX -1


//
// ChromosomeInfo represents the per chromosome information
// necessary to write out SAM/BAM records.  In addition, it
// contains a single internal index used to point to the vector
// offset where the chromosome bases start.
//
// This is mildly non-optimal for larger collections of chromosomes
// or contigs - one use case described having millions of contigs,
// in which case, this structure alone would take a gigabyte or more.
//
struct ChromosomeInfo
{
    static const int  MAX_GENOME_INFO_STRING=128;

    void constructorClear()
    {
        memset(this,0, sizeof(*this));
    }
    void setChromosomeName(const char *n)
    {
        strncpy(name, n, sizeof(name)-1);
    }
    genomeIndex_t   start;                              // internal offset to combined genome vector
    genomeIndex_t   size;                               // SAM SQ:LN value
    char            md5[2*MD5_DIGEST_LENGTH + 1];       // 32 chars plus NUL, SAM SQ:M5 value
    char            name[MAX_GENOME_INFO_STRING];       // SAM SQ:SN value
    char            assemblyID[MAX_GENOME_INFO_STRING]; // SAM SQ:AS value
    char            uri[MAX_GENOME_INFO_STRING];        // SAM SQ:UR value
    char            species[MAX_GENOME_INFO_STRING];    // SAM SQ:SP value

    // handy setting methods:
    void setAssemblyID(const char *newID)
    {
        strncpy(assemblyID, newID, sizeof(assemblyID) - 1);
    }
    void setSpecies(const char *newSpecies)
    {
        strncpy(species, newSpecies, sizeof(newSpecies));
    }
    void setURI(const char *newURI)
    {
        strncpy(uri, newURI, sizeof(uri));
    }
};

class genomeSequenceMmapHeader : public MemoryMapArrayHeader
{
    friend class GenomeSequence;
    friend std::ostream &operator << (std::ostream &, genomeSequenceMmapHeader &);
private:
    uint32_t    _chromosomeCount;
    bool        _colorSpace;

    ChromosomeInfo  _chromosomes[0];

public:
    //
    // getHeaderSize is special in that it must not access any
    // member variables, since it is called when the header has
    // not been created yet.
    //
    static size_t  getHeaderSize(int chromosomeCount)
    {
        return sizeof(genomeSequenceMmapHeader) + sizeof(ChromosomeInfo[1]) * chromosomeCount;
    }
    //
    // below methods return TRUE if it failed, false otherwise (primarily
    // a length check).
    //
};

std::ostream &operator << (std::ostream &stream, genomeSequenceMmapHeader &h);

//
// define the genomeSequence array type:
//
// NB the access/set routines use the encoded base values in the range
// 0-15, not the corresponding base pair character.
//
inline uint8_t genomeSequenceAccess(void *base, genomeIndex_t index)
{
    if ((index&1)==0)
    {
        return ((uint8_t *) base)[index>>1] & 0xf;
    }
    else
    {
        return (((uint8_t *) base)[index>>1] & 0xf0) >> 4;
    }
};
inline void genomeSequenceSet(void *base, genomeIndex_t index, uint8_t v)
{
    if ((index&1)==0)
    {
        ((uint8_t *) base)[index>>1] = (((uint8_t *) base)[index>>1] & 0xf0) | v;
    }
    else
    {
        ((uint8_t *) base)[index>>1] = (((uint8_t *) base)[index>>1] & 0x0f) | v<<4;
    }
}

inline size_t mmapGenomeSequenceElementCount2Bytes(genomeIndex_t i)
{
    return sizeof(uint8_t) * i / 2;
}

#define UMFA_COOKIE 0x1b7933a1  // unique cookie id
#define UMFA_VERSION 20100401U  // YYYYMMDD of last change to file layout

typedef MemoryMapArray<
uint8_t,
genomeIndex_t,
UMFA_COOKIE,
UMFA_VERSION,
genomeSequenceAccess,
genomeSequenceSet,
mmapGenomeSequenceElementCount2Bytes,
genomeSequenceMmapHeader
> genomeSequenceArray;

class PackedRead
{
    void set(int index, int val)
    {
        packedBases[index>>1] =
            (packedBases[index>>1]             // original value
             & ~(7<<((index&0x01)<<2)))         // logical AND off the original value
            | ((val&0x0f)<<((index&0x1)<<2));  // logical OR in the new value
    }
public:
    std::vector<uint8_t> packedBases;
    uint32_t    length;
    int size()
    {
        return length;
    }
    void clear()
    {
        packedBases.clear();
        length=0;
    }
    void set(const char *rhs, int padWithNCount = 0);
    uint8_t operator [](int index)
    {
        return (packedBases[index>>1] >> ((index&0x1)<<2)) & 0xf;
    }
};

// std::string &operator = (std::string &lhs, const PackedRead &rhs);
//

//!  Create/Access/Modify/Load Genome Sequences stored as binary mapped files.
/*!
  GenomeSequence is designed to be a high performance shared access
  reference object.

  It is implemented as a MemoryMapArray template object with unsigned 8
  bit ints, each of which stores two bases.  Although 2 bits could be used,
  most references have more than four symbols (usually at least including
  'N', indicating an unknown or masked out base).

  Normal use of this class follows these steps:
   -# create the reference
    -# instantiate the GenomeSequence class object
    -# create the actual file (memory mapped) that is to hold the data
    -# populate the data using GenomeSequence::set
   -# use the reference
    -# use the reference by instantiating a GenomeSequence object
    -# either use the constructor with the reference filename
    -# or use GenomeSequence::setReferenceName() followed by ::open
    -# access the bases via the overloaded array operator []
    -# check sequence length by using GenomeSequence::getNumberBases()
   -# accessing chromosomes in the reference
    -# you typically will need to know about the chromosomes in the sequence
    -# see methods and docs with prefix 'getChromosome'

  Sharing is accomplished using the mmap() function via the MemoryMap
  base class.  This allows a potentially large genome reference to be
  shared among a number of simultaneously executing instances of one or
  more programs sharing the same reference.
 */


class GenomeSequence : public genomeSequenceArray
{
public:
    static const int baseAIndex = 000;
    static const int baseTIndex = 001;
    static const int baseCIndex = 002;
    static const int baseGIndex = 003;
    static const int baseNIndex = 004; // baseN --> bad read
    static const int baseXIndex = 005; // baseX --> unknown (bad data)
    //
    // two arrays for converting back and forth between base pair character
    // value (ASCII) to a base integer in the range 0..3.  Note there is actually
    // a value 4 and 5, for 'N' (indelible) and 'M' (unknown to me).
    //
    static unsigned char base2int[];        // base space read (ATCG)
    static const char int2base[];
    static const char int2colorSpace[];
    static unsigned char base2complement[];

private:
    int                     _debugFlag;

    std::ostream            *_progressStream;
    bool                    _colorSpace;

    std::string             _baseFilename;   // for later use by WordIndex create and open
    std::string             _referenceFilename;
    std::string             _fastaFilename;
    std::string             _umfaFilename;
    std::string             _application;        // only used in ::create()

    MemoryMap               _umfaFile;

    void setup(const char *referenceFilename);

public:
    /// Simple constructor - no implicit file open
    GenomeSequence();
    void constructorClear();
    /// \brief attempt to open an existing sequence
    ///
    /// \param referenceFilename the name of the reference fasta file to open
    /// \param debug if true, additional debug information is printed
    GenomeSequence(std::string &referenceFilename)
    {
        constructorClear();
        setup(referenceFilename.c_str());
    }

    /// Smarter constructor - attempt to open an existing sequence
    ///
    /// \param referenceFilename the name of the reference fasta file to open
    /// \param debug if true, additional debug information is printed
    GenomeSequence(const char *referenceFilename)
    {
        constructorClear();
        setup(referenceFilename);
    }

    /// Close the file if open and destroy the object
    ~GenomeSequence();

    /// open the reference specified using GenomeSequence::setReferenceName
    ///
    /// \param isColorSpace open the color space reference
    /// \param flags pass through to the ::open() call (O_RDWR lets you modify the contents)
    /// \return false for success, true otherwise
    bool open(bool isColorSpace = false, int flags = O_RDONLY);

    /// open the given file as the genome (no filename munging occurs).
    ///
    /// \param filename the name of the file to open
    /// \param flags pass through to the ::open() call (O_RDWR lets you modify the contents)
    /// \return false for success, true otherwise
    bool open(const char *filename, int flags = O_RDONLY)
    {
        _umfaFilename = filename;
        return genomeSequenceArray::open(filename, flags);
    }

    /// if true, then show progress when creating and pre-fetching
private:
    bool    _searchCommonFileSuffix;
public:
    bool create();
    //
    // backwards compatability:
    bool create(bool isColor) {setColorSpace(isColor); return create(); }

    // NEW API?

    // load time modifiers:
    void setProgressStream(std::ostream &progressStream) {_progressStream = &progressStream;}
    void setColorSpace(bool colorSpace) {_colorSpace = colorSpace; }
    void setSearchCommonFileSuffix(bool searchCommonFileSuffix) {_searchCommonFileSuffix = searchCommonFileSuffix;}

    bool loadFastaData(const char *filename);

    /// set the reference name that will be used in open()
    /// \param referenceFilename the name of the reference fasta file to open
    /// \return false for success, true otherwise
    ///
    /// \sa open()
    bool setReferenceName(std::string referenceFilename);

    /// set the application name in the binary file header
    ///
    /// \param application name of the application
    void setApplication(std::string application)
    {
        _application = application;     // used in ::create() to set application name
    }
    const std::string &getFastaName()
    {
        return _fastaFilename;
    }
    const std::string &getReferenceName()
    {
        return _referenceFilename;
    }

    /// tell us if we are a color space reference or not
    /// \return true if colorspace, false otherwise
    bool isColorSpace()
    {
        return _colorSpace;
    }

    /// return the number of bases represented in this reference
    /// \return count of bases
    genomeIndex_t   getNumberBases()
    {
        return getElementCount();
    }

    /// given a whole genome index, get the chromosome it is located in
    ///
    /// This is done via a binary search of the chromosome table in the
    /// header of the mapped file, so it is O(log(N))
    ///
    /// \param 0-based position the base in the genome
    /// \return 0-based index into chromosome table - INVALID_CHROMOSOME_INDEX if error
    int getChromosome(genomeIndex_t position);
    /// given a chromosome name, return the chromosome index
    ///
    /// This is done via a linear search of the chromosome table in the
    /// header of the mapped file, so it is O(N)
    ///
    /// \param chromosomeName the name of the chromosome - exact match only
    /// \return 0-based index into chromosome table - INVALID_CHROMOSOME_INDEX if error
    int getChromosome(const char *chromosomeName);
    /// Return the number of chromosomes in the genome
    /// \return number of chromosomes in the genome
    int getChromosomeCount();

    /// given a chromosome, return the genome base it starts in
    ///
    /// \param 0-based chromosome index
    /// \return 0-based genome index of the base that starts the chromosome
    genomeIndex_t getChromosomeStart(int chromosomeIndex)
    {
        if (chromosomeIndex==INVALID_CHROMOSOME_INDEX) return INVALID_GENOME_INDEX;
        return header->_chromosomes[chromosomeIndex].start;
    }

    /// given a chromosome, return its size in bases
    ///
    /// \param 0-based chromosome index
    /// \return size of the chromosome in bases
    genomeIndex_t getChromosomeSize(int chromosomeIndex)
    {
        if (chromosomeIndex==INVALID_CHROMOSOME_INDEX) return 0;
        return header->_chromosomes[chromosomeIndex].size;
    }

    /// given a chromosome name and position, return the genome position
    ///
    /// \param chromosomeName name of the chromosome - exact match only
    /// \param chromosomeIndex 1-based chromosome position
    /// \return genome index of the above chromosome position
    genomeIndex_t getGenomePosition(
        const char *chromosomeName,
        unsigned int chromosomeIndex);

    /// given a chromosome index and position, return the genome position
    ///
    /// \param chromosome index of the chromosome
    /// \param chromosomeIndex 1-based chromosome position
    /// \return genome index of the above chromosome position
    genomeIndex_t getGenomePosition(
        int chromosome,
        unsigned int chromosomeIndex);

    /// given the chromosome name, get the corresponding 0 based genome index
    /// for the start of that chromosome
    genomeIndex_t getGenomePosition(const char *chromosomeName);
    genomeIndex_t getGenomePosition(int chromosomeIndex);

    const std::string &getBaseFilename()
    {
        return _baseFilename;
    }
    const char *getChromosomeName(int chromosomeIndex)
    {
        return header->_chromosomes[chromosomeIndex].name;
    }

    void setDebugFlag(bool d)
    {
        _debugFlag = d;
    }

    genomeIndex_t sequenceLength()
    {
        return (genomeIndex_t) header->elementCount;
    }
    const char *chromosomeName(int chr)
    {
        return header->_chromosomes[chr].name;
    }

    void sanityCheck(MemoryMap &fasta);
    std::string IntegerToSeq(unsigned int n, unsigned int wordsize);

    bool wordMatch(unsigned int index, std::string &word);
    bool printNearbyWords(unsigned int index, unsigned int variance, std::string &word);
    char BasePair(char c)
    {
        return base2complement[(int) c];
    }
    void dumpSequenceSAMDictionary(std::ostream&);
    void dumpHeaderTSV(std::ostream&);
    void loadHeaderFromTSV(std::istream&);

    ///
    /// Return the bases in base space or color space for within range index, ot
    /// @param index the array-like index (0 based).
    /// @return ACTGN in base space; 0123N for color space; and 'N' for invalid.
    /// For color space, index i represents the transition of base at position (i-1) to base at position i
    ///
    /// NB: bounds checking here needs to be deprecated - do not assume it
    /// will exist - the call must clip reads so that this routine is never
    /// called with a index value larger than the genome.
    ///
    /// The reason for this is simply that this routine gets called hundreds
    /// of billions of time in one run of karma, which will absolutely kill
    /// performance.  Every single instruction here matters a great, great deal.
    ///

    //
    // 3.5% improvement for color space matching:
    // I guess the compiler isn't inlining 3 functions deep.
    //

#if 0
    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // The following code does not work even in the base space,
    // since the memory layout has changed. USE IT WITH CAUTIOUS!
    //
    // This block of code is a functional duplicate of the following
    // code - leave this here for reference and possibly later
    // performance testing as well as compiler evaluation.
    inline char operator[](genomeIndex_t index)
    {
        return int2base[(*((genomeSequenceArray*) this))[index]];
    }
#endif

    inline char operator[](genomeIndex_t index)
    {
        uint8_t val;
        if (index < getNumberBases())
        {
            if ((index&1)==0)
            {
                val = ((uint8_t *) data)[index>>1] & 0xf;
            }
            else
            {
                val = (((uint8_t *) data)[index>>1] & 0xf0) >> 4;
            }
        }
        else
        {
            val = baseNIndex;
        }
        val = isColorSpace() ? int2colorSpace[val] : int2base[val];
        return val;
    }

    inline uint8_t getInteger(genomeIndex_t index)
    {
        return (*((genomeSequenceArray*) this))[index];
    }
    inline void set(genomeIndex_t index, char value)
    {
        genomeSequenceArray::set(index, base2int[(uint8_t) value]);
    }
    /// obtain the pointer to the raw data for other access methods
    ///
    /// this is a fairly ugly hack to reach into the
    /// raw genome vector, get the byte that encodes
    /// two bases, and return it.  This is used by
    /// karma ReadIndexer::getSumQ to compare genome
    /// matchines by byte (two bases at a time) to speed
    /// it up.
    ///
    uint8_t *getDataPtr(genomeIndex_t index)
    {
        return ((uint8_t *) data + index/2);
    }
private:
    ///
    /// when creating the genome mapped file, we call this to set
    /// the MD5 checksum of the chromosome sequence and length so that
    /// we can write the SAM SQ headers properly.
    /// NB: operates on the last fully loaded chromosome.
    bool setChromosomeMD5andLength(uint32_t whichChromosome);
public:
    // replace read with the reversed one
    void getReverseRead(std::string &read);
    void getReverseRead(String& read);
    // debug the given read - print nice results
    int debugPrintReadValidation(
        std::string &read,
        std::string &quality,
        char   direction,
        genomeIndex_t   readLocation,
        int sumQuality,
        int mismatchCount,
        bool recurse = true
    );

    void getString(std::string &str, int chromosome, genomeIndex_t index, int baseCount);
    void getString(String &str, int chromosome, genomeIndex_t index, int baseCount);
    //
    // get the sequence from this GenomeSequence.
    // if baseCount < 0, get the reverse complement
    // that starts at index (but do not reverse the string?)
    //
    void getString(std::string &str, genomeIndex_t index, int baseCount);
    void getString(String &str, genomeIndex_t index, int baseCount);

    void getHighLightedString(std::string &str, genomeIndex_t index, int baseCount, genomeIndex_t highLightStart, genomeIndex_t highLightEnd);

    void print30(genomeIndex_t);

    // for debugging, not for speed:
    genomeIndex_t simpleLocalAligner(std::string &read, std::string &quality, genomeIndex_t index, int windowSize);

    /// Return the mismatch count, disregarding CIGAR strings
    ///
    /// \param read is the sequence we're counting mismatches in
    /// \param location is where in the genmoe we start comparing
    /// \param exclude is a wildcard character (e.g. '.' or 'N')
    ///
    /// \return number of bases that don't match the reference, except those that match exclude
    int getMismatchCount(std::string &read, genomeIndex_t location, char exclude='\0')
    {
        int mismatchCount = 0;
        for (uint32_t i=0; i<read.size(); i++)
            if (read[i]!=exclude) mismatchCount += read[i]!=(*this)[location + i];
        return mismatchCount;
    };

    /// brute force sumQ - no sanity checking
    ///
    /// \param read shotgun sequencer read string
    /// \param qualities phred quality string of same length
    /// \param location the alignment location to check sumQ
    int getSumQ(std::string &read, std::string &qualities, genomeIndex_t location)
    {
        int sumQ = 0;
        for (uint32_t i=0; i<read.size(); i++)
            sumQ += (read[i]!=(*this)[location + i] ? (qualities[i]-33) : 0);
        return sumQ;
    };
    // return a string highlighting mismatch postions with '^' chars:
    void getMismatchHatString(std::string &result, const std::string &read, genomeIndex_t location);
    void getMismatchString(std::string &result, const std::string &read, genomeIndex_t location);

    void getChromosomeAndIndex(std::string &, genomeIndex_t);
    void getChromosomeAndIndex(String &, genomeIndex_t);

    /// check a SAM format read, using phred quality scores and
    /// the CIGAR string to determine if it is correct.
    ///
    ///
    /// \param read the read in base space
    /// \param qualities the phred encoded qualities (Sanger, not Illumina)
    /// \param cigar the SAM file CIGAR column
    /// \param sumQ if >0 on entry, is checked against the computed sumQ
    /// \param insertions count of insertions found in
    ///
    ///
    bool checkRead(
        std::string &read,
        std::string &qualities,
        std::string &cigar,
        int &sumQ,              // input and output
        int &gapOpenCount,      // output only
        int &gapExtendCount,    // output only
        int &gapDeleteCount,    // output only
        std::string &result
    );

    bool populateDBSNP(mmapArrayBool_t &dbSNP, std::ifstream &inputFile);

    /// user friendly dbSNP loader.
    ///
    /// \param inputFileName may be empty, point to a text file or a dbSNP vector file
    ///
    /// In all cases, dbSNP is returned the same length as this genome.
    ///
    /// When no SNPs are loaded, all values are false.
    ///
    /// When a text file is given, the file is parsed with two space
    /// separated columns - the first column is the chromosome name, and
    /// the second is the 1-based chromosome position of the SNP.
    ///
    /// \return false if a dbSNP file was correctly loaded, true otherwise
    ///
    bool loadDBSNP(mmapArrayBool_t &dbSNP, const char *inputFileName);
};

#endif